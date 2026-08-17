// .wworld / .wmap loading + rendering for the windward client.
// Geometry and map content come from the editor export; lighting is the
// client's own: its sun direction, its 4096 depth-compare shadow map, its
// horizon haze, so islands sit in the world like everything else.
#include "wmap.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <array>
#include "cgltf.h"
#include "stb_image.h"

// ---------------------------------------------------------------- data

static int         HN = 257;        // heightmap samples per side
static int         MASK_N = 512;    // splat mask resolution
// editor terrain is 48 units across; scale it up so an island reads at
// the client's world scale (override with "scale <s>" in the .wworld)
static float gScale = 2.0f;
static float gEditorHalf = 24.0f;   // the map's own half-extent
static float TER_HALF = 24.0f * 2.0f;
// Blade candidates and AO texels per side. Both scale with the island's
// own size so grass stays as dense, and contact AO as sharp, on a big
// island as on a small one -- fixed counts thinned out as maps grew.
static int         GRASS_N = 320;
static int         AO_N = 1024;

// match the editor's density scaling for an island of this half-extent
static void resolutions_for_island(float half)
{
    float mult = half / 24.0f;
    int g = (int)(320.0f * mult / 32.0f + 0.5f) * 32;
    GRASS_N = g < 320 ? 320 : (g > 800 ? 800 : g);
    int a = (int)(1024.0f * mult / 256.0f + 0.5f) * 256;
    AO_N = a < 1024 ? 1024 : (a > 4096 ? 4096 : a);
}

static bool gActive = false;
static WmapHeights gOut;
static std::string gAssetsDir;      // <root>/terrain/assets

static std::vector<float> gHeights(HN* HN, 0.0f);   // raw editor heights
static std::vector<Uint8> gMask(MASK_N* MASK_N, 0);
static std::vector<Uint8> gMask2(MASK_N* MASK_N, 0);
static std::vector<Uint8> gKill(MASK_N* MASK_N, 255);
static float gYOff = 0.0f;          // editor Y -> world Y
static float gCenter[2] = { 0.0f, 0.0f };   // island center in world xz

// the slice of the editor's tune blob the map content needs
struct Tune {
    float bladeDensity = 0.8f;
    float edgeBreak = 0.5f;
    float waterline = -3.0f;
    float grassDensity = 1.0f;      // unused here (baked in kill mask)
    float groundAO = 0.0f;
    float aoRadius = 2.5f;
    float islandDepth = 0.0f;
    float islandFrill = 0.0f;
    float islandBulge = 0.0f;
    bool  trimSkirt = false;   // underside follows the coastline
};
static Tune gTune;

struct PropMat {
    GLuint tex = 0;
    bool gray = false;
    float kd[3] = { 1, 1, 1 };
    float ka[3] = { 1, 1, 1 };
    std::string name, texName;
};
struct PropSub {
    int first = 0, count = 0, mat = 0;
};
struct PropMesh {
    std::string id;                 // "category/label"
    GLuint vao = 0, vbo = 0;
    std::vector<PropSub> subs;
    std::vector<PropMat> mats;
    float boundH = 1.0f;
    bool loaded = false;
};
struct PropInst {
    int mesh;
    float x, y, z, yaw, scale;
};
static std::vector<PropMesh> gMeshes;
static std::vector<PropInst> gProps;

struct StyleOv {
    std::string tex;
    float kd[3], ka[3];
};
static std::unordered_map<std::string, StyleOv> gStyles;

struct BladeInst {
    float x, y, z, rot, seed;
};
static std::vector<BladeInst> gBlades;

// GL objects
static GLuint gTerProg = 0, gTerVao = 0, gTerVbo = 0, gTerIbo = 0;
static GLsizei gTerIdx = 0;
static GLuint gMaskTex = 0, gMask2Tex = 0;
static GLuint gGrassTex = 0, gDirtTex = 0, gDirt2Tex = 0, gCliffTex = 0;
static GLuint gAOTex = 0;
static GLuint gGrassProg = 0, gGrassVao = 0;
static GLsizei gBladeCount = 0;
static GLuint gPropProg = 0, gSkirtProg = 0;
static GLuint gSkirtVao = 0;
static float  gSkirtPivot[2] = { 0.0f, 0.0f };   // island centroid, editor space
static GLsizei gSkirtIdx = 0;
static GLuint gDepthProg = 0, gDepthPropProg = 0;
// distant island silhouettes, one per chart quadrant
static GLuint gProxyProg = 0, gProxyVao = 0;
static GLsizei gProxyIdx = 0;
struct ProxyIsle {
    float x, z, seed, radius, height;
    int first = 0, count = 0;   // its slice of the baked silhouette mesh
    int cx = -99, cy = -99;
};
// One buffer holding a coarse mesh of every charted island, in world
// space. A distant island used to be a cone invented from a hash of its
// cell coordinates, so it looked nothing like the island that streamed in
// when you got close -- flying away turned your island into a stranger.
static GLuint gSilVao = 0, gSilVbo = 0;
static std::vector<float> gSilVerts;   // x,y,z,up
struct ChartIsle;
static void bake_silhouette(const ChartIsle& c, ProxyIsle& pr);
static void upload_silhouettes();
static std::vector<ProxyIsle> gProxies;
static float gSeaLevel = -2.7f;
static float gPlayer[3] = { 0.0f, -1000.0f, 0.0f };
static float gTime = 0.0f;
static int gChartSize = 7;
// Spacing between quadrants, in world units. This has to be a property of
// the CHART, not of whichever island happens to be loaded: derived from
// the loaded island it changed every time streaming swapped one in, so
// every island's world position moved and the player appeared to be
// teleported. Sized to hold the largest island the chart names.
static float gQuadSize = 240.0f;
static int gTestCell[2] = { -1, -1 };
struct ChartIsle { int cx, cy; std::string path; };
static std::vector<ChartIsle> gChart;
static int gSpawnCell[2] = { -1, -1 };   // "spawn x y" in the chart
// A props-only island: its heightmap exists for the shore field and for
// something to stand on, but there is no ground to draw -- the model the
// props place IS the island, the way the built-in test island is.
static bool gPropsOnly = false;
// per-quadrant wind ribbon height from the chart; 0 means "as the game has it"
static float gWindH[8][8] = {};      // every island the chart names
static int gLoadedCell[2] = { -99, -99 };  // which one is resident
static float gIslandTop = 0.0f;
// How far the island actually reaches from its centre. The disc that shades
// the sea at range used to be the whole map's half-extent, so a small model
// dropped into a big quadrant threw a shadow the size of the quadrant.
static float gIslandRadius = 0.0f;   // highest terrain point, world units
static float gTestRadius = 26.0f, gTestTop = 6.0f;
static float gIslandYConst = 0.0f, gWaterSkimK = -2.7f;
// height texture for the skirt shader (lazy, created on first draw)
static GLuint gSkirtHeightTex = 0;

// ---------------------------------------------------------------- shaders
// lighting mirrors the client's island shader: same sun, same wrap-toon
// shade curve, same shadow compare + haze

static const char* kShadowFn = R"GLSL(
uniform sampler2DShadow uShadow;
// The far cascade: the near map is tight so shadows are crisp underfoot,
// which leaves anything past its box casting nothing at all. This covers
// that range, coarsely.
uniform sampler2DShadow uShadow2;
uniform mat4 uLightVP2;
float shadow_far(vec3 world) {
    vec4 sp = uLightVP2 * vec4(world, 1.0);
    vec3 c = sp.xyz * 0.5 + 0.5;
    if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0))) ||
        c.z > 1.0)
        return 1.0;
    float z = c.z - 0.0016;   // coarser texels want a little more bias
    vec2 tf = vec2(1.0 / 2048.0);
    float sf2 = 0.0;
    sf2 += texture(uShadow2, vec3(c.xy + vec2(-0.5, -0.5) * tf, z));
    sf2 += texture(uShadow2, vec3(c.xy + vec2( 0.5, -0.5) * tf, z));
    sf2 += texture(uShadow2, vec3(c.xy + vec2(-0.5,  0.5) * tf, z));
    sf2 += texture(uShadow2, vec3(c.xy + vec2( 0.5,  0.5) * tf, z));
    return sf2 * 0.25;
}

float shadow_factor(vec4 sp) {
    vec3 c = sp.xyz * 0.5 + 0.5;
    if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0))) ||
        c.z > 1.0)
        return 1.0;
    float z = c.z - 0.0007;
    float s = 0.0;
    vec2 t = vec2(1.0 / 4096.0);
    s += texture(uShadow, vec3(c.xy + vec2(-0.5, -0.5) * t, z));
    s += texture(uShadow, vec3(c.xy + vec2( 0.5, -0.5) * t, z));
    s += texture(uShadow, vec3(c.xy + vec2(-0.5,  0.5) * t, z));
    s += texture(uShadow, vec3(c.xy + vec2( 0.5,  0.5) * t, z));
    vec2 e = min(c.xy, 1.0 - c.xy);
    float fade = smoothstep(0.0, 0.06, min(e.x, e.y));
    return mix(1.0, s * 0.25, fade);
}

// Near map where it reaches, far map beyond. The near box is small so its
// shadows stay sharp, and outside it every lookup came back lit -- which is
// why an island a few hundred units off cast a disc on the sea but nothing
// on itself, tree included.
float shadow_any(vec4 sp, vec3 world) {
    vec3 c = sp.xyz * 0.5 + 0.5;
    bool near_ok = all(greaterThanEqual(c.xy, vec2(0.02))) &&
                   all(lessThanEqual(c.xy, vec2(0.98))) && c.z <= 1.0;
    return near_ok ? shadow_factor(sp) : shadow_far(world);
}
)GLSL";

static std::string terFS()
{
    std::string s = R"GLSL(#version 330 core
in vec3 vWorld;
in vec3 vNrm;
in vec4 vShadowPos;
out vec4 fragColor;
uniform sampler2D uMask;
uniform sampler2D uMask2;
uniform sampler2D uGrassTex;
uniform sampler2D uDirtTex;
uniform sampler2D uDirt2Tex;
uniform sampler2D uCliffTex;
uniform sampler2D uAOMap;
uniform vec3 uEye;
uniform vec2 uCenter;
uniform float uHalf;
uniform float uEdgeBreak;
uniform float uGrassAO;
uniform float uGrassAORad;
uniform float uScale;
uniform float uEditorHalf;
)GLSL";
    s += kShadowFn;
    s += R"GLSL(
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x),
               mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    mat2 rot = mat2(0.8, -0.6, 0.6, 0.8);
    for (int i = 0; i < 4; i++) { v += a * vnoise(p); p = rot * p * 2.03; a *= 0.5; }
    return v;
}
void main() {
    // sample textures in EDITOR space so scaling the island doesn't
    // stretch the painted materials
    vec2 lxz = (vWorld.xz - uCenter) / uScale;
    vec2 maskUv = (lxz + vec2(uEditorHalf)) / (2.0 * uEditorHalf);
    float m = texture(uMask, maskUv).r;
    float m2 = texture(uMask2, maskUv).r;

    vec3 grass = texture(uGrassTex, lxz * 0.16).rgb;
    float tintN = fbm(lxz * 0.10);
    grass *= mix(vec3(0.92, 0.96, 0.80), vec3(1.06, 1.05, 0.95), tintN);

    vec3 dirtA = texture(uDirtTex, lxz * 0.20).rgb;
    vec3 dirtB = texture(uDirtTex, lxz.yx * 0.083 + 0.37).rgb;
    vec3 dirt = mix(dirtA, dirtB, 0.45);
    dirt *= mix(vec3(0.86, 0.80, 0.68), vec3(1.08, 1.03, 0.92),
                fbm(lxz * 0.21));
    vec3 soft = texture(uDirt2Tex, lxz * 0.15).rgb;
    soft *= mix(vec3(0.96, 0.94, 0.88), vec3(1.05, 1.03, 0.98),
                fbm(lxz * 0.17 + 5.1));

    float n = fbm(lxz * 1.1) - 0.5;
    float amp = uEdgeBreak * 0.8;
    float band = 0.03 + uEdgeBreak * 0.25;
    float edge = smoothstep(0.5 - band, 0.5 + band, m + n * amp);
    float edge2 = smoothstep(0.5 - band, 0.5 + band, m2 + n * amp);
    vec3 col = mix(grass, dirt, edge);
    col = mix(col, soft, edge2);
    col *= (1.0 - 0.13 * edge * (1.0 - edge) * 4.0) *
           (1.0 - 0.05 * edge2 * (1.0 - edge2) * 4.0);

    vec3 nrm = normalize(vNrm);
    float slope = 1.0 - nrm.y;
    vec3 cliffX = texture(uCliffTex, vec2(lxz.y, vWorld.y / uScale) * 0.14).rgb;
    vec3 cliffZ = texture(uCliffTex, vec2(lxz.x, vWorld.y / uScale) * 0.14).rgb;
    float wx = abs(nrm.x) / max(abs(nrm.x) + abs(nrm.z), 1e-4);
    vec3 cliff = mix(cliffZ, cliffX, wx);
    float cliffM = smoothstep(0.22, 0.42, slope + (fbm(lxz * 1.7) - 0.5) * 0.18);
    col = mix(col, cliff, cliffM);

    float open = textureLod(uAOMap, maskUv, uGrassAORad).r;
    col *= 1.0 - uGrassAO * (1.0 - open) * (1.0 - cliffM);

    // client toon light + shadow + haze
    const vec3 L = normalize(vec3(0.45, 0.35, -0.60));
    float nl = clamp(dot(nrm, L) * 0.5 + 0.5, 0.0, 1.0);
    float shade = mix(0.62, 1.05, smoothstep(0.25, 0.75, nl));
    shade *= mix(0.58, 1.0, shadow_any(vShadowPos, vWorld));
    col *= shade;
    float d = length(vWorld - uEye);
    // Wind Waker distance read: atmospheric haze washes it out first,
    // then it settles into a dark silhouette, then dissolves at the rim
    // The pale stage used to reach 0.6 toward white by 330 units, which
    // flattened every shadow and every bit of cliff AO -- an island at
    // middle distance went shadeless, then darkened again as the
    // silhouette below took over, so it read as detail popping in rather
    // than as haze. Keep it light and hold it off until the silhouette is
    // ready to carry the falloff, so the sequence only ever runs one way.
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(170.0, 430.0, d) * 0.26);
    col = mix(col, vec3(0.16, 0.26, 0.38), smoothstep(300.0, 560.0, d));
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(540.0, 780.0, d));
    fragColor = vec4(col, 1.0);
}
)GLSL";
    return s;
}

static const char* kTerVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
uniform mat4 uViewProj;
uniform mat4 uLightVP;
out vec3 vWorld;
out vec3 vNrm;
out vec4 vShadowPos;
void main() {
    vWorld = aPos;
    vNrm = aNrm;
    vShadowPos = uLightVP * vec4(aPos, 1.0);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)GLSL";

static const char* kGrassVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aBlade;   // x -1..1, y 0..1, plane 0/1
layout(location=1) in vec3 aRoot;    // world root
layout(location=2) in vec2 aRS;      // rot, seed
uniform mat4 uViewProj;
uniform mat4 uLightVP;
uniform float uTime;
uniform sampler2DShadow uShadow;
// The far cascade: the near map is tight so shadows are crisp underfoot,
// which leaves anything past its box casting nothing at all. This covers
// that range, coarsely.
uniform sampler2DShadow uShadow2;
uniform mat4 uLightVP2;
uniform float uScale;
uniform vec3 uPlayer;      // blades bow away from whoever walks through
out float vV;
out vec2 vLxz;
out float vSeed;
out float vShadow;
out vec3 vWorldPos;
uniform vec2 uCenter;
void main() {
    float planeRot = aRS.x + aBlade.z * 1.5707963;
    vec2 dir = vec2(cos(planeRot), sin(planeRot));
    float hgt = (0.28 + fract(aRS.y * 3.17) * 0.30) * uScale;
    float wid = (0.035 + fract(aRS.y * 5.71) * 0.02) * uScale;
    vec3 p = aRoot;
    p.xz += dir * aBlade.x * wid * (1.0 - aBlade.y * 0.7);
    p.y  += aBlade.y * hgt;
    float phase = dot(aRoot.xz, vec2(0.8, 0.6)) * 0.7 + uTime * 1.8;
    float gust  = sin(phase) * 0.5 + sin(phase * 0.37 + 1.7) * 0.5;
    float sway  = (gust * 0.10 + sin(uTime * 3.1 + aRS.y * 6.28) * 0.03)
                  * aBlade.y * aBlade.y;
    p.xz += vec2(0.85, 0.53) * sway;

    // trampling: within a small radius the blade bows away from the
    // player and flattens, tips moving most so roots stay planted
    vec2 away = aRoot.xz - uPlayer.xz;
    float pd = length(away);
    float reach = 1.5 * uScale;
    if (pd < reach) {
        float w = 1.0 - pd / reach;
        // only what he is actually standing among: ignore blades far
        // below a flying player or above him on a terrace
        float vert = 1.0 - clamp(abs(uPlayer.y - aRoot.y) /
                                 (2.2 * uScale), 0.0, 1.0);
        float push = w * w * vert;
        vec2 dirp = pd > 0.001 ? away / pd : vec2(1.0, 0.0);
        float lean = aBlade.y * aBlade.y;
        p.xz += dirp * push * hgt * 0.85 * lean;
        p.y  -= push * hgt * 0.45 * lean;
    }

    // one shadow probe per blade at the root, via the client shadow map
    vec4 sp = uLightVP * vec4(aRoot, 1.0);
    vec3 c = sp.xyz * 0.5 + 0.5;
    vShadow = 1.0;
    if (all(greaterThan(c.xy, vec2(0.0))) && all(lessThan(c.xy, vec2(1.0))) &&
        c.z < 1.0)
        vShadow = mix(0.58, 1.0, texture(uShadow, vec3(c.xy, c.z - 0.0012)));

    vV = aBlade.y;
    vLxz = (aRoot.xz - uCenter) / uScale;
    vSeed = fract(aRS.y * 11.13);
    vWorldPos = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
)GLSL";

static const char* kGrassFS = R"GLSL(#version 330 core
in float vV;
in vec2 vLxz;
in float vSeed;
in float vShadow;
in vec3 vWorldPos;
out vec4 fragColor;
uniform sampler2D uGrassTex;
uniform vec3 uEye;
void main() {
    vec3 groundCol = textureLod(uGrassTex, vLxz * 0.16, 4.5).rgb;
    vec3 root = groundCol * 0.55;
    vec3 tip  = groundCol * (1.15 + vSeed * 0.15);
    vec3 col = mix(root, tip, vV * vV) * vShadow;
    // same distance read as the land it grows on, or the field stays
    // vivid green while the island behind it turns to silhouette
    float d = length(vWorldPos - uEye);
    // The pale stage used to reach 0.6 toward white by 330 units, which
    // flattened every shadow and every bit of cliff AO -- an island at
    // middle distance went shadeless, then darkened again as the
    // silhouette below took over, so it read as detail popping in rather
    // than as haze. Keep it light and hold it off until the silhouette is
    // ready to carry the falloff, so the sequence only ever runs one way.
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(170.0, 430.0, d) * 0.26);
    col = mix(col, vec3(0.16, 0.26, 0.38), smoothstep(300.0, 560.0, d));
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(540.0, 780.0, d));
    fragColor = vec4(col, 1.0);
}
)GLSL";

static std::string propFS()
{
    std::string s = R"GLSL(#version 330 core
in vec3 vNrm;
in vec2 vUv;
in float vLocalY;
in vec3 vWorld;
in vec3 vVCol;
in vec4 vShadowPos;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec3 uKd;
uniform vec3 uKa;
uniform vec3 uEye;
uniform float uBoundH;
uniform int uHasTex;
uniform int uGrayMask;
)GLSL";
    s += kShadowFn;
    s += R"GLSL(
void main() {
    vec4 t = uHasTex == 1 ? texture(uTex, vUv) : vec4(1.0);
    if (uHasTex == 1) {
        if (uGrayMask == 1) {
            if (t.r < 0.35 && t.a > 0.99) discard;
        }
        if (t.a < 0.5) discard;
    }
    vec3 grad = mix(uKa, uKd, clamp(vLocalY / max(uBoundH, 0.001), 0.0, 1.0));
    vec3 col = uGrayMask == 1 ? grad * (0.55 + 0.9 * t.r) : t.rgb;
    col *= vVCol;
    const vec3 L = normalize(vec3(0.45, 0.35, -0.60));
    vec3 n = normalize(vNrm);
    float sf = shadow_any(vShadowPos, vWorld);
    if (uGrayMask == 1) {
        float wrap = clamp(dot(n, L) * 0.55 + 0.45, 0.0, 1.0);
        col *= (0.42 + 0.62 * wrap) * mix(0.60, 1.0, sf);
    } else {
        // Shade a textured prop exactly as the client shades its own
        // models. Both use the same texture on the same mesh, so any
        // difference here is the reason an imported island looked duller
        // than the built-in one: this branch took the raw dot product,
        // which on a flat deck is about 0.42 and lands at 0.86, where the
        // model path wraps it to 0.71 first and comes out slightly over 1.
        float nl = clamp(dot(n, L) * 0.5 + 0.5, 0.0, 1.0);
        float shade = mix(0.62, 1.05, smoothstep(0.25, 0.75, nl));
        shade *= mix(0.58, 1.0, sf);
        col *= shade;
    }
    float d = length(vWorld - uEye);
    // Wind Waker distance read: atmospheric haze washes it out first,
    // then it settles into a dark silhouette, then dissolves at the rim
    // The pale stage used to reach 0.6 toward white by 330 units, which
    // flattened every shadow and every bit of cliff AO -- an island at
    // middle distance went shadeless, then darkened again as the
    // silhouette below took over, so it read as detail popping in rather
    // than as haze. Keep it light and hold it off until the silhouette is
    // ready to carry the falloff, so the sequence only ever runs one way.
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(170.0, 430.0, d) * 0.26);
    col = mix(col, vec3(0.16, 0.26, 0.38), smoothstep(300.0, 560.0, d));
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(540.0, 780.0, d));
    fragColor = vec4(col, 1.0);
}
)GLSL";
    return s;
}

static const char* kPropVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUv;
layout(location=3) in vec3 aVCol;
uniform mat4 uViewProj;
uniform mat4 uLightVP;
uniform mat4 uModel;
uniform float uTime;
uniform float uBoundH;
uniform int uGrayMask;
out vec3 vNrm;
out vec2 vUv;
out float vLocalY;
out vec3 vWorld;
out vec3 vVCol;
out vec4 vShadowPos;

// the client's island wind, so pack props lean in step with its tree:
// a steady downwind lean plus gusts, weighted by height above the base
// so trunks stay planted, and a fine flutter on leaf cards
vec3 wind_sway(vec3 p, float w, float t, int leafy) {
    if (w <= 0.0) return p;
    const vec2 W = normalize(vec2(-1.0, -0.35));
    float gust = sin(t * 0.9 + (p.x + p.z) * 0.10) +
                 0.4 * sin(t * 2.1 + p.x * 0.13 + 1.7);
    vec2 perp = vec2(-W.y, W.x);
    vec2 sway = W * (0.55 + gust) +
                perp * (0.35 * sin(t * 1.15 + p.z * 0.14 + 0.8));
    p.xz += sway * (0.10 * w * w);
    if (leafy == 1)
        p.xz += vec2(sin(t * 5.7 + p.y * 2.1),
                     cos(t * 5.1 + p.x * 1.7)) * (0.020 * w);
    return p;
}

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    world.xyz = wind_sway(world.xyz,
                          clamp(aPos.y / max(uBoundH, 0.001), 0.0, 1.0),
                          uTime, uGrayMask);
    vNrm = mat3(uModel) * aNorm;
    vUv = aUv;
    vLocalY = aPos.y;
    vWorld = world.xyz;
    vVCol = aVCol;
    vShadowPos = uLightVP * world;
    gl_Position = uViewProj * world;
}
)GLSL";

// Distant islands: every other chart quadrant gets a dark silhouette
// proxy on the horizon, the Wind Waker trick that turns open sea into a
// navigable map. Shape varies per cell from its seed.
static const char* kProxyVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aPos;   // baked from the island's own heightmap
layout(location=1) in float aUp;
uniform mat4 uViewProj;
out vec3 vWorld;
out float vUp;
void main() {
    vWorld = aPos;
    vUp = aUp;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)GLSL";

static const char* kProxyFS = R"GLSL(#version 330 core
in vec3 vWorld;
in float vUp;
out vec4 fragColor;
uniform vec3 uEye;
void main() {
    vec3 dark = mix(vec3(0.16, 0.26, 0.38), vec3(0.24, 0.36, 0.46), vUp);
    // The proxy had no sun term at all, so an island swapping between its
    // real geometry and this one went shaded, then flat, then shaded --
    // the swap read as the lighting dropping out rather than as distance.
    // No normals are baked into the proxy, so take one from the surface
    // itself and run the same wrap the props use.
    const vec3 L = normalize(vec3(0.45, 0.35, -0.60));
    vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    if (n.y < 0.0) n = -n;
    float nl = clamp(dot(n, L) * 0.5 + 0.5, 0.0, 1.0);
    dark *= mix(0.72, 1.12, smoothstep(0.25, 0.75, nl));
    float d = length(vWorld - uEye);
    // and haze over the range the props use, so both reach the horizon
    // colour together instead of one washing out ahead of the other
    vec3 col = mix(dark, vec3(0.66, 0.80, 0.95),
                   smoothstep(540.0, 780.0, d));
    fragColor = vec4(col, 1.0);
}
)GLSL";

static const char* kSkirtVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aData;    // x, z (editor local), t
uniform mat4 uViewProj;
uniform vec2 uCenter;
uniform float uYOff;
uniform sampler2D uHeight;
uniform float uHalf;
uniform float uDepth;
uniform float uFrill;
uniform float uBulge;
uniform float uScale;
uniform float uEditorHalf;
uniform vec2  uPivot;   // island centroid in editor space: a trimmed
                        // underside must not taper about the map origin
out vec3 vWorld;
out vec3 vNrm;
out float vT;
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x),
               mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
void main() {
    vec2 xz = aData.xy;
    float t = aData.z;
    vec3 pos;
    if (t < 0.5) {
        vec2 uv = (xz + vec2(uEditorHalf)) / (2.0 * uEditorHalf);
        pos = vec3(xz.x * uScale,
                   texture(uHeight, uv).r * uScale + uYOff, xz.y * uScale);
    } else if (t < 1.5) {
        float nn = vnoise(xz * 0.35);
        float f1 = vnoise(xz * 0.45 + 11.0) - 0.5;
        float f2 = vnoise(xz * 1.3 + 7.0) - 0.5;
        float taper = mix(0.82, 1.30, uBulge) + (f1 * 0.5 + f2 * 0.2) * uFrill;
        vec2 d = xz - uPivot;
        pos = vec3((uPivot.x + d.x * taper) * uScale,
                   uYOff - uDepth * uScale *
                       (0.55 + 0.5 * nn + f2 * 0.5 * uFrill),
                   (uPivot.y + d.y * taper) * uScale);
    } else {
        pos = vec3(uPivot.x * uScale, uYOff - uDepth * uScale * 1.25,
                   uPivot.y * uScale);
    }
    pos.xz += uCenter;
    vWorld = pos;
    vNrm = normalize(vec3(xz.x - uPivot.x, uDepth * 0.02 + 6.0,
                          xz.y - uPivot.y));
    if (t > 1.5) vNrm = vec3(0.0, -1.0, 0.0);
    vT = min(t, 1.5);
    gl_Position = uViewProj * vec4(pos, 1.0);
}
)GLSL";

static const char* kSkirtFS = R"GLSL(#version 330 core
in vec3 vWorld;
in vec3 vNrm;
in float vT;
out vec4 fragColor;
uniform sampler2D uCliffTex;
uniform vec3 uEye;
void main() {
    vec3 n = normalize(vNrm);
    vec3 cx = texture(uCliffTex, vWorld.zy * 0.10).rgb;
    vec3 cz = texture(uCliffTex, vWorld.xy * 0.10).rgb;
    float wx = abs(n.x) / max(abs(n.x) + abs(n.z), 1e-4);
    vec3 col = mix(cz, cx, wx);
    const vec3 L = normalize(vec3(0.45, 0.35, -0.60));
    float diff = max(dot(n, L), 0.0);
    col *= 0.62 + 0.38 * diff;
    col *= mix(1.0, 0.45, vT / 1.5);
    float d = length(vWorld - uEye);
    // Wind Waker distance read: atmospheric haze washes it out first,
    // then it settles into a dark silhouette, then dissolves at the rim
    // The pale stage used to reach 0.6 toward white by 330 units, which
    // flattened every shadow and every bit of cliff AO -- an island at
    // middle distance went shadeless, then darkened again as the
    // silhouette below took over, so it read as detail popping in rather
    // than as haze. Keep it light and hold it off until the silhouette is
    // ready to carry the falloff, so the sequence only ever runs one way.
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(170.0, 430.0, d) * 0.26);
    col = mix(col, vec3(0.16, 0.26, 0.38), smoothstep(300.0, 560.0, d));
    col = mix(col, vec3(0.66, 0.80, 0.95), smoothstep(540.0, 780.0, d));
    fragColor = vec4(col, 1.0);
}
)GLSL";

static const char* kDepthVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uLightVP;
void main() { gl_Position = uLightVP * vec4(aPos, 1.0); }
)GLSL";
static const char* kDepthFS = R"GLSL(#version 330 core
void main() {}
)GLSL";
static const char* kDepthPropVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUv;
uniform mat4 uLightVP;
uniform mat4 uModel;
uniform float uTime;
uniform float uBoundH;
uniform int uGrayMask;
out vec2 vUv;

// the client's island wind, so pack props lean in step with its tree:
// a steady downwind lean plus gusts, weighted by height above the base
// so trunks stay planted, and a fine flutter on leaf cards
vec3 wind_sway(vec3 p, float w, float t, int leafy) {
    if (w <= 0.0) return p;
    const vec2 W = normalize(vec2(-1.0, -0.35));
    float gust = sin(t * 0.9 + (p.x + p.z) * 0.10) +
                 0.4 * sin(t * 2.1 + p.x * 0.13 + 1.7);
    vec2 perp = vec2(-W.y, W.x);
    vec2 sway = W * (0.55 + gust) +
                perp * (0.35 * sin(t * 1.15 + p.z * 0.14 + 0.8));
    p.xz += sway * (0.10 * w * w);
    if (leafy == 1)
        p.xz += vec2(sin(t * 5.7 + p.y * 2.1),
                     cos(t * 5.1 + p.x * 1.7)) * (0.020 * w);
    return p;
}

void main() {
    vUv = aUv;
    vec4 world = uModel * vec4(aPos, 1.0);
    world.xyz = wind_sway(world.xyz,
                          clamp(aPos.y / max(uBoundH, 0.001), 0.0, 1.0),
                          uTime, uGrayMask);
    gl_Position = uLightVP * world;
}
)GLSL";
static const char* kDepthPropFS = R"GLSL(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform int uHasTex;
uniform int uGrayMask;
void main() {
    if (uHasTex == 1) {
        vec4 t = texture(uTex, vUv);
        if (uGrayMask == 1) { if (t.r < 0.35 && t.a > 0.99) discard; }
        if (t.a < 0.5) discard;
    }
}
)GLSL";

// ---------------------------------------------------------------- helpers

static GLuint compile_prog(const char* vs, const char* fs)
{
    auto sh = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            SDL_Log("wmap shader error:\n%s", log);
        }
        return s;
    };
    GLuint p = glCreateProgram();
    GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
        SDL_Log("wmap link failed");
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static GLuint tex_from_bmp(const std::string& path, bool* gray = nullptr,
                           bool repeat = true)
{
    SDL_Surface* raw = SDL_LoadBMP(path.c_str());
    if (!raw)
        return 0;
    SDL_Surface* s = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (!s)
        return 0;
    if (gray) {
        bool g = true;
        const Uint8* px = (const Uint8*)s->pixels;
        for (int y = 0; y < s->h && g; y += SDL_max(1, s->h / 32))
            for (int x = 0; x < s->w && g; x += SDL_max(1, s->w / 32)) {
                const Uint8* p = px + y * s->pitch + x * 4;
                if (abs(p[0] - p[1]) > 10 || abs(p[1] - p[2]) > 10)
                    g = false;
            }
        *gray = g;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, s->pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    SDL_DestroySurface(s);
    return t;
}

// x,z in WORLD units; returns the editor height scaled to world
static float height_at(float x, float z)
{
    float u = (x / gScale + gEditorHalf) / (2.0f * gEditorHalf) * (HN - 1);
    float v = (z / gScale + gEditorHalf) / (2.0f * gEditorHalf) * (HN - 1);
    int i = SDL_clamp((int)u, 0, HN - 2);
    int j = SDL_clamp((int)v, 0, HN - 2);
    float fu = SDL_clamp(u - i, 0.0f, 1.0f);
    float fv = SDL_clamp(v - j, 0.0f, 1.0f);
    float a = gHeights[j * HN + i], b = gHeights[j * HN + i + 1];
    float c = gHeights[(j + 1) * HN + i], d = gHeights[(j + 1) * HN + i + 1];
    // A props-only map marks open water as -100: a sentinel, not a height.
    // Blending it with a neighbouring deck cell gives about -50, ground far
    // under the sea, so the outer half cell of every island evaporates --
    // and the collision field is built from this, so the damage is done
    // before anything downstream can help. Where a corner is not a real
    // height, take the highest that is.
    float h;
    if (a > -50.0f && b > -50.0f && c > -50.0f && d > -50.0f) {
        h = (a * (1 - fu) + b * fu) * (1 - fv) +
            (c * (1 - fu) + d * fu) * fv;
    } else {
        // nearest, not highest: the highest reaches a full cell past the
        // last real sample and puts ground out over the water
        h = (fu < 0.5f) ? ((fv < 0.5f) ? a : c)
                        : ((fv < 0.5f) ? b : d);
    }
    return h * gScale;
}

// x,z in WORLD units
static Uint8 mask_at(const std::vector<Uint8>& m, float x, float z)
{
    int i = SDL_clamp((int)((x / gScale + gEditorHalf) /
                           (2.0f * gEditorHalf) * MASK_N), 0, MASK_N - 1);
    int j = SDL_clamp((int)((z / gScale + gEditorHalf) /
                           (2.0f * gEditorHalf) * MASK_N), 0, MASK_N - 1);
    return m[j * MASK_N + i];
}

// ---------------------------------------------------------------- loading

static void build_heightfield();
static void build_blades();

static bool load_wmap(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "TERMAP0", 7) != 0) {
        fclose(f);
        return false;
    }
    gPropsOnly = magic[7] == '9';
    if (magic[7] >= '8') {
        // v8 carries the map's world size and the resolutions that scale
        // with it -- without this the whole file reads 16 bytes shifted
        float half = 24.0f;
        Uint32 res[3] = { 257, 512, 256 };
        if (fread(&half, 4, 1, f) != 1 || fread(res, 4, 3, f) != 3) {
            fclose(f);
            return false;
        }
        gEditorHalf = half;
        HN = (int)res[0];
        MASK_N = (int)res[1];
        TER_HALF = gEditorHalf * gScale;
        SDL_Log("wmap: map is %.0f units, %d heights, %d masks",
                half * 2.0f, HN, MASK_N);
    } else {
        gEditorHalf = 24.0f;
        HN = 257;
        MASK_N = 512;
        TER_HALF = gEditorHalf * gScale;
    }
    resolutions_for_island(gEditorHalf);
    gHeights.assign((size_t)HN * HN, 0.0f);
    gMask.assign((size_t)MASK_N * MASK_N, 0);
    gMask2.assign((size_t)MASK_N * MASK_N, 0);
    gKill.assign((size_t)MASK_N * MASK_N, 255);
    fread(gHeights.data(), sizeof(float), gHeights.size(), f);
    fread(gMask.data(), 1, gMask.size(), f);
    fread(gKill.data(), 1, gKill.size(), f);
    if (magic[7] >= '3')
        fread(gMask2.data(), 1, gMask2.size(), f);
    struct RawInst {
        std::string id;
        float tr[5];
    };
    std::vector<RawInst> raw;
    if (magic[7] >= '4') {
        Uint32 n = 0;
        if (fread(&n, 4, 1, f) == 1) {
            for (Uint32 i = 0; i < n; i++) {
                Uint16 len = 0;
                if (fread(&len, 2, 1, f) != 1)
                    break;
                std::string id(len, '\0');
                fread(id.data(), 1, len, f);
                RawInst ri;
                ri.id = id;
                if (fread(ri.tr, sizeof(float), 5, f) != 5)
                    break;
                raw.push_back(ri);
            }
        }
    }
    // v5 settings + tune blob: pull only map-content fields
    float blade = 0.8f, edgeB = 0.5f, cam[5];
    if (magic[7] >= '5') {
        if (fread(&blade, 4, 1, f) == 1 && fread(&edgeB, 4, 1, f) == 1 &&
            fread(cam, 4, 5, f) == 5) {
            gTune.bladeDensity = blade;
            gTune.edgeBreak = edgeB;
        }
        if (magic[7] >= '7') {
            Uint32 tsz = 0;
            if (fread(&tsz, 4, 1, f) == 1 && tsz >= 4) {
                std::vector<Uint8> blob(tsz);
                if (fread(blob.data(), 1, tsz, f) == tsz) {
                    // editor TuneBlob layout (floats then ints; see editor)
                    const float* fb = (const float*)blob.data();
                    // [0..11] radius,strength,paintTarget,falloff,
                    // grassDensity,terrace,propScale,propScaleRand,
                    // propDensity,propSpacing,propYawFixed,uiScale
                    size_t nf = tsz / 4;
                    auto get = [&](size_t idx, float def) {
                        return idx < nf ? fb[idx] : def;
                    };
                    // TuneBlob layout: floats 0..11, ints 12..21,
                    // floats 22..25, propSelId 26..49 (96 bytes),
                    // islandDepth 50, waterline 51, showWater 52,
                    // islandFrill 53, islandBulge 54
                    gTune.grassDensity = get(4, 1.0f);
                    gTune.groundAO = get(23, 0.0f);
                    gTune.aoRadius = get(24, 2.5f);
                    gTune.islandDepth = get(50, 0.0f);
                    gTune.waterline = get(51, -3.0f);
                    gTune.islandFrill = get(53, 0.0f);
                    gTune.islandBulge = get(54, 0.0f);
                    // int at 75: trim the underside to the coastline
                    const int* ib = (const int*)blob.data();
                    gTune.trimSkirt = (75 < (int)nf && ib[75] != 0);
                }
            }
        }
    }
    fclose(f);

    // resolve prop mesh ids -> library indices
    std::unordered_map<std::string, int> byId;
    for (int i = 0; i < (int)gMeshes.size(); i++)
        byId[gMeshes[i].id] = i;
    for (const RawInst& ri : raw) {
        auto it = byId.find(ri.id);
        if (it == byId.end())
            continue;
        gProps.push_back({ it->second, ri.tr[0] * gScale + gCenter[0],
                           ri.tr[1] * gScale, ri.tr[2] * gScale + gCenter[1],
                           ri.tr[3], ri.tr[4] * gScale });
    }
    SDL_Log("wmap: %s -- %d prop instances from %d saved, library %d meshes",
            path.c_str(), (int)gProps.size(), (int)raw.size(),
            (int)gMeshes.size());
    if (!raw.empty() && gProps.empty())
        SDL_Log("wmap: no prop ids matched, first saved id was '%s'",
                raw[0].id.c_str());
    return true;
}

static void load_styles()
{
    FILE* f = fopen((gAssetsDir + "/props/styles.txt").c_str(), "rb");
    if (!f)
        return;
    char line[512], key[256], tex[256];
    while (fgets(line, sizeof line, f)) {
        StyleOv s{};
        if (sscanf(line, "style %255s %255s %f %f %f %f %f %f", key, tex,
                   &s.kd[0], &s.kd[1], &s.kd[2],
                   &s.ka[0], &s.ka[1], &s.ka[2]) == 8) {
            s.tex = tex;
            gStyles[key] = s;
        }
    }
    fclose(f);
}

static void scan_props()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string dir = gAssetsDir + "/props";
    std::vector<std::string> cats;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory() && e.path().filename() != "textures")
            cats.push_back(e.path().filename().string());
    std::sort(cats.begin(), cats.end());
    for (const std::string& c : cats) {
        std::vector<fs::path> objs;
        for (const auto& e : fs::directory_iterator(dir + "/" + c, ec))
            if (e.path().extension() == ".obj" ||
                e.path().extension() == ".glb")
                objs.push_back(e.path());
        std::sort(objs.begin(), objs.end());
        for (const auto& p : objs) {
            PropMesh m;
            m.id = c + "/" + p.stem().string();
            gMeshes.push_back(m);
        }
    }
}

static int mat_role(const std::string& name)
{
    std::string l;
    for (char c : name)
        l += (char)tolower((unsigned char)c);
    if (l.find("leaf") != std::string::npos ||
        l.find("leaves") != std::string::npos ||
        l.find("foliage") != std::string::npos ||
        l.find("frond") != std::string::npos ||
        l.find("needle") != std::string::npos ||
        l.find("petal") != std::string::npos)
        return 1;
    if (l.find("bark") != std::string::npos ||
        l.find("trunk") != std::string::npos ||
        l.find("stem") != std::string::npos ||
        l.find("branch") != std::string::npos ||
        l.find("wood") != std::string::npos ||
        l.find("root") != std::string::npos)
        return 0;
    return 2;
}
static const char* kRoles[3] = { "Trunk", "Leaves", "Other" };

static std::unordered_map<std::string, std::pair<GLuint, bool>> gTexCache;
static std::pair<GLuint, bool> prop_texture(const std::string& file)
{
    auto it = gTexCache.find(file);
    if (it != gTexCache.end())
        return it->second;
    bool gray = false;
    GLuint t = tex_from_bmp(gAssetsDir + "/props/" + file, &gray);
    gTexCache[file] = { t, gray };
    return { t, gray };
}

// A prop imported from Blender is a glb, not an OBJ. cgltf is already
// here for the client's own models, so this reads one into the same
// interleaved layout the OBJ path builds -- baking each node's world
// transform in, since a glb places its objects with node transforms and
// reading the mesh list alone would pile them all on the origin.
static bool load_prop_glb(PropMesh& m, const std::string& path)
{
    cgltf_options opt{};
    cgltf_data* d = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &d) != cgltf_result_success)
        return false;
    if (cgltf_load_buffers(&opt, d, path.c_str()) != cgltf_result_success) {
        cgltf_free(d);
        return false;
    }
    // Top/bottom colours the editor pulled out of each Blender Color
    // Ramp on import, saved beside the model. glTF has no way to carry a
    // ramp, so without these a stylised material arrives as its flat base
    // colour -- white leaves over a grayscale mask.
    std::unordered_map<std::string, std::array<float, 6>> grads;
    {
        FILE* gf = fopen((path + ".grad").c_str(), "rb");
        if (gf) {
            char nm[256];
            float t0, t1, t2, b0, b1, b2;
            while (fscanf(gf, "%255s %f %f %f %f %f %f", nm, &t0, &t1, &t2,
                          &b0, &b1, &b2) == 7)
                grads[nm] = { t0, t1, t2, b0, b1, b2 };
            fclose(gf);
        }
    }
    std::vector<float> data;
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (cgltf_size ni = 0; ni < d->nodes_count; ni++) {
        const cgltf_node& nd = d->nodes[ni];
        if (!nd.mesh)
            continue;
        cgltf_float xf[16];
        cgltf_node_transform_world(&nd, xf);
        for (cgltf_size pi = 0; pi < nd.mesh->primitives_count; pi++) {
            const cgltf_primitive& pr = nd.mesh->primitives[pi];
            if (pr.type != cgltf_primitive_type_triangles)
                continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr;
            const cgltf_accessor *uv = nullptr, *col = nullptr;
            for (cgltf_size ai = 0; ai < pr.attributes_count; ai++) {
                const cgltf_attribute& at = pr.attributes[ai];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && !uv)
                    uv = at.data;
                else if (at.type == cgltf_attribute_type_color && !col)
                    col = at.data;
            }
            if (!pos)
                continue;
            PropMat mat;
            if (pr.material && pr.material->name) {
                mat.name = pr.material->name;
                auto gi = grads.find(mat.name);
                if (gi != grads.end())
                    for (int k = 0; k < 3; k++) {
                        mat.kd[k] = gi->second[k];
                        mat.ka[k] = gi->second[3 + k];
                    }
            }
            if (pr.material && pr.material->has_pbr_metallic_roughness) {
                const cgltf_pbr_metallic_roughness& pbr =
                    pr.material->pbr_metallic_roughness;
                // The gradient multiplies the texture, so a material with
                // a texture and no Color Ramp must leave it at white --
                // taking the base-colour factor and darkening the bottom to
                // 0.72 of it tinted the whole model down twice over, which
                // is why an imported island read darker than the same mesh
                // drawn by the client's own model path.
                const bool hasTex = pbr.base_color_texture.texture != nullptr;
                if (grads.find(mat.name) == grads.end())
                    for (int k = 0; k < 3; k++) {
                        mat.kd[k] = hasTex ? 1.0f : pbr.base_color_factor[k];
                        mat.ka[k] = hasTex ? 1.0f
                                           : pbr.base_color_factor[k] * 0.72f;
                    }
                if (pbr.base_color_texture.texture &&
                    pbr.base_color_texture.texture->image) {
                    const cgltf_image* im = pbr.base_color_texture.texture->image;
                    if (im->buffer_view && im->buffer_view->buffer &&
                        im->buffer_view->buffer->data) {
                        int w = 0, h = 0, ch = 0;
                        const unsigned char* bytes =
                            (const unsigned char*)im->buffer_view->buffer->data +
                            im->buffer_view->offset;
                        stbi_uc* px = stbi_load_from_memory(
                            bytes, (int)im->buffer_view->size, &w, &h, &ch, 4);
                        if (px) {
                            bool gray = true;
                            const int step = w * h > 4096 ? (w * h) / 4096 : 1;
                            for (int i = 0; i < w * h && gray; i += step) {
                                const stbi_uc* q = px + (size_t)i * 4;
                                if (abs((int)q[0] - (int)q[1]) > 6 ||
                                    abs((int)q[1] - (int)q[2]) > 6)
                                    gray = false;
                            }
                            // Only a material the Color Ramp spoke for is a
                            // mask to be tinted. Judging by whether the image
                            // is desaturated calls grey rock a mask, and a
                            // mask is drawn as its own grey values times the
                            // gradient -- which is why the cliff came out
                            // dark while the tinted parts looked right.
                            mat.gray = gray &&
                                       grads.find(mat.name) != grads.end();
                            glGenTextures(1, &mat.tex);
                            glBindTexture(GL_TEXTURE_2D, mat.tex);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                                         GL_RGBA, GL_UNSIGNED_BYTE, px);
                            glGenerateMipmap(GL_TEXTURE_2D);
                            glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_MIN_FILTER,
                                            GL_LINEAR_MIPMAP_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            stbi_image_free(px);
                        }
                    }
                }
            }
            PropSub sub;
            sub.mat = (int)m.mats.size();
            m.mats.push_back(mat);
            sub.first = (int)(data.size() / 11);
            const cgltf_size n = pr.indices ? pr.indices->count : pos->count;
            for (cgltf_size k = 0; k < n; k++) {
                const cgltf_size v =
                    pr.indices ? cgltf_accessor_read_index(pr.indices, k) : k;
                float p3[3] = { 0, 0, 0 }, n3[3] = { 0, 1, 0 };
                float t2[2] = { 0, 0 }, c4[4] = { 1, 1, 1, 1 };
                cgltf_accessor_read_float(pos, v, p3, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, n3, 3);
                if (uv)  cgltf_accessor_read_float(uv, v, t2, 2);
                // Vertex colours multiply the texture in the prop shader,
                // and the client's own model path ignores them -- so a mesh
                // carrying baked shading in COLOR_0 came out darker as a
                // prop than the identical model drawn as an island. Take
                // them only where there is no texture to darken.
                if (col && !mat.tex) cgltf_accessor_read_float(col, v, c4, 4);
                const float wx = xf[0]*p3[0] + xf[4]*p3[1] + xf[8]*p3[2] + xf[12];
                const float wy = xf[1]*p3[0] + xf[5]*p3[1] + xf[9]*p3[2] + xf[13];
                const float wz = xf[2]*p3[0] + xf[6]*p3[1] + xf[10]*p3[2] + xf[14];
                float nx = xf[0]*n3[0] + xf[4]*n3[1] + xf[8]*n3[2];
                float ny = xf[1]*n3[0] + xf[5]*n3[1] + xf[9]*n3[2];
                float nz = xf[2]*n3[0] + xf[6]*n3[1] + xf[10]*n3[2];
                const float nl = sqrtf(nx*nx + ny*ny + nz*nz);
                if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; }
                lo[0] = SDL_min(lo[0], wx); hi[0] = SDL_max(hi[0], wx);
                lo[1] = SDL_min(lo[1], wy); hi[1] = SDL_max(hi[1], wy);
                lo[2] = SDL_min(lo[2], wz); hi[2] = SDL_max(hi[2], wz);
                data.insert(data.end(), { wx, wy, wz, nx, ny, nz,
                                          t2[0], t2[1], c4[0], c4[1], c4[2] });
            }
            sub.count = (int)(data.size() / 11) - sub.first;
            if (sub.count > 0)
                m.subs.push_back(sub);
        }
    }
    cgltf_free(d);
    if (data.empty())
        return false;
    m.boundH = SDL_max(0.05f, hi[1] - lo[1]);
    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(),
                 GL_STATIC_DRAW);
    for (int a = 0; a < 4; a++)
        glEnableVertexAttribArray(a);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                          (void*)(6 * sizeof(float)));
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                          (void*)(8 * sizeof(float)));
    glBindVertexArray(0);
    m.loaded = true;
    SDL_Log("wmap: loaded glb prop %s (%d verts)", m.id.c_str(),
            (int)(data.size() / 11));
    return true;
}

static bool load_prop_mesh(PropMesh& m)
{
    if (m.loaded)
        return true;
    size_t slash = m.id.find('/');
    std::string base = gAssetsDir + "/props/" + m.id.substr(0, slash) + "/" +
                       m.id.substr(slash + 1);
    std::string path = base + ".obj";
    if (!std::filesystem::exists(path) &&
        std::filesystem::exists(base + ".glb"))
        return load_prop_glb(m, base + ".glb");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::vector<float> vs, ns, ts, vcs, data;
    std::unordered_map<std::string, int> matIndex;
    int curMat = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        float a, b, c, r, g, bl;
        char name[256];
        int vn = sscanf(line, "v %f %f %f %f %f %f", &a, &b, &c, &r, &g, &bl);
        if (line[0] == 'v' && line[1] == ' ' && vn >= 3) {
            vs.insert(vs.end(), { a, b, c });
            if (vn == 6)
                vcs.insert(vcs.end(), { r, g, bl });
            else
                vcs.insert(vcs.end(), { 1, 1, 1 });
        } else if (sscanf(line, "vn %f %f %f", &a, &b, &c) == 3) {
            ns.insert(ns.end(), { a, b, c });
        } else if (sscanf(line, "vt %f %f", &a, &b) == 2) {
            ts.insert(ts.end(), { a, b });
        } else if (sscanf(line, "mtllib %255s", name) == 1) {
            std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
            FILE* mf = fopen((dir + name).c_str(), "rb");
            if (mf) {
                char ml[512], mn[256];
                float mr, mg, mb;
                while (fgets(ml, sizeof ml, mf)) {
                    if (sscanf(ml, "newmtl %255s", mn) == 1) {
                        matIndex[mn] = (int)m.mats.size();
                        m.mats.push_back({});
                        m.mats.back().name = mn;
                    } else if (!m.mats.empty() &&
                               sscanf(ml, "Kd %f %f %f", &mr, &mg, &mb) == 3) {
                        m.mats.back().kd[0] = mr;
                        m.mats.back().kd[1] = mg;
                        m.mats.back().kd[2] = mb;
                    } else if (!m.mats.empty() &&
                               sscanf(ml, "Ka %f %f %f", &mr, &mg, &mb) == 3) {
                        m.mats.back().ka[0] = mr;
                        m.mats.back().ka[1] = mg;
                        m.mats.back().ka[2] = mb;
                    } else if (!m.mats.empty() &&
                               sscanf(ml, "map_Kd %255s", mn) == 1) {
                        auto pt = prop_texture(mn);
                        m.mats.back().tex = pt.first;
                        m.mats.back().gray = pt.second;
                        m.mats.back().texName = mn;
                    }
                }
                fclose(mf);
            }
        } else if (sscanf(line, "usemtl %255s", name) == 1) {
            auto it = matIndex.find(name);
            curMat = it != matIndex.end() ? it->second : 0;
            if (m.subs.empty() || m.subs.back().count > 0)
                m.subs.push_back({ (int)(data.size() / 11), 0, curMat });
            else
                m.subs.back().mat = curMat;
        } else if (line[0] == 'f' && line[1] == ' ') {
            int vi[3];
            if (sscanf(line, "f %d/%*d/%*d %d/%*d/%*d %d/%*d/%*d",
                       &vi[0], &vi[1], &vi[2]) == 3) {
                if (m.subs.empty())
                    m.subs.push_back({ 0, 0, 0 });
                for (int k = 0; k < 3; k++) {
                    int i = vi[k] - 1;
                    data.insert(data.end(),
                                { vs[i * 3], vs[i * 3 + 1], vs[i * 3 + 2] });
                    if ((size_t)(i * 3 + 2) < ns.size())
                        data.insert(data.end(),
                                    { ns[i * 3], ns[i * 3 + 1], ns[i * 3 + 2] });
                    else
                        data.insert(data.end(), { 0, 1, 0 });
                    if ((size_t)(i * 2 + 1) < ts.size())
                        data.insert(data.end(), { ts[i * 2], ts[i * 2 + 1] });
                    else
                        data.insert(data.end(), { 0, 0 });
                    data.insert(data.end(),
                                { vcs[i * 3], vcs[i * 3 + 1], vcs[i * 3 + 2] });
                    m.boundH = SDL_max(m.boundH, vs[i * 3 + 1]);
                }
                m.subs.back().count += 3;
            }
        }
    }
    fclose(f);
    if (data.empty())
        return false;
    if (m.mats.empty())
        m.mats.push_back({});
    // style presets
    for (PropMat& mat : m.mats) {
        auto it = gStyles.find(m.id + "|" + kRoles[mat_role(mat.name)]);
        if (it == gStyles.end())
            continue;
        memcpy(mat.kd, it->second.kd, sizeof mat.kd);
        memcpy(mat.ka, it->second.ka, sizeof mat.ka);
        if (!it->second.tex.empty() && it->second.tex != "-") {
            auto pt = prop_texture("textures/" + it->second.tex);
            if (pt.first) {
                mat.tex = pt.first;
                mat.gray = pt.second;
            }
        }
    }
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(),
                 GL_STATIC_DRAW);
    for (int a = 0; a < 4; a++)
        glEnableVertexAttribArray(a);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 44, (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 44, (void*)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 44, (void*)24);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 44, (void*)32);
    glBindVertexArray(0);
    m.loaded = true;
    return true;
}

// ---------------------------------------------------------------- public

bool wmap_load(const char* exeBase, float islandYConst, float waterSkim)
{
    // find a .wworld near the exe (build dirs sit under the repo)
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string worldPath, rootDir;
    fs::path dir = exeBase;
    for (int up = 0; up < 6 && worldPath.empty(); up++) {
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".wworld") {
                worldPath = e.path().string();
                rootDir = dir.string();
                break;
            }
        dir = dir.parent_path();
        if (dir.empty())
            break;
    }
    if (worldPath.empty())
        return false;

    // parse the chart: the first assigned island is the playable one, the
    // rest become distant silhouettes
    std::string mapPath;
    float waterline = -3.0f;
    int chartSize = 7, cellX = 0, cellY = 0;
    std::vector<std::pair<int, int>> assigned;
    int testX = -1, testY = -1;
    {
        FILE* f = fopen(worldPath.c_str(), "rb");
        if (!f)
            return false;
        char line[1200], p[1024];
        int x, y, n;
        float wl;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "waterline %f", &wl) == 1)
                waterline = wl;
            else if (sscanf(line, "size %d", &n) == 1)
                chartSize = n;
            else if (sscanf(line, "scale %f", &wl) == 1) {
                gScale = SDL_max(0.1f, wl);
                TER_HALF = 24.0f * gScale;
            }
            else if (sscanf(line, "wind %d %d %f", &x, &y, &wl) == 3) {
                if (x >= 0 && y >= 0 && x < 8 && y < 8)
                    gWindH[y][x] = wl;
            }
            else if (sscanf(line, "spawn %d %d", &x, &y) == 2) {
                gSpawnCell[0] = x;
                gSpawnCell[1] = y;
            }
            else if (sscanf(line, "testisland %d %d", &x, &y) == 2) {
                testX = x;
                testY = y;
            }
            else if (sscanf(line, "cell %d %d %1023[^\n]", &x, &y, p) == 3) {
                assigned.push_back({ x, y });
                std::string q = p;
                // tolerate CRLF and stray trailing spaces in the path
                while (!q.empty() &&
                       (q.back() == '\r' || q.back() == '\n' ||
                        q.back() == ' ' || q.back() == '\t'))
                    q.pop_back();
                gChart.push_back({ x, y, q });   // every island, for streaming
                // the start cell wins outright; otherwise first listed
                if (mapPath.empty() ||
                    (x == gSpawnCell[0] && y == gSpawnCell[1])) {
                    mapPath = q;
                    cellX = x;
                    cellY = y;
                }
            }
        }
        fclose(f);
    }
    if (mapPath.empty())
        return false;
    // read each island's header for its world size, so quadrants are
    // spaced to fit the biggest one and never move again
    {
        float maxHalf = 24.0f;
        for (const ChartIsle& c : gChart) {
            FILE* mf = fopen(c.path.c_str(), "rb");
            if (!mf)
                continue;
            char mg[8];
            float half = 24.0f;
            if (fread(mg, 1, 8, mf) == 8 && memcmp(mg, "TERMAP0", 7) == 0 &&
                mg[7] >= '8' && fread(&half, 4, 1, mf) == 1)
                maxHalf = SDL_max(maxHalf, half);
            fclose(mf);
        }
        gQuadSize = SDL_max(240.0f, maxHalf * 2.0f * gScale * 1.3f);
        SDL_Log("wmap: quadrants %.0f units apart (largest island %.0f)",
                gQuadSize, maxHalf * 2.0f * gScale);
    }
    if (gSpawnCell[0] >= 0)
        SDL_Log("wmap: chart starts at %c%d", 'A' + gSpawnCell[0],
                gSpawnCell[1] + 1);
    else
        SDL_Log("wmap: chart names no start cell, using the first island");
    if (gSpawnCell[0] >= 0)
        SDL_Log("wmap: chart starts at %c%d", 'A' + gSpawnCell[0],
                gSpawnCell[1] + 1);
    else
        SDL_Log("wmap: chart names no start cell, using the first island");

    // the editor's asset library: search up from the world file's dir
    {
        fs::path d = rootDir;
        for (int up = 0; up < 6; up++) {
            if (fs::exists(d / "terrain" / "assets" / "grass.bmp", ec)) {
                gAssetsDir = (d / "terrain" / "assets").string();
                break;
            }

            if (d.parent_path() == d)
                break;
            d = d.parent_path();
        }
    }
    if (gAssetsDir.empty()) {
        SDL_Log("wmap: could not find terrain/assets near %s",
                rootDir.c_str());
        return false;
    }
    SDL_Log("wmap: assets at %s", gAssetsDir.c_str());
    // world placement must be known before the island loads: prop
    // instances are baked into world space as they are read
    gTune.waterline = waterline;
    gIslandYConst = islandYConst;
    gWaterSkimK = waterSkim;
    gYOff = waterSkim - waterline * gScale;
    gSeaLevel = waterSkim;
    gChartSize = chartSize;
    gTestCell[0] = testX;
    gTestCell[1] = testY;
    // the chart's own spacing, not one derived from this island: placing
    // the island by its own size put it somewhere the rest of the world
    // (spawn, streaming, the silhouettes, the map) did not agree with
    gCenter[0] = (cellX - (chartSize - 1) * 0.5f) * gQuadSize;
    gCenter[1] = (cellY - (chartSize - 1) * 0.5f) * gQuadSize;
    // silhouettes stand in for islands the chart really has but that are
    // not loaded -- never invent land the player cannot sail to
    for (const auto& c : assigned) {
        if (c.first == cellX && c.second == cellY)
            continue;                   // this one is loaded for real
        if (c.first == testX && c.second == testY)
            continue;                   // the test island draws for real
        unsigned s = (unsigned)(c.first * 73856093 ^ c.second * 19349663);
        float r0 = ((s >> 8) & 1023) / 1023.0f;
        float r1 = ((s >> 18) & 1023) / 1023.0f;
        float wx, wz;
        wmap_cell_center(c.first, c.second, &wx, &wz);
        ProxyIsle pr{ wx, wz, r0 * 6.2831853f,
                      34.0f + r1 * 30.0f, 12.0f + r0 * 16.0f, 0, 0,
                      c.first, c.second };
        for (const ChartIsle& ci : gChart)
            if (ci.cx == c.first && ci.cy == c.second) {
                bake_silhouette(ci, pr);
                break;
            }
        gProxies.push_back(pr);
    }
    upload_silhouettes();
    load_styles();
    scan_props();
    if (!load_wmap(mapPath)) {
        SDL_Log("wmap: could not open island '%s'", mapPath.c_str());
        return false;
    }

    build_heightfield();
    build_blades();
    gActive = true;
    SDL_Log("wmap: world %s, %d blades", worldPath.c_str(),
            (int)gBlades.size());
    return true;
}

void build_island_gl();

// The island most recently streamed out. Its terrain is kept and still
// drawn, so leaving an island does not replace it with a stand-in while
// it still fills the screen -- the real thing stays until it has hazed
// down to the horizon, and only then does the silhouette take over.
// Grass, props and its shadow go, since none of them read at that range.
struct RetainedIsle {
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLsizei idx = 0;
    GLuint maskTex = 0, mask2Tex = 0, aoTex = 0;
    GLuint grassVao = 0;
    GLsizei blades = 0;
    std::vector<PropInst> props;   // world-space already, so they just draw
    float center[2] = { 0.0f, 0.0f };
    float terHalf = 0.0f, editorHalf = 24.0f;
    int cx = -99, cy = -99;
};
static RetainedIsle gPrev;

static void release_retained()
{
    if (gPrev.vao) {
        glDeleteVertexArrays(1, &gPrev.vao);
        glDeleteBuffers(1, &gPrev.vbo);
        glDeleteBuffers(1, &gPrev.ibo);
    }
    if (gPrev.grassVao) glDeleteVertexArrays(1, &gPrev.grassVao);
    if (gPrev.maskTex)  glDeleteTextures(1, &gPrev.maskTex);
    if (gPrev.mask2Tex) glDeleteTextures(1, &gPrev.mask2Tex);
    if (gPrev.aoTex)    glDeleteTextures(1, &gPrev.aoTex);
    gPrev = RetainedIsle();
}

// Release the resident island's buffers before another takes its place.
static void free_island_gl()
{
    // hand the terrain over to the retained slot rather than deleting it
    release_retained();
    if (gTerVao) {
        gPrev.vao = gTerVao; gPrev.vbo = gTerVbo; gPrev.ibo = gTerIbo;
        gPrev.idx = gTerIdx;
        gPrev.maskTex = gMaskTex;
        gPrev.mask2Tex = gMask2Tex;
        gPrev.aoTex = gAOTex;
        gPrev.center[0] = gCenter[0];
        gPrev.center[1] = gCenter[1];
        gPrev.terHalf = TER_HALF;
        gPrev.editorHalf = gEditorHalf;
        gPrev.cx = gLoadedCell[0];
        gPrev.cy = gLoadedCell[1];
        // its foliage comes along: trees and grass are most of what an
        // island looks like, and dropping them left bare rock behind
        gPrev.grassVao = gGrassVao;
        gPrev.blades = gBladeCount;
        gPrev.props = gProps;
        gGrassVao = 0;
        gTerVao = gTerVbo = gTerIbo = 0;
        gMaskTex = gMask2Tex = gAOTex = 0;
    }
    if (gGrassVao) { glDeleteVertexArrays(1, &gGrassVao); gGrassVao = 0; }
    if (gMaskTex)  { glDeleteTextures(1, &gMaskTex);  gMaskTex = 0; }
    if (gMask2Tex) { glDeleteTextures(1, &gMask2Tex); gMask2Tex = 0; }
    if (gAOTex)    { glDeleteTextures(1, &gAOTex);    gAOTex = 0; }
    if (gSkirtHeightTex) {
        glDeleteTextures(1, &gSkirtHeightTex);
        gSkirtHeightTex = 0;
    }
    gProps.clear();
    gBlades.clear();
    gBladeCount = 0;
}

void wmap_set_player(float x, float y, float z)
{
    gPlayer[0] = x; gPlayer[1] = y; gPlayer[2] = z;
}

float wmap_scale() { return gScale; }

void wmap_set_test_island_size(float radius, float top)
{
    if (radius > 1.0f) gTestRadius = radius;
    gTestTop = top;
}

// The client's collision + foam field, and the grass field: both
// rebuilt per island so streaming can swap them.
static void build_heightfield()
{
    // heightfield for the client: heights (pre-offset, minus kIslandY) and
    // signed shore distance via a two-pass chamfer transform
    // Pad the field with open water around the island: the foam ring
    // breathes several units outward from the shore, and without room to
    // spread it gets clipped to the island's own rect.
    const float PAD = 28.0f;
    const float HALF = TER_HALF + PAD;
    // The test island collides against 512 samples across ~69 units --
    // 0.135 per cell. This field pads 28 units around the island for the
    // foam ring, so at 513 it was 0.30 per cell: less than half the
    // detail, which is why a chart island felt coarser underfoot than the
    // built-in one however finely its heightmap was baked.
    const int PN = 1025;
    gOut.x0 = gCenter[0] - HALF; gOut.x1 = gCenter[0] + HALF;
    gOut.y0 = -gCenter[1] - HALF; gOut.y1 = -gCenter[1] + HALF;
    gOut.nx = PN;
    gOut.ny = PN;
    gOut.data.assign((size_t)PN * PN * 2, 0.0f);
    const float cell = 2.0f * HALF / (PN - 1);
    // how far the island skirt reaches past the rim
    const float flare = 0.82f + (1.30f - 0.82f) * gTune.islandBulge;
    std::vector<float> dist((size_t)PN * PN, 1e9f);
    std::vector<char> land((size_t)PN * PN, 0);
    for (int j = 0; j < PN; j++)
        for (int i = 0; i < PN; i++) {
            // heightfield by = -z: row j maps to by, so sample z = -by
            float by = -HALF + cell * j;
            float x = -HALF + cell * i;
            bool inside = fabsf(x) <= TER_HALF && fabsf(by) <= TER_HALF;
            float h = inside ? height_at(x, -by) + gYOff : -100.0f;
            gOut.data[((size_t)j * PN + i) * 2] = h - gIslandYConst;
            // The skirt is solid rock filling the whole footprint, so at
            // sea level the island's silhouette is its rect -- not where
            // the terrain surface happens to cross the water. Without
            // this a tapered rim pulls the foam ring inland, under the
            // overhang, instead of leaving it at the cliff base.
            // A full-map skirt makes the whole footprint solid rock, so
            // the shore field has to treat it all as land. A TRIMMED
    // skirt follows the coastline instead, so the heightfield
            // is the silhouette again.
            const bool skirt = gTune.islandDepth > 0.05f && !gTune.trimSkirt;
            bool isLand = inside && (skirt || h > gWaterSkimK - 0.4f);
            if (gPropsOnly && inside) {
                // These maps carry the model's outline at the waterline in
                // place of the soft-dirt layer, because an overhanging
                // island's deck -- which is what the heights describe --
                // is not where it meets the sea. Foam belongs at the water.
                const int mi = SDL_clamp((int)((x / gScale + gEditorHalf) /
                                               (2.0f * gEditorHalf) * MASK_N),
                                         0, MASK_N - 1);
                const int mj = SDL_clamp((int)((-by / gScale + gEditorHalf) /
                                               (2.0f * gEditorHalf) * MASK_N),
                                         0, MASK_N - 1);
                isLand = gMask2[(size_t)mj * MASK_N + mi] > 128;
            }
            // The skirt flares out past the terrain rim, so the silhouette
            // at the waterline is the skirt's, not the heightfield's. Count
            // that flare as land or the foam ring hides under the overhang.
            if (!isLand && flare > 1.0f &&
                fabsf(x) <= TER_HALF * flare && fabsf(by) <= TER_HALF * flare) {
                const float rimX = SDL_clamp(x, -TER_HALF, TER_HALF);
                const float rimZ = SDL_clamp(-by, -TER_HALF, TER_HALF);
                if (height_at(rimX, rimZ) + gYOff > gWaterSkimK - 0.4f)
                    isLand = true;
            }
            land[(size_t)j * PN + i] = isLand;
            dist[(size_t)j * PN + i] = isLand ? 1e9f : 0.0f;
        }
    const int HN2 = PN;   // the loops below walk the padded grid
    // chamfer passes give distance-to-water; sign: + on water side
    auto relax = [&](int i, int j, int di, int dj, float w) {
        int a = j * PN + i, b = (j + dj) * PN + (i + di);
        dist[a] = SDL_min(dist[a], dist[b] + w);
    };
    for (int j = 0; j < PN; j++)
        for (int i = 0; i < PN; i++) {
            if (i > 0) relax(i, j, -1, 0, cell);
            if (j > 0) relax(i, j, 0, -1, cell);
            if (i > 0 && j > 0) relax(i, j, -1, -1, cell * 1.414f);
        }
    for (int j = PN - 1; j >= 0; j--)
        for (int i = PN - 1; i >= 0; i--) {
            if (i < PN - 1) relax(i, j, 1, 0, cell);
            if (j < PN - 1) relax(i, j, 0, 1, cell);
            if (i < PN - 1 && j < PN - 1) relax(i, j, 1, 1, cell * 1.414f);
        }
    // water-side positive distances (distance to land) via mirrored pass
    std::vector<float> dist2((size_t)PN * PN);
    for (size_t k = 0; k < dist2.size(); k++)
        dist2[k] = land[k] ? 0.0f : 1e9f;
    {
        auto relax2 = [&](int i, int j, int di, int dj, float w) {
            int a = j * PN + i, b = (j + dj) * PN + (i + di);
            dist2[a] = SDL_min(dist2[a], dist2[b] + w);
        };
        for (int j = 0; j < PN; j++)
            for (int i = 0; i < PN; i++) {
                if (i > 0) relax2(i, j, -1, 0, cell);
                if (j > 0) relax2(i, j, 0, -1, cell);
                if (i > 0 && j > 0) relax2(i, j, -1, -1, cell * 1.414f);
            }
        for (int j = PN - 1; j >= 0; j--)
            for (int i = PN - 1; i >= 0; i--) {
                if (i < PN - 1) relax2(i, j, 1, 0, cell);
                if (j < PN - 1) relax2(i, j, 0, 1, cell);
                if (i < PN - 1 && j < PN - 1)
                    relax2(i, j, 1, 1, cell * 1.414f);
            }
    }
    for (int j = 0; j < PN; j++)
        for (int i = 0; i < PN; i++) {
            size_t k = (size_t)j * PN + i;
            gOut.data[k * 2 + 1] = land[k] ? -dist[k] : dist2[k];
            // Only clear the height where there is genuinely nothing to
            // stand on. Land here means "the sea should foam around this",
            // which for an overhanging island is a smaller area than the
            // surface you can walk on -- clearing by it dropped the deck.
            if (!land[k] && gOut.data[k * 2] < gWaterSkimK - 0.4f)
                gOut.data[k * 2] = -100.0f;   // open water marker
        }

    {
        // what the sea will actually see: land cells and how far the shore
        // field reaches, so a missing foam ring is diagnosable
        int nland = 0;
        float dmin = 1e9f, dmax = -1e9f;
        for (size_t k = 0; k < (size_t)PN * PN; k++) {
            if (land[k]) nland++;
            const float sd = gOut.data[k * 2 + 1];
            if (sd < dmin) dmin = sd;
            if (sd > dmax) dmax = sd;
        }
        SDL_Log("wmap: shore field %d land cells of %d, dist %.1f..%.1f",
                nland, PN * PN, dmin, dmax);
    }
    // measure the island while its land is known
    gIslandRadius = 0.0f;
    for (int j = 0; j < PN; j++)
        for (int i = 0; i < PN; i++) {
            if (!land[(size_t)j * PN + i])
                continue;
            const float x = -HALF + cell * i, by = -HALF + cell * j;
            const float r = sqrtf(x * x + by * by);
            if (r > gIslandRadius)
                gIslandRadius = r;
        }
    gIslandTop = -1e9f;
    for (float h : gHeights)
        gIslandTop = SDL_max(gIslandTop, h * gScale + gYOff);

}

static void build_blades()
{
    // grass blade instances: bake the editor's density rules on CPU
    unsigned rng = 12345u;
    auto frand = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) * (1.0f / 16777216.0f);
    };
    for (int j = 0; j < GRASS_N; j++)
        for (int i = 0; i < GRASS_N * 2; i++) {
            float x = -TER_HALF + 2.0f * TER_HALF * (i + frand()) /
                      (GRASS_N * 2);
            float z = -TER_HALF + 2.0f * TER_HALF * (j + frand()) / GRASS_N;
            float rot = frand() * 6.2831853f;
            float seed = frand();
            float m = mask_at(gMask, x, z) / 255.0f;
            float m2 = mask_at(gMask2, x, z) / 255.0f;
            float mm = SDL_max(m, m2);
            if (mm > 0.30f + fmodf(seed * 7.31f, 1.0f) * 0.12f)
                continue;
            float dens = (1.0f - mask_at(gKill, x, z) / 255.0f) *
                         gTune.bladeDensity;
            if (fmodf(seed * 9.77f, 1.0f) >= dens)
                continue;
            // no blades on cliff faces (the editor culls by slope too)
            const float e = 0.4f;
            float hx = height_at(x + e, z) - height_at(x - e, z);
            float hz = height_at(x, z + e) - height_at(x, z - e);
            float ny = 2.0f * e / sqrtf(hx * hx + 4.0f * e * e + hz * hz);
            if (1.0f - ny > 0.20f + fmodf(seed * 4.77f, 1.0f) * 0.08f)
                continue;
            gBlades.push_back({ x + gCenter[0], height_at(x, z) + gYOff,
                                z + gCenter[1], rot, seed });
        }

}

// Load one charted island: its data, world placement, heightfield and
// grass, then its GL. Used at startup and whenever we stream.
static bool load_island_at(int cx, int cy, const std::string& path)
{
    gProps.clear();
    gBlades.clear();
    wmap_cell_center(cx, cy, &gCenter[0], &gCenter[1]);
    if (!load_wmap(path)) {
        SDL_Log("wmap: could not open island '%s'", path.c_str());
        return false;
    }
    gLoadedCell[0] = cx;
    gLoadedCell[1] = cy;
    build_heightfield();
    build_blades();
    // the island we are standing on should not also be a silhouette
    gProxies.clear();
    gSilVerts.clear();
    for (const ChartIsle& c : gChart) {
        if ((c.cx == cx && c.cy == cy) ||
            (c.cx == gTestCell[0] && c.cy == gTestCell[1]))
            continue;
        unsigned sd = (unsigned)(c.cx * 73856093 ^ c.cy * 19349663);
        float r0 = ((sd >> 8) & 1023) / 1023.0f;
        float r1 = ((sd >> 18) & 1023) / 1023.0f;
        float wx, wz;
        wmap_cell_center(c.cx, c.cy, &wx, &wz);
        ProxyIsle pr{ wx, wz, r0 * 6.2831853f,
                      34.0f + r1 * 30.0f, 12.0f + r0 * 16.0f, 0, 0,
                      c.cx, c.cy };
        bake_silhouette(c, pr);
        gProxies.push_back(pr);
    }
    upload_silhouettes();
    return true;
}

static void upload_silhouettes()
{
    if (gSilVerts.empty())
        return;
    if (!gSilVao) {
        glGenVertexArrays(1, &gSilVao);
        glGenBuffers(1, &gSilVbo);
    }
    glBindVertexArray(gSilVao);
    glBindBuffer(GL_ARRAY_BUFFER, gSilVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(gSilVerts.size() * sizeof(float)),
                 gSilVerts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 16, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 16, (void*)12);
    glBindVertexArray(0);
}

// Read an island's heightmap and lay down a coarse mesh of it at its
// place on the chart. Cheap: a 28x28 grid regardless of how detailed the
// island really is, which is plenty at the range these are seen from.
static void bake_silhouette(const ChartIsle& c, ProxyIsle& pr)
{
    FILE* f = fopen(c.path.c_str(), "rb");
    if (!f)
        return;
    char mg[8];
    float half = 24.0f;
    Uint32 res[3] = { 257, 512, 256 };
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, "TERMAP0", 7) != 0) {
        fclose(f);
        return;
    }
    int hn = 257;
    if (mg[7] >= '8') {
        if (fread(&half, 4, 1, f) != 1 || fread(res, 4, 3, f) != 3) {
            fclose(f);
            return;
        }
        hn = (int)res[0];
    }
    std::vector<float> h((size_t)hn * hn, 0.0f);
    const size_t got = fread(h.data(), sizeof(float), h.size(), f);
    fclose(f);
    if (got != h.size())
        return;

    // Resolution follows the island: a 463-unit island needs far more
    // than a 48-unit one to read as itself. Still tiny next to the real
    // terrain -- a few thousand triangles against hundreds of thousands.
    const int G = (int)SDL_clamp(half * 2.0f * gScale / 5.0f, 48.0f, 192.0f);
    std::vector<float> g((size_t)G * G, 0.0f);
    for (int j = 0; j < G; j++)
        for (int i = 0; i < G; i++) {
            // take the max of each patch: a silhouette should keep peaks
            int i0 = i * (hn - 1) / G, i1 = (i + 1) * (hn - 1) / G;
            int j0 = j * (hn - 1) / G, j1 = (j + 1) * (hn - 1) / G;
            float m = -1e9f;
            for (int jj = j0; jj <= j1; jj++)
                for (int ii = i0; ii <= i1; ii++)
                    m = SDL_max(m, h[(size_t)jj * hn + ii]);
            g[(size_t)j * G + i] = m;
        }

    float top = 0.0f;
    for (float v : g)
        top = SDL_max(top, v);
    pr.height = SDL_max(1.0f, top * gScale);
    pr.radius = half * gScale;

    const float cell = 2.0f * half / (G - 1);
    auto put = [&](int i, int j) {
        const float lx = -half + i * cell, lz = -half + j * cell;
        float y = g[(size_t)j * G + i] * gScale + gYOff;
        if (y < gSeaLevel)
            y = gSeaLevel;          // land meets water, never below it
        const float up = SDL_clamp((y - gSeaLevel) / pr.height, 0.0f, 1.0f);
        gSilVerts.insert(gSilVerts.end(),
                         { pr.x + lx * gScale, y, pr.z + lz * gScale, up });
    };
    pr.first = (int)(gSilVerts.size() / 4);
    for (int j = 0; j < G - 1; j++)
        for (int i = 0; i < G - 1; i++) {
            put(i, j); put(i + 1, j); put(i, j + 1);
            put(i + 1, j); put(i + 1, j + 1); put(i, j + 1);
        }
    pr.count = (int)(gSilVerts.size() / 4) - pr.first;
}

bool wmap_stream(float px, float pz)
{
    if (!gActive || gChart.size() < 2)
        return false;
    // nearest charted island wins, once we are properly inside its quadrant
    int best = -1;
    float bestD2 = 1e30f;
    for (int i = 0; i < (int)gChart.size(); i++) {
        float wx, wz;
        wmap_cell_center(gChart[i].cx, gChart[i].cy, &wx, &wz);
        const float dx = wx - px, dz = wz - pz;
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    if (best < 0)
        return false;
    const ChartIsle& c = gChart[best];
    if (c.cx == gLoadedCell[0] && c.cy == gLoadedCell[1])
        return false;
    // Reach most of the way to the next quadrant. Held at 0.75 the real
    // island only appeared once you were nearly on top of it, so an
    // island you were flying straight at stayed a silhouette far longer
    // than it should have.
    if (bestD2 > (gQuadSize * 1.25f) * (gQuadSize * 1.25f))
        return false;              // still out at sea between islands
    // Hold on to the island being left until it is genuinely far off.
    // Only one island can be resident, so a swap replaces the real
    // terrain behind you with its silhouette -- do that while it still
    // fills the screen and it reads as the island changing shape. Kept
    // until it is a quadrant away, it has already hazed down to a
    // smudge on the horizon by the time the stand-in takes over.
    if (gLoadedCell[0] > -50) {
        float lx, lz;
        wmap_cell_center(gLoadedCell[0], gLoadedCell[1], &lx, &lz);
        const float ldx = lx - px, ldz = lz - pz;
        const float curD2 = ldx * ldx + ldz * ldz;
        // never swap for a marginal gain: midway between two islands
        // the nearest flips back and forth, and each flip is a full reload
        if (bestD2 > curD2 * 0.7f)
            return false;
    }
    SDL_Log("wmap: streaming in %c%d", 'A' + c.cx, c.cy + 1);
    free_island_gl();
    if (!load_island_at(c.cx, c.cy, c.path))
        return false;
    build_island_gl();
    return true;
}

bool wmap_active() { return gActive; }
const WmapHeights& wmap_heights() { return gOut; }
void wmap_island_center(float* x, float* z)
{
    *x = gCenter[0];
    *z = gCenter[1];
}

void wmap_cell_center(int cx, int cy, float* x, float* z)
{
    *x = (cx - (gChartSize - 1) * 0.5f) * gQuadSize;
    *z = (cy - (gChartSize - 1) * 0.5f) * gQuadSize;
}

void wmap_quadrant_center(float wx, float wz, float* x, float* z)
{
    const float quad = gQuadSize;
    const float off = (gChartSize - 1) * 0.5f * quad;
    // chart cells are laid out on a fixed grid: snap to the nearest one
    *x = roundf((wx + off) / quad) * quad - off;
    *z = roundf((wz + off) / quad) * quad - off;
}

float wmap_block_height(float wx, float wz)
{
    if (!gActive)
        return -1000.0f;
    const float lx = wx - gCenter[0], lz = wz - gCenter[1];
    // the skirt flares outward from the rim: treat a margin past the shore
    // as solid so a camera dropping below the rim is pushed back in
    const float margin = TER_HALF * 0.07f;   // just the overhang, not the bay
    if (fabsf(lx) > TER_HALF + margin || fabsf(lz) > TER_HALF + margin)
        return -1000.0f;
    const float cx = SDL_clamp(lx, -TER_HALF, TER_HALF);
    const float cz = SDL_clamp(lz, -TER_HALF, TER_HALF);
    return height_at(cx, cz) + gYOff;
}

int wmap_shadow_discs(float* out4, int maxCount)
{
    if (!gActive)
        return 0;
    int n = 0;
    auto add = [&](float x, float z, float r, float top) {
        if (n >= maxCount) return;
        out4[n * 4 + 0] = x;
        out4[n * 4 + 1] = z;
        out4[n * 4 + 2] = r;
        out4[n * 4 + 3] = top;
        ++n;
    };
    // the loaded island: its own marched shadow is exact up close, but a
    // disc still carries it once the sea is hazy and far away
    // its own radius, not the map's: a small island in a large quadrant was
    // shading the sea for the whole quadrant
    add(gCenter[0], gCenter[1],
        gIslandRadius > 1.0f ? gIslandRadius * 1.05f : TER_HALF * 0.95f,
        gIslandTop);
    if (gTestCell[0] >= 0) {
        float tx, tz;
        wmap_cell_center(gTestCell[0], gTestCell[1], &tx, &tz);
        add(tx, tz, gTestRadius, gTestTop);
    }
    for (const ProxyIsle& pr : gProxies)
        add(pr.x, pr.z, pr.radius * 0.85f, gSeaLevel + pr.height);
    return n;
}

bool wmap_flight_ring(float wx, float wz, float* radius, float* height)
{
    if (!gActive)
        return false;
    float qx = 0.0f, qz = 0.0f;
    wmap_quadrant_center(wx, wz, &qx, &qz);
    if (fabsf(qx - gCenter[0]) > 1.0f || fabsf(qz - gCenter[1]) > 1.0f)
        return false;               // not the island's quadrant
    *radius = TER_HALF * 1.35f;     // outside the shore
    *height = gIslandTop + 14.0f;   // above the peaks and their trees
    return true;
}

// ---- chart readout, for the on-screen map
int wmap_chart_size() { return gChartSize; }
float wmap_quad_size() { return gQuadSize; }

int wmap_chart_cells(int* out2, int maxCount)
{
    int n = 0;
    for (const ChartIsle& c : gChart) {
        if (n >= maxCount)
            break;
        out2[n * 2 + 0] = c.cx;
        out2[n * 2 + 1] = c.cy;
        ++n;
    }
    return n;
}

bool wmap_spawn_cell(int* cx, int* cy)
{
    if (gSpawnCell[0] < 0)
        return false;
    *cx = gSpawnCell[0];
    *cy = gSpawnCell[1];
    return true;
}

void wmap_loaded_cell(int* cx, int* cy)
{
    *cx = gLoadedCell[0];
    *cy = gLoadedCell[1];
}

// where the chart says the game should start, if it says at all
// the height the chart asks for at a world position, or 0 for the default
float wmap_wind_height(float wx, float wz)
{
    if (!gActive)
        return 0.0f;
    const float off = (gChartSize - 1) * 0.5f;
    const int cx = (int)(wx / gQuadSize + off + 0.5f);
    const int cy = (int)(wz / gQuadSize + off + 0.5f);
    if (cx < 0 || cy < 0 || cx >= 8 || cy >= 8)
        return 0.0f;
    return gWindH[cy][cx];
}

bool wmap_spawn_center(float* x, float* z)
{
    if (gSpawnCell[0] < 0)
        return false;
    wmap_cell_center(gSpawnCell[0], gSpawnCell[1], x, z);
    return true;
}

// True when a charted island already occupies that quadrant: the
// built-in test island has to stand down rather than sit inside it.
static bool cell_is_charted(int cx, int cy)
{
    for (const ChartIsle& c : gChart)
        if (c.cx == cx && c.cy == cy)
            return true;
    return false;
}

bool wmap_test_island(float* x, float* z)
{
    if (!gActive || gTestCell[0] < 0)
        return false;
    // a charted island in that quadrant takes precedence -- otherwise
    // the two occupy the same water and read as one broken island
    if (cell_is_charted(gTestCell[0], gTestCell[1]))
        return false;
    wmap_cell_center(gTestCell[0], gTestCell[1], x, z);
    return true;
}

void wmap_init_gl()
{
    if (!gActive)
        return;
    gTerProg = compile_prog(kTerVS, terFS().c_str());
    gGrassProg = compile_prog(kGrassVS, kGrassFS);
    gPropProg = compile_prog(kPropVS, propFS().c_str());
    gSkirtProg = compile_prog(kSkirtVS, kSkirtFS);
    gDepthProg = compile_prog(kDepthVS, kDepthFS);
    gDepthPropProg = compile_prog(kDepthPropVS, kDepthPropFS);

    gGrassTex = tex_from_bmp(gAssetsDir + "/grass.bmp");
    gDirtTex = tex_from_bmp(gAssetsDir + "/dirt.bmp");
    gDirt2Tex = tex_from_bmp(gAssetsDir + "/dirt2.bmp");
    gCliffTex = tex_from_bmp(gAssetsDir + "/cliff.bmp");

    build_island_gl();
}

// GL that belongs to one island: rebuilt whenever we stream a new one in
void build_island_gl()
{
    auto mask_tex = [](const std::vector<Uint8>& m) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, MASK_N, MASK_N, 0, GL_RED,
                     GL_UNSIGNED_BYTE, m.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    };
    gMaskTex = mask_tex(gMask);
    gMask2Tex = mask_tex(gMask2);

    // baked ground-AO image from blade roots (mip level = radius)
    {
        std::vector<Uint8> ao((size_t)AO_N * AO_N, 255);
        for (const BladeInst& b : gBlades) {
            int cx = (int)((b.x - gCenter[0] + TER_HALF) /
                           (2 * TER_HALF) * AO_N);
            int cy = (int)((b.z - gCenter[1] + TER_HALF) /
                           (2 * TER_HALF) * AO_N);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int px = cx + dx, py = cy + dy;
                    if (px < 0 || py < 0 || px >= AO_N || py >= AO_N)
                        continue;
                    ao[(size_t)py * AO_N + px] = 0;
                }
        }
        glGenTextures(1, &gAOTex);
        glBindTexture(GL_TEXTURE_2D, gAOTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, AO_N, AO_N, 0, GL_RED,
                     GL_UNSIGNED_BYTE, ao.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // terrain mesh: positions + normals in world space (yOff applied)
    {
        std::vector<float> v;
        v.reserve((size_t)HN * HN * 6);
        const float cell = 2.0f * TER_HALF / (HN - 1);
        for (int j = 0; j < HN; j++)
            for (int i = 0; i < HN; i++) {
                float x = -TER_HALF + cell * i + gCenter[0];
                float z = -TER_HALF + cell * j + gCenter[1];
                float h = gHeights[j * HN + i] * gScale + gYOff;
                float hx1 = gHeights[j * HN + SDL_min(i + 1, HN - 1)] * gScale;
                float hx0 = gHeights[j * HN + SDL_max(i - 1, 0)] * gScale;
                float hz1 = gHeights[SDL_min(j + 1, HN - 1) * HN + i] * gScale;
                float hz0 = gHeights[SDL_max(j - 1, 0) * HN + i] * gScale;
                float nx = hx0 - hx1, nz = hz0 - hz1, ny = 2.0f * cell;
                float nl = sqrtf(nx * nx + ny * ny + nz * nz);
                v.insert(v.end(), { x, h, z, nx / nl, ny / nl, nz / nl });
            }
        std::vector<unsigned> idx;
        idx.reserve((size_t)(HN - 1) * (HN - 1) * 6);
        for (int j = 0; j < HN - 1; j++)
            for (int i = 0; i < HN - 1; i++) {
                unsigned a = j * HN + i, b = a + 1, c = a + HN, d = c + 1;
                idx.insert(idx.end(), { a, c, b, b, c, d });
            }
        gTerIdx = (GLsizei)idx.size();
        glGenVertexArrays(1, &gTerVao);
        glGenBuffers(1, &gTerVbo);
        glGenBuffers(1, &gTerIbo);
        glBindVertexArray(gTerVao);
        glBindBuffer(GL_ARRAY_BUFFER, gTerVbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gTerIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned),
                     idx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
        glBindVertexArray(0);
    }

    // grass: blade template + instance buffer
    {
        const float blade[12][3] = {
            { -1, 0, 0 }, { 1, 0, 0 }, { -1, 1, 0 },
            { 1, 0, 0 }, { 1, 1, 0 }, { -1, 1, 0 },
            { -1, 0, 1 }, { 1, 0, 1 }, { -1, 1, 1 },
            { 1, 0, 1 }, { 1, 1, 1 }, { -1, 1, 1 },
        };
        std::vector<float> inst;
        inst.reserve(gBlades.size() * 5);
        for (const BladeInst& b : gBlades)
            inst.insert(inst.end(), { b.x, b.y, b.z, b.rot, b.seed });
        gBladeCount = (GLsizei)gBlades.size();
        GLuint bladeVbo = 0, instVbo = 0;
        glGenVertexArrays(1, &gGrassVao);
        glGenBuffers(1, &bladeVbo);
        glGenBuffers(1, &instVbo);
        glBindVertexArray(gGrassVao);
        glBindBuffer(GL_ARRAY_BUFFER, bladeVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof blade, blade, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(float),
                     inst.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 20, (void*)0);
        glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 20, (void*)12);
        glVertexAttribDivisor(2, 1);
        glBindVertexArray(0);
    }

    // skirt ring
    {
        // ring in EDITOR space; the shader multiplies by uScale
        const int N = 128;
        const float E = gEditorHalf;
        std::vector<float> ring;
        gSkirtPivot[0] = gSkirtPivot[1] = 0.0f;
        bool trimmed = false;
        if (gTune.trimSkirt) {
            // Same radial contour the editor traces: hang the underside
            // off the island's coastline so its shape below the water
            // matches the shape above it.
            const float cell = 2.0f * E / (HN - 1);
            const float sea = gTune.waterline + 0.05f;
            double cx = 0, cz = 0;
            int n = 0;
            for (int j = 0; j < HN; j++)
                for (int i = 0; i < HN; i++)
                    if (gHeights[(size_t)j * HN + i] > sea) {
                        cx += -E + i * cell;
                        cz += -E + j * cell;
                        n++;
                    }
            if (n > 32) {
                cx /= n; cz /= n;
                const int P2 = 256;
                std::vector<float> rad((size_t)P2, 0.0f);
                for (int p = 0; p < P2; p++) {
                    float a = 6.2831853f * p / P2;
                    float dx = cosf(a), dz = sinf(a);
                    float found = 0.0f;
                    for (float r = 0.0f; r < E * 2.2f; r += cell * 0.75f) {
                        float x = (float)cx + dx * r, z = (float)cz + dz * r;
                        if (fabsf(x) > E || fabsf(z) > E)
                            break;
                        // height_at takes world units and returns world
                        if (height_at(x * gScale, z * gScale) / gScale > sea)
                            found = r;
                    }
                    rad[p] = found;
                }
                for (int pass = 0; pass < 2; pass++) {
                    std::vector<float> t = rad;
                    for (int p = 0; p < P2; p++)
                        rad[p] = (t[(p + P2 - 1) % P2] + 2.0f * t[p] +
                                  t[(p + 1) % P2]) * 0.25f;
                }
                for (int p = 0; p < P2; p++) {
                    float a = 6.2831853f * p / P2;
                    float r = rad[p] + cell * 1.5f;
                    ring.insert(ring.end(), {
                        SDL_clamp((float)cx + cosf(a) * r, -E, E),
                        SDL_clamp((float)cz + sinf(a) * r, -E, E) });
                }
                gSkirtPivot[0] = (float)cx;
                gSkirtPivot[1] = (float)cz;
                trimmed = true;
            }
        }
        if (!trimmed) {
            for (int i = 0; i < N; i++)
                ring.insert(ring.end(), { -E + 2.0f * E * i / N, -E });
            for (int j = 0; j < N; j++)
                ring.insert(ring.end(), { E, -E + 2.0f * E * j / N });
            for (int i = N; i > 0; i--)
                ring.insert(ring.end(), { -E + 2.0f * E * i / N, E });
            for (int j = N; j > 0; j--)
                ring.insert(ring.end(), { -E, -E + 2.0f * E * j / N });
        }
        int P = (int)ring.size() / 2;
        std::vector<float> sv;
        for (int p = 0; p < P; p++) {
            sv.insert(sv.end(), { ring[p * 2], ring[p * 2 + 1], 0.0f });
            sv.insert(sv.end(), { ring[p * 2], ring[p * 2 + 1], 1.0f });
        }
        int centerIdx = P * 2;
        sv.insert(sv.end(), { gSkirtPivot[0], gSkirtPivot[1], 2.0f });
        std::vector<unsigned> si;
        for (int p = 0; p < P; p++) {
            unsigned a = p * 2, b = a + 1;
            unsigned c = ((p + 1) % P) * 2, d = c + 1;
            si.insert(si.end(), { a, b, c, c, b, d });
            si.insert(si.end(), { b, (unsigned)centerIdx, d });
        }
        gSkirtIdx = (GLsizei)si.size();
        GLuint vbo = 0, ibo = 0;
        glGenVertexArrays(1, &gSkirtVao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
        glBindVertexArray(gSkirtVao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sv.size() * sizeof(float), sv.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, si.size() * sizeof(unsigned),
                     si.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }

    // silhouette proxy: a radial disc the shader shapes per island
    if (!gProxyProg) {
        gProxyProg = compile_prog(kProxyVS, kProxyFS);
        const int RINGS = 10, SEG = 40;
        std::vector<float> pv;
        for (int r = 0; r <= RINGS; r++)
            for (int s = 0; s < SEG; s++)
                pv.insert(pv.end(), { (float)r / RINGS,
                                      6.2831853f * s / SEG });
        std::vector<unsigned> pi;
        for (int r = 0; r < RINGS; r++)
            for (int s = 0; s < SEG; s++) {
                unsigned a = r * SEG + s, b = r * SEG + (s + 1) % SEG;
                unsigned c = a + SEG, d = b + SEG;
                pi.insert(pi.end(), { a, c, b, b, c, d });
            }
        gProxyIdx = (GLsizei)pi.size();
        GLuint vbo = 0, ibo = 0;
        glGenVertexArrays(1, &gProxyVao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
        glBindVertexArray(gProxyVao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, pv.size() * sizeof(float), pv.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, pi.size() * sizeof(unsigned),
                     pi.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }

    // prop meshes used by instances
    int okMeshes = 0, badMeshes = 0;
    for (const PropInst& pi : gProps) {
        PropMesh& pm = gMeshes[pi.mesh];
        if (pm.loaded)
            continue;
        if (load_prop_mesh(pm))
            okMeshes++;
        else {
            badMeshes++;
            if (badMeshes == 1)
                SDL_Log("wmap: prop mesh failed to load: '%s'",
                        pm.id.c_str());
        }
    }
    SDL_Log("wmap: prop meshes loaded %d, failed %d", okMeshes, badMeshes);

    // shift prop instance Y by yOff once
    for (PropInst& pi : gProps)
        pi.y += gYOff;
}


void wmap_draw_shadow(const Mat4& lightVP)
{
    if (!gActive)
        return;
    // ground that is not drawn must not cast either: a props-only island
    // was throwing the shadow of a footprint nobody can see
    if (!gPropsOnly) {
        glUseProgram(gDepthProg);
        glUniformMatrix4fv(glGetUniformLocation(gDepthProg, "uLightVP"), 1,
                           GL_FALSE, lightVP.m);
        glBindVertexArray(gTerVao);
        glDrawElements(GL_TRIANGLES, gTerIdx, GL_UNSIGNED_INT, nullptr);
        // the island streamed out of is still drawn, so it still casts
        if (gPrev.vao) {
            glBindVertexArray(gPrev.vao);
            glDrawElements(GL_TRIANGLES, gPrev.idx, GL_UNSIGNED_INT, nullptr);
        }
    }
    // ...and so do its props: without this an island keeps its terrain, its
    // grass and its trees the moment it stops being resident, but loses
    // every shadow they cast.
    std::vector<PropInst> castProps = gProps;
    if (gPrev.vao && !gPrev.props.empty())
        castProps.insert(castProps.end(), gPrev.props.begin(),
                         gPrev.props.end());
    if (!castProps.empty()) {
        glUseProgram(gDepthPropProg);
        glUniformMatrix4fv(glGetUniformLocation(gDepthPropProg, "uLightVP"),
                           1, GL_FALSE, lightVP.m);
        glUniform1i(glGetUniformLocation(gDepthPropProg, "uTex"), 0);
        glUniform1f(glGetUniformLocation(gDepthPropProg, "uTime"), gTime);
        GLint dModel = glGetUniformLocation(gDepthPropProg, "uModel");
        GLint dHas = glGetUniformLocation(gDepthPropProg, "uHasTex");
        GLint dGray = glGetUniformLocation(gDepthPropProg, "uGrayMask");
        glActiveTexture(GL_TEXTURE0);
        for (const PropInst& inst : castProps) {
            PropMesh& pm = gMeshes[inst.mesh];
            if (!pm.loaded)
                continue;
            float c = cosf(inst.yaw), s = sinf(inst.yaw), sc = inst.scale;
            Mat4 mdl{};
            mdl.m[0] = c * sc;  mdl.m[2] = -s * sc;
            mdl.m[5] = sc;
            mdl.m[8] = s * sc;  mdl.m[10] = c * sc;
            mdl.m[12] = inst.x; mdl.m[13] = inst.y; mdl.m[14] = inst.z;
            mdl.m[15] = 1.0f;
            glUniformMatrix4fv(dModel, 1, GL_FALSE, mdl.m);
            glUniform1f(glGetUniformLocation(gDepthPropProg, "uBoundH"),
                        pm.boundH);
            glBindVertexArray(pm.vao);
            for (const PropSub& sub : pm.subs) {
                const PropMat& mat = pm.mats[sub.mat];
                glUniform1i(dHas, mat.tex ? 1 : 0);
                glUniform1i(dGray, mat.gray ? 1 : 0);
                if (mat.tex)
                    glBindTexture(GL_TEXTURE_2D, mat.tex);
                glDrawArrays(GL_TRIANGLES, sub.first, sub.count);
            }
        }
    }
}

void wmap_draw(const Mat4& viewProj, const Vec3& eye, const Mat4& lightVP,
               GLuint shadowTex, float timeSec, const Mat4& lightVPFar,
               GLuint shadowTexFar)
{
    if (!gActive)
        return;
    // lazy: skirt height texture on first draw
    if (!gSkirtHeightTex) {
        glGenTextures(1, &gSkirtHeightTex);
        glBindTexture(GL_TEXTURE_2D, gSkirtHeightTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, HN, HN, 0, GL_RED, GL_FLOAT,
                     gHeights.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gTime = timeSec;
    const float eyeA[3] = { eye.x, eye.y, eye.z };
    const float center[2] = { gCenter[0], gCenter[1] };

    // distant island silhouettes on the horizon
    if (gProxyProg && gSilVao && !gProxies.empty()) {
        glUseProgram(gProxyProg);
        glUniformMatrix4fv(glGetUniformLocation(gProxyProg, "uViewProj"), 1,
                           GL_FALSE, viewProj.m);
        glUniform3fv(glGetUniformLocation(gProxyProg, "uEye"), 1, eyeA);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(gSilVao);
        for (const ProxyIsle& pr : gProxies) {
            if (pr.count <= 0)
                continue;
            const float dx = pr.x - eye.x, dz = pr.z - eye.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 > 1600.0f * 1600.0f)
                continue;
            // the streamed island takes over well before this
            if (d2 < (pr.radius * 1.5f) * (pr.radius * 1.5f))
                continue;
            // the island we just left is still being drawn for real
            if (gPrev.vao && gPrev.cx == pr.cx && gPrev.cy == pr.cy)
                continue;
            glDrawArrays(GL_TRIANGLES, pr.first, pr.count);
        }
        glBindVertexArray(0);
        glEnable(GL_CULL_FACE);
    }

    auto bindT = [&](GLuint prog, const char* name, int unit, GLuint tex) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(prog, name), unit);
    };
    // terrain -- nothing to draw when the island is its props
    if (!gPropsOnly) {
    glUseProgram(gTerProg);
    glUniformMatrix4fv(glGetUniformLocation(gTerProg, "uViewProj"), 1,
                       GL_FALSE, viewProj.m);
    glUniformMatrix4fv(glGetUniformLocation(gTerProg, "uLightVP"), 1,
                       GL_FALSE, lightVP.m);
    glUniform3fv(glGetUniformLocation(gTerProg, "uEye"), 1, eyeA);
    glUniform2fv(glGetUniformLocation(gTerProg, "uCenter"), 1, center);
    glUniform1f(glGetUniformLocation(gTerProg, "uHalf"), TER_HALF);
    glUniform1f(glGetUniformLocation(gTerProg, "uEdgeBreak"), gTune.edgeBreak);
    glUniform1f(glGetUniformLocation(gTerProg, "uScale"), gScale);
    glUniform1f(glGetUniformLocation(gTerProg, "uEditorHalf"), gEditorHalf);
    glUniform1f(glGetUniformLocation(gTerProg, "uGrassAO"), gTune.groundAO);
    glUniform1f(glGetUniformLocation(gTerProg, "uGrassAORad"), gTune.aoRadius);
    bindT(gTerProg, "uMask", 0, gMaskTex);
    bindT(gTerProg, "uMask2", 1, gMask2Tex);
    bindT(gTerProg, "uGrassTex", 2, gGrassTex);
    bindT(gTerProg, "uDirtTex", 3, gDirtTex);
    bindT(gTerProg, "uDirt2Tex", 4, gDirt2Tex);
    bindT(gTerProg, "uCliffTex", 5, gCliffTex);
    bindT(gTerProg, "uAOMap", 6, gAOTex);
    bindT(gTerProg, "uShadow", 7, shadowTex);
    glUniformMatrix4fv(glGetUniformLocation(gTerProg, "uLightVP2"), 1,
                       GL_FALSE, lightVPFar.m);
    bindT(gTerProg, "uShadow2", 8, shadowTexFar);
    glBindVertexArray(gTerVao);
    glDrawElements(GL_TRIANGLES, gTerIdx, GL_UNSIGNED_INT, nullptr);

    // the island last streamed out, still the real terrain, until it is
    // far enough that its silhouette can take over unnoticed
    if (gPrev.vao) {
        const float pdx = gPrev.center[0] - eye.x;
        const float pdz = gPrev.center[1] - eye.z;
        if (pdx * pdx + pdz * pdz > 2200.0f * 2200.0f) {
            release_retained();
        } else {
            glUniform2fv(glGetUniformLocation(gTerProg, "uCenter"), 1,
                         gPrev.center);
            glUniform1f(glGetUniformLocation(gTerProg, "uHalf"),
                        gPrev.terHalf);
            glUniform1f(glGetUniformLocation(gTerProg, "uEditorHalf"),
                        gPrev.editorHalf);
            bindT(gTerProg, "uMask", 0, gPrev.maskTex);
            bindT(gTerProg, "uMask2", 1, gPrev.mask2Tex);
            bindT(gTerProg, "uAOMap", 6, gPrev.aoTex);
            glBindVertexArray(gPrev.vao);
            glDrawElements(GL_TRIANGLES, gPrev.idx, GL_UNSIGNED_INT, nullptr);
            // put the resident island's own bindings back
            glUniform2fv(glGetUniformLocation(gTerProg, "uCenter"), 1, center);
            glUniform1f(glGetUniformLocation(gTerProg, "uHalf"), TER_HALF);
            glUniform1f(glGetUniformLocation(gTerProg, "uEditorHalf"),
                        gEditorHalf);
            bindT(gTerProg, "uMask", 0, gMaskTex);
            bindT(gTerProg, "uMask2", 1, gMask2Tex);
            bindT(gTerProg, "uAOMap", 6, gAOTex);
        }
    }

    // skirt
    }   // !gPropsOnly: no ground was drawn
    if (!gPropsOnly && gTune.islandDepth > 0.05f) {
        glUseProgram(gSkirtProg);
        glUniformMatrix4fv(glGetUniformLocation(gSkirtProg, "uViewProj"), 1,
                           GL_FALSE, viewProj.m);
        glUniform2fv(glGetUniformLocation(gSkirtProg, "uCenter"), 1, center);
        glUniform1f(glGetUniformLocation(gSkirtProg, "uYOff"), gYOff);
        glUniform1f(glGetUniformLocation(gSkirtProg, "uHalf"), TER_HALF);
        glUniform1f(glGetUniformLocation(gSkirtProg, "uScale"), gScale);
        glUniform1f(glGetUniformLocation(gSkirtProg, "uEditorHalf"), gEditorHalf);
        glUniform1f(glGetUniformLocation(gSkirtProg, "uDepth"),
                    gTune.islandDepth);
        glUniform1f(glGetUniformLocation(gSkirtProg, "uFrill"),
                    gTune.islandFrill);
        glUniform1f(glGetUniformLocation(gSkirtProg, "uBulge"),
                    gTune.islandBulge);
        glUniform2f(glGetUniformLocation(gSkirtProg, "uPivot"),
                    gSkirtPivot[0], gSkirtPivot[1]);
        glUniform3fv(glGetUniformLocation(gSkirtProg, "uEye"), 1, eyeA);
        bindT(gSkirtProg, "uHeight", 0, gSkirtHeightTex);
        bindT(gSkirtProg, "uCliffTex", 1, gCliffTex);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(gSkirtVao);
        glDrawElements(GL_TRIANGLES, gSkirtIdx, GL_UNSIGNED_INT, nullptr);
    }

    // props -- the retained island's count too, since the island just
    // streamed in may have none of its own and the whole block would be
    // skipped, taking the retained island's trees with it
    if (!gProps.empty() || (gPrev.vao && !gPrev.props.empty())) {
        glUseProgram(gPropProg);
        glUniformMatrix4fv(glGetUniformLocation(gPropProg, "uViewProj"), 1,
                           GL_FALSE, viewProj.m);
        glUniformMatrix4fv(glGetUniformLocation(gPropProg, "uLightVP"), 1,
                           GL_FALSE, lightVP.m);
        glUniform3fv(glGetUniformLocation(gPropProg, "uEye"), 1, eyeA);
        glUniform1f(glGetUniformLocation(gPropProg, "uTime"), timeSec);
        bindT(gPropProg, "uShadow", 5, shadowTex);
        glUniformMatrix4fv(glGetUniformLocation(gPropProg, "uLightVP2"), 1,
                           GL_FALSE, lightVPFar.m);
        bindT(gPropProg, "uShadow2", 8, shadowTexFar);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(gPropProg, "uTex"), 0);
        GLint locModel = glGetUniformLocation(gPropProg, "uModel");
        GLint locKd = glGetUniformLocation(gPropProg, "uKd");
        GLint locKa = glGetUniformLocation(gPropProg, "uKa");
        GLint locBH = glGetUniformLocation(gPropProg, "uBoundH");
        GLint locHas = glGetUniformLocation(gPropProg, "uHasTex");
        GLint locGray = glGetUniformLocation(gPropProg, "uGrayMask");
        glDisable(GL_CULL_FACE);
        // the island still being retained keeps its trees: its meshes are
        // already loaded and its instances are in world space
        std::vector<PropInst> drawProps = gProps;
        if (gPrev.vao && !gPrev.props.empty())
            drawProps.insert(drawProps.end(), gPrev.props.begin(),
                             gPrev.props.end());
        for (const PropInst& inst : drawProps) {
            PropMesh& pm = gMeshes[inst.mesh];
            if (!pm.loaded)
                continue;
            float c = cosf(inst.yaw), s = sinf(inst.yaw), sc = inst.scale;
            Mat4 mdl{};
            mdl.m[0] = c * sc;  mdl.m[2] = -s * sc;
            mdl.m[5] = sc;
            mdl.m[8] = s * sc;  mdl.m[10] = c * sc;
            mdl.m[12] = inst.x; mdl.m[13] = inst.y; mdl.m[14] = inst.z;
            mdl.m[15] = 1.0f;
            glUniformMatrix4fv(locModel, 1, GL_FALSE, mdl.m);
            glUniform1f(locBH, pm.boundH);
            glBindVertexArray(pm.vao);
            for (const PropSub& sub : pm.subs) {
                const PropMat& mat = pm.mats[sub.mat];
                glUniform3fv(locKd, 1, mat.kd);
                glUniform3fv(locKa, 1, mat.ka);
                glUniform1i(locHas, mat.tex ? 1 : 0);
                glUniform1i(locGray, mat.gray ? 1 : 0);
                if (mat.tex)
                    glBindTexture(GL_TEXTURE_2D, mat.tex);
                glDrawArrays(GL_TRIANGLES, sub.first, sub.count);
            }
        }
    }

    // grass -- likewise gated on either island having any
    if (gBladeCount > 0 || (gPrev.grassVao && gPrev.blades > 0)) {
        glUseProgram(gGrassProg);
        glUniformMatrix4fv(glGetUniformLocation(gGrassProg, "uViewProj"), 1,
                           GL_FALSE, viewProj.m);
        glUniformMatrix4fv(glGetUniformLocation(gGrassProg, "uLightVP"), 1,
                           GL_FALSE, lightVP.m);
        glUniform1f(glGetUniformLocation(gGrassProg, "uTime"), timeSec);
        glUniform2fv(glGetUniformLocation(gGrassProg, "uCenter"), 1, center);
        glUniform1f(glGetUniformLocation(gGrassProg, "uScale"), gScale);
        glUniform3fv(glGetUniformLocation(gGrassProg, "uPlayer"), 1, gPlayer);
        glUniform3fv(glGetUniformLocation(gGrassProg, "uEye"), 1, eyeA);
        bindT(gGrassProg, "uGrassTex", 0, gGrassTex);
        bindT(gGrassProg, "uShadow", 1, shadowTex);
        glDisable(GL_CULL_FACE);
        if (gGrassVao && gBladeCount > 0) {
            glBindVertexArray(gGrassVao);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 12, gBladeCount);
        }
        if (gPrev.grassVao && gPrev.blades > 0) {
            glUniform2fv(glGetUniformLocation(gGrassProg, "uCenter"), 1,
                         gPrev.center);
            glBindVertexArray(gPrev.grassVao);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 12, gPrev.blades);
        }
    }
    glBindVertexArray(0);
    // the client keeps the shadow map on unit 2 for the whole frame (the
    // sea samples it there); our material binds trampled it
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shadowTex);
    glActiveTexture(GL_TEXTURE0);
}
