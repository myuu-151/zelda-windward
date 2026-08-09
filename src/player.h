// Character controller + animation state machine for Link.
//   move: WASD / left stick (camera relative)   roll: space / south button
//   attack: J or left click / west button       guard: hold shift / right shoulder
// Attacks chain: pressing attack during Slash cancels into Slash2 at the
// designed frame window (same rule as the Blender combo design).
#pragma once

#include "math3d.h"
#include "model.h"

// crossfading clip player: loops wrap one frame early (baked loops duplicate
// the wrap frame), one-shots clamp and report finished
struct AnimPlayer {
    const Model* model = nullptr;
    const AnimClip* clip = nullptr;
    const AnimClip* prev_clip = nullptr;
    float time = 0, prev_time = 0;
    float fade = 0, fade_len = 0;
    float rate = 1.0f;  // playback speed multiplier
    bool loop = true;

    // upper-body overlay (e.g. guard stance over running legs)
    const AnimClip* overlay = nullptr;
    float overlay_time = 0;
    float overlay_blend = 0;  // eases 0..1
    const std::vector<uint8_t>* overlay_mask = nullptr;

    void play(const AnimClip* c, bool looped, float fade_seconds = 0.12f);
    void set_overlay(const AnimClip* c, const std::vector<uint8_t>* mask);
    void update(float dt);
    void apply(Model& m);  // writes m.palette
    bool finished() const;
    float clip_end() const;
};

enum class PlayerState { Locomotion, Roll, Attack1, Attack2 };

struct Player {
    Vec3 pos{};
    float yaw = 0;            // faces +Z at 0
    float speed = 0;          // current planar speed
    PlayerState state = PlayerState::Locomotion;
    AnimPlayer anim;
    Vec3 roll_dir{};
    bool attack_buffered = false;

    struct Clips {
        const AnimClip* idle = nullptr;
        const AnimClip* run = nullptr;
        const AnimClip* roll = nullptr;      // recovery ends in idle pose
        const AnimClip* roll_run = nullptr;  // recovery ends in run pose
        const AnimClip* slash = nullptr;
        const AnimClip* slash2 = nullptr;
        const AnimClip* guard = nullptr;
    } clips;

    struct Input {
        float move_x = 0, move_z = 0;  // camera-relative, magnitude <= 1
        bool attack_pressed = false;   // edge
        bool roll_pressed = false;     // edge
        bool guard_held = false;
    };

    std::vector<uint8_t> upper_mask;  // Root-bone subtree (torso/arms/head)

    void init(const Model& m);
    void update(const Input& in, float dt);
    Mat4 model_matrix() const;
};
