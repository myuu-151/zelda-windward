#include "model.h"

#include <SDL3/SDL.h>

#include <cstring>

#include "gl_loader.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

namespace {

int node_index(const cgltf_data* data, const cgltf_node* node)
{
    return node ? static_cast<int>(node - data->nodes) : -1;
}

uint32_t upload_texture(const cgltf_texture* tex, bool* has_alpha)
{
    if (!tex || !tex->image) return 0;
    const cgltf_image* img = tex->image;
    if (!img->buffer_view) return 0;

    const auto* bytes =
        static_cast<const stbi_uc*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        bytes, static_cast<int>(img->buffer_view->size), &w, &h, &comp, 4);
    if (!pixels) {
        SDL_Log("texture decode failed: %s", stbi_failure_reason());
        return 0;
    }
    if (has_alpha) {
        *has_alpha = false;
        for (int i = 0; i < w * h && !*has_alpha; ++i) {
            if (pixels[i * 4 + 3] < 250) *has_alpha = true;
        }
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);  // smooth (bilinear)
    // the rip's materials rely on mirrored tiling (half-textures mirrored
    // across the face) -- plain GL_REPEAT turns the eyes into black blobs
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    stbi_image_free(pixels);
    return id;
}

}  // namespace

bool Model::load(const char* glb_path)
{
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, glb_path, &data) != cgltf_result_success) {
        SDL_Log("cgltf parse failed: %s", glb_path);
        return false;
    }
    if (cgltf_load_buffers(&opts, data, glb_path) != cgltf_result_success) {
        SDL_Log("cgltf buffer load failed");
        cgltf_free(data);
        return false;
    }
    return load_parsed(data);
}

bool Model::load_memory(const void* bytes, size_t size)
{
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&opts, bytes, size, &data) != cgltf_result_success) {
        SDL_Log("cgltf parse (memory) failed");
        return false;
    }
    // GLB keeps its buffers in the blob itself, so no base path is needed
    if (cgltf_load_buffers(&opts, data, nullptr) != cgltf_result_success) {
        SDL_Log("cgltf buffer load (memory) failed");
        cgltf_free(data);
        return false;
    }
    return load_parsed(data);
}

bool Model::load_parsed(cgltf_data* data)
{
    // ---- nodes (rest pose) ----
    nodes.resize(data->nodes_count);
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& n = data->nodes[i];
        ModelNode& out = nodes[i];
        out.parent = node_index(data, n.parent);
        if (n.name) out.name = n.name;
        if (n.has_translation) out.t = {n.translation[0], n.translation[1], n.translation[2]};
        if (n.has_rotation) out.r = {n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]};
        if (n.has_scale) out.s = {n.scale[0], n.scale[1], n.scale[2]};
        if (n.mesh) mesh_node = static_cast<int>(i);
    }

    // ---- skin ----
    if (data->skins_count == 0) {
        SDL_Log("glb has no skin");
        cgltf_free(data);
        return false;
    }
    const cgltf_skin& skin = data->skins[0];
    skin_joints.resize(skin.joints_count);
    inverse_bind.resize(skin.joints_count);
    for (size_t j = 0; j < skin.joints_count; ++j) {
        skin_joints[j] = node_index(data, skin.joints[j]);
        float m[16];
        cgltf_accessor_read_float(skin.inverse_bind_matrices, j, m, 16);
        std::memcpy(inverse_bind[j].m, m, sizeof(m));
    }

    // ---- mesh ----
    const cgltf_mesh* mesh = nullptr;
    for (size_t i = 0; i < data->nodes_count; ++i) {
        if (data->nodes[i].mesh && data->nodes[i].skin) {
            mesh = data->nodes[i].mesh;
            break;
        }
    }
    if (!mesh) {
        SDL_Log("no skinned mesh in glb");
        cgltf_free(data);
        return false;
    }

    std::vector<SkinVertex> vertices;
    std::vector<uint32_t> indices;
    for (size_t p = 0; p < mesh->primitives_count; ++p) {
        const cgltf_primitive& prim = mesh->primitives[p];
        const cgltf_accessor* a_pos = nullptr;
        const cgltf_accessor* a_nrm = nullptr;
        const cgltf_accessor* a_uv = nullptr;
        const cgltf_accessor* a_jnt = nullptr;
        const cgltf_accessor* a_wgt = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a) {
            const cgltf_attribute& at = prim.attributes[a];
            if (at.type == cgltf_attribute_type_position) a_pos = at.data;
            else if (at.type == cgltf_attribute_type_normal) a_nrm = at.data;
            else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) a_uv = at.data;
            else if (at.type == cgltf_attribute_type_joints) a_jnt = at.data;
            else if (at.type == cgltf_attribute_type_weights) a_wgt = at.data;
        }
        if (!a_pos || !prim.indices) continue;

        const uint32_t base_vertex = static_cast<uint32_t>(vertices.size());
        const size_t vcount = a_pos->count;
        for (size_t v = 0; v < vcount; ++v) {
            SkinVertex sv{};
            cgltf_accessor_read_float(a_pos, v, sv.pos, 3);
            if (a_nrm) cgltf_accessor_read_float(a_nrm, v, sv.normal, 3);
            if (a_uv) cgltf_accessor_read_float(a_uv, v, sv.uv, 2);
            if (a_jnt) {
                cgltf_uint j[4] = {0, 0, 0, 0};
                cgltf_accessor_read_uint(a_jnt, v, j, 4);
                for (int k = 0; k < 4; ++k) sv.joints[k] = static_cast<uint8_t>(j[k]);
            }
            if (a_wgt) {
                cgltf_accessor_read_float(a_wgt, v, sv.weights, 4);
            } else {
                sv.weights[0] = 1.0f;
            }
            vertices.push_back(sv);
        }

        Submesh sub;
        sub.first_index = static_cast<uint32_t>(indices.size());
        sub.index_count = static_cast<uint32_t>(prim.indices->count);
        for (size_t i = 0; i < prim.indices->count; ++i) {
            indices.push_back(base_vertex +
                              static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
        }
        const char* mat_name =
            (prim.material && prim.material->name) ? prim.material->name : "?";
        if (prim.material && prim.material->has_pbr_metallic_roughness) {
            const cgltf_texture_view& tv =
                prim.material->pbr_metallic_roughness.base_color_texture;
            sub.gl_texture = upload_texture(tv.texture, &sub.alpha_blend);
        }
        if (prim.material && prim.material->alpha_mode != cgltf_alpha_mode_opaque)
            sub.alpha_blend = true;
        SDL_Log("submesh %zu: mat=%s tex=%u alpha=%d indices=%u", p, mat_name,
                sub.gl_texture, sub.alpha_blend ? 1 : 0, sub.index_count);
        submeshes.push_back(sub);
    }
    // ---- attachments: meshes on plain nodes (bone-parented props like the
    // shield). Bake the node's offset from its anchor joint into the verts
    // and weight them 100% to that joint -- they ride the skinning for free.
    {
        std::vector<int> joint_slot(data->nodes_count, -1);
        for (size_t j = 0; j < skin_joints.size(); ++j)
            joint_slot[skin_joints[j]] = static_cast<int>(j);
        for (size_t i = 0; i < data->nodes_count; ++i) {
            const cgltf_node& n = data->nodes[i];
            if (!n.mesh || n.skin) continue;
            // accumulate transform up to the nearest skin-joint ancestor
            float lm[16];
            cgltf_node_transform_local(&n, lm);
            Mat4 rel;
            std::memcpy(rel.m, lm, sizeof(lm));
            int slot = -1;
            for (const cgltf_node* a = n.parent; a; a = a->parent) {
                const int ai = node_index(data, a);
                if (joint_slot[ai] >= 0) {
                    slot = joint_slot[ai];
                    break;
                }
                cgltf_node_transform_local(a, lm);
                Mat4 pm;
                std::memcpy(pm.m, lm, sizeof(lm));
                rel = pm * rel;
            }
            if (slot < 0) continue;  // not hanging off the skeleton

            // dedicated palette slot: palette[slot] = world(anchor) * offset,
            // so the game can re-socket the prop (sheath <-> hand) at runtime
            Attachment att;
            att.name = n.name ? n.name : "prop";
            att.slot = static_cast<int>(skin_joints.size() + attachments.size());
            att.anchor_node = skin_joints[slot];
            att.offset = rel;
            const int prop_slot = att.slot;
            attachments.push_back(att);

            for (size_t p = 0; p < n.mesh->primitives_count; ++p) {
                const cgltf_primitive& prim = n.mesh->primitives[p];
                const cgltf_accessor* a_pos = nullptr;
                const cgltf_accessor* a_nrm = nullptr;
                const cgltf_accessor* a_uv = nullptr;
                for (size_t a = 0; a < prim.attributes_count; ++a) {
                    const cgltf_attribute& at = prim.attributes[a];
                    if (at.type == cgltf_attribute_type_position) a_pos = at.data;
                    else if (at.type == cgltf_attribute_type_normal) a_nrm = at.data;
                    else if (at.type == cgltf_attribute_type_texcoord && at.index == 0)
                        a_uv = at.data;
                }
                if (!a_pos || !prim.indices) continue;
                const uint32_t base_vertex = static_cast<uint32_t>(vertices.size());
                for (size_t v = 0; v < a_pos->count; ++v) {
                    SkinVertex sv{};
                    cgltf_accessor_read_float(a_pos, v, sv.pos, 3);
                    if (a_nrm) cgltf_accessor_read_float(a_nrm, v, sv.normal, 3);
                    if (a_uv) cgltf_accessor_read_float(a_uv, v, sv.uv, 2);
                    sv.joints[0] = static_cast<uint8_t>(prop_slot);
                    sv.weights[0] = 1.0f;
                    vertices.push_back(sv);
                }
                Submesh sub;
                sub.first_index = static_cast<uint32_t>(indices.size());
                sub.index_count = static_cast<uint32_t>(prim.indices->count);
                for (size_t k = 0; k < prim.indices->count; ++k)
                    indices.push_back(base_vertex + static_cast<uint32_t>(
                                          cgltf_accessor_read_index(prim.indices, k)));
                if (prim.material && prim.material->has_pbr_metallic_roughness) {
                    sub.gl_texture = upload_texture(
                        prim.material->pbr_metallic_roughness.base_color_texture.texture,
                        &sub.alpha_blend);
                }
                if (prim.material && prim.material->alpha_mode != cgltf_alpha_mode_opaque)
                    sub.alpha_blend = true;
                submeshes.push_back(sub);
                SDL_Log("attachment %s: %zu verts, palette slot %d, anchor node %d",
                        n.name ? n.name : "?", static_cast<size_t>(a_pos->count),
                        prop_slot, att.anchor_node);
            }
        }
    }

    SDL_Log("model: %zu verts, %zu indices, %zu submeshes, %zu joints, %zu nodes",
            vertices.size(), indices.size(), submeshes.size(), skin_joints.size(),
            nodes.size());

    // ---- animations ----
    clips.resize(data->animations_count);
    for (size_t a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& anim = data->animations[a];
        AnimClip& clip = clips[a];
        clip.name = anim.name ? anim.name : ("anim" + std::to_string(a));
        for (size_t c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            if (!ch.target_node || !ch.sampler) continue;
            int path;
            int comps;
            switch (ch.target_path) {
                case cgltf_animation_path_type_translation: path = 0; comps = 3; break;
                case cgltf_animation_path_type_rotation: path = 1; comps = 4; break;
                case cgltf_animation_path_type_scale: path = 2; comps = 3; break;
                default: continue;
            }
            AnimChannel out;
            out.node = node_index(data, ch.target_node);
            out.path = path;
            const cgltf_accessor* in = ch.sampler->input;
            const cgltf_accessor* val = ch.sampler->output;
            out.times.resize(in->count);
            for (size_t k = 0; k < in->count; ++k)
                cgltf_accessor_read_float(in, k, &out.times[k], 1);
            out.values.resize(val->count * comps);
            for (size_t k = 0; k < val->count; ++k)
                cgltf_accessor_read_float(val, k, &out.values[k * comps], comps);
            if (!out.times.empty()) {
                clip.duration = std::max(clip.duration, out.times.back());
                clip.start = clip.channels.empty()
                                 ? out.times.front()
                                 : std::min(clip.start, out.times.front());
            }
            clip.channels.push_back(std::move(out));
        }
        SDL_Log("clip %-12s %5.2fs  (%zu channels)", clip.name.c_str(),
                clip.duration, clip.channels.size());
    }

    // ---- GPU upload ----
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(SkinVertex)),
                 vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                 indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = sizeof(SkinVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SkinVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SkinVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SkinVertex, uv)));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, stride,
                           reinterpret_cast<void*>(offsetof(SkinVertex, joints)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SkinVertex, weights)));
    glBindVertexArray(0);

    local_pose.resize(nodes.size());
    world_pose.resize(nodes.size());
    palette.resize(skin_joints.size() + attachments.size());

    // parents-first traversal order (exporter order is usually fine, but
    // don't bet the skeleton on it)
    topo_order.reserve(nodes.size());
    std::vector<char> placed(nodes.size(), 0);
    while (topo_order.size() < nodes.size()) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (placed[i]) continue;
            const int p = nodes[i].parent;
            if (p < 0 || placed[p]) {
                topo_order.push_back(static_cast<int>(i));
                placed[i] = 1;
            }
        }
    }

    cgltf_free(data);
    return true;
}

void Model::sample(const AnimClip& clip, float t, Pose& pose) const
{
    // rest pose first
    pose.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        pose[i] = {nodes[i].t, nodes[i].r, nodes[i].s};

    // overlay animated channels
    for (const AnimChannel& ch : clip.channels) {
        if (ch.node < 0 || ch.times.empty()) continue;
        const size_t n = ch.times.size();
        size_t k1 = 0;
        while (k1 < n && ch.times[k1] < t) ++k1;
        const size_t k0 = k1 > 0 ? k1 - 1 : 0;
        if (k1 >= n) k1 = n - 1;
        const float t0 = ch.times[k0];
        const float t1 = ch.times[k1];
        const float f = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
        const float fc = f < 0 ? 0.0f : (f > 1 ? 1.0f : f);

        if (ch.path == 0 || ch.path == 2) {
            const float* v0 = &ch.values[k0 * 3];
            const float* v1 = &ch.values[k1 * 3];
            const Vec3 v = lerp({v0[0], v0[1], v0[2]}, {v1[0], v1[1], v1[2]}, fc);
            if (ch.path == 0) pose[ch.node].t = v;
            else pose[ch.node].s = v;
        } else {
            const float* q0 = &ch.values[k0 * 4];
            const float* q1 = &ch.values[k1 * 4];
            pose[ch.node].r = nlerp({q0[0], q0[1], q0[2], q0[3]},
                                    {q1[0], q1[1], q1[2], q1[3]}, fc);
        }
    }
}

void Model::blend(const Pose& a, const Pose& b, float w, Pose& out)
{
    out.resize(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        out[i].t = lerp(a[i].t, b[i].t, w);
        out[i].r = nlerp(a[i].r, b[i].r, w);
        out[i].s = lerp(a[i].s, b[i].s, w);
    }
}

void Model::palette_from(const Pose& pose)
{
    for (size_t i = 0; i < nodes.size(); ++i)
        local_pose[i] = mat4_from_trs(pose[i].t, pose[i].r, pose[i].s);
    for (const int i : topo_order) {
        const int p = nodes[i].parent;
        world_pose[i] = (p >= 0) ? world_pose[p] * local_pose[i] : local_pose[i];
    }
    for (size_t j = 0; j < skin_joints.size(); ++j)
        palette[j] = world_pose[skin_joints[j]] * inverse_bind[j];
    for (const Attachment& att : attachments)
        if (att.anchor_node >= 0)
            palette[att.slot] = world_pose[att.anchor_node] * att.offset;
}

int Model::find_node(const char* name) const
{
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == name) return static_cast<int>(i);
    return -1;
}

Model::Attachment* Model::find_attachment(const char* name)
{
    for (Attachment& a : attachments)
        if (a.name == name) return &a;
    return nullptr;
}

void Model::evaluate(const AnimClip& clip, float t)
{
    sample(clip, t, scratch_a);
    palette_from(scratch_a);
}

const AnimClip* Model::find_clip(const std::string& name) const
{
    for (const AnimClip& c : clips)
        if (c.name == name) return &c;
    return nullptr;
}

std::vector<uint8_t> Model::subtree_mask(const char* root_name) const
{
    std::vector<uint8_t> mask(nodes.size(), 0);
    for (const int i : topo_order) {
        const int p = nodes[i].parent;
        mask[i] = nodes[i].name == root_name || (p >= 0 && mask[p]);
    }
    return mask;
}
