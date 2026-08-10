// Skinned glTF model: mesh + node hierarchy + skin + animation clips.
// Loading is cgltf; GPU upload is raw GL 3.3. Animation is sampled on the
// CPU per frame into a joint-matrix palette (few dozen bones -- cheap).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"

struct SkinVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t joints[4];
    float weights[4];
};

struct Submesh {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t gl_texture = 0;   // 0 = untextured
    bool alpha_blend = false;  // face decals (eyes/mouth/brows)
};

struct ModelNode {
    int parent = -1;  // index into nodes, -1 = root
    std::string name;
    Vec3 t{};
    Quat r{};
    Vec3 s{1, 1, 1};
};

// one animated property track on one node
struct AnimChannel {
    int node = -1;
    int path = 0;  // 0=translation 1=rotation 2=scale
    std::vector<float> times;
    std::vector<float> values;  // vec3 or quat per key
};

struct AnimClip {
    std::string name;
    float start = 0;     // first key time (Blender frame 1 exports as 1/30)
    float duration = 0;  // last key time
    std::vector<AnimChannel> channels;
};

struct TRS {
    Vec3 t{};
    Quat r{};
    Vec3 s{1, 1, 1};
};
using Pose = std::vector<TRS>;  // one TRS per node

struct Model {
    // GPU
    uint32_t vao = 0, vbo = 0, ebo = 0;
    std::vector<Submesh> submeshes;

    // scene graph (rest pose) + skin
    std::vector<ModelNode> nodes;
    std::vector<int> skin_joints;        // node index per palette slot
    std::vector<Mat4> inverse_bind;      // per palette slot
    int mesh_node = -1;                  // node carrying the skinned mesh

    std::vector<AnimClip> clips;
    std::vector<int> topo_order;  // node indices, parents always first

    // scratch, sized to nodes/joints
    std::vector<Mat4> local_pose;
    std::vector<Mat4> world_pose;
    std::vector<Mat4> palette;

    bool load(const char* glb_path);     // logs and returns false on failure
    bool load_memory(const void* bytes, size_t size);  // embedded glb blob
    bool load_parsed(struct cgltf_data* data);  // shared tail; frees `data`

    // sample `clip` at time t (seconds, caller wraps/clamps)
    void sample(const AnimClip& clip, float t, Pose& out) const;
    static void blend(const Pose& a, const Pose& b, float w, Pose& out);
    void palette_from(const Pose& pose);           // fills `palette`
    void evaluate(const AnimClip& clip, float t);  // sample + palette_from

    const AnimClip* find_clip(const std::string& name) const;

    // bone-attached prop (shield/sword): verts stay in the prop's local
    // space, weighted to its own palette slot; the game re-sockets it by
    // changing anchor_node/offset (palette[slot] = world(anchor) * offset)
    struct Attachment {
        std::string name;
        int slot = -1;         // palette slot (after the skin joints)
        int anchor_node = -1;  // node driving it
        Mat4 offset;           // local placement relative to the anchor
    };
    std::vector<Attachment> attachments;

    int find_node(const char* name) const;
    Attachment* find_attachment(const char* name);

    // 1 for `root_name` and every node under it (parents-first walk)
    std::vector<uint8_t> subtree_mask(const char* root_name) const;

    Pose scratch_a, scratch_b, scratch_mix, scratch_ov;
};
