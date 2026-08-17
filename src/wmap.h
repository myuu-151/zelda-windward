// World-map loader: .wworld sea charts + .wmap islands exported from the
// windward-editor. Loads the first assigned island, renders its painted
// terrain / grass / props / skirt with the client's sun, shadow map and
// horizon haze, and produces a heightfield (with shore distances) that
// feeds the client's existing collision, foam ring and water shading.
#pragma once

#include <vector>
#include "math3d.h"
#include "gl_loader.h"

// same memory layout the client's HeightField consumes (2 floats/cell:
// height with kIslandY pre-subtracted, signed shore distance)
struct WmapHeights {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int nx = 0, ny = 0;
    std::vector<float> data;
};

// scan exeBase and a few ancestors for a .wworld, load its first island
// and the editor asset library; false if nothing found
bool wmap_load(const char* exeBase, float islandYConst, float waterSkim);
bool wmap_active();
const WmapHeights& wmap_heights();
// world-space center of the loaded island (its chart quadrant)
void wmap_island_center(float* x, float* z);
// world-space centre of any chart quadrant
void wmap_cell_center(int cx, int cy, float* x, float* z);
// centre of the quadrant containing a world position
void wmap_quadrant_center(float wx, float wz, float* x, float* z);
// a flight ring that clears the loaded island: radius past its shore and
// height above its peaks. False when the position is not over the island.
bool wmap_flight_ring(float wx, float wz, float* radius, float* height);
// Height of solid island at a world position for camera collision: unlike
// the terrain heightfield this also covers the overhanging skirt just off
// the shore, so the camera cannot slip underneath the rim. -1000 = clear.
float wmap_block_height(float wx, float wz);
// What the camera boom is allowed to hit: the same surface, minus any part
// the map marks as one the view should rotate through. Falls back to
// wmap_block_height for maps that carry no such field.
float wmap_cam_block_height(float wx, float wz);

// Mesh collision. A heightfield holds one surface per spot, so a branch is
// recorded as the ground beneath it; these ask what is under a point at or
// below a given height, which is the question walking actually asks.
bool wmap_mesh_ready();
// Highest surface under (wx, wz) that is no higher than yMax.
bool wmap_mesh_floor(float wx, float wz, float yMax, float* outY);
// Whether anything stands between yLo and yHi there -- a wall, not a step.
bool wmap_mesh_wall(float wx, float wz, float yLo, float yHi);
// Push a capsule out of the geometry it overlaps and report what it stands
// on. pos is his feet, moved in place. Returns whether anything supports him.
bool wmap_mesh_resolve(float* pos, float radius, float height, float* groundY);
// Highest surface at a spot, for putting him on top of the island rather
// than wherever a one-surface field happened to say the ground was.
bool wmap_mesh_top(float wx, float wz, float* outY);
// The same, but only the parts marked as stopping the camera.
bool wmap_mesh_top_cam(float wx, float wz, float* outY);
// The highest camera-blocking surface at or below yMax, so geometry above
// the lens does not count as being in its way.
bool wmap_mesh_cam_below(float wx, float wz, float yMax, float* outY);
// Whether a sphere at p is inside camera-blocking geometry.
bool wmap_mesh_cam_touching(const float* p, float radius);
// Step a point around what it overlaps, keeping the boom's length.
bool wmap_mesh_cam_push(float* p, float radius);
// Where the segment a->b first crosses camera-blocking geometry, 0..1.
bool wmap_mesh_cam_segment(const float* a, const float* b, float* tOut);
// Every island on the chart as a disc -- x, z, radius, top height -- so the
// sea can shade under them at ranges no shadow map reaches. Returns count.
int wmap_shadow_discs(float* out4, int maxCount);
// world units per editor unit for the loaded island
float wmap_scale();
// measured size of the built-in test island, so its ring and shadow match
// the mesh instead of a guessed radius
void wmap_set_test_island_size(float radius, float top);
// the built-in test island's quadrant ("testisland <x> <y>" in the
// .wworld); false when the chart does not place it
bool wmap_test_island(float* x, float* z);
// the quadrant the chart starts the player in ("spawn <x> <y>"); false
// when the chart does not say, and the loaded island is used instead
bool wmap_spawn_center(float* x, float* z);
// wind ribbon height the chart asks for at a world position; 0 = default
float wmap_wind_height(float wx, float wz);
// chart readout for the on-screen map: grid size, quadrant spacing in
// world units, which cells hold an island, and which cell is which
int wmap_chart_size();
float wmap_quad_size();
int wmap_chart_cells(int* out2, int maxCount);
bool wmap_spawn_cell(int* cx, int* cy);
void wmap_loaded_cell(int* cx, int* cy);

void wmap_init_gl();   // after the GL context + loader are ready
void wmap_draw(const Mat4& viewProj, const Vec3& eye, const Mat4& lightVP,
               GLuint shadowTex, float timeSec, const Mat4& lightVPFar,
               GLuint shadowTexFar);
void wmap_draw_shadow(const Mat4& lightVP);
// who is walking through the grass this frame, so blades bow aside
void wmap_set_player(float x, float y, float z);
// Swap in whichever charted island the player is nearest, releasing the
// one left behind. True when the loaded island changed, so the caller can
// refresh the collision heightfield and shore texture.
bool wmap_stream(float px, float pz);
