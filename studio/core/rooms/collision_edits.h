// A room's collision edited by hand (the Maps view's "Edit the collision"): faces of the room -- the
// game's own, or those made from the modder's model -- removed, moved to another kind (ground, wall,
// ceiling) or given other names, and simple shapes (planes, ramps, boxes, wedges) added. The edits live in
// the room's change (room_change.h) and are applied over the collision the change makes otherwise.
#pragma once

#include "core/rooms/room_collision.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace studio {

// A face known by its kind and corners (20.12, in the order they turn, the smallest corner first): what an
// edit names, so that it finds the same face again whatever order the faces are written in. Held in place
// (no allocation): keys are made for every face the map view draws.
struct FaceKey {
    using Corner = std::array<std::int32_t, 3>;
    int list = kGround;
    std::size_t count = 0;            // corners used, 3-4 (0: none)
    std::array<Corner, 4> corners{};  // those past `count` stay zero
    const Corner* begin() const {
        return corners.data();
    }
    const Corner* end() const {
        return corners.data() + count;
    }
    // by kind, then corner by corner (a prefix first)
    bool operator<(const FaceKey& o) const {
        if (list != o.list)
            return list < o.list;
        return std::lexicographical_compare(begin(), end(), o.begin(), o.end());
    }
    bool operator==(const FaceKey& o) const = default;
};
FaceKey face_key(int list, const RoomFace& f);

// What becomes of one face.
struct FaceEdit {
    bool removed = false;
    int list = -1;                   // its new kind; -1: as it is
    bool renamed = false;            // its names replaced by `names` (none: no name)
    std::vector<std::string> names;  // a gate (gateKK), a wall that opens (col_wallKK), surface tags
    bool operator==(const FaceEdit&) const = default;
};

// A shape of the modder's: made at `at` (the middle of its bottom), sized (whole sizes along x, y, z),
// turned about x, then y, then z (degrees); its faces sorted into grounds, walls and ceilings as a model's
// are, unless `list` makes them all one kind. A plane lies flat (y ignored); a ramp is a slope rising along
// +z by y; a box is closed; a wedge is a ramp closed below, at its sides and its high end.
struct CollisionShape {
    enum Kind { plane, ramp, box, wedge };
    Kind kind = plane;
    std::array<float, 3> at{0, 0, 0}, turn{0, 0, 0}, size{2, 1, 2};
    bool both_sides = false;  // a plane or ramp stood on (or stopped at) from either side
    std::vector<std::string> names;
    int list = -1;  // every face of it a ground, wall or ceiling; -1: each by its slope
    bool operator==(const CollisionShape&) const = default;
};
const char* shape_word(CollisionShape::Kind k);

struct CollisionEdits {
    std::map<FaceKey, FaceEdit> faces;
    std::vector<CollisionShape> shapes;
    bool empty() const {
        return faces.empty() && shapes.empty();
    }
    bool operator==(const CollisionEdits&) const = default;
};

// A point of the shape at unit coordinates (x and z from -0.5 to 0.5, y from 0 to 1) where the shape puts
// it, and the shape's axis `axis` (0 x, 1 y, 2 z) turned as the shape is (unit length).
std::array<float, 3> shape_point(const CollisionShape& s, const std::array<float, 3>& unit);
std::array<float, 3> shape_axis(const CollisionShape& s, int axis);

// A shape of `kind` fitted over `points` (the corners of the faces it stands for, in units), `normal` their
// faces' normals summed. A plane lies on them, turned to face the way they face (a wall's, a slope's, a floor's,
// a ceiling's); a ramp or wedge spans their extent from above, rising the way they rise (along x or z); a box
// spans their extent. No points: a shape of default size at `fallback`.
CollisionShape fit_shape(CollisionShape::Kind kind, const std::vector<std::array<float, 3>>& points,
                         const std::array<float, 3>& normal, const std::array<float, 3>& fallback);

// The shape lowered (or raised) to rest on the floor of `c` under it: the faces met looking down from a
// little over its bottom's middle and corners, the shape moved so the first to touch does. `skip` leaves
// faces out (the shape's own). False, the shape as it was, when no floor is under it.
bool drop_to_ground(const RoomCollision& c, CollisionShape* s, const std::function<bool(int, std::size_t)>& skip);

// A shape's triangles in the game's units, each turned so that (b - a) x (c - a) points to the side stood
// on or pushed from.
std::vector<std::array<std::array<float, 3>, 3>> shape_triangles(const CollisionShape& s);

struct EditsApplied {
    // per list and face (as written): the shape it was made of, -1 for the room's own
    std::array<std::vector<int>, 3> shape_of;
    std::size_t removed = 0, moved = 0, renamed = 0;
    std::size_t unmatched = 0;  // edits naming a face the room no longer has
};

// `c` with the edits applied: the faces named removed, moved, renamed, the shapes' faces added, the tree
// built again. `original` is the game's collision (its tree's smallest node). False with the reason.
bool apply_collision_edits(RoomCollision* c, const RoomCollision& original, const CollisionEdits& e, float ground_limit,
                           EditsApplied* out, std::string* why);

// The edits as lines of a room change, and one such line read (`word` its first word): 1 read, 0 not an
// edit's word, -1 wrong (the reason in `why`).
std::string edits_text(const CollisionEdits& e);
int read_edit_line(const std::string& word, const std::string& rest, CollisionEdits* e, std::string* why);

}  // namespace studio
