// SPDX-License-Identifier: MIT
// ===========================================================================
//  NEON RUN - a 3D endless runner
//  Built entirely on creaddinscart.studio.cpp.sl / v3D-1.0.0 (header-only,
//  C++17, software rasterizer, no GPU, no window system).
//
//  The library deliberately ships without a window backend, so this file
//  supplies one: POSIX termios raw input plus an ANSI truecolor half-block
//  presenter. Everything else - the z-buffered triangle rasterizer, fog,
//  point lights, the physics world, the particle system, the fixed-step
//  game loop, the input state machine and the bitmap-font HUD - is v3D.
//
//  Modes
//    neonrun                     play in a terminal (truecolor required)
//    neonrun --replay N <dir>    headless autopilot, N frames into <dir>
//    neonrun --shot <file>       one rendered frame (.png or .ppm)
//    neonrun --ascii             one ASCII frame, for terminals without color
//    neonrun --info              the library's system report
//    neonrun --help
// ===========================================================================
#include <creaddinscart.studio.cpp.sl/v3D-1.0.0>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#  include <signal.h>
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#endif

using namespace v3d;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
namespace cfg {
// The camera sits behind the ship looking toward +Z, which makes the view
// matrix left-handed about the screen: world +X lands on the LEFT half
// (camera.right() == (-1,0,0)). Listing the lanes in decreasing x means
// "lane index grows toward screen right", so A/Left, D/Right and the mouse
// pointer all agree with what the picture shows.
constexpr float kLaneX[3]   = { 2.75f, 0.0f, -2.75f };
constexpr float kStandH     =  1.18f;
constexpr float kDuckH      =  0.52f;
constexpr float kPlayerR    =  0.58f;
constexpr float kSpeedStart =  9.0f;
constexpr float kSpeedMax   = 27.0f;
constexpr float kSpeedRamp  =  0.40f;   // speed gained per second of running
constexpr float kGravity    = -26.0f;
constexpr float kJumpVy     =  8.6f;
constexpr float kDuckTime   =  0.58f;
constexpr float kHitInvuln  =  1.30f;
constexpr float kTileZ      =  4.0f;
constexpr float kPylonZ     =  8.0f;
constexpr float kArchZ      = 24.0f;
constexpr float kViewAhead  = 86.0f;
constexpr float kLookAhead  = 15.0f;
constexpr int   kShields    =  3;
constexpr std::uint32_t kSeed = 0x51f0a9u;
} // namespace cfg

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
namespace pal {
const Color kVoid     = from_rgb8(  6,   8,  20);
const Color kFog      = from_rgb8( 10,  14,  34);
const Color kTileA    = from_rgb8( 74, 116, 214);   // track tint, bright row
const Color kTileB    = from_rgb8( 40,  58, 132);   // track tint, dim row
const Color kPanel    = from_rgb8( 18,  22,  44);   // HUD backing plate
const Color kRailA    = from_rgb8(255,  45, 149);   // magenta
const Color kRailB    = from_rgb8( 45, 226, 255);   // cyan
const Color kPylon    = from_rgb8( 40,  52, 104);
const Color kPylonCap = from_rgb8(130, 245, 255);
const Color kArch     = from_rgb8( 74,  42, 136);
const Color kBlock    = from_rgb8(255,  60, 140);
const Color kHurdle   = from_rgb8(255, 180,  40);
const Color kBeam     = from_rgb8(255,  70,  70);
const Color kOrb      = from_rgb8( 90, 255, 200);
const Color kShip     = from_rgb8(225, 250, 255);
const Color kShell    = from_rgb8(255, 150, 255);
const Color kHud      = from_rgb8(238, 246, 255);
const Color kHudDim   = from_rgb8(120, 140, 190);
const Color kDanger   = from_rgb8(255,  80,  90);
} // namespace pal

// ---------------------------------------------------------------------------
// Deterministic xorshift32, so a replay really is a replay.
// ---------------------------------------------------------------------------
struct Rng {
    std::uint32_t s = cfg::kSeed;
    std::uint32_t next() noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int below(int n) noexcept { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
    float unit() noexcept { return static_cast<float>(next() >> 8) / 16777216.0f; }
};

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------
enum class Kind : int { Block, Hurdle, Beam, Orb };

struct Entity {
    Kind  kind  = Kind::Block;
    int   lane  = 1;
    float z     = 0.0f;
    float phase = 0.0f;
    bool  alive = true;
};

struct Box { Vec3 center; Vec3 size; };

// Also the axis-aligned collision volume.
static Box entity_bounds(const Entity& e) {
    const float x = cfg::kLaneX[e.lane];
    switch (e.kind) {
        case Kind::Block:  return { {x, 0.90f, e.z}, {1.85f, 1.80f, 1.85f} };
        case Kind::Hurdle: return { {x, 0.30f, e.z}, {2.30f, 0.60f, 0.60f} };
        case Kind::Beam:   return { {x, 1.45f, e.z}, {2.30f, 1.20f, 0.60f} };
        case Kind::Orb:    return { {x, 1.05f, e.z}, {0.80f, 0.80f, 0.80f} };
    }
    return {};
}

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------
class Game {
public:
    Rng rng;

    float speed     = cfg::kSpeedStart;
    float player_z  = 0.0f;
    float player_x  = 0.0f;
    int   lane      = 1;
    float duck_t    = 0.0f;
    float invuln_t  = 0.0f;
    int   shields   = cfg::kShields;
    long  score     = 0;
    long  best      = 0;
    long  orbs      = 0;
    long  hits      = 0;
    long  hits_kind[4] = {0, 0, 0, 0};      // obstacles eaten (diagnostics)
    float score_frac = 0.0f;  // distance score accumulates in float space
    bool  started   = false;
    bool  over      = false;
    bool  quit      = false;
    float elapsed   = 0.0f;
    float shake     = 0.0f;
    Vec2  shake_off = {0, 0};
    float fps       = 0.0f;

    std::vector<Entity> ents;
    float next_spawn_z = 26.0f;

    PhysicsWorld world;      // one rigid body: the ship, vertical only
    std::size_t ship = 0;

    ParticleSystem trail;    // engine wash under the ship
    ParticleSystem sparkle;  // orb pickup
    ParticleSystem blast;    // impact

    InputState input;
    std::vector<Key> edges;  // discrete key events queued by the backend
    // Pointer state, filled in by the terminal backend between frames.
    Vec2  mouse_norm = {0.5f, 0.5f};   // 0..1 across the window
    float mouse_wheel = 0.0f;          // +up / -down, accumulated
    float mouse_hold  = 0.0f;          // seconds the pointer still owns steering

    Mesh cube   = make_cube(1.0f);
    Mesh plane  = make_plane(8.6f, cfg::kTileZ, 1);
    Mesh orb_m  = make_sphere(0.40f, 12, 8);
    Mesh ring_m = make_torus(0.95f, 0.10f, 16, 6);

    // Textures carry brightness only; the per-draw tint carries the hue.
    Texture tex_tile  = Texture::checker(32, 4, kWhite, from_rgb8(58, 62, 86));
    Texture tex_white = Texture::solid(kWhite, 2);
    Texture tex_orb   = Texture::gradient(16, from_rgb8(130, 130, 150), kWhite);

    explicit Game(std::uint32_t seed = cfg::kSeed) {
        rng.s = seed ? seed : 1u;
        reset();
    }

    void reset() {
        speed = cfg::kSpeedStart;
        player_z = 0.0f; player_x = 0.0f; lane = 1;
        duck_t = 0.0f; invuln_t = 0.0f; shields = cfg::kShields;
        score = 0; orbs = 0; elapsed = 0.0f; score_frac = 0.0f;
        hits = 0; hits_kind[0] = hits_kind[1] = hits_kind[2] = hits_kind[3] = 0;
        over = false; started = false; shake = 0.0f; shake_off = {0, 0};
        ents.clear();
        next_spawn_z = 26.0f;
        edges.clear();

        world = PhysicsWorld{};
        world.gravity = {0, cfg::kGravity, 0};
        world.ground_y = 0.0f;
        world.use_ground = true;
        RigidBody body;
        body.radius = cfg::kPlayerR;
        body.restitution = 0.0f;
        body.drag = 0.0f;
        body.position = {0.0f, cfg::kPlayerR, 0.0f};
        ship = world.bodies.size();
        world.add(body);

        trail = ParticleSystem{};
        trail.seed = 0x1234abcdu; trail.emitting = false;
        trail.config = EmitterConfig{};
        trail.config.life_min = 0.25f; trail.config.life_max = 0.55f;
        trail.config.speed_min = 0.4f; trail.config.speed_max = 1.6f;
        trail.config.spread = 0.9f;    trail.config.size = 1.4f;
        trail.config.gravity = 1.5f;   trail.config.drag = 1.2f;
        trail.config.color_start = from_rgb8(90, 150, 255);
        trail.config.color_end   = from_rgb8(18, 20, 60);
        trail.config.max_particles = 900;

        sparkle = ParticleSystem{};
        sparkle.seed = 0x77aa1155u; sparkle.emitting = false;
        sparkle.config = EmitterConfig{};
        sparkle.config.life_min = 0.30f; sparkle.config.life_max = 0.70f;
        sparkle.config.speed_min = 2.0f; sparkle.config.speed_max = 6.5f;
        sparkle.config.spread = 1.0f;    sparkle.config.size = 2.0f;
        sparkle.config.gravity = -6.0f;  sparkle.config.drag = 1.0f;
        sparkle.config.color_start = kWhite;
        sparkle.config.color_end   = pal::kOrb;
        sparkle.config.max_particles = 1200;

        blast = ParticleSystem{};
        blast.seed = 0xdeadbeefu; blast.emitting = false;
        blast.config = EmitterConfig{};
        blast.config.life_min = 0.45f; blast.config.life_max = 1.10f;
        blast.config.speed_min = 4.0f; blast.config.speed_max = 15.0f;
        blast.config.spread = 1.2f;    blast.config.size = 3.0f;
        blast.config.gravity = -9.0f;  blast.config.drag = 0.7f;
        blast.config.color_start = kWhite;
        blast.config.color_end   = pal::kDanger;
        blast.config.max_particles = 1500;

        spawn_ahead();
    }

    float difficulty() const noexcept {
        return std::min(1.0f, (speed - cfg::kSpeedStart) /
                              (cfg::kSpeedMax - cfg::kSpeedStart));
    }
    bool grounded() const {
        const RigidBody& b = world.bodies[ship];
        return b.position.y <= world.ground_y + b.radius + 0.02f &&
               std::fabs(b.velocity.y) < 0.6f;
    }
    float ship_y() const { return world.bodies[ship].position.y - cfg::kPlayerR; }
    float ship_height() const { return duck_t > 0.0f ? cfg::kDuckH : cfg::kStandH; }

    // ---- world generation --------------------------------------------------
    void spawn_ahead() {
        while (next_spawn_z < player_z + cfg::kViewAhead) {
            const float d = difficulty();
            switch (roll_pattern(d)) {
                case 0: {                                   // pillars, never all 3
                    const int n = (d > 0.30f && rng.below(3) != 0) ? 2 : 1;
                    int lanes[3] = {0, 1, 2};
                    for (int i = 2; i > 0; --i) std::swap(lanes[i], lanes[rng.below(i + 1)]);
                    for (int i = 0; i < n; ++i)
                        ents.push_back({Kind::Block, lanes[i], next_spawn_z, rng.unit() * 6.28f, true});
                    break;
                }
                case 1:                                     // jump wall
                    for (int l = 0; l < 3; ++l) ents.push_back({Kind::Hurdle, l, next_spawn_z, 0.0f, true});
                    break;
                case 2:                                     // duck beam
                    for (int l = 0; l < 3; ++l) ents.push_back({Kind::Beam, l, next_spawn_z, 0.0f, true});
                    break;
                case 3: {                                   // orb trail
                    const int l = rng.below(3);
                    for (int i = 0; i < 4; ++i)
                        ents.push_back({Kind::Orb, l, next_spawn_z + i * 2.4f, i * 0.7f, true});
                    break;
                }
                default: {                                  // mixed
                    const int a = rng.below(3);
                    ents.push_back({Kind::Block, a, next_spawn_z, 0.0f, true});
                    int b = rng.below(2); if (b >= a) ++b;
                    ents.push_back({Kind::Hurdle, b, next_spawn_z + 1.0f, 0.0f, true});
                    break;
                }
            }
            next_spawn_z += 17.0f - 8.0f * d + rng.unit() * 3.0f;
        }
        if (!ents.empty() && ents.front().z < player_z - 30.0f) {
            ents.erase(ents.begin(),
                       std::find_if(ents.begin(), ents.end(),
                                    [&](const Entity& e) { return e.z >= player_z - 30.0f; }));
        }
    }

    int roll_pattern(float d) {
        const int r = rng.below(100);
        if (r < 32) return 0;
        if (r < 54) return 1;
        if (r < 72) return 2;
        if (r < 90) return 3;
        (void)d;
        return 4;
    }

    // ---- one fixed simulation step ----------------------------------------
    void update(float dt) {
        // Fold the backend's discrete events into the library's InputState.
        input.reset();
        for (Key k : edges) input.press(k);
        edges.clear();
        if (input.was_pressed(Key::Escape)) { quit = true; return; }

        if (!started) {
            if (input.was_pressed(Key::Space) || input.was_pressed(Key::Enter) ||
                input.was_pressed(Key::Digit1)) started = true;
            return;
        }
        if (over) {
            if (input.was_pressed(Key::Digit1)) { reset(); started = true; }
            return;
        }

        elapsed += dt;
        speed = std::min(cfg::kSpeedMax, speed + cfg::kSpeedRamp * dt);
        player_z += speed * dt;
        score_frac += speed * dt * 2.0f;
        while (score_frac >= 1.0f) { score_frac -= 1.0f; ++score; }

        if (input.was_pressed(Key::Left))  { lane = std::max(0, lane - 1); mouse_hold = 0.0f; }
        if (input.was_pressed(Key::Right)) { lane = std::min(2, lane + 1); mouse_hold = 0.0f; }

        RigidBody& b = world.bodies[ship];
        b.velocity.x = 0.0f;
        b.velocity.z = 0.0f;

        if (input.was_pressed(Key::Up) || input.was_pressed(Key::Space))
            if (grounded()) b.velocity.y = cfg::kJumpVy;
        if (input.was_pressed(Key::Down)) {
            duck_t = cfg::kDuckTime;
            if (!grounded()) b.velocity.y = std::min(b.velocity.y, -13.0f);   // fast fall
        }

        // Mouse steering: the pointer owns the lane while it is being moved,
        // the wheel and the buttons stand in for jump and duck.
        if (mouse_hold > 0.0f) {
            mouse_hold = std::max(0.0f, mouse_hold - dt);
            input.set_mouse(mouse_norm.x, mouse_norm.y);
            const int ml = std::max(0, std::min(2, static_cast<int>(mouse_norm.x * 3.0f)));
            if (ml != lane) lane = ml;
        }
        if (mouse_wheel != 0.0f) { input.add_wheel(mouse_wheel); mouse_wheel = 0.0f; }
        if (input.wheel > 0.0f) { if (grounded()) b.velocity.y = cfg::kJumpVy; }
        else if (input.wheel < 0.0f) { duck_t = cfg::kDuckTime; }
        if (input.was_pressed(Key::MouseLeft)) { if (grounded()) b.velocity.y = cfg::kJumpVy; }
        if (input.was_pressed(Key::MouseRight)) duck_t = cfg::kDuckTime;
        if (duck_t > 0.0f) duck_t -= dt;
        if (invuln_t > 0.0f) invuln_t -= dt;

        world.step(dt);

        // eased lateral slide toward the target lane, then glue the collider
        player_x += (cfg::kLaneX[lane] - player_x) * std::min(1.0f, dt * 14.0f);
        b.position.x = player_x;
        b.position.z = player_z;

        if (shake > 0.0f) {
            shake = std::max(0.0f, shake - dt * 1.9f);
            const float k = ease_out_elastic(1.0f - shake) * shake;
            shake_off = { std::sin(elapsed * 47.0f) * k * 0.55f,
                          std::sin(elapsed * 31.0f + 1.7f) * k * 0.35f };
        } else {
            shake_off = {0, 0};
        }

        trail.config.position  = {player_x, ship_y() - 0.20f, player_z - 0.7f};
        trail.config.direction = {0, 0.6f, -1.0f};
        trail.config.emit_rate = 30.0f + speed * 5.0f;
        trail.emitting = true;
        sparkle.config.position = {player_x, ship_y() + 0.6f, player_z};
        blast.config.position   = {player_x, ship_y() + 0.6f, player_z};

        trail.update(dt);
        sparkle.update(dt);
        blast.update(dt);

        collide();
        spawn_ahead();
        input.end_frame();
    }

    void collide() {
        const float py0 = ship_y();
        const float py1 = py0 + ship_height();
        const float px0 = player_x - 0.52f, px1 = player_x + 0.52f;
        const float pz0 = player_z - 0.52f, pz1 = player_z + 0.52f;

        for (auto& e : ents) {
            if (!e.alive) continue;
            if (e.z < player_z - 6.0f || e.z > player_z + 12.0f) continue;
            const Box bx = entity_bounds(e);
            if (px1 <= bx.center.x - bx.size.x * 0.5f || px0 >= bx.center.x + bx.size.x * 0.5f) continue;
            if (pz1 <= bx.center.z - bx.size.z * 0.5f || pz0 >= bx.center.z + bx.size.z * 0.5f) continue;
            if (py1 <= bx.center.y - bx.size.y * 0.5f || py0 >= bx.center.y + bx.size.y * 0.5f) continue;

            if (e.kind == Kind::Orb) {
                e.alive = false;
                score += 75;
                ++orbs;
                sparkle.config.position = bx.center;
                sparkle.burst(46);
                continue;
            }
            if (invuln_t > 0.0f) continue;

            e.alive = false;
            ++hits;
            ++hits_kind[static_cast<int>(e.kind)];
            --shields;
            invuln_t = cfg::kHitInvuln;
            shake = 1.0f;
            speed = std::max(cfg::kSpeedStart, speed * 0.72f);
            blast.config.position = bx.center;
            blast.burst(180);
            if (shields <= 0) { shields = 0; over = true; best = std::max(best, score); }
            return;
        }
    }

    // ---- autopilot: drives the same InputState a human would --------------
    // Distance from the ship to the nearest live entity of `kind` in lane `l`,
    // searched over [from, to] measured in metres ahead of the ship.
    float nearest_in(int l, Kind kind, float from, float to) const {
        float best = 1e9f;
        for (const auto& e : ents) {
            if (!e.alive || e.lane != l || e.kind != kind) continue;
            const float d = e.z - player_z;
            if (d > from && d < to && d < best) best = d;
        }
        return best;
    }

    // A lane is closed only by a pillar: hurdles are jumped and beams are ducked,
    // so treating those as blockers leaves the autopilot with nowhere to go.
    bool lane_clear(int l, float from, float to) const {
        return nearest_in(l, Kind::Block, from, to) > 1e8f;
    }

    // Comfortable = no pillar for a long stretch and nothing vertical imminent,
    // so a lane change never drops the ship straight onto a bar.
    bool lane_comfortable(int l, float horizon, float vertical_lead) const {
        return lane_clear(l, player_z - 2.0f, player_z + horizon) &&
               nearest_in(l, Kind::Hurdle, player_z - 1.0f, vertical_lead) > 1e8f &&
               nearest_in(l, Kind::Beam,   player_z - 1.0f, vertical_lead) > 1e8f;
    }

    void press_lane(int target) {
        if (target < lane) edges.push_back(Key::Left);
        else if (target > lane) edges.push_back(Key::Right);
    }

    // Progressive relaxation: a wide clean lane if one exists, otherwise the
    // lane whose next pillar is furthest away.
    void steer_away(float horizon) {
        const float vlead = player_z + std::max(8.0f, speed * 0.75f);
        for (int step = 1; step <= 2; ++step) {
            for (int s : {-step, step}) {
                const int l = lane + s;
                if (l < 0 || l > 2) continue;
                if (lane_comfortable(l, horizon, vlead)) { press_lane(l); return; }
            }
        }
        for (int step = 1; step <= 2; ++step) {
            for (int s : {-step, step}) {
                const int l = lane + s;
                if (l < 0 || l > 2) continue;
                if (lane_clear(l, player_z - 2.0f, player_z + std::max(10.0f, horizon * 0.45f))) {
                    press_lane(l); return;
                }
            }
        }
        int best_l = lane;
        float best_gap = nearest_in(lane, Kind::Block, player_z, 1e9f);
        for (int l = 0; l < 3; ++l) {
            const float d = nearest_in(l, Kind::Block, player_z, 1e9f);
            if (d > best_gap) { best_gap = d; best_l = l; }
        }
        press_lane(best_l);
    }

    // ---- autopilot: drives the same InputState a human would --------------
    void autopilot() {
        if (over) { edges.push_back(Key::Digit1); return; }
        if (!started) { edges.push_back(Key::Space); return; }

        const float horizon = 20.0f + speed * 1.30f;
        const float d_block  = nearest_in(lane, Kind::Block,  0.9f, horizon);
        const float d_hurdle = nearest_in(lane, Kind::Hurdle, 0.9f, horizon);
        const float d_beam   = nearest_in(lane, Kind::Beam,   0.9f, horizon);

        // A pillar needs the longest lead time, so it wins whenever it is closer
        // than the vertical obstacle that follows it.
        const bool dodge = d_block < 1e8f &&
                           (d_block <= d_hurdle || d_block < 6.0f) &&
                           (d_block <= d_beam   || d_block < 6.0f);
        if (dodge) { steer_away(horizon); return; }

        if (d_hurdle < 1e8f && d_hurdle <= d_beam) {
            const float takeoff = speed * 0.30f + 0.8f;
            // never lift off if a pillar waits inside the landing zone
            const bool trap = nearest_in(lane, Kind::Block,
                                         player_z + d_hurdle,
                                         player_z + d_hurdle + takeoff * 2.2f) < 1e8f;
            if (trap) steer_away(horizon);
            else if (d_hurdle <= takeoff && grounded()) edges.push_back(Key::Space);
            return;
        }
        if (d_beam < 1e8f) {
            // re-arm every frame while approaching: the duck simply never lapses
            if (d_beam < speed * 0.42f + 2.2f) edges.push_back(Key::Down);
            return;
        }

        // Nothing to do - graze an orb if the way stays clear.
        int want = -1; float want_gap = 1e9f;
        for (const auto& e : ents) {
            if (!e.alive || e.kind != Kind::Orb) continue;
            const float d = e.z - player_z;
            if (d > 4.0f && d < 30.0f && d < want_gap) { want_gap = d; want = e.lane; }
        }
        if (want >= 0 && want != lane && lane_comfortable(want, horizon, player_z + 10.0f))
            press_lane(want);
    }

    // ---- drawing ----------------------------------------------------------
    void box(TexturedRenderer& r, Vec3 center, Vec3 size, Color tint,
             const Texture& tex) const {
        r.draw_textured_mesh(cube, translation(center) * scaling(size), tex, tint);
    }

    void render(TexturedRenderer& r) const {
        r.framebuffer.clear(pal::kVoid);          // not r.clear(): keep the void colour
        r.clear_point_lights();

        const float pz = player_z;
        const float py = ship_y();

        r.add_point_light({{player_x, py + 2.0f, pz + 1.2f}, pal::kRailB, 1.45f, 22.0f});
        r.add_point_light({{0.0f, 6.0f, pz + 34.0f}, from_rgb8(160, 130, 255), 0.90f, 70.0f});

        // Ground: recycled checker tiles, so the pattern itself carries the speed.
        const int k0 = static_cast<int>(std::floor((pz - 14.0f) / cfg::kTileZ));
        const int k1 = static_cast<int>(std::ceil((pz + cfg::kViewAhead) / cfg::kTileZ));
        for (int k = k0; k <= k1; ++k) {
            const float z = (k + 0.5f) * cfg::kTileZ;
            r.draw_textured_mesh(plane, translation({0, 0, z}), tex_tile,
                                 ((k & 1) == 0) ? pal::kTileA : pal::kTileB);
        }

        // Side rails strobe magenta / cyan.
        for (int k = k0; k <= k1; ++k) {
            const float z = (k + 0.5f) * cfg::kTileZ;
            const Color c = ((k & 1) == 0) ? pal::kRailA : pal::kRailB;
            box(r, { 4.55f, 0.14f, z}, {0.30f, 0.28f, cfg::kTileZ}, c, tex_white);
            box(r, {-4.55f, 0.14f, z}, {0.30f, 0.28f, cfg::kTileZ}, c, tex_white);
        }

        const int p0 = static_cast<int>(std::floor((pz - 8.0f) / cfg::kPylonZ));
        const int p1 = static_cast<int>(std::ceil((pz + cfg::kViewAhead) / cfg::kPylonZ));
        for (int k = p0; k <= p1; ++k) {
            const float z = (k + 0.5f) * cfg::kPylonZ;
            for (float sx : {-5.35f, 5.35f}) {
                box(r, {sx, 1.70f, z}, {0.34f, 3.40f, 0.34f}, pal::kPylon, tex_white);
                box(r, {sx, 3.55f, z}, {0.62f, 0.30f, 0.62f}, pal::kPylonCap, tex_white);
            }
        }

        const int a0 = static_cast<int>(std::floor((pz - 8.0f) / cfg::kArchZ));
        const int a1 = static_cast<int>(std::ceil((pz + cfg::kViewAhead) / cfg::kArchZ));
        for (int k = a0; k <= a1; ++k) {
            const float z = (k + 0.5f) * cfg::kArchZ;
            box(r, {0, 6.10f, z}, {11.4f, 0.55f, 0.55f}, pal::kArch, tex_white);
        }

        for (const auto& e : ents) {
            if (!e.alive) continue;
            if (e.z < pz - 8.0f || e.z > pz + cfg::kViewAhead) continue;
            const Box bx = entity_bounds(e);
            switch (e.kind) {
                case Kind::Block:
                    box(r, bx.center, bx.size, pal::kBlock, tex_white);
                    box(r, {bx.center.x, bx.center.y + bx.size.y * 0.5f + 0.07f, bx.center.z},
                        {bx.size.x * 0.86f, 0.14f, bx.size.z * 0.86f}, kWhite, tex_white);
                    break;
                case Kind::Hurdle:
                    box(r, bx.center, bx.size, pal::kHurdle, tex_white);
                    break;
                case Kind::Beam:
                    box(r, bx.center, bx.size, pal::kBeam, tex_white);
                    box(r, {bx.center.x, bx.center.y - bx.size.y * 0.5f - 0.11f, bx.center.z},
                        {bx.size.x * 0.9f, 0.18f, bx.size.z * 0.9f}, kWhite, tex_white);
                    break;
                case Kind::Orb: {
                    const float bob = std::sin(elapsed * 3.0f + e.phase) * 0.16f;
                    r.draw_textured_mesh(orb_m,
                        translation({bx.center.x, bx.center.y + bob, bx.center.z}) *
                        rotation_y(elapsed * 2.2f + e.phase), tex_orb, pal::kOrb);
                    break;
                }
            }
        }

        // Ship: a duck-aware hull plus a spinning shell that leans into turns.
        {
            const bool blink = invuln_t > 0.0f && (static_cast<int>(elapsed * 14.0f) & 1);
            const Color tint = blink ? pal::kDanger : pal::kShip;
            const float h = ship_height();
            const Vec3 c = {player_x, py + h * 0.5f, pz};
            r.draw_textured_mesh(cube, translation(c) * scaling({1.02f, h, 1.02f}),
                                 tex_white, tint);
            const float lean = std::clamp((cfg::kLaneX[lane] - player_x) * 0.55f, -0.5f, 0.5f);
            r.draw_textured_mesh(ring_m,
                translation(c) * rotation_z(lean) * rotation_y(elapsed * 1.6f) *
                scaling({1.35f, 1.35f * (h / cfg::kStandH), 1.35f}),
                tex_white, blink ? pal::kDanger : pal::kShell);
        }

        trail.draw(r);
        sparkle.draw(r);
        blast.draw(r);
    }
};

// ---------------------------------------------------------------------------
// HUD - drawn into the presented framebuffer, in screen pixels
// ---------------------------------------------------------------------------
static std::string pad6(long v) {
    char b[24]; std::snprintf(b, sizeof(b), "%06ld", v % 1000000L); return b;
}
static std::string num(long v) {
    char b[24]; std::snprintf(b, sizeof(b), "%ld", v); return b;
}
static int text_w(const char* s, int scale) { return static_cast<int>(std::strlen(s)) * 6 * scale; }

// Pick the longest alternative that fits in `avail` pixels at `scale`.
// Returns an empty string when even the shortest one does not fit, so the
// caller skips the line rather than drawing a clipped one.
static std::string fit(int avail, int scale, std::initializer_list<std::string> alts) {
    for (const std::string& t : alts) {
        if (text_w(t.c_str(), scale) <= avail) return t;
    }
    return {};
}

static void draw_hud(Framebuffer& fb, const Game& g) {
    const int W = fb.width, H = fb.height;
    const bool tight = W < 130;                       // narrow terminal
    const int s = std::max(1, std::min(W / (tight ? 90 : 170), H / 44));
    const int pad = 2 * s;
    const int avail = W - 2 * pad;

    auto put = [&](const std::string& t, int x, int y, Color c, int sc) {
        if (!t.empty() && x >= 0 && x + text_w(t.c_str(), sc) <= W) hud_text(fb, t, x, y, c, sc);
    };

    const std::string dist = num(static_cast<long>(g.player_z)) + "M";

    const int bw = std::max(14, tight ? W / 4 : W / 5);
    const int bh = std::max(5, s * 4);
    const int bx = W - bw - pad;
    const int by = pad;
    hud_bar(fb, bx, by, bw, bh,
            (g.speed - cfg::kSpeedStart) / (cfg::kSpeedMax - cfg::kSpeedStart),
            pal::kRailB, pal::kPanel);

    // Left column: pick labels that fit next to the speed bar.
    const int lw = bx - pad - 6 * s;
    put(fit(lw, s, {"NEON RUN"}), pad, by, pal::kHud, s);
    put(fit(lw, s, {"SCORE " + pad6(g.score), "SCORE " + num(g.score), "S" + num(g.score)}),
        pad, by + s * 9, g.over ? pal::kDanger : pal::kHud, s);
    put(fit(lw, s, {"DIST " + dist, "D" + dist}), pad, by + s * 18, pal::kHudDim, s);
    put(fit(lw, s, {"ORB " + num(g.orbs), "O" + num(g.orbs)}), pad, by + s * 27, pal::kOrb, s);
    if (!tight) {
        put(fit(bw + pad, s, {"SPEED"}), bx, by + bh + 3, pal::kHudDim, s);
        put(fit(bw + pad, s, {num(static_cast<long>(g.fps + 0.5f)) + "FPS", "FPS"}),
            bx, by + bh + 3 + s * 8, pal::kHudDim, s);
    }

    for (int i = 0; i < cfg::kShields; ++i) {
        const bool on = i < g.shields;
        const int r = std::max(2, s * 2);
        hud_circle(fb, W - pad - r - i * (r * 2 + s), by + bh + (tight ? 2 : s * 20 + 3), r,
                   on ? pal::kRailA : pal::kPanel, on);
    }

    if (g.started && !g.over) return;

    // Title / game-over card.
    const bool title = !g.started;
    const std::string head = title ? "NEON RUN" : "RUN OVER";
    int ts = std::max(1, (avail - 2 * s) / (6 * static_cast<int>(head.size())));
    ts = std::min(ts, title ? 3 : 2);

    std::vector<std::string> body;
    std::vector<Color> cols;
    if (title) {
        struct Row { std::initializer_list<std::string> alts; Color c; };
        const Row rows[] = {
            {{"3D ENDLESS RUNNER ON V3D 1.0.0", "ENDLESS RUNNER ON V3D", "V3D RUNNER"}, pal::kHudDim},
            {{"LEFT RIGHT  SWITCH LANE",        "L/R SWITCH LANE",       "L/R LANE"},    pal::kHud},
            {{"SPACE UP   JUMP THE AMBER BAR",  "SPACE JUMPS THE BAR",   "SPACE JUMP"},  pal::kHurdle},
            {{"DOWN       DUCK THE RED BEAM",   "DOWN DUCKS THE BEAM",   "DOWN DUCK"},   pal::kBeam},
            {{"GRAB GREEN ORBS - 3 SHIELDS",    "ORBS +75, 3 SHIELDS",   "ORBS +75"},    pal::kOrb},
        };
        for (const auto& r : rows) {
            const std::string t = fit(avail - 4 * s, s, r.alts);
            if (!t.empty()) { body.push_back(t); cols.push_back(r.c); }
        }
    } else {
        const std::pair<std::string, Color> rows[] = {
            {"SCORE " + pad6(g.score), pal::kHud},
            {"DIST " + dist, pal::kHudDim},
            {"ORBS " + num(g.orbs), pal::kOrb},
        };
        for (const auto& r : rows)
            if (text_w(r.first.c_str(), s) <= avail - 4 * s) body.push_back(r.first), cols.push_back(r.second);
    }

    std::string cta;
    if (title) cta = fit(avail - 4 * s, s, {"PRESS SPACE TO RUN", "SPACE TO RUN", "SPACE"});
    else       cta = fit(avail - 4 * s, s, {"PRESS R TO RUN AGAIN", "R TO RUN AGAIN", "R AGAIN", "R"});

    const int lh = std::max(s * 8, (H - ts * 10) / static_cast<int>(body.size() + 2));
    const int card_h = std::min(H - 2, ts * 9 + lh * (static_cast<int>(body.size()) + 2) + s * 6);
    int widest = text_w(head.c_str(), ts);
    for (const auto& t : body) widest = std::max(widest, text_w(t.c_str(), s));
    widest = std::max(widest, text_w(cta.c_str(), s));
    const int card_w = std::min(W - 2, widest + 6 * s);
    const int cx0 = std::max(1, (W - card_w) / 2);
    const int cy0 = std::max(1, (H - card_h) / 2);

    hud_rect(fb, cx0, cy0, card_w, card_h, pal::kPanel, true);
    hud_rect(fb, cx0, cy0, card_w, card_h, title ? pal::kRailB : pal::kDanger);

    int y = cy0 + s * 4;
    put(head, std::max(cx0 + s, (W - text_w(head.c_str(), ts)) / 2), y,
        title ? pal::kHud : pal::kDanger, ts);
    y += ts * 9 + s * 3;
    for (std::size_t i = 0; i < body.size(); ++i) {
        put(body[i], std::max(cx0 + s, (W - text_w(body[i].c_str(), s)) / 2), y, cols[i], s);
        y += lh;
    }
    y += s;
    put(cta, std::max(cx0 + s, (W - text_w(cta.c_str(), s)) / 2), y,
        title ? pal::kRailA : pal::kRailB, s);
}

// ---------------------------------------------------------------------------
// Platform layer A: ANSI truecolor half-block presenter (POSIX terminals)
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
namespace {
struct termios g_saved_tio;
bool g_raw = false;
volatile std::sig_atomic_t g_stop_flag = 0;
volatile std::sig_atomic_t g_resized = 1;

void on_signal(int) { g_stop_flag = 1; }
void on_winch(int)  { g_resized = 1; }

void term_restore() {
    // Leave mouse tracking and the alternate picture in a clean state even if
    // we are being torn down by a signal.
    std::fputs("\x1b[?2026l\x1b[?1006l\x1b[?1003l\x1b[?1000l\x1b[0m\x1b[?25h", stdout);
    std::fflush(stdout);
    if (g_raw) { ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio); g_raw = false; }
}
void term_enter() {
    if (!::isatty(STDIN_FILENO)) return;
    if (::tcgetattr(STDIN_FILENO, &g_saved_tio) != 0) return;
    termios t = g_saved_tio;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return;
    g_raw = true;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGWINCH, on_winch);
    std::atexit(term_restore);
    // Do NOT put the tty into O_NONBLOCK: stdin and stdout are dup'd from one
    // pty file description, so the flag would also make stdout non-blocking and
    // every fwrite of a frame would be truncated to whatever fits the buffer.
    // VMIN = 0 / VTIME = 0 above already makes read() return immediately.
    // 1000 = click, 1003 = any-event (motion), 1006 = SGR coordinates.
    // While these are on a terminal needs its modifier key to select text.
    std::fputs("\x1b[2J\x1b[?25l\x1b[?1000h\x1b[?1003h\x1b[?1006h\x1b[H", stdout);
    std::fflush(stdout);
}
void term_size(int& cols, int& rows) {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col; rows = ws.ws_row;
    } else { cols = 110; rows = 34; }
    cols = std::max(48, std::min(cols, 400));
    rows = std::max(18, std::min(rows, 160));
}

void push_rgb(std::string& o, Color c) {
    char b[24];
    std::snprintf(b, sizeof(b), "%d;%d;%d", to_rgb8(c.r), to_rgb8(c.g), to_rgb8(c.b));
    o += b;
}

// One cell covers 1 pixel wide by 2 tall, so framebuffer pixels stay square
// and the picture keeps its aspect ratio on a 2:1 character grid.
void present(std::string& out, const Framebuffer& fb) {
    out.clear();
    // Mode 2026 is "synchronized output": terminals that implement it (iTerm2,
    // Kitty, WezTerm, Ghostty, Windows Terminal) commit the whole frame at once
    // instead of painting it top to bottom, which is what removes the tearing a
    // full-screen repaint otherwise shows. Others ignore the unknown mode.
    out += "\x1b[?2026h\x1b[H";
    const int rows = fb.height / 2;
    int key = -1;
    for (int r = 0; r < rows; ++r) {
        const int y = r * 2;
        for (int x = 0; x < fb.width; ++x) {
            const Color t = fb.get(x, y);
            const Color b = fb.get(x, y + 1);
            const int fg = (to_rgb8(t.r) >> 3) << 10 | (to_rgb8(t.g) >> 3) << 5 | (to_rgb8(t.b) >> 3);
            const int bg = (to_rgb8(b.r) >> 3) << 10 | (to_rgb8(b.g) >> 3) << 5 | (to_rgb8(b.b) >> 3);
            const int k = fg * 4096 + bg;
            if (k != key) {
                out += "\x1b[38;2;";
                push_rgb(out, t);
                out += ";48;2;";
                push_rgb(out, b);
                out += 'm';
                key = k;
            }
            out += "\xe2\x96\x80";
        }
        // No newline on the final row. The cursor is already on the bottom
        // line, so one more LF would scroll the screen by a row every frame -
        // that scroll is what looked like the picture shaking.
        if (r + 1 < rows) out += "\x1b[0m\r\n";
        key = -1;
    }
    out += "\x1b[0m\x1b[?2026l";
}

// Escape sequences arrive split across reads however the terminal batches
// them, so bytes are assembled in a small buffer and a sequence is only
// consumed once its final byte is present.
std::string g_in;

void push_plain(Game& g, char c) {
    switch (c) {
        case 'a': case 'A': case 'h': g.edges.push_back(Key::Left);   break;
        case 'd': case 'D': case 'l': g.edges.push_back(Key::Right);  break;
        case 'w': case 'W': case 'k': g.edges.push_back(Key::Up);     break;
        case 's': case 'S': case 'j': g.edges.push_back(Key::Down);   break;
        case ' ':  case '0':          g.edges.push_back(Key::Space);  break;
        case '\n': case '\r':         g.edges.push_back(Key::Enter);  break;
        case 'r':  case 'R':          g.edges.push_back(Key::Digit1); break;
        case 'q':  case 'Q': case 0x03: g.edges.push_back(Key::Escape); break;
        default: break;
    }
}

static std::vector<int> csi_ints(const std::string& p) {
    std::vector<int> v;
    int cur = 0; bool seen = false;
    for (char c : p) {
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); seen = true; }
        else if (c == ';') { v.push_back(seen ? cur : 0); cur = 0; seen = false; }
    }
    v.push_back(seen ? cur : 0);
    return v;
}

void dispatch_csi(Game& g, bool sgr_mouse, const std::string& p, char fin,
                  int cols, int rows) {
    if (sgr_mouse) {
        const std::vector<int> v = csi_ints(p);
        if (v.size() < 3) return;
        const int b = v[0], cx = v[1], cy = v[2];
        const bool motion = (b & 32) != 0;
        const bool wheel  = (b & 64) != 0;
        // Only motion and wheel carry a position that is worth steering to.
        // A click reports the cell it landed on too, and honouring that would
        // let "jump" also drag the ship sideways.
        if (motion || wheel) {
            g.mouse_norm = {
                std::max(0.0f, std::min(1.0f, static_cast<float>(cx - 1) /
                                              std::max(1, cols - 1))),
                std::max(0.0f, std::min(1.0f, static_cast<float>(cy - 1) /
                                              std::max(1, rows - 1)))};
            g.mouse_hold = 0.7f;
        }
        if (wheel) {                          // 64 up, 65 down
            g.mouse_wheel += ((b & 1) ? -1.0f : 1.0f);
            return;
        }
        if (motion || fin == 'm') return;     // motion and releases are not actions
        // A click is jump or duck only. It deliberately does not grab the
        // lane, or clicking to leap would also yank the ship sideways.
        switch (b & 3) {
            case 0: g.edges.push_back(Key::MouseLeft);   break;
            case 1: g.edges.push_back(Key::MouseMiddle); break;
            case 2: g.edges.push_back(Key::MouseRight);  break;
            default: break;
        }
        return;
    }
    switch (fin) {
        case 'A': g.edges.push_back(Key::Up);    break;
        case 'B': g.edges.push_back(Key::Down);  break;
        case 'C': g.edges.push_back(Key::Right); break;
        case 'D': g.edges.push_back(Key::Left);  break;
        case 'H': g.edges.push_back(Key::Enter); break;
        default: break;                         // ignore the rest
    }
}

void poll_input(Game& g, int cols, int rows) {
    unsigned char buf[1024];
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        g_in.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        if (g_in.size() > 8192) break;         // pathological backlog: stop reading
    }

    std::size_t i = 0;
    const std::size_t L = g_in.size();
    while (i < L) {
        const char c = g_in[i];
        if (c != 0x1b) { push_plain(g, c); ++i; continue; }
        if (i + 1 == L) { g.edges.push_back(Key::Escape); ++i; continue; }
        const char c1 = g_in[i + 1];
        if (c1 != '[' && c1 != 'O') { ++i; continue; }

        std::size_t j = i + 2;
        const bool sgr_mouse = (c1 == '[' && j < L && g_in[j] == '<');
        if (sgr_mouse) ++j;
        std::string params;
        char fin = 0;
        for (; j < L; ++j) {
            const char d = g_in[j];
            if ((d >= '0' && d <= '9') || d == ';' || (sgr_mouse && d == '<')) {
                params += d; continue;
            }
            if (d >= 0x40 && d <= 0x7E) { fin = d; ++j; break; }
            j = L; break;                       // not a CSI - abandon it
        }
        if (!fin) break;                        // incomplete: keep it for next frame
        dispatch_csi(g, sgr_mouse, params, fin, cols, rows);
        i = j;
    }
    if (i >= L) g_in.clear();
    else g_in.erase(0, i);
}

// Optional 2:1 horizontal box filter. It smooths diagonal edges but also
// smears every vertical one, so it is off by default: at 80 columns a blurred
// picture reads as worse than a jaggy one. --smooth turns it back on.
void downsample2x(const Framebuffer& src, Framebuffer& dst) {
    const int dw = dst.width, dh = dst.height;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            const Color a = src.get(x * 2, y);
            const Color b = src.get(x * 2 + 1, y);
            dst.set_pixel(x, y, {(a.r + b.r) * 0.5f, (a.g + b.g) * 0.5f,
                                 (a.b + b.b) * 0.5f, 1.0f});
        }
    }
}
} // namespace
#endif // !_WIN32

// ---------------------------------------------------------------------------
// Platform layer B: PNG writer (deflate via the system zlib) for headless runs
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include(<zlib.h>)
#    include <zlib.h>
#    define NEONRUN_PNG 1
#  endif
#endif

static bool write_png(const std::string& path, const Framebuffer& fb) {
#if defined(NEONRUN_PNG)
    const int W = fb.width, H = fb.height;
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<std::size_t>(H) * (1 + static_cast<std::size_t>(W) * 3));
    for (int y = 0; y < H; ++y) {
        raw.push_back(0);
        for (int x = 0; x < W; ++x) {
            const Color c = fb.get(x, y);
            raw.push_back(static_cast<unsigned char>(to_rgb8(c.r)));
            raw.push_back(static_cast<unsigned char>(to_rgb8(c.g)));
            raw.push_back(static_cast<unsigned char>(to_rgb8(c.b)));
        }
    }
    uLongf zl = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> z(zl);
    if (compress2(z.data(), &zl, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return false;
    z.resize(zl);

    std::vector<unsigned char> png;
    const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);
    auto chunk = [&png](const char type[4], const unsigned char* d, uLong len) {
        const std::size_t at = png.size();
        unsigned char hdr[8];
        hdr[0] = (len >> 24) & 0xFF; hdr[1] = (len >> 16) & 0xFF;
        hdr[2] = (len >> 8) & 0xFF;  hdr[3] = len & 0xFF;
        hdr[4] = type[0]; hdr[5] = type[1]; hdr[6] = type[2]; hdr[7] = type[3];
        png.insert(png.end(), hdr, hdr + 8);
        if (len) png.insert(png.end(), d, d + len);
        // CRC runs over the type field plus the data
        const uLong c = crc32(0L, png.data() + at + 4, static_cast<uInt>(len + 4));
        unsigned char tail[4];
        tail[0] = (c >> 24) & 0xFF; tail[1] = (c >> 16) & 0xFF;
        tail[2] = (c >> 8) & 0xFF;  tail[3] = c & 0xFF;
        png.insert(png.end(), tail, tail + 4);
    };
    unsigned char ihdr[13];
    ihdr[0] = (W >> 24) & 0xFF; ihdr[1] = (W >> 16) & 0xFF;
    ihdr[2] = (W >> 8) & 0xFF;  ihdr[3] = W & 0xFF;
    ihdr[4] = (H >> 24) & 0xFF; ihdr[5] = (H >> 16) & 0xFF;
    ihdr[6] = (H >> 8) & 0xFF;  ihdr[7] = H & 0xFF;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), static_cast<uLong>(z.size()));
    chunk("IEND", nullptr, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const std::size_t wrote = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return wrote == png.size();
#else
    (void)path; (void)fb;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Shared camera / light rig
// ---------------------------------------------------------------------------
static void rig(TexturedRenderer& r, const Game& g) {
    const float pz = g.player_z;
    const float py = g.ship_y();
    r.camera.look_at({g.player_x * 0.42f + g.shake_off.x,
                      3.25f + g.shake_off.y + py * 0.30f,
                      pz - 7.6f},
                     {g.player_x * 0.72f + g.shake_off.x * 0.4f,
                      1.20f + py * 0.55f,
                      pz + cfg::kLookAhead},
                     kUp);
    r.camera.fov_degrees = 64.0f;
    r.camera.near_z = 0.2f;
    r.camera.far_z  = 140.0f;
    r.light.direction = {-0.22f, -1.0f, -0.55f};
    r.light.color     = from_rgb8(210, 220, 255);
    r.light.intensity = 0.80f;
    r.light.ambient   = from_rgb8(58, 66, 110);
    r.use_lighting    = true;
    r.backface_cull   = true;
    r.fog_enabled     = true;
    r.fog_color       = pal::kFog;
    r.fog_near        = 20.0f;
    r.fog_far         = 78.0f;
}

// ---------------------------------------------------------------------------
// mode: play
// ---------------------------------------------------------------------------
static int mode_play(int target_fps, bool smooth) {
#if defined(_WIN32)
    std::fputs("the terminal backend here is POSIX only; use --replay or --shot\n", stderr);
    return 2;
#else
    (void)target_fps;
    int cols = 110, rows = 34;
    term_size(cols, rows);
    term_enter();

    Game g;
    // One framebuffer pixel per half cell, so the picture is 1:1 with the
    // terminal grid and nothing is resampled on the way out.
    TexturedRenderer scene(1, 1, Camera{}, pal::kVoid);
    scene.set_size(smooth ? cols * 2 : cols, rows * 2, pal::kVoid);
    Framebuffer view(cols, rows * 2, pal::kVoid);

    std::string out;
    out.reserve(static_cast<std::size_t>(cols) * rows * 14);

    GameLoop loop;
    loop.fixed_dt = 1.0f / 60.0f;

    auto frame_start = std::chrono::steady_clock::now();
    const auto budget = std::chrono::microseconds(
        static_cast<long>(1000000.0 / std::max(10, std::min(target_fps, 240))));

    loop.run(
        [&](float dt) { g.update(dt); },
        [&](float) {
            poll_input(g, cols, rows);
            if (g_resized) {
                g_resized = 0;
                int c2, r2; term_size(c2, r2);
                if (c2 != cols || r2 != rows) {
                    cols = c2; rows = r2;
                    scene.set_size(smooth ? cols * 2 : cols, rows * 2, pal::kVoid);
                    view = Framebuffer(cols, rows * 2, pal::kVoid);
                    std::fputs("\x1b[2J", stdout);
                }
            }
            g.fps = loop.stats.fps;
            rig(scene, g);
            g.render(scene);
            if (smooth) downsample2x(scene.framebuffer, view);
            draw_hud(smooth ? view : scene.framebuffer, g);
            present(out, smooth ? view : scene.framebuffer);
            std::fwrite(out.data(), 1, out.size(), stdout);
            std::fflush(stdout);

            const auto due = frame_start + budget;
            const auto now = std::chrono::steady_clock::now();
            if (now < due) std::this_thread::sleep_until(due);
            frame_start = std::chrono::steady_clock::now();
        },
        [&] { return g.quit || g_stop_flag != 0; });

    std::fputs("\x1b[0m\x1b[?25h\n", stdout);
    term_restore();
    std::printf("score %ld   distance %ld m   orbs %ld\n",
                g.score, static_cast<long>(g.player_z), g.orbs);
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// mode: headless deterministic replay, driven through GameLoop::advance()
// ---------------------------------------------------------------------------
static int mode_replay(int frames, const std::string& dir, std::uint32_t seed, int w, int h,
                     int skip) {
    Game g(seed);
    g.started = true;
    TexturedRenderer r(w, h, Camera{}, pal::kVoid);
    std::printf("%s", system_report().c_str());

    // Warm the run up first so a short capture shows mid-game speed, not the
    // empty opening stretch.
    for (int i = 0; i < skip; ++i) { g.autopilot(); g.update(1.0f / 60.0f); if (g.over) break; }

    GameLoop loop;
    loop.fixed_dt = 1.0f / 60.0f;
    int written = 0;

    for (int i = 0; i < frames; ++i) {
        g.autopilot();
        g.fps = 1.0f / loop.fixed_dt;
        loop.advance(loop.fixed_dt,
                     [&](float dt) { g.update(dt); },
                     [&](float) {
                         rig(r, g);
                         g.render(r);
                         draw_hud(r.framebuffer, g);
                         char name[512];
                         std::snprintf(name, sizeof(name), "%s/f%04d.png", dir.c_str(), written);
                         if (write_png(name, r.framebuffer)) { ++written; return; }
                         std::snprintf(name, sizeof(name), "%s/f%04d.ppm", dir.c_str(), written);
                         if (r.framebuffer.save_ppm(name)) { ++written; return; }
                         std::fprintf(stderr, "cannot write into %s\n", dir.c_str());
                     });
        if (g.over && i > 60) break;
    }
    std::printf("replay: %d frames written  score=%ld  dist=%ld m  speed=%.1f  "
                "orbs=%ld  survived=%.1fs\n",
                written, g.score, static_cast<long>(g.player_z), g.speed, g.orbs, g.elapsed);
    return written > 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// mode: probe - drive the autopilot until it dies, and report why
// ---------------------------------------------------------------------------
static int mode_probe(int steps, std::uint32_t seed) {
    Game g(seed);
    g.started = true;
    int i = 0;
    for (; i < steps; ++i) {
        g.autopilot();
        g.update(1.0f / 60.0f);
        if (g.over) break;
    }
    std::printf("seed=%-3u steps=%-5d survived=%5.2fs dist=%-5ldm score=%-6ld orbs=%-3ld "
                "hits=%ld [blk %ld hur %ld beam %ld] speed=%.1f %s\n",
                seed, i, g.elapsed, static_cast<long>(g.player_z), g.score, g.orbs, g.hits,
                g.hits_kind[0], g.hits_kind[1], g.hits_kind[2], g.speed,
                g.over ? "DIED" : "alive");
    return g.over ? 1 : 0;
}

// Renders just the overlay card, at a chosen resolution, for layout checks.
static int mode_card(const std::string& which, const std::string& path, int w, int h) {
    Game g;
    g.started = (which == "over");
    if (g.started) { g.over = true; g.shields = 0; g.score = 12345; g.orbs = 42; g.player_z = 987.0f; }
    TexturedRenderer r(w, h, Camera{}, pal::kVoid);
    rig(r, g);
    g.render(r);
    draw_hud(r.framebuffer, g);
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0)
        return write_png(path, r.framebuffer) ? 0 : 1;
    return r.framebuffer.save_ppm(path) ? 0 : 1;
}

static int mode_shot(const std::string& path, int w, int h) {
    Game g;
    g.started = true;
    TexturedRenderer r(w, h, Camera{}, pal::kVoid);
    for (int i = 0; i < 170; ++i) { g.autopilot(); g.update(1.0f / 60.0f); }
    g.fps = 60.0f;
    rig(r, g);
    g.render(r);
    draw_hud(r.framebuffer, g);
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0)
        return write_png(path, r.framebuffer) ? 0 : 1;
    return r.framebuffer.save_ppm(path) ? 0 : 1;
}

static int mode_ascii() {
    Game g;
    g.started = true;
    TexturedRenderer r(160, 80, Camera{}, pal::kVoid);
    for (int i = 0; i < 150; ++i) { g.autopilot(); g.update(1.0f / 60.0f); }
    rig(r, g);
    g.render(r);
    std::printf("%s", r.framebuffer.to_ascii(100, 34).c_str());
    return 0;
}

static void usage() {
    std::printf(
        "NEON RUN - 3D endless runner on creaddinscart.studio.cpp.sl/v3D-1.0.0\n"
        "\n"
        "  neonrun                    play in a truecolor terminal\n"
        "  neonrun --replay N <dir>   headless autopilot, N frames into <dir>\n"
        "  neonrun --shot <file>      one frame (.png or .ppm)\n"
        "  neonrun --ascii            one ASCII frame\n"
        "  neonrun --probe N          autopilot stress test, N steps\n"
        "  neonrun --skip N           warm up N steps before --replay starts writing\n"
        "  neonrun --card C <file>    render the 'title' or 'over' overlay card\n"
        "  neonrun --info             library system report\n"
        "\n"
        "  --width N   replay/shot width   (default 640)\n"
        "  --height N  replay/shot height  (default 360)\n"
        "  --seed N    world generator seed (default 0x51f0a9)\n"
        "  --fps N     terminal present rate, 10..240 (default 60; try 30 on\n"
        "              Apple Terminal if the repaint cannot keep up)\n"
        "  --smooth    supersample the scene 2x horizontally. Softer diagonals,\n"
        "              blurrier everything else; off by default\n"
        "\n"
        "  keys:   arrows or WASD switch lane / jump / duck\n"
        "          SPACE jump   DOWN duck   R restart   Q quit\n"
        "  mouse:  move to steer the lane, left click or wheel up jumps,\n"
        "          right click or wheel down ducks\n");
}

int main(int argc, char** argv) {
    int w = 640, h = 360;
    int skip = 0;
    int fps = 60;
    bool smooth = false;
    std::uint32_t seed = cfg::kSeed;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](int n, const char* what) -> bool {
            if (i + n >= argc) { std::fprintf(stderr, "%s needs %s\n", a.c_str(), what); return false; }
            return true;
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--info")  { std::printf("%s", system_report().c_str()); return 0; }
        else if (a == "--ascii") return mode_ascii();
        else if (a == "--card" && need(2, "title|over and a file name")) {
            const std::string which = argv[++i];
            return mode_card(which, argv[++i], w, h);
        }
        else if (a == "--probe" && need(1, "a step count"))
            return mode_probe(std::atoi(argv[++i]), seed);
        else if (a == "--width"  && need(1, "a number")) { w = std::atoi(argv[++i]); }
        else if (a == "--height" && need(1, "a number")) { h = std::atoi(argv[++i]); }
        else if (a == "--skip"   && need(1, "a number")) { skip = std::atoi(argv[++i]); }
        else if (a == "--fps"    && need(1, "a number")) { fps = std::atoi(argv[++i]); }
        else if (a == "--smooth") { smooth = true; }
        else if (a == "--seed"   && need(1, "a number"))
            seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--shot"   && need(1, "a file name")) return mode_shot(argv[++i], w, h);
        else if (a == "--replay" && need(2, "N and a directory")) {
            const int n = std::atoi(argv[++i]);
            const std::string dir = argv[++i];
            return mode_replay(std::max(1, n), dir, seed, w, h, std::max(0, skip));
        }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(); return 2; }
    }
#if !defined(_WIN32)
    if (!::isatty(STDIN_FILENO)) { usage(); return 2; }
#endif
    return mode_play(fps, smooth);
}
