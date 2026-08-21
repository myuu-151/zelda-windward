#include "dungeon.h"

#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include <SDL3/SDL.h>

namespace {

// ---- kit grid. Measured off the pieces themselves, not guessed: a wall is
// 4 wide and 4 tall on a 1-thick footprint, a floor tile is 4x4, and every
// piece has its base at y = 0.
constexpr float kTile = 4.0f;
constexpr float kWallTop = 4.0f;
constexpr int   kCourses = 2;    // one course is shorter than the camera,
                                 // so you see over it into nothing
constexpr int   kGridW = 7, kGridH = 7;
constexpr int   kSlot = 13;      // slot pitch in tiles: has to
                                 // clear the widest two rooms
                                 // that can end up adjacent
constexpr int   kPathLen = 9;
constexpr int   kBranches = 4;

// Room footprints in tiles -- odd only, so every room has a centre tile a
// doorway can sit on. Rolled per room rather than fixed per kind: a floor
// where every room is the same box reads as a spreadsheet.
int room_tiles(int kind, unsigned r)
{
    switch (kind) {
        case 0: return 7;                       // entrance
        case 2: return 7;                       // safe
        case 3: return (r % 2) ? 5 : 3;         // treasure: a nook or a room
        case 4: return (r % 3) ? 11 : 9;        // boss: the big one
        default: {                              // normal: mostly mid, some
            const int roll = (int)(r % 10);     // small, some halls
            if (roll < 2) return 5;
            if (roll < 7) return 7;
            return 9;
        }
    }
}
enum { KIND_ENTRANCE = 0, KIND_NORMAL, KIND_SAFE, KIND_TREASURE, KIND_BOSS };

struct Cell {
    int x, y;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y; }
};
struct CellHash {
    size_t operator()(const Cell& c) const {
        return (size_t)(c.x * 73856093) ^ (size_t)(c.y * 19349663);
    }
};

const int kDirX[4] = { 0, 0, 1, -1 };   // N S E W
const int kDirY[4] = { 1, -1, 0, 0 };

// A wall spans its local X and is 1 thick in Z, so a wall running along Z
// wants a quarter turn. These are the yaws that put a wall on each side of a
// room, and the yaw that faces a wall-mounted piece into the room.
const float kSideYaw[4]  = { 0.0f, 3.14159265f, 1.57079633f, 4.71238898f };

// A wall-mounted piece protrudes along its local +Z once glTF has turned
// Blender's -Y into +Z, and the instance transform sends local +Z to
// (sin yaw, cos yaw) in world XZ. So the yaw that points one into the room
// off each wall is the one solving (sin, cos) = -side direction.
const float kInnerYaw[4] = { 3.14159265f,   // N wall: face -Z
                             0.0f,          // S wall: face +Z
                             4.71238898f,   // E wall: face -X
                             1.57079633f }; // W wall: face +X

// The bracket's back plane sits on its origin, and a wall is 1 thick and
// centred on the tile edge -- so mounting it on the edge buries it half a
// wall deep. Push it out to the inner face.
constexpr float kMountOut = 0.5f;

struct Rng {
    unsigned s;
    explicit Rng(unsigned seed) : s(seed ? seed : 1u) {}
    unsigned next() {                      // xorshift32
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    int range(int n) { return n > 0 ? (int)(next() % (unsigned)n) : 0; }
    float unit() { return (float)(next() % 100000u) / 100000.0f; }
    float between(float a, float b) { return a + (b - a) * unit(); }
};

struct Builder {
    DungeonFloor* out;
    float ox, oy, oz;

    void put(const char* name, float tx, float ty, float y, float yaw) {
        DungeonPiece p;
        p.id = std::string("Dungeon/") + name;
        p.x = ox + tx * kTile;
        p.z = oz + ty * kTile;
        p.y = oy + y;
        p.yaw = yaw;
        out->pieces.push_back(p);
    }
};

// Flat variants only. floor_tile_large_rocks stands 0.54 above the floor --
// that is knee height, so it is an obstacle, not a texture, and scattering it
// at random walls off doorways and snags him mid-room. floor_dirt_large's
// 0.11 lip is small enough to walk over, and the grate's top is flush (it
// recesses downward).
const char* floor_variant(Rng& r)
{
    int v = r.range(100);
    if (v < 68) return "floor_tile_large";
    if (v < 88) return "floor_dirt_large";
    return "floor_tile_big_grate";
}

// Perimeter variants must be SOLID. wall_arched and wall_broken are
// openings -- dropped in at random they punch doors that lead nowhere.
const char* wall_variant(Rng& r)
{
    int v = r.range(100);
    if (v < 68) return "wall";
    if (v < 82) return "wall_cracked";
    if (v < 88) return "wall_pillar";
    if (v < 94) return "wall_shelves";
    return "wall_scaffold";
}

}  // namespace

void dungeon_generate(unsigned seed, float ox, float oy, float oz,
                      DungeonFloor* out)
{
    out->pieces.clear();
    out->flames.clear();
    if (!seed)
        seed = (unsigned)SDL_GetTicks() * 2654435761u + 1u;
    Rng rng(seed);

    // ---------------------------------------------------------- carve
    std::vector<Cell> path;
    std::unordered_set<Cell, CellHash> visited;
    Cell start{ rng.range(kGridW), 0 };
    path.push_back(start);
    visited.insert(start);

    while ((int)path.size() < kPathLen) {
        Cell cur = path.back();
        int opts[16], n = 0;
        for (int d = 0; d < 4; d++) {
            Cell nb{ cur.x + kDirX[d], cur.y + kDirY[d] };
            if (nb.x < 0 || nb.x >= kGridW || nb.y < 0 || nb.y >= kGridH)
                continue;
            if (visited.count(nb))
                continue;
            // bias north so a floor reads as a journey, not a puddle
            int w = (d == 0) ? 3 : 1;
            for (int k = 0; k < w && n < 16; k++)
                opts[n++] = d;
        }
        if (!n) {
            if (path.size() == 1)
                break;
            path.pop_back();      // backtrack
            continue;
        }
        int d = opts[rng.range(n)];
        Cell nb{ cur.x + kDirX[d], cur.y + kDirY[d] };
        path.push_back(nb);
        visited.insert(nb);
    }

    std::unordered_map<Cell, int, CellHash> kinds;
    for (const Cell& c : path)
        kinds[c] = KIND_NORMAL;
    kinds[path.front()] = KIND_ENTRANCE;
    kinds[path.back()] = KIND_BOSS;
    if (path.size() > 3)
        kinds[path[path.size() / 2]] = KIND_SAFE;

    // links, as a set of ordered cell pairs
    std::vector<std::pair<Cell, Cell>> links;
    for (size_t i = 1; i < path.size(); i++)
        links.push_back({ path[i - 1], path[i] });

    for (int b = 0; b < kBranches; b++) {
        if (path.size() < 3)
            break;
        Cell anchor = path[1 + rng.range((int)path.size() - 2)];
        int opts[4], n = 0;
        for (int d = 0; d < 4; d++) {
            Cell nb{ anchor.x + kDirX[d], anchor.y + kDirY[d] };
            if (nb.x < 0 || nb.x >= kGridW || nb.y < 0 || nb.y >= kGridH)
                continue;
            if (visited.count(nb))
                continue;
            opts[n++] = d;
        }
        if (!n)
            continue;
        int d = opts[rng.range(n)];
        Cell nb{ anchor.x + kDirX[d], anchor.y + kDirY[d] };
        visited.insert(nb);
        kinds[nb] = KIND_TREASURE;
        links.push_back({ anchor, nb });
    }

    // which sides of each room need a doorway
    std::unordered_map<Cell, int, CellHash> doors;   // bitmask over N S E W
    for (const auto& lk : links) {
        int dx = lk.second.x - lk.first.x;
        int dy = lk.second.y - lk.first.y;
        for (int d = 0; d < 4; d++) {
            if (kDirX[d] == dx && kDirY[d] == dy)
                doors[lk.first] |= 1 << d;
            if (kDirX[d] == -dx && kDirY[d] == -dy)
                doors[lk.second] |= 1 << d;
        }
    }

    // Decide each room's footprint once. The corridor pass needs the same
    // answer the room pass used, or the glue misses the walls it connects.
    std::unordered_map<Cell, int, CellHash> sizes;
    for (const auto& kv : kinds)
        sizes[kv.first] = room_tiles(kv.second, rng.next());

    Builder B{ out, ox, oy, oz };
    out->rooms = (int)kinds.size();

    // ---------------------------------------------------------- rooms
    for (const auto& kv : kinds) {
        const Cell& cell = kv.first;
        const int kind = kv.second;
        const int w = sizes[cell];
        const int h = (w - 1) / 2;
        const int cx = cell.x * kSlot, cy = cell.y * kSlot;
        const int x0 = cx - h, x1 = cx + h, y0 = cy - h, y1 = cy + h;
        const int mask = doors.count(cell) ? doors[cell] : 0;

        for (int i = x0; i <= x1; i++)
            for (int j = y0; j <= y1; j++) {
                B.put(floor_variant(rng), (float)i, (float)j, 0.0f, 0.0f);
                // roof. A floor tile is a 0.15-thick slab, not a plane, so
                // it has a real underside and reads as a ceiling from below.
                B.put("floor_tile_large", (float)i, (float)j,
                      kCourses * kWallTop, 0.0f);
            }

        // perimeter, one side at a time
        for (int d = 0; d < 4; d++) {
            const bool horiz = (d < 2);                 // N/S run along X
            const int count = horiz ? (x1 - x0 + 1) : (y1 - y0 + 1);
            for (int k = 0; k < count; k++) {
                int i = horiz ? (x0 + k) : ((d == 2) ? x1 : x0);
                int j = horiz ? ((d == 0) ? y1 : y0) : (y0 + k);
                float px = horiz ? (float)i : (float)i + (d == 2 ? 0.5f : -0.5f);
                float py = horiz ? (float)j + (d == 0 ? 0.5f : -0.5f) : (float)j;
                bool centre = horiz ? (i == cx) : (j == cy);
                if ((mask & (1 << d)) && centre)
                    B.put("wall_doorway", px, py, 0.0f, kSideYaw[d]);
                else
                    B.put(wall_variant(rng), px, py, 0.0f, kSideYaw[d]);
                // upper courses are always plain and solid -- an opening up
                // there would be a hole in the skyline
                for (int c = 1; c < kCourses; c++)
                    B.put("wall", px, py, c * kWallTop, kSideYaw[d]);
            }
        }
        // pillars cover the 1-unit corner gap the wall thickness leaves
        const float cs[4][2] = { { (float)x0 - 0.5f, (float)y0 - 0.5f },
                                 { (float)x1 + 0.5f, (float)y0 - 0.5f },
                                 { (float)x0 - 0.5f, (float)y1 + 0.5f },
                                 { (float)x1 + 0.5f, (float)y1 + 0.5f } };
        for (int c = 0; c < 4; c++)
            for (int course = 0; course < kCourses; course++)
                B.put("pillar", cs[c][0], cs[c][1], course * kWallTop, 0.0f);

        // a torch on each inside face, stood off the wall and facing in
        for (int d = 0; d < 4; d++) {
            int i = (d < 2) ? cx + (rng.range(3) - 1) : ((d == 2) ? x1 : x0);
            int j = (d < 2) ? ((d == 0) ? y1 : y0) : cy + (rng.range(3) - 1);
            float px = (d < 2) ? (float)i : (float)i + (d == 2 ? 0.5f : -0.5f);
            float py = (d < 2) ? (float)j + (d == 0 ? 0.5f : -0.5f) : (float)j;
            // inward, in world units, converted back to tiles for put()
            const float inx = -(float)kDirX[d] * kMountOut / kTile;
            const float inz = -(float)kDirY[d] * kMountOut / kTile;
            B.put("torch_mounted", px + inx, py + inz, 2.7f, kInnerYaw[d]);
            // the flame sits at the head, a little further out and up
            const float fx = px + inx * 1.7f;
            const float fz = py + inz * 1.7f;
            out->flames.push_back(ox + fx * kTile);
            out->flames.push_back(oy + 3.45f);
            out->flames.push_back(oz + fz * kTile);
        }

        // contents
        switch (kind) {
            case KIND_TREASURE:
                B.put("chest", (float)cx, (float)cy, 0.0f, 3.14159265f);
                B.put("coin_stack_medium", (float)cx - 0.35f,
                      (float)cy - 0.4f, 0.0f, 0.0f);
                break;
            case KIND_BOSS:
                B.put("floor_tile_big_spikes", (float)cx - 2.0f, (float)cy,
                      0.02f, 0.0f);
                B.put("chest_gold", (float)cx + 2.0f, (float)cy, 0.0f,
                      3.14159265f);
                for (int c = 0; c < 4; c++)
                    B.put("pillar_decorated", (float)cx + (c & 1 ? 2 : -2),
                          (float)cy + (c & 2 ? 2 : -2), 0.0f, 0.0f);
                break;
            case KIND_SAFE:
                B.put("table_long_tablecloth", (float)cx, (float)cy, 0.0f, 0.0f);
                B.put("shelves", (float)cx, (float)y1 - 1.0f, 0.0f,
                      3.14159265f);
                break;
            default: {
                int n = 2 + rng.range(3);
                static const char* kJunk[] = {
                    "barrel_large", "barrel_small_stack", "box_large",
                    "box_stacked", "crates_stacked", "table_medium_broken",
                    "trunk_medium_A", "shelf_large"
                };
                for (int q = 0; q < n; q++) {
                    float fi = (float)(x0 + 1 + rng.range(w - 2));
                    float fj = (float)(y0 + 1 + rng.range(w - 2));
                    B.put(kJunk[rng.range(8)], fi + rng.between(-0.3f, 0.3f),
                          fj + rng.between(-0.3f, 0.3f), 0.0f,
                          rng.between(0.0f, 6.28f));
                }
                break;
            }
        }

        if (kind == KIND_ENTRANCE) {
            out->entrance[0] = ox + cx * kTile;
            out->entrance[1] = oy;
            out->entrance[2] = oz + cy * kTile;
        } else if (kind == KIND_BOSS) {
            out->boss[0] = ox + cx * kTile;
            out->boss[1] = oy;
            out->boss[2] = oz + cy * kTile;
        }
    }

    // ---------------------------------------------------------- corridors
    for (const auto& lk : links) {
        const Cell& a = lk.first;
        const Cell& b = lk.second;
        const int ha = (sizes[a] - 1) / 2;
        const int hb = (sizes[b] - 1) / 2;
        const int acx = a.x * kSlot, acy = a.y * kSlot;
        const int bcx = b.x * kSlot, bcy = b.y * kSlot;

        if (acy == bcy) {                             // runs along X
            int lo = (acx < bcx) ? acx + ha + 1 : bcx + hb + 1;
            int hi = (acx < bcx) ? bcx - hb - 1 : acx - ha - 1;
            for (int i = lo; i <= hi; i++) {
                B.put(floor_variant(rng), (float)i, (float)acy, 0.0f, 0.0f);
                B.put("floor_tile_large", (float)i, (float)acy,
                      kCourses * kWallTop, 0.0f);
                for (int c = 0; c < kCourses; c++) {
                    B.put("wall", (float)i, (float)acy + 0.5f,
                          c * kWallTop, kSideYaw[0]);
                    B.put("wall", (float)i, (float)acy - 0.5f,
                          c * kWallTop, kSideYaw[1]);
                }
            }
        } else {                                      // runs along Z
            int lo = (acy < bcy) ? acy + ha + 1 : bcy + hb + 1;
            int hi = (acy < bcy) ? bcy - hb - 1 : acy - ha - 1;
            for (int j = lo; j <= hi; j++) {
                B.put(floor_variant(rng), (float)acx, (float)j, 0.0f, 0.0f);
                B.put("floor_tile_large", (float)acx, (float)j,
                      kCourses * kWallTop, 0.0f);
                for (int c = 0; c < kCourses; c++) {
                    B.put("wall", (float)acx + 0.5f, (float)j,
                          c * kWallTop, kSideYaw[2]);
                    B.put("wall", (float)acx - 0.5f, (float)j,
                          c * kWallTop, kSideYaw[3]);
                }
            }
        }
    }
}
