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

void wmap_init_gl();   // after the GL context + loader are ready
void wmap_draw(const Mat4& viewProj, const Vec3& eye, const Mat4& lightVP,
               GLuint shadowTex, float timeSec);
void wmap_draw_shadow(const Mat4& lightVP);
