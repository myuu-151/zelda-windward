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
// Every island on the chart as a disc -- x, z, radius, top height -- so the
// sea can shade under them at ranges no shadow map reaches. Returns count.
int wmap_shadow_discs(float* out4, int maxCount);
// world units per editor unit for the loaded island
float wmap_scale();
// the built-in test island's quadrant ("testisland <x> <y>" in the
// .wworld); false when the chart does not place it
bool wmap_test_island(float* x, float* z);

void wmap_init_gl();   // after the GL context + loader are ready
void wmap_draw(const Mat4& viewProj, const Vec3& eye, const Mat4& lightVP,
               GLuint shadowTex, float timeSec);
void wmap_draw_shadow(const Mat4& lightVP);
// who is walking through the grass this frame, so blades bow aside
void wmap_set_player(float x, float y, float z);
