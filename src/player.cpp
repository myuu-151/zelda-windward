#include "player.h"

#include <SDL3/SDL.h>

#include <cmath>

namespace {
constexpr float kRunSpeed = 5.4f;       // ground speed at full stick
constexpr float kAccel = 18.0f;
constexpr float kTurnRate = 12.0f;      // rad/s toward move direction
constexpr float kRollSpeed = 12.0f;  // bursts well above run speed
constexpr float kFrame = 1.0f / 30.0f;  // clips are authored at 30 fps
// phase 0 == Blender frame 1, so frame N is phase (N-1)/30
constexpr float kSlashChainTime = 10.0f * (1.0f / 30.0f);  // cancel frame (f11)

float angle_wrap(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
}  // namespace

void AnimPlayer::play(const AnimClip* c, bool looped, float fade_seconds)
{
    if (!c) return;
    if (c == clip && looped) return;  // already looping it -- don't restart
    prev_clip = clip;
    prev_time = time;
    clip = c;
    time = 0;
    loop = looped;
    fade = fade_len = (prev_clip ? fade_seconds : 0.0f);
}

float AnimPlayer::clip_end() const
{
    // `time` is a phase: 0 == the clip's first key. Keyed span = duration-start;
    // loops append one extra frame interval that interpolates back to the start.
    if (!clip) return 0.0f;
    const float span = clip->duration - clip->start;
    return loop ? span + kFrame : span;
}

void AnimPlayer::set_overlay(const AnimClip* c, const std::vector<uint8_t>* mask)
{
    if (overlay != c) overlay_time = 0;
    overlay = c;
    overlay_mask = mask;
}

void AnimPlayer::update(float dt)
{
    if (!clip) return;
    time += dt * rate;
    const float end = clip_end();
    if (loop) {
        if (end > 0 && time >= end) time = std::fmod(time, end);
    } else if (time > end) {
        time = end;
    }
    if (fade > 0) fade = std::max(0.0f, fade - dt);

    // overlay eases in/out and loops on its own clock
    const float blend_step = dt / 0.12f;
    overlay_blend = overlay ? std::min(1.0f, overlay_blend + blend_step)
                            : std::max(0.0f, overlay_blend - blend_step);
    if (overlay) {
        overlay_time += dt;
        const float span = overlay->duration - overlay->start + kFrame;
        if (span > 0 && overlay_time >= span) overlay_time = std::fmod(overlay_time, span);
    }
}

bool AnimPlayer::finished() const
{
    return clip && !loop && time >= clip_end();
}

namespace {
// phase -> pose, with a seamless loop wrap: past the last key, interpolate
// back to the first pose across one frame interval (what Blender's looping
// playback does implicitly between the last and first frame)
void sample_phase(const Model& m, const AnimClip& clip, float phase, bool looping, Pose& out)
{
    const float span = clip.duration - clip.start;
    if (looping && phase > span) {
        static Pose tail, head;
        m.sample(clip, clip.duration, tail);
        m.sample(clip, clip.start, head);
        const float w = std::min(1.0f, (phase - span) / kFrame);
        Model::blend(tail, head, w, out);
    } else {
        m.sample(clip, clip.start + phase, out);
    }
}
}  // namespace

void AnimPlayer::apply(Model& m)
{
    if (!clip) return;
    sample_phase(m, *clip, time, loop, m.scratch_a);
    Pose* result = &m.scratch_a;
    if (fade > 0 && prev_clip) {
        sample_phase(m, *prev_clip, prev_time, false, m.scratch_b);
        const float w = fade / fade_len;  // 1 = fully previous, 0 = fully current
        Model::blend(m.scratch_a, m.scratch_b, w, m.scratch_mix);
        result = &m.scratch_mix;
    }
    if (overlay_blend > 0 && overlay && overlay_mask) {
        // masked nodes (upper body) blend toward the overlay clip's pose
        sample_phase(m, *overlay, overlay_time, true, m.scratch_ov);
        Pose& r = *result;
        for (size_t i = 0; i < r.size() && i < overlay_mask->size(); ++i) {
            if (!(*overlay_mask)[i]) continue;
            // rotations only: the base clip keeps its translations so the
            // run's vertical bob still carries the guarding upper body
            r[i].r = nlerp(r[i].r, m.scratch_ov[i].r, overlay_blend);
            r[i].s = lerp(r[i].s, m.scratch_ov[i].s, overlay_blend);
        }
    }
    m.palette_from(*result);
}

void Player::init(const Model& m)
{
    clips.idle = m.find_clip("Idle");
    clips.run = m.find_clip("Run");
    clips.roll = m.find_clip("Roll");
    clips.roll_run = m.find_clip("RollRun");
    clips.slash = m.find_clip("Slash");
    clips.slash2 = m.find_clip("Slash2");
    clips.guard = m.find_clip("Guard");
    clips.hop_l = m.find_clip("SideHopL");
    clips.hop_r = m.find_clip("SideHopR");
    clips.hop_b = m.find_clip("BackHop");
    clips.run_back = m.find_clip("RunBack");
    clips.strafe_l = m.find_clip("StrafeL");
    clips.strafe_r = m.find_clip("StrafeR");
    upper_mask = m.subtree_mask("Root");
    anim.play(clips.idle, true);
}

void Player::update(const Input& in, float dt)
{
    const float move_len = std::sqrt(in.move_x * in.move_x + in.move_z * in.move_z);
    const bool moving = move_len > 0.15f;
    locked = in.guard_held && in.has_target;

    switch (state) {
        case PlayerState::Locomotion: {
            if (locked) {
                // --- Z-target mode: always face the target, hop to strafe ---
                Vec3 to_t = in.target_pos - pos;
                to_t.y = 0;
                to_t = normalize(to_t);
                const float target_yaw = std::atan2(to_t.x, to_t.z);
                yaw += angle_wrap(target_yaw - yaw) * std::min(1.0f, 14.0f * dt);
                anim.set_overlay(clips.guard, &upper_mask);

                // strafe: move along the stick in any direction while
                // continuing to face the target
                const float target_speed =
                    moving ? kRunSpeed * 0.7f * std::min(move_len, 1.0f) : 0.0f;
                speed += (target_speed - speed) * std::min(1.0f, kAccel * dt);
                if (moving && speed > 0.01f) {
                    pos.x += (in.move_x / move_len) * speed * dt;
                    pos.z += (in.move_z / move_len) * speed * dt;
                }
                // directional cycle: forward run, backpedal, or side-steps,
                // picked by the dominant axis relative to the facing
                const AnimClip* cycle = clips.idle;
                if (speed > 0.3f && moving) {
                    const Vec3 left{to_t.z, 0, -to_t.x};
                    const float f = (in.move_x * to_t.x + in.move_z * to_t.z) / move_len;
                    const float l = (in.move_x * left.x + in.move_z * left.z) / move_len;
                    if (std::fabs(f) >= std::fabs(l))
                        cycle = f >= 0 ? clips.run
                                       : (clips.run_back ? clips.run_back : clips.run);
                    else
                        cycle = l >= 0 ? (clips.strafe_l ? clips.strafe_l : clips.run)
                                       : (clips.strafe_r ? clips.strafe_r : clips.run);
                }
                anim.play(cycle, true, 0.10f);
                anim.rate = (cycle != clips.idle)
                                ? std::max(0.6f, speed / kRunSpeed)
                                : 1.0f;
                anim.update(dt);
                return;
            }
            if (in.roll_pressed && moving) {  // rolling requires movement input
                state = PlayerState::Roll;
                roll_dir = moving ? Vec3{in.move_x / move_len, 0, in.move_z / move_len}
                                  : Vec3{std::sin(yaw), 0, std::cos(yaw)};
                yaw = std::atan2(roll_dir.x, roll_dir.z);
                anim.rate = 40.0f / 30.0f;  // roll plays at 40fps-equivalent
                anim.play(clips.roll, false, 0.06f);
                break;
            }
            if (in.attack_pressed) {
                state = PlayerState::Attack1;
                attack_buffered = false;
                speed = 0;
                anim.rate = 1.0f;
                anim.play(clips.slash, false, 0.06f);
                break;
            }
            // guard is an upper-body overlay: shield up, legs keep running
            anim.set_overlay(in.guard_held ? clips.guard : nullptr, &upper_mask);
            const float max_speed = in.guard_held ? kRunSpeed * 0.7f : kRunSpeed;
            const float target_speed = moving ? max_speed * std::min(move_len, 1.0f) : 0.0f;
            speed += (target_speed - speed) * std::min(1.0f, kAccel * dt);
            if (moving) {
                const float target_yaw = std::atan2(in.move_x, in.move_z);
                yaw += angle_wrap(target_yaw - yaw) * std::min(1.0f, kTurnRate * dt);
            }
            pos.x += std::sin(yaw) * speed * dt;
            pos.z += std::cos(yaw) * speed * dt;
            anim.play(speed > 0.3f ? clips.run : clips.idle, true);
            // scale the run cycle to the actual ground speed (no foot skating)
            anim.rate = (anim.clip == clips.run)
                            ? std::max(0.6f, speed / kRunSpeed)
                            : 1.0f;
            break;
        }
        case PlayerState::Roll: {
            // burst that eases toward run speed over the clip -- the whole
            // roll stays slightly faster than running
            const float t = anim.clip ? anim.time / anim.clip->duration : 1.0f;
            const float s = kRollSpeed * (1.0f - 0.1f * t);
            pos.x += roll_dir.x * s * dt;
            pos.z += roll_dir.z * s * dt;
            // Roll and RollRun share frames 1-19; before the recovery branches,
            // hot-swap to whichever ending matches the stick (no pop)
            constexpr float kBranch = 18.0f / 30.0f;  // phase of frame 19
            if (anim.time < kBranch && clips.roll_run) {
                anim.clip = moving ? clips.roll_run : clips.roll;
            }
            if (anim.finished()) {
                state = PlayerState::Locomotion;
                anim.rate = 1.0f;
                if (anim.clip == clips.roll_run) {
                    // recovery ended in the run contact pose: continue the run
                    speed = kRunSpeed;
                    anim.play(clips.run, true, 0.05f);
                } else {
                    speed = 0;
                    anim.play(clips.idle, true, 0.10f);
                }
            }
            break;
        }
        case PlayerState::Attack1: {
            if (in.attack_pressed) attack_buffered = true;
            if (attack_buffered && anim.time >= kSlashChainTime) {
                state = PlayerState::Attack2;
                attack_buffered = false;
                anim.play(clips.slash2, false, 0.05f);
            } else if (anim.finished()) {
                state = PlayerState::Locomotion;
            }
            break;
        }
        case PlayerState::Attack2: {
            if (anim.finished()) state = PlayerState::Locomotion;
            break;
        }
        case PlayerState::Hop: {
            constexpr float kHopSpeed = 3.4f;
            pos.x += hop_dir.x * kHopSpeed * dt;
            pos.z += hop_dir.z * kHopSpeed * dt;
            if (locked) {  // keep facing the target through the hop
                Vec3 to_t = in.target_pos - pos;
                to_t.y = 0;
                to_t = normalize(to_t);
                yaw += angle_wrap(std::atan2(to_t.x, to_t.z) - yaw) *
                       std::min(1.0f, 14.0f * dt);
            }
            if (anim.finished()) {
                state = PlayerState::Locomotion;
                anim.play(clips.idle, true, 0.08f);
            }
            break;
        }
    }
    if (state == PlayerState::Roll || state == PlayerState::Attack1 ||
        state == PlayerState::Attack2)
        anim.set_overlay(nullptr, &upper_mask);  // shield drops during those
    anim.update(dt);
}

Mat4 Player::model_matrix() const
{
    const Quat q{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    return mat4_from_trs(pos, q, {1, 1, 1});
}
