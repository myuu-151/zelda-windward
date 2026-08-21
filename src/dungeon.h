// Procedural dungeon floor, assembled from the KayKit Dungeon Remastered
// kit that lives in the shared prop library as the "Dungeon" category.
//
// Nothing here is sculpted. A floor is a grid of room slots; a self-avoiding
// random walk carves the critical path from entrance to boss, dead-end buds
// become treasure rooms, and every slot is filled with kit pieces snapped to
// the kit's native 4-unit grid. The output is a plain list of placements the
// caller turns into prop instances -- which is why the client gets rendering,
// collision and camera blocking for free.
#pragma once

#include <string>
#include <vector>

struct DungeonPiece {
    std::string id;      // prop library id, e.g. "Dungeon/wall"
    float x, y, z;       // world space
    float yaw;           // radians, about +Y
};

struct DungeonFloor {
    std::vector<DungeonPiece> pieces;
    // Where a flame belongs, in world space, three floats each. The kit has
    // no lit wall torch -- torch_mounted is the bracket alone -- so the fire
    // is drawn by the client as an additive billboard at these points.
    std::vector<float> flames;
    float entrance[3] = { 0, 0, 0 };   // where to drop the player
    float boss[3] = { 0, 0, 0 };
    int rooms = 0;
};

// seed 0 picks one from the clock, so a portal saved with seed 0 opens
// somewhere new every time.
void dungeon_generate(unsigned seed, float ox, float oy, float oz,
                      DungeonFloor* out);
