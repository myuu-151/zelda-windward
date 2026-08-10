// Zelda fangame client -- SDL3 + OpenGL 3.3 core.
// Milestone 1: skinned Link from assets/link.glb, all clips playable.
//   left/right : switch animation clip
//   drag       : orbit camera   wheel : zoom
//   esc        : quit
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gl_loader.h"
#include "math3d.h"
#include "model.h"
#include "player.h"

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr double kFixedDt = 1.0 / 60.0;
constexpr Vec3 kTargetPos{0.0f, 0.35f, 4.0f};  // the lock-on cube

const char* kSkinVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in uvec4 aJoints;
layout(location=4) in vec4 aWeights;

uniform mat4 uViewProj;
uniform mat4 uModel;
uniform mat4 uPalette[64];

out vec3 vNormal;
out vec2 vUV;

void main() {
    mat4 skin = aWeights.x * uPalette[aJoints.x]
              + aWeights.y * uPalette[aJoints.y]
              + aWeights.z * uPalette[aJoints.z]
              + aWeights.w * uPalette[aJoints.w];
    vec4 world = uModel * skin * vec4(aPos, 1.0);
    vNormal = normalize(mat3(uModel) * mat3(skin) * aNormal);
    vUV = aUV;
    gl_Position = uViewProj * world;
}
)GLSL";

const char* kSkinFS = R"GLSL(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
uniform sampler2D uTex;
uniform vec3 uSunDir;
out vec4 fragColor;

void main() {
    vec4 albedo = texture(uTex, vUV);
    // ramp the filtered alpha so decal edges (eyebrows) stay smooth but sharp
    albedo.a = smoothstep(0.35, 0.65, albedo.a);
    if (albedo.a < 0.01) discard;
    float ndl = max(dot(normalize(vNormal), -uSunDir), 0.0);
    // two-band toon shading
    float light = ndl > 0.35 ? 1.0 : 0.72;
    fragColor = vec4(albedo.rgb * light, albedo.a);
}
)GLSL";

const char* kCubeVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in float aShade;
uniform mat4 uViewProj;
out float vShade;
void main() { vShade = aShade; gl_Position = uViewProj * vec4(aPos, 1.0); }
)GLSL";

const char* kCubeFS = R"GLSL(
#version 330 core
in float vShade;
out vec4 fragColor;
void main() { fragColor = vec4(vec3(0.75, 0.45, 0.25) * vShade, 1.0); }
)GLSL";

const char* kGridVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uViewProj;
out vec3 vWorld;
void main() { vWorld = aPos; gl_Position = uViewProj * vec4(aPos, 1.0); }
)GLSL";

const char* kGridFS = R"GLSL(
#version 330 core
in vec3 vWorld;
out vec4 fragColor;
void main() {
    vec2 g = abs(fract(vWorld.xz - 0.5) - 0.5) / fwidth(vWorld.xz);
    float line = 1.0 - min(min(g.x, g.y), 1.0);
    vec3 base = vec3(0.16, 0.32, 0.20);
    fragColor = vec4(mix(base, vec3(0.24, 0.44, 0.28), line), 1.0);
}
)GLSL";

GLuint compile_shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        SDL_Log("shader compile error:\n%s", log);
        return 0;
    }
    return s;
}

GLuint link_program(const char* vs_src, const char* fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        SDL_Log("program link error:\n%s", log);
        return 0;
    }
    return p;
}

struct OrbitCamera {
    float yaw = 0.6f;       // radians around the character
    float pitch = 0.35f;
    float distance = 3.4f;
    Vec3 target{0.0f, 0.85f, 0.0f};

    Vec3 eye() const
    {
        const float cp = std::cos(pitch);
        return target + Vec3{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)} * distance;
    }
};

struct App {
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    bool running = true;
    double sim_time = 0.0;

    Model link;
    int clip_index = 0;
    double clip_time = 0.0;
    OrbitCamera cam;
    bool dragging = false;

    Player player;
    float lock_blend = 0.0f;   // eased 0..1 lock-on camera engagement
    float free_yaw = 0.6f;     // camera angle to restore after lock-on
    float free_pitch = 0.35f;
    bool was_locked = false;
    bool lock_cam_manual = false;  // user orbited during lock: stop auto-framing
    bool viewer_mode = false;  // F1: clip viewer (arrow keys) vs play mode
    SDL_Gamepad* pad = nullptr;
    bool key_attack = false, key_roll = false;  // edge flags for this sim step
};

bool save_screenshot(const char* path, int fb_w, int fb_h)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(fb_w) * fb_h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // GL reads bottom-up; flip rows
    std::vector<uint8_t> flipped(pixels.size());
    const size_t row = static_cast<size_t>(fb_w) * 4;
    for (int y = 0; y < fb_h; ++y)
        std::memcpy(&flipped[row * y], &pixels[row * (fb_h - 1 - y)], row);
    SDL_Surface* surf = SDL_CreateSurfaceFrom(fb_w, fb_h, SDL_PIXELFORMAT_ABGR8888,
                                              flipped.data(), static_cast<int>(row));
    if (!surf) return false;
    const bool ok = SDL_SaveBMP(surf, path);
    SDL_DestroySurface(surf);
    SDL_Log("screenshot %s: %s", path, ok ? "saved" : SDL_GetError());
    return ok;
}

void set_title(App& app, Uint64 fps)
{
    char title[160];
    if (app.viewer_mode) {
        const std::string clip =
            app.link.clips.empty() ? "none" : app.link.clips[app.clip_index].name;
        std::snprintf(title, sizeof(title),
                      "Zelda Fangame [viewer]  clip: %s (left/right)  |  %llu fps",
                      clip.c_str(), static_cast<unsigned long long>(fps));
    } else {
        std::snprintf(title, sizeof(title),
                      "Zelda Fangame  WASD move, space roll, J attack, shift guard, "
                      "F1 viewer  |  %llu fps",
                      static_cast<unsigned long long>(fps));
    }
    SDL_SetWindowTitle(app.window, title);
}

}  // namespace

int main(int argc, char** argv)
{
    // --screenshot <path> [clip] [time] [distance] : render one frame, save, exit
    const char* shot_path = nullptr;
    const char* shot_clip = "Idle";
    float shot_time = 0.5f;
    float shot_distance = 0.0f;
    float shot_yaw = 1000.0f;  // sentinel = keep default
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--screenshot" && i + 1 < argc) {
            shot_path = argv[i + 1];
            if (i + 2 < argc) shot_clip = argv[i + 2];
            if (i + 3 < argc) shot_time = static_cast<float>(std::atof(argv[i + 3]));
            if (i + 4 < argc) shot_distance = static_cast<float>(std::atof(argv[i + 4]));
            if (i + 5 < argc) shot_yaw = static_cast<float>(std::atof(argv[i + 5]));
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    App app;
    app.window = SDL_CreateWindow("Zelda Fangame", kWindowWidth, kWindowHeight,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app.window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    app.gl = SDL_GL_CreateContext(app.window);
    if (!app.gl || !gl_load_functions()) {
        SDL_Log("GL context/loader failed: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    SDL_Log("GL_VERSION:  %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    // asset path: exe dir /../../assets (build/Debug/zelda.exe) with cwd fallback
    std::string glb = std::string(SDL_GetBasePath()) + "..\\..\\assets\\link.glb";
    if (!app.link.load(glb.c_str()) && !app.link.load("assets/link.glb")) {
        SDL_Log("could not load link.glb");
        return 1;
    }
    if (const AnimClip* idle = app.link.find_clip("Idle"))
        app.clip_index = static_cast<int>(idle - app.link.clips.data());

    // --- shield sockets: sheath (as authored) and forearm (computed from the
    // guard stance: keep the sheathed orientation but spin it to face front,
    // planted ahead of the chest, then expressed relative to the forearm) ---
    Model::Attachment shield_sheath{};
    Model::Attachment shield_arm{};
    if (Model::Attachment* sh = app.link.find_attachment("Shield")) {
        shield_sheath = *sh;
        const int forearm = app.link.find_node("RarmB_jnt_bone_id");
        if (const AnimClip* guard_clip = app.link.find_clip("Guard");
            guard_clip && forearm >= 0) {
            app.link.sample(*guard_clip, guard_clip->start + 0.2f, app.link.scratch_a);
            app.link.palette_from(app.link.scratch_a);  // fills world_pose
            const Mat4 sheath_world =
                app.link.world_pose[shield_sheath.anchor_node] * shield_sheath.offset;
            Mat4 desired = mat4_from_trs({0, 0, 0}, {0, 1, 0, 0}, {1, 1, 1}) * sheath_world;
            desired.m[12] = -0.05f;  // guard placement: ahead of the chest
            desired.m[13] = 0.70f;
            desired.m[14] = 0.30f;
            shield_arm.name = shield_sheath.name;
            shield_arm.slot = shield_sheath.slot;
            shield_arm.anchor_node = forearm;
            shield_arm.offset = mat4_inverse(app.link.world_pose[forearm]) * desired;
        }
    }
    if (shot_path) {
        if (const AnimClip* c = app.link.find_clip(shot_clip))
            app.clip_index = static_cast<int>(c - app.link.clips.data());
        app.clip_time = shot_time;
        if (shot_distance > 0) app.cam.distance = shot_distance;
        if (shot_yaw < 100.0f) app.cam.yaw = shot_yaw;
        app.viewer_mode = true;  // screenshots use the plain clip viewer
    }
    bool want_shot = false;
    app.player.init(app.link);

    const GLuint skin_prog = link_program(kSkinVS, kSkinFS);
    const GLuint grid_prog = link_program(kGridVS, kGridFS);
    if (!skin_prog || !grid_prog) return 1;
    const GLint u_viewproj = glGetUniformLocation(skin_prog, "uViewProj");
    const GLint u_model = glGetUniformLocation(skin_prog, "uModel");
    const GLint u_palette = glGetUniformLocation(skin_prog, "uPalette");
    const GLint u_tex = glGetUniformLocation(skin_prog, "uTex");
    const GLint u_sun = glGetUniformLocation(skin_prog, "uSunDir");
    const GLint g_viewproj = glGetUniformLocation(grid_prog, "uViewProj");

    // ground quad
    const float ground[] = {-20, 0, -20, 20, 0, -20, 20, 0, 20,
                            -20, 0, -20, 20, 0, 20, -20, 0, 20};
    GLuint grid_vao = 0, grid_vbo = 0;
    glGenVertexArrays(1, &grid_vao);
    glBindVertexArray(grid_vao);
    glGenBuffers(1, &grid_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ground), ground, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
    glBindVertexArray(0);

    // target cube (per-face shade for readability)
    const GLuint cube_prog = link_program(kCubeVS, kCubeFS);
    const GLint c_viewproj = glGetUniformLocation(cube_prog, "uViewProj");
    std::vector<float> cube;
    {
        const float h = 0.35f;
        const Vec3 c = kTargetPos;
        const float faces[6][4] = {  // axis, sign, shade
            {0, 1, 0, 0.85f}, {0, -1, 0, 0.55f}, {1, 1, 0, 1.0f},
            {1, -1, 0, 0.4f}, {2, 1, 0, 0.7f}, {2, -1, 0, 0.7f}};
        for (const auto& f : faces) {
            const int axis = static_cast<int>(f[0]);
            const float sgn = f[1];
            const float shade = f[3];
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            float base[3] = {c.x, c.y, c.z};
            base[axis] += sgn * h;
            const float du[2] = {-h, h};
            float quad[4][3];
            int corner = 0;
            for (const float su : du)
                for (const float sv : du) {
                    float p[3] = {base[0], base[1], base[2]};
                    p[u] += su;
                    p[v] += sv;
                    for (int k = 0; k < 3; ++k) quad[corner][k] = p[k];
                    ++corner;
                }
            const int tri[6] = {0, 1, 2, 2, 1, 3};
            for (const int idx : tri) {
                cube.insert(cube.end(), quad[idx], quad[idx] + 3);
                cube.push_back(shade);
            }
        }
    }
    GLuint cube_vao = 0, cube_vbo = 0;
    glGenVertexArrays(1, &cube_vao);
    glBindVertexArray(cube_vao);
    glGenBuffers(1, &cube_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, cube_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(cube.size() * sizeof(float)),
                 cube.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 16, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(12));
    glBindVertexArray(0);

    Uint64 prev_ns = SDL_GetTicksNS();
    double accumulator = 0.0;
    Uint64 fps_frames = 0;
    Uint64 fps_last_ns = prev_ns;
    set_title(app, 0);

    while (app.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    app.running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_ESCAPE) app.running = false;
                    if (ev.key.key == SDLK_F2) want_shot = true;
                    if (ev.key.key == SDLK_F1 && !ev.key.repeat) {
                        app.viewer_mode = !app.viewer_mode;
                        set_title(app, 0);
                    }
                    if (!ev.key.repeat && !app.viewer_mode) {
                        if (ev.key.key == SDLK_J) app.key_attack = true;
                        if (ev.key.key == SDLK_SPACE) app.key_roll = true;
                    }
                    if (app.viewer_mode && !app.link.clips.empty()) {
                        const int n = static_cast<int>(app.link.clips.size());
                        if (ev.key.key == SDLK_RIGHT) {
                            app.clip_index = (app.clip_index + 1) % n;
                            app.clip_time = 0.0;
                            set_title(app, 0);
                        } else if (ev.key.key == SDLK_LEFT) {
                            app.clip_index = (app.clip_index + n - 1) % n;
                            app.clip_time = 0.0;
                            set_title(app, 0);
                        }
                    }
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!app.pad) app.pad = SDL_OpenGamepad(ev.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (app.pad) { SDL_CloseGamepad(app.pad); app.pad = nullptr; }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (!app.viewer_mode) {
                        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) app.key_roll = true;
                        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) app.key_attack = true;
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) app.dragging = true;
                    if (ev.button.button == SDL_BUTTON_RIGHT && !app.viewer_mode)
                        app.key_attack = true;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT) app.dragging = false;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (app.dragging) {
                        app.cam.yaw -= ev.motion.xrel * 0.008f;
                        app.cam.pitch += ev.motion.yrel * 0.006f;
                        if (app.cam.pitch > 1.4f) app.cam.pitch = 1.4f;
                        if (app.cam.pitch < -0.2f) app.cam.pitch = -0.2f;
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    app.cam.distance -= ev.wheel.y * 0.3f;
                    if (app.cam.distance < 1.2f) app.cam.distance = 1.2f;
                    if (app.cam.distance > 12.0f) app.cam.distance = 12.0f;
                    break;
                default:
                    break;
            }
        }

        const Uint64 now_ns = SDL_GetTicksNS();
        double frame_dt = static_cast<double>(now_ns - prev_ns) / 1e9;
        prev_ns = now_ns;
        if (frame_dt > 0.25) frame_dt = 0.25;
        accumulator += frame_dt;
        while (accumulator >= kFixedDt) {
            app.sim_time += kFixedDt;
            app.clip_time += kFixedDt;
            if (!app.viewer_mode) {
                // gather input: keyboard + gamepad, camera-relative
                const bool* keys = SDL_GetKeyboardState(nullptr);
                float in_x = 0, in_y = 0;  // stick space: y = forward
                if (keys[SDL_SCANCODE_W]) in_y += 1;
                if (keys[SDL_SCANCODE_S]) in_y -= 1;
                if (keys[SDL_SCANCODE_D]) in_x += 1;
                if (keys[SDL_SCANCODE_A]) in_x -= 1;
                bool guard = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                if (app.pad) {
                    in_x += SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
                    in_y -= SDL_GetGamepadAxis(app.pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
                    guard = guard ||
                            SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
                }
                const float mag = std::sqrt(in_x * in_x + in_y * in_y);
                if (mag > 1.0f) { in_x /= mag; in_y /= mag; }
                // camera basis on the ground plane
                const Vec3 eye = app.cam.eye();
                Vec3 fwd = app.cam.target - eye;
                fwd.y = 0;
                fwd = normalize(fwd);
                const Vec3 right{-fwd.z, 0, fwd.x};

                Player::Input in;
                in.move_x = fwd.x * in_y + right.x * in_x;
                in.move_z = fwd.z * in_y + right.z * in_x;
                in.attack_pressed = app.key_attack;
                in.roll_pressed = app.key_roll;
                in.guard_held = guard;
                in.has_target = true;
                in.target_pos = kTargetPos;
                app.key_attack = app.key_roll = false;
                app.player.update(in, static_cast<float>(kFixedDt));
            }
            accumulator -= kFixedDt;
        }

        // shield rides the forearm while guarding, the back otherwise
        if (Model::Attachment* sh = app.link.find_attachment("Shield")) {
            const bool arm_socket =
                app.viewer_mode
                    ? (!app.link.clips.empty() &&
                       app.link.clips[app.clip_index].name == "Guard")
                    : (app.player.anim.overlay != nullptr);
            *sh = (arm_socket && shield_arm.slot >= 0) ? shield_arm : shield_sheath;
        }

        // animate
        if (app.viewer_mode) {
            if (!app.link.clips.empty()) {
                const AnimClip& clip = app.link.clips[app.clip_index];
                float t = 0.0f;
                if (clip.duration > 0.0f)
                    t = static_cast<float>(std::fmod(app.clip_time, clip.duration));
                app.link.evaluate(clip, t);
            }
        } else {
            app.player.anim.apply(app.link);
            // camera: free mode tracks Link exactly (no lag); the lock-on
            // framing fades in/out via lock_blend so toggling never snaps
            const Vec3 head = app.player.pos + Vec3{0, 0.85f, 0};
            const float k = std::min(1.0f, 3.5f * static_cast<float>(frame_dt));
            app.lock_blend += ((app.player.locked ? 1.0f : 0.0f) - app.lock_blend) * k;
            if (app.player.locked && !app.was_locked) {
                app.free_yaw = app.cam.yaw;  // remember the pre-lock angle
                app.free_pitch = app.cam.pitch;
                app.lock_cam_manual = false;
            }
            app.was_locked = app.player.locked;
            if (app.player.locked && app.dragging) app.lock_cam_manual = true;
            if (!app.player.locked && app.lock_blend > 0.01f && !app.dragging) {
                // ease back to where the camera was before locking on
                float dy = app.free_yaw - app.cam.yaw;
                while (dy > 3.14159265f) dy -= 6.2831853f;
                while (dy < -3.14159265f) dy += 6.2831853f;
                app.cam.yaw += dy * k;
                app.cam.pitch += (app.free_pitch - app.cam.pitch) * k;
            }
            if (app.player.locked && !app.lock_cam_manual) {
                Vec3 away = app.player.pos - kTargetPos;
                away.y = 0;
                // behind Link, nudged off-axis so he sits off-center (cinematic)
                const float want_yaw = std::atan2(away.x, away.z) + 0.22f;
                float dy = want_yaw - app.cam.yaw;
                while (dy > 3.14159265f) dy -= 6.2831853f;
                while (dy < -3.14159265f) dy += 6.2831853f;
                app.cam.yaw += dy * k;
                app.cam.pitch += (0.18f - app.cam.pitch) * k;  // low, dramatic
            }
            // bias toward the cube by a CAPPED offset so backing away from it
            // doesn't slide Link toward the lens (fake zoom)
            Vec3 to_cube = (kTargetPos + Vec3{0, 0.5f, 0}) - head;
            const float cube_dist = std::sqrt(dot(to_cube, to_cube));
            const float bias = std::min(0.6f, cube_dist * 0.15f);
            const Vec3 lock_target =
                cube_dist > 1e-4f ? head + to_cube * (bias / cube_dist) : head;
            app.cam.target = lerp(head, lock_target, app.lock_blend);
        }

        // render
        int fb_w = 0, fb_h = 0;
        SDL_GetWindowSizeInPixels(app.window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.35f, 0.58f, 0.71f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        const float aspect = fb_h > 0 ? static_cast<float>(fb_w) / fb_h : 1.0f;
        const Mat4 proj = mat4_perspective(50.0f * 3.14159265f / 180.0f, aspect, 0.05f, 100.0f);
        const Mat4 view = mat4_look_at(app.cam.eye(), app.cam.target, {0, 1, 0});
        const Mat4 viewproj = proj * view;

        glUseProgram(grid_prog);
        glUniformMatrix4fv(g_viewproj, 1, GL_FALSE, viewproj.m);
        glBindVertexArray(grid_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glUseProgram(cube_prog);
        glUniformMatrix4fv(c_viewproj, 1, GL_FALSE, viewproj.m);
        glBindVertexArray(cube_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        glUseProgram(skin_prog);
        glUniformMatrix4fv(u_viewproj, 1, GL_FALSE, viewproj.m);
        const Mat4 model_mtx =
            app.viewer_mode ? Mat4{} : app.player.model_matrix();
        glUniformMatrix4fv(u_model, 1, GL_FALSE, model_mtx.m);
        glUniformMatrix4fv(u_palette, static_cast<GLsizei>(app.link.palette.size()),
                           GL_FALSE, app.link.palette.data()->m);
        const Vec3 sun = normalize({-0.4f, -1.0f, -0.35f});
        glUniform3fv(u_sun, 1, &sun.x);
        glUniform1i(u_tex, 0);
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_CULL_FACE);

        glBindVertexArray(app.link.vao);
        // opaque first, then alpha decals (eyes/brows/mouth)
        for (int pass = 0; pass < 2; ++pass) {
            const bool alpha_pass = pass == 1;
            if (alpha_pass) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
            }
            for (const Submesh& sub : app.link.submeshes) {
                if (sub.alpha_blend != alpha_pass) continue;
                glBindTexture(GL_TEXTURE_2D, sub.gl_texture);
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.index_count),
                               GL_UNSIGNED_INT,
                               reinterpret_cast<void*>(sub.first_index * sizeof(uint32_t)));
            }
            if (alpha_pass) {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }
        }
        glBindVertexArray(0);

        if (want_shot || shot_path) {
            save_screenshot(shot_path ? shot_path : "screenshot.bmp", fb_w, fb_h);
            want_shot = false;
            if (shot_path) app.running = false;
        }

        SDL_GL_SwapWindow(app.window);

        ++fps_frames;
        if (now_ns - fps_last_ns >= 1'000'000'000ull) {
            set_title(app, fps_frames);
            fps_frames = 0;
            fps_last_ns = now_ns;
        }
    }

    SDL_GL_DestroyContext(app.gl);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
