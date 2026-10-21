// A mission world's archive and a room's collision: synthetic archives read and written back byte for byte, trees
// checked, built from faces and walked; collision made from a model; a room
// replaced in a mod and given back. Every file made here.
#include "test.h"

#include "core/rooms/collision_edits.h"
#include "core/rooms/room_change.h"
#include "core/rooms/room_collision.h"
#include "core/rooms/worlds.h"

#include "khdays/assets/gltf_export.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

using namespace studio;

namespace {

using Bytes = std::vector<std::uint8_t>;

void add16(Bytes& d, std::uint32_t v) {
    d.push_back(static_cast<std::uint8_t>(v & 0xFF));
    d.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void add32(Bytes& d, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        d.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void put32(Bytes& d, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

constexpr std::int32_t kUnit = 4096;  // 1.0 in 20.12

// A flat square face (ground or ceiling) or an upright one (wall), corners in units.
RoomFace face(int x0, int z0, int x1, int z1, int y0 = 0, int y1 = 0) {
    RoomFace f;
    f.corners = 4;
    f.v = {{{x0 * kUnit, y0 * kUnit, z0 * kUnit},
            {x1 * kUnit, y0 * kUnit, z0 * kUnit},
            {x1 * kUnit, y1 * kUnit, z1 * kUnit},
            {x0 * kUnit, y1 * kUnit, z1 * kUnit}}};
    f.bounds = {std::min(x0, x1) * kUnit, std::min(z0, z1) * kUnit, std::max(x0, x1) * kUnit, std::max(z0, z1) * kUnit};
    f.planes[0] = 7;  // stored planes are kept as they are
    return f;
}

NamedRecord named(const char* text) {
    NamedRecord r;
    for (std::size_t i = 0; i < 8 && text[i] != '\0'; ++i)
        r.name[i] = text[i];
    return r;
}

// A room: one ground, two walls, one ceiling, two named records, one 0x18
// record, the tree as built over a 32-unit root.
RoomCollision room() {
    RoomCollision c;
    c.header.assign(0xb0, 0);
    c.header[0x70] = 2;  // the resource flags, kept
    c.records18.assign(0x18, 0x5a);
    RoomFace g = face(-4, -4, 4, 4);
    g.slots[0] = 1;
    g.extra = 0x00137fff;
    c.faces[kGround] = {g};
    RoomFace w = face(-4, -4, 4, -4, 0, 3);
    w.slots[0] = 0;
    c.faces[kWall] = {w, face(4, -4, 4, 4, 0, 3)};
    c.faces[kCeiling] = {face(-2, -2, 2, 2, 3, 3)};
    c.named = {named("nohit"), named("gate01")};
    build_tree(&c, 0, 0, 32 * kUnit, 2 * kUnit);
    return c;
}

// A P2 container of `entries` (each stored as given, none compressed).
Bytes p2(const std::vector<Bytes>& entries) {
    P2File p;
    p.base = 0x200;
    p.head.assign(0x200, 0);
    p.head[0] = 'P';
    p.head[1] = '2';
    p.head[2] = static_cast<std::uint8_t>(entries.size());
    put32(p.head, 0xc, 0x200);
    for (const auto& e : entries) {
        P2Entry entry;
        entry.data = e;
        p.entries.push_back(entry);
    }
    return write_p2(p);
}

// Two rooms: room 0 (id 7) with one link and one object (member 3), its
// collision at 2; room 1 (id 9) with no links, an object at the empty entry 4,
// its collision the empty entry 4.
Bytes room_table() {
    Bytes t;
    t.push_back(2);
    t.push_back(0xff);
    t.push_back(0xff);
    t.push_back(0);
    add32(t, 0x0c);
    add32(t, 0x0c + 0x30);
    // room 0 at 0x0c: header, then its object at +0x24 and its link row at +0x28 (offsets from the room)
    for (const std::uint8_t b : {std::uint8_t{7}, std::uint8_t{1}, std::uint8_t{1}, std::uint8_t{2}})
        t.push_back(b);
    for (int k = 0; k < 4; ++k)
        add32(t, 0);
    add32(t, 0xa000);
    add32(t, 0x32000);
    add32(t, 0x28);  // link rows
    add32(t, 0x24);  // object 0
    add32(t, 0x0380);
    for (const std::uint8_t b : {std::uint8_t{3}, std::uint8_t{1}, std::uint8_t{0}, std::uint8_t{0xff}})
        t.push_back(b);
    add16(t, 0x0003);
    add16(t, 0x0100);
    // room 1 at 0x3c
    for (const std::uint8_t b : {std::uint8_t{9}, std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{4}})
        t.push_back(b);
    for (int k = 0; k < 4; ++k)
        add32(t, 0);
    add32(t, 0);
    add32(t, 0);
    add32(t, 0x24);
    add32(t, 0x24);
    add32(t, 0x0480);
    return t;
}

Bytes gate_models() {
    Bytes g;
    add32(g, 1);
    g.push_back(3);
    g.push_back(0);
    g.push_back(0);
    g.push_back(0);
    for (int part = 0; part < 5; ++part) {
        g.push_back(static_cast<std::uint8_t>(part));
        g.push_back(1);
        add16(g, 0x7fff);
        add32(g, 0);
        for (int b = 0; b < 8; ++b)
            g.push_back(0);
        add32(g, static_cast<std::uint32_t>(part * kUnit));
        add32(g, 0);
        add32(g, static_cast<std::uint32_t>(-part * kUnit));
    }
    return g;
}

}  // namespace

TEST(a_room_collision_reads_back_byte_for_byte) {
    const RoomCollision c = room();
    CHECK(check_tree(c).empty());
    const Bytes data = write_collision(c);
    RoomCollision back;
    std::string why;
    CHECK(parse_collision(data, &back, &why));
    CHECK(back.faces[kGround].size() == 1 && back.faces[kWall].size() == 2 && back.faces[kCeiling].size() == 1);
    CHECK(back.named.size() == 2 && back.named[1].text() == "gate01" && back.records18.size() == 0x18);
    CHECK(back.faces[kGround][0].extra == 0x00137fff && back.faces[kGround][0].planes[0] == 7 &&
          back.header[0x70] == 2);
    CHECK(back.root(2) == 32 * kUnit && back.root(3) == 2 * kUnit);
    CHECK(write_collision(back) == data);
    // the sections as the game lays them: the 0x18 records at 0xb0, the (empty) pointer table and the tree after them
    CHECK(data[0x76] == 1 && data[0x94] == 0xb0 && data[0x98] == 0xc8 && data[0x9c] == 0xc8);
}

TEST(a_world_archive_reads_back_entry_for_entry) {
    const Bytes collision = write_collision(room());
    const Bytes file = p2({room_table(), gate_models(), collision, Bytes(64, 0x11), Bytes()});
    World w;
    std::string why;
    CHECK(parse_world(file, "zz", &w, &why));
    CHECK(w.rooms.size() == 2 && w.gate_models.size() == 1);
    const WorldRoom& r0 = w.rooms[0];
    CHECK(r0.id == 7 && r0.collision_member == 2 && r0.budget == 0xa000);
    CHECK(r0.links.size() == 1 && r0.links[0].key == 3 && r0.links[0].target == 1 && r0.links[0].gate_state == -1 &&
          r0.links[0].flags == 3 && r0.links[0].radius == 0x100);
    CHECK(r0.objects.size() == 1 && r0.objects[0].member == 3 && r0.objects[0].flags == 0x80);
    const WorldRoom& r1 = w.rooms[1];
    CHECK(r1.id == 9 && r1.collision_member == 4 && r1.objects.size() == 1 && r1.objects[0].member == 4);
    CHECK(w.archive.entries[4].data.empty());
    CHECK(w.gate_models[0].key == 3 && w.gate_models[0].parts[2].member == 1 &&
          w.gate_models[0].parts[2].turn == 0x7fff);
    CHECK(w.gate_models[0].parts[2].scale[0] == 2 * kUnit && w.gate_models[0].parts[2].scale[2] == -2 * kUnit);
    // nothing changed: every entry the same
    P2File back;
    CHECK(parse_p2(write_world(w, {}), &back, &why));
    CHECK(back.entries.size() == 5);
    for (std::size_t e = 0; e < back.entries.size(); ++e)
        CHECK(back.entries[e].data == w.archive.entries[e].data);
    // a collision replaced: only that entry differs
    RoomCollision other = room();
    other.faces[kGround][0].slots[0] = 0;
    const Bytes replaced = write_collision(other);
    CHECK(parse_p2(write_world(w, {{2, replaced}}), &back, &why));
    CHECK(back.entries[2].data == replaced && back.entries[3].data == w.archive.entries[3].data);
}

TEST(a_tree_is_built_from_faces_and_walked_as_the_game_walks_it) {
    // 40 small grounds on a grid, a few walls across them: one node holding all, then built
    RoomCollision flat;
    flat.header.assign(0xb0, 0);
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 5; ++j)
            flat.faces[kGround].push_back(face(-16 + 4 * i, -10 + 4 * j, -13 + 4 * i, -7 + 4 * j));
    }
    for (int i = 0; i < 6; ++i)
        flat.faces[kWall].push_back(face(-15 + 5 * i, 0, -12 + 5 * i, 0, 0, 2));
    flat.faces[kCeiling].push_back(face(-30, -30, 30, 30, 4, 4));  // across the whole room: stays at the root
    RoomCollision one = flat;
    one.set_root(0, 0, 64 * kUnit, 2 * kUnit);
    RoomNode root;
    root.first = {0, 0, 0};
    one.nodes = {root};
    for (auto& list : one.faces)
        list.back().flags = 0x8000;
    CHECK(check_tree(one).empty());
    RoomCollision built = flat;
    build_tree(&built, 0, 0, 64 * kUnit, 2 * kUnit);
    CHECK(check_tree(built).empty());
    CHECK(built.nodes.size() > 1);
    CHECK(built.nodes[0].first[kCeiling] == 0);  // the big ceiling at the root
    CHECK(compare_walks(one, built).empty());
    // a broken tree is told: a run not closed, a face outside its node, flags wrong
    RoomCollision open = built;
    for (auto& f : open.faces[kGround])
        f.flags = 0;
    CHECK(!check_tree(open).empty());
    RoomCollision flags = built;
    flags.nodes[0].flags = 0;
    CHECK(!check_tree(flags).empty());
    CHECK(!compare_walks(one, flags).empty());
}

namespace {

using Point = std::array<float, 3>;

// Two triangles of a quad whose corners turn counter-clockwise seen from its front.
void quad(CollisionSource* s, Point a, Point b, Point c, Point d, const char* material = "") {
    s->triangles.push_back({a, b, c});
    s->triangles.push_back({a, c, d});
    s->materials.push_back(material);
    s->materials.push_back(material);
}

double area(const RoomFace& f) {
    double sum = 0;
    for (std::size_t i = 1; i + 1 < f.corners; ++i) {
        const double ax = f.v[i][0] - f.v[0][0], az = f.v[i][2] - f.v[0][2];
        const double bx = f.v[i + 1][0] - f.v[0][0], bz = f.v[i + 1][2] - f.v[0][2];
        sum += std::abs(ax * bz - az * bx) / 2;
    }
    return sum / (static_cast<double>(kUnit) * kUnit);
}

}  // namespace

TEST(a_collision_is_made_from_a_model_as_the_game_reads_it) {
    const RoomCollision original = room();
    CollisionRules rules;
    rules.drop_original = {"gate01"};  // here only the model's faces
    CollisionReport report;
    std::string why;

    // one quad -> one ground face of four corners, its plane the game's
    CollisionSource floor;
    quad(&floor, {-3, 0, -3}, {-3, 0, 3}, {3, 0, 3}, {3, 0, -3});
    RoomCollision built;
    CHECK(build_collision(floor, original, rules, &built, &report, &why));
    CHECK(built.faces[kGround].size() == 1 && built.faces[kWall].empty() && built.faces[kCeiling].empty());
    CHECK(built.faces[kGround][0].corners == 4);
    CHECK(built.faces[kGround][0].planes[2] == 0x00 && built.faces[kGround][0].planes[3] == 0x10);  // normal y 4096
    CHECK(check_collision(built, original).empty());
    CHECK(built.records18 == original.records18 && built.header[0x70] == 2);
    // the original's ground kind and last word on the new ground
    CHECK(built.faces[kGround][0].slots[3] == original.faces[kGround][0].slots[3]);
    CHECK(built.faces[kGround][0].extra == 0x00137fff);
    // gate01 is on no face now: said
    bool told = false;
    for (const auto& w : report.warnings)
        told = told || (w.kind == RoomWarning::gate_unplaced && w.args[0] == "gate01");
    CHECK(told);

    // a box seen from outside: its top a ground, its bottom a ceiling, four walls
    CollisionSource box;
    quad(&box, {-2, 4, -2}, {-2, 4, 2}, {2, 4, 2}, {2, 4, -2});
    quad(&box, {-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2});
    quad(&box, {2, 0, -2}, {2, 4, -2}, {2, 4, 2}, {2, 0, 2});
    quad(&box, {-2, 0, -2}, {-2, 0, 2}, {-2, 4, 2}, {-2, 4, -2});
    quad(&box, {-2, 0, 2}, {2, 0, 2}, {2, 4, 2}, {-2, 4, 2});
    quad(&box, {-2, 0, -2}, {-2, 4, -2}, {2, 4, -2}, {2, 0, -2});
    CHECK(build_collision(box, original, rules, &built, &report, &why));
    CHECK(built.faces[kGround].size() == 1 && built.faces[kWall].size() == 4 && built.faces[kCeiling].size() == 1);
    for (const auto& list : built.faces) {
        for (const auto& f : list)
            CHECK(f.corners == 4);
    }
    CHECK(check_collision(built, original).empty());

    // a flat hexagon -> convex pieces of at most four corners covering it
    CollisionSource hexagon;
    Point h[6];
    for (int i = 0; i < 6; ++i) {
        const float a = -static_cast<float>(i) * 3.14159265f / 3.0f;  // clockwise in x/z: counter-clockwise from above
        h[i] = {4.0f * std::cos(a), 1.0f, 4.0f * std::sin(a)};
    }
    for (int i = 1; i + 1 < 6; ++i) {
        hexagon.triangles.push_back({h[0], h[i], h[i + 1]});
        hexagon.materials.push_back("");
    }
    CHECK(build_collision(hexagon, original, rules, &built, &report, &why));
    CHECK(built.faces[kWall].empty() && built.faces[kCeiling].empty());
    CHECK(built.faces[kGround].size() == 2);
    double total = 0;
    for (const auto& f : built.faces[kGround])
        total += area(f);
    CHECK(std::abs(total - 6 * 16 * std::sqrt(3.0) / 4) < 0.01);
    CHECK(check_collision(built, original).empty());

    // a slope past the ground limit is a wall, one below it a ground
    CollisionSource steep, gentle;
    const float rise80 = 4.0f * std::tan(80.0f * 3.14159265f / 180.0f),
                rise70 = 4.0f * std::tan(70.0f * 3.14159265f / 180.0f);
    quad(&steep, {0, 0, -2}, {0, 0, 2}, {4, rise80, 2}, {4, rise80, -2});
    quad(&gentle, {0, 0, -2}, {0, 0, 2}, {4, rise70, 2}, {4, rise70, -2});
    CHECK(build_collision(steep, original, rules, &built, &report, &why));
    CHECK(built.faces[kWall].size() == 1 && built.faces[kGround].empty());
    CHECK(build_collision(gentle, original, rules, &built, &report, &why));
    CHECK(built.faces[kGround].size() == 1);
}

TEST(the_original_gates_stay_where_no_new_face_takes_them) {
    const RoomCollision original = room();
    CollisionSource floor;
    quad(&floor, {-3, 0, -3}, {-3, 0, 3}, {3, 0, 3}, {3, 0, -3});
    RoomCollision built;
    CollisionReport report;
    std::string why;
    CHECK(build_collision(floor, original, CollisionRules{}, &built, &report, &why));
    // gate01's ground copied as the game had it, beside the new one; said
    CHECK(built.faces[kGround].size() == 2);
    int kept = 0;
    for (std::size_t j = 0; j < built.faces[kGround].size(); ++j) {
        const RoomFace& f = built.faces[kGround][j];
        if (f.slots[0] == 1) {
            ++kept;
            CHECK(f.v == original.faces[kGround][0].v && f.extra == original.faces[kGround][0].extra);
            CHECK(report.face_triangles[kGround][j].empty());
        } else {
            CHECK(report.face_triangles[kGround][j] == std::vector<std::size_t>({0, 1}));
        }
    }
    CHECK(kept == 1);
    bool said = false;
    for (const auto& w : report.warnings)
        said = said || (w.kind == RoomWarning::name_kept && w.args[0] == "gate01");
    CHECK(said);
    CHECK(check_collision(built, original).empty());
    // a marker moved lands where it was put
    CollisionRules rules;
    rules.drop_original = {"gate01"};
    const auto before = markers(original);
    CHECK(before.size() == 1);
    rules.markers[before[0].name] = {kUnit, 0, -kUnit, 16384};
    CHECK(build_collision(floor, original, rules, &built, &report, &why));
    const auto after = markers(built);
    CHECK((after.size() == 1 && after[0].place == std::array<std::int32_t, 3>{kUnit, 0, -kUnit} &&
           after[0].extra == 16384));
    for (const auto& w : report.warnings)
        CHECK(w.kind != RoomWarning::marker_floating);  // over the new floor
}

TEST(names_given_to_a_model_land_in_their_slots) {
    const RoomCollision original = room();
    CollisionRules rules;
    rules.names_by_material["stone"] = {"slide"};
    rules.names_by_triangle[0] = {"gate01"};
    rules.names_by_triangle[1] = {"gate01"};
    CollisionSource s;
    quad(&s, {-3, 0, -3}, {-3, 0, 3}, {3, 0, 3}, {3, 0, -3}, "stone");  // triangles 0, 1: gate01 and slide
    quad(&s, {3, 0, -3}, {3, 0, 3}, {9, 0, 3}, {9, 0, -3}, "stone");    // slide only: not joined with the gate's
    RoomCollision built;
    CollisionReport report;
    std::string why;
    CHECK(build_collision(s, original, rules, &built, &report, &why));
    CHECK(built.faces[kGround].size() == 2);
    // every name the room had, and the new one after them
    CHECK(built.named.size() == 3 && built.named[2].text() == "slide");
    int gates = 0, slides = 0;
    for (const auto& f : built.faces[kGround]) {
        if (f.slots[0] == 1)
            ++gates;  // gate01 is the room's second record
        if (f.slots[1] == 2)
            ++slides;
    }
    CHECK(gates == 1 && slides == 2);
    for (const auto& w : report.warnings)
        CHECK(w.kind != RoomWarning::gate_unplaced);
    CHECK(check_collision(built, original).empty());
    // a face given three names besides a gate is refused, the triangle named
    rules.names_by_material["stone"] = {"slide", "nohit", "nocam"};
    CHECK(!build_collision(s, original, rules, &built, &report, &why));
    CHECK(why.find("triangle") != std::string::npos);
}

TEST(a_wide_room_keeps_its_tree_within_the_walk) {
    // tiny grounds over 400 units: the tree stops at the eighth level
    CollisionSource s;
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 100; ++j) {
            const float x = -200.0f + 4.0f * static_cast<float>(i), z = -200.0f + 4.0f * static_cast<float>(j);
            quad(&s, {x, 0, z}, {x, 0, z + 1}, {x + 1, 0, z + 1}, {x + 1, 0, z});
        }
    }
    RoomCollision built;
    CollisionReport report;
    std::string why;
    CHECK(build_collision(s, room(), CollisionRules{}, &built, &report, &why));
    CHECK(built.root(2) >= 400 * kUnit && built.root(2) / built.root(3) <= 128);
    CHECK(check_tree(built).empty());
    CHECK(check_collision(built, room()).empty());
    // more than the game's largest room: said, not refused
    bool told = false;
    for (const auto& w : report.warnings)
        told = told || w.kind == RoomWarning::too_big;
    CHECK(told == (report.bytes > kLargestShippedCollision));
}

TEST(a_room_change_reads_back_as_written) {
    RoomReplacement r;
    r.world = "tt";
    r.room = 3;
    r.model = "my room.glb";
    r.collision = "walls.gltf";
    r.place.scale = 2.5f;
    r.place.turn = {0, 90, 0};
    r.place.move = {1.5f, -2, 3};
    r.ground_limit = 0.5f;
    r.objects[7] = ObjectUse::keep;
    r.objects[8] = ObjectUse::hide;
    r.drop_original = {"col_wall01"};
    r.markers["pent06_0"] = {-4096, 410, -3277, 16384};
    r.gate_points[{6, 0}] = {1, 2, 3};
    r.names_by_material["Stone Floor"] = {"slide", "nocam"};
    r.names_by_triangle[12] = {"gate01"};
    r.keep_collision = true;
    const auto dir = studio_test::scratch("room_change_text");
    const auto file = dir / "3.txt";
    studio_test::write(file, change_text(r));
    RoomReplacement back;
    std::string why;
    CHECK(read_change(file, &back, &why));
    CHECK(back.world == "tt" && back.room == 3 && back.model == "my room.glb" && back.collision == "walls.gltf");
    CHECK(back.place.scale == 2.5f && back.place.turn[1] == 90 && back.place.move[0] == 1.5f &&
          back.place.move[1] == -2);
    CHECK(back.ground_limit == 0.5f && back.objects.size() == 2 && back.objects[8] == ObjectUse::hide);
    CHECK(back.names_by_material["Stone Floor"] == std::vector<std::string>({"slide", "nocam"}));
    CHECK(back.names_by_triangle[12] == std::vector<std::string>({"gate01"}));
    CHECK(back.drop_original.count("col_wall01") == 1 && back.markers["pent06_0"][3] == 16384);
    CHECK((back.gate_points[{6, 0}] == std::array<std::int32_t, 3>{1, 2, 3}));
    CHECK(back.keep_collision);
    CHECK(change_text(back) == change_text(r));
    // not a change of this studio, or a word it does not know: refused, said where
    studio_test::write(file, "something else\n");
    CHECK(!read_change(file, &back, &why));
    studio_test::write(file, change_text(r) + "teleport 1 2 3\n");
    CHECK(!read_change(file, &back, &why) && why.find("line") != std::string::npos);

    // no model: the game's room kept, only its markers and exits' models moved
    RoomReplacement only;
    only.world = "tt";
    only.room = 3;
    only.keep_collision = true;
    only.markers["pent06_0"] = {-4096, 410, -3277, 16384};
    only.gate_points[{6, 1}] = {7, 8, 9};
    CHECK(markers_only(only));
    CHECK(change_text(only).find("model") == std::string::npos);
    studio_test::write(file, change_text(only));
    CHECK(read_change(file, &back, &why));
    CHECK(markers_only(back) && back.keep_collision && back.markers.size() == 1 && back.gate_points.size() == 1);
    CHECK(change_text(back) == change_text(only));
    // with no model the floor and walls cannot be anything but the game's
    only.keep_collision = false;
    studio_test::write(file, change_text(only));
    CHECK(!read_change(file, &back, &why));
}

TEST(a_model_is_placed_scaled_turned_then_moved) {
    namespace assets = khdays::assets;
    assets::ImportedModel m;
    assets::NeutralMesh mesh;
    assets::NeutralVertex v;
    v.position = {1, 0, 0};
    v.normal = {1, 0, 0};
    mesh.vertices = {v, v, v};
    mesh.indices = {0, 1, 2};
    m.model.meshes = {mesh};
    RoomPlacement p;
    p.scale = 2;
    p.turn = {0, 90, 0};
    p.move = {1, 0, 0};
    place_model(&m, p);
    const auto& q = m.model.meshes[0].vertices[0];
    CHECK(std::abs(q.position[0] - 1) < 1e-5f && std::abs(q.position[1]) < 1e-5f &&
          std::abs(q.position[2] + 2) < 1e-5f);
    CHECK(std::abs(q.normal[2] + 2) < 1e-5f);  // turned as the point (its length comes back in the importer's units)
    // a mesh drawn both sides: a steep face stands both ways, a floor one way
    m.model.meshes[0].double_sided = true;
    m.model.meshes[0].vertices[0].position = {0, 0, 0};
    m.model.meshes[0].vertices[1].position = {1, 0, 0};
    m.model.meshes[0].vertices[2].position = {0, 1, 0};  // upright
    CHECK(collision_source(m).triangles.size() == 2);
    m.model.meshes[0].vertices[2].position = {0, 0, 1};  // flat
    CHECK(collision_source(m).triangles.size() == 1);
}

TEST(a_room_is_saved_into_a_mod_and_given_back) {
    namespace assets = khdays::assets;
    namespace fs = std::filesystem;
    const auto dir = studio_test::scratch("room_save");
    // the player's copy: world zz (two rooms, room 0's collision at entry 2, its model entry 3)
    const Bytes original = write_collision(room());
    studio_test::write(dir / "nitrofs" / "mi" / "wd" / "wd_zz", [&] {
        const Bytes f = p2({room_table(), gate_models(), original, Bytes(64, 0x11), Bytes()});
        return std::string(f.begin(), f.end());
    }());
    // the graphics dump names entry 3's model
    studio_test::write(dir / "dump" / "mi" / "wd" / "wd_zz#3" / "mdl" / "zz_room.gltf", "{}");
    // the modder's model: a floor of 10 x 10
    assets::NeutralModel floor;
    assets::NeutralMesh mesh;
    mesh.material_name = "floor";
    for (const auto& p : std::vector<std::array<float, 3>>{{-5, 0, -5}, {-5, 0, 5}, {5, 0, 5}, {5, 0, -5}}) {
        assets::NeutralVertex v;
        v.position = p;
        v.normal = {0, 1, 0};
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    floor.meshes = {mesh};
    studio_test::write(dir / "in" / "floor.gltf", assets::to_gltf(floor, {}));
    // a model the modder replaced by hand in the same world: never touched
    const fs::path mod = dir / "mods" / "Test";
    studio_test::write(mod / "graphics" / "mi" / "wd" / "wd_zz#9" / "mdl" / "own.gltf", "{}");

    RoomReplacement r;
    r.world = "zz";
    r.room = 0;
    r.gate_points[{3, 2}] = {5 * kUnit, kUnit, 6 * kUnit};  // gate model 3's third point moved
    WorldOutcome o;
    std::string why;
    CHECK(save_change(dir / "nitrofs", dir / "dump", mod, r, dir / "in" / "floor.gltf", {}, &o, &why));
    std::error_code ec;
    CHECK(fs::exists(mod / "studio" / "rooms" / "zz" / "0.txt", ec) &&
          fs::exists(mod / "studio" / "rooms" / "zz" / "0" / "floor.gltf", ec));
    CHECK(fs::exists(mod / "graphics" / "mi" / "wd" / "wd_zz#3" / "mdl" / "zz_room.gltf", ec));
    World w;
    CHECK(load_world(mod / "files", "zz", &w, &why));
    RoomCollision made, before;
    CHECK(parse_collision(w.archive.entries[2].data, &made, &why) && parse_collision(original, &before, &why));
    CHECK(made.faces[kGround].size() == 2 && made.faces[kWall].empty());  // the floor and gate01's ground, kept
    CHECK(check_collision(made, before).empty());
    CHECK(w.archive.entries[3].data == Bytes(64, 0x11) && w.rooms.size() == 2);
    CHECK((w.gate_models[0].parts[2].position == std::array<std::int32_t, 3>{5 * kUnit, kUnit, 6 * kUnit}));
    CHECK(w.gate_models[0].parts[2].scale[0] == 2 * kUnit &&
          w.gate_models[0].parts[1].scale[0] == kUnit);  // the rest as it was
    // saved again from what the mod holds: the same archive
    std::ifstream first(mod / "files" / "mi" / "wd" / "wd_zz", std::ios::binary);
    const std::string once((std::istreambuf_iterator<char>(first)), std::istreambuf_iterator<char>());
    first.close();
    CHECK(write_world_outputs(dir / "nitrofs", dir / "dump", mod, "zz", &o, &why));
    std::ifstream second(mod / "files" / "mi" / "wd" / "wd_zz", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(second)), std::istreambuf_iterator<char>()) == once);
    second.close();

    // only the look replaced: the game's own collision stays as it was, the model still the modder's
    r.keep_collision = true;
    CHECK(save_change(dir / "nitrofs", dir / "dump", mod, r, dir / "in" / "floor.gltf", {}, &o, &why));
    World kept;
    CHECK(load_world(mod / "files", "zz", &kept, &why));
    CHECK(kept.archive.entries[2].data == original);
    CHECK(fs::exists(mod / "graphics" / "mi" / "wd" / "wd_zz#3" / "mdl" / "zz_room.gltf", ec));
    r.keep_collision = false;

    // given back: everything it wrote gone, the modder's own model kept
    CHECK(revert_change(dir / "nitrofs", dir / "dump", mod, "zz", 0, &o, &why));
    CHECK(o.removed);
    CHECK(!fs::exists(mod / "files" / "mi" / "wd" / "wd_zz", ec));
    CHECK(!fs::exists(mod / "graphics" / "mi" / "wd" / "wd_zz#3", ec));
    CHECK(!fs::exists(mod / "studio" / "rooms" / "zz", ec));
    CHECK(fs::exists(mod / "graphics" / "mi" / "wd" / "wd_zz#9" / "mdl" / "own.gltf", ec));
    // a room with no collision in the game's data cannot be replaced
    r.room = 1;
    CHECK(!save_change(dir / "nitrofs", dir / "dump", mod, r, dir / "in" / "floor.gltf", {}, &o, &why));
}

TEST(a_rooms_markers_and_exits_move_with_nothing_else_changed) {
    namespace fs = std::filesystem;
    const auto dir = studio_test::scratch("room_markers_only");
    const Bytes original = write_collision(room());
    studio_test::write(dir / "nitrofs" / "mi" / "wd" / "wd_zz", [&] {
        const Bytes f = p2({room_table(), gate_models(), original, Bytes(64, 0x11), Bytes()});
        return std::string(f.begin(), f.end());
    }());
    const fs::path mod = dir / "mods" / "Test";
    const std::string name = markers(room()).at(0).name;

    RoomReplacement r;
    r.world = "zz";
    r.room = 0;
    r.keep_collision = true;
    r.markers[name] = {2 * kUnit, kUnit / 10, -3 * kUnit, 16384};
    r.gate_points[{3, 2}] = {5 * kUnit, kUnit, 6 * kUnit};
    CHECK(markers_only(r));
    // the room's objects all stay the game's
    World game;
    std::string why;
    CHECK(load_world(dir / "nitrofs", "zz", &game, &why));
    for (const auto& [member, use] : object_uses(game.rooms[0], r))
        CHECK(use == ObjectUse::keep);

    WorldOutcome o;
    CHECK(save_change(dir / "nitrofs", dir / "dump", mod, r, {}, {}, &o, &why));
    std::error_code ec;
    CHECK(fs::exists(mod / "studio" / "rooms" / "zz" / "0.txt", ec));
    CHECK(!fs::exists(mod / "studio" / "rooms" / "zz" / "0", ec));  // no model of the modder's copied
    CHECK(!fs::exists(mod / "graphics", ec));                       // none written for the port
    World w;
    CHECK(load_world(mod / "files", "zz", &w, &why));
    RoomCollision made, before;
    CHECK(parse_collision(w.archive.entries[2].data, &made, &why) && parse_collision(original, &before, &why));
    const auto after = markers(made);
    CHECK(after.size() == 1 && after[0].name == name);
    CHECK((after[0].place == std::array<std::int32_t, 3>{2 * kUnit, kUnit / 10, -3 * kUnit}) &&
          after[0].extra == 16384);
    // everything else of the collision as the game had it, byte for byte
    RoomCollision back = made;
    set_markers(&back, markers(before));
    CHECK(write_collision(back) == original);
    CHECK(w.archive.entries[3].data == Bytes(64, 0x11));
    CHECK((w.gate_models[0].parts[2].position == std::array<std::int32_t, 3>{5 * kUnit, kUnit, 6 * kUnit}));
    // the change as the mod holds it
    RoomReplacement again;
    CHECK(read_change(mod / "studio" / "rooms" / "zz" / "0.txt", &again, &why));
    CHECK(markers_only(again) && again.markers.size() == 1);
    // the kept collision is what the window shows before saving
    RoomCollision shown;
    std::size_t moved = 0;
    CHECK(kept_collision(game, r, &shown, &moved, &why) && moved == 1 &&
          write_collision(shown) == write_collision(made));

    // a marker the room does not have moves nothing; nothing moved leaves the game's bytes
    r.markers.clear();
    r.markers["nothere"] = {0, 0, 0, 0};
    CHECK(save_change(dir / "nitrofs", dir / "dump", mod, r, {}, {}, &o, &why));
    CHECK(load_world(mod / "files", "zz", &w, &why) && w.archive.entries[2].data == original);
    r.markers.clear();
    CHECK(save_change(dir / "nitrofs", dir / "dump", mod, r, {}, {}, &o, &why));
    CHECK(load_world(mod / "files", "zz", &w, &why) && w.archive.entries[2].data == original);

    // given back: everything it wrote gone
    CHECK(revert_change(dir / "nitrofs", dir / "dump", mod, "zz", 0, &o, &why));
    CHECK(o.removed);
    CHECK(!fs::exists(mod / "files" / "mi" / "wd" / "wd_zz", ec));
    CHECK(!fs::exists(mod / "studio" / "rooms" / "zz", ec));
}

namespace {
std::array<float, 3> tri_normal(const std::array<std::array<float, 3>, 3>& t) {
    const std::array<float, 3> a{t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]},
        b{t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]};
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
}  // namespace

TEST(collision_shapes_face_the_side_stood_on) {
    CollisionShape s;
    s.size = {4, 2, 6};
    s.at = {1, 2, 3};
    // a plane: two triangles facing up, at its height
    auto t = shape_triangles(s);
    CHECK(t.size() == 2);
    for (const auto& tri : t)
        CHECK(tri_normal(tri)[1] > 0 && std::abs(tri[0][1] - 2.0f) < 1e-5f);
    s.both_sides = true;
    CHECK(shape_triangles(s).size() == 4);
    s.both_sides = false;
    // a ramp: up and towards its low end (-z), rising by its height over its length
    s.kind = CollisionShape::ramp;
    t = shape_triangles(s);
    CHECK(t.size() == 2);
    float lo = 1e9f, hi = -1e9f;
    for (const auto& tri : t) {
        const auto n = tri_normal(tri);
        CHECK(n[1] > 0 && n[2] < 0);
        for (const auto& p : tri)
            lo = std::min(lo, p[1]), hi = std::max(hi, p[1]);
    }
    CHECK(std::abs(lo - 2.0f) < 1e-5f && std::abs(hi - 4.0f) < 1e-5f);
    // a box and a wedge: closed, every face outwards
    for (const auto kind : {CollisionShape::box, CollisionShape::wedge}) {
        s.kind = kind;
        t = shape_triangles(s);
        CHECK(t.size() == (kind == CollisionShape::box ? 12u : 8u));
        const std::array<float, 3> inside{1, 2.5f, kind == CollisionShape::box ? 3.0f : 4.0f};
        for (const auto& tri : t) {
            const auto n = tri_normal(tri);
            const std::array<float, 3> mid{(tri[0][0] + tri[1][0] + tri[2][0]) / 3 - inside[0],
                                           (tri[0][1] + tri[1][1] + tri[2][1]) / 3 - inside[1],
                                           (tri[0][2] + tri[1][2] + tri[2][2]) / 3 - inside[2]};
            CHECK(n[0] * mid[0] + n[1] * mid[1] + n[2] * mid[2] > 0);
        }
    }
    // the handles' points and axes: a shape turned a quarter about y has its +z side towards +x
    CollisionShape h;
    h.at = {1, 2, 3};
    h.size = {2, 4, 6};
    h.turn = {0, 90, 0};
    const auto side = shape_point(h, {0, 0.5f, 0.5f});
    CHECK(std::abs(side[0] - 4.0f) < 1e-4f && std::abs(side[1] - 4.0f) < 1e-4f && std::abs(side[2] - 3.0f) < 1e-4f);
    const auto zx = shape_axis(h, 2);
    CHECK(std::abs(zx[0] - 1.0f) < 1e-5f && std::abs(zx[2]) < 1e-5f);
    CHECK(std::abs(shape_axis(h, 1)[1] - 1.0f) < 1e-5f);
    // turned a quarter about y: the ramp's low end towards -x
    s.kind = CollisionShape::ramp;
    s.turn = {0, 90, 0};
    for (const auto& tri : shape_triangles(s))
        CHECK(tri_normal(tri)[0] < 0);
}

TEST(a_rooms_collision_is_edited_by_hand) {
    const RoomCollision game = room();
    RoomCollision c = game;
    CollisionEdits e;
    e.faces[face_key(kCeiling, game.faces[kCeiling][0])].removed = true;  // the ceiling away
    e.faces[face_key(kWall, game.faces[kWall][0])].list = kGround;        // a wall made a ground
    FaceEdit& g = e.faces[face_key(kGround, game.faces[kGround][0])];     // the ground renamed
    g.renamed = true;
    g.names = {"slide"};
    CollisionShape box;
    box.kind = CollisionShape::box;
    box.at = {10, 0, 0};
    e.shapes.push_back(box);
    EditsApplied a;
    std::string why;
    CHECK(apply_collision_edits(&c, game, e, 0x333 / 4096.0f, &a, &why));
    CHECK(a.removed == 1 && a.moved == 1 && a.renamed == 1 && a.unmatched == 0);
    // the box: its top a ground, its four sides walls, its bottom a ceiling
    CHECK(c.faces[kGround].size() == 3 && c.faces[kWall].size() == 5 && c.faces[kCeiling].size() == 1);
    int from_box = 0;
    for (int l = 0; l < 3; ++l) {
        CHECK(a.shape_of[static_cast<std::size_t>(l)].size() == c.faces[static_cast<std::size_t>(l)].size());
        for (const int s : a.shape_of[static_cast<std::size_t>(l)])
            from_box += s == 0 ? 1 : 0;
    }
    CHECK(from_box == 6);
    CHECK(check_tree(c).empty());
    CHECK(check_collision(c, game, true).empty());
    // the renamed ground: no gate any more, the tag in a slot
    bool renamed = false;
    for (const auto& f : c.faces[kGround]) {
        if (f.v == game.faces[kGround][0].v) {
            renamed = f.slots[0] == 0xff && f.slots[1] < c.named.size() && c.named[f.slots[1]].text() == "slide";
        }
    }
    CHECK(renamed);
    CHECK(c.records18 == game.records18 && c.header[0x70] == 2);
    // a shape made all one kind whatever its slope: a ramp's face a wall, a box's six faces grounds
    for (const int forced : {kWall, kGround}) {
        RoomCollision f = game;
        CollisionEdits only;
        CollisionShape s;
        s.kind = forced == kWall ? CollisionShape::ramp : CollisionShape::box;
        s.at = {20, 0, 0};
        s.list = forced;
        only.shapes.push_back(s);
        CHECK(apply_collision_edits(&f, game, only, 0x333 / 4096.0f, &a, &why));
        const std::size_t made = forced == kWall ? 1u : 6u;
        for (int l = 0; l < 3; ++l) {
            const std::size_t mine = static_cast<std::size_t>(std::count(
                a.shape_of[static_cast<std::size_t>(l)].begin(), a.shape_of[static_cast<std::size_t>(l)].end(), 0));
            CHECK(mine == (l == forced ? made : 0u));
        }
        CHECK(check_tree(f).empty() && check_collision(f, game, true).empty());
        for (const auto& w : f.faces[kWall])
            CHECK(w.slots[3] == 0xff || w.slots[3] < f.named.size());  // a wall's slot 3 names a record or nothing
    }
    // an edit naming a face the room does not have: said, nothing changed
    CollisionEdits gone;
    RoomFace elsewhere = face(50, 50, 52, 52);
    gone.faces[face_key(kGround, elsewhere)].removed = true;
    RoomCollision same = game;
    CHECK(apply_collision_edits(&same, game, gone, 0x333 / 4096.0f, &a, &why));
    CHECK(a.unmatched == 1 && write_collision(same) == write_collision(game));
    // the same face whatever corner it is written from
    RoomFace turned = game.faces[kGround][0];
    std::rotate(turned.v.begin(), turned.v.begin() + 2, turned.v.begin() + 4);
    CHECK(face_key(kGround, turned) == face_key(kGround, game.faces[kGround][0]));
}

TEST(a_ray_finds_surfaces_and_a_shape_drops_onto_the_floor) {
    // a floor at height 2 and a step at height 3 (planes, facing up), made as the editor makes them
    const RoomCollision game = room();
    RoomCollision c = game;
    CollisionEdits e;
    CollisionShape floor;
    floor.at = {0, 2, 0};
    floor.size = {20, 1, 20};
    e.shapes.push_back(floor);
    CollisionShape step = floor;
    step.at = {5, 3, 0};
    step.size = {2, 1, 2};
    e.shapes.push_back(step);
    EditsApplied a;
    std::string why;
    CHECK(apply_collision_edits(&c, game, e, 0x333 / 4096.0f, &a, &why));
    // looking down: the floor, its normal up; from below: nothing (a floor is met from above)
    RayHit hit;
    CHECK(cast_ray(c, {-3, 10, 1}, {0, -1, 0}, {}, &hit));
    CHECK(hit.list == kGround && std::abs(hit.at[1] - 2.0) < 1e-6 && hit.normal[1] > 0.99);
    CHECK(std::abs(hit.distance - 8.0) < 1e-6);
    CHECK(!cast_ray(c, {-3, 1, 1}, {0, 1, 0}, {}, nullptr));  // (under it, over the test room's own faces)
    // aslant onto the step, nearer than the floor behind it
    CHECK(cast_ray(c, {5, 10, -7}, {0, -7, 7}, {}, &hit) && std::abs(hit.at[1] - 3.0) < 1e-4);
    // the step's own faces left out: the floor under it
    auto not_step = [&](int l, std::size_t j) { return a.shape_of[static_cast<std::size_t>(l)][j] == 1; };
    CHECK(cast_ray(c, {5, 10, 0}, {0, -1, 0}, not_step, &hit) && std::abs(hit.at[1] - 2.0) < 1e-6);

    // a box over the floor drops onto it; one half over the step rests on the step
    CollisionShape box;
    box.kind = CollisionShape::box;
    box.at = {-5, 6, 0};
    CHECK(drop_to_ground(c, &box, {}));
    CHECK(std::abs(box.at[1] - 2.0f) < 1e-4f);
    box.at = {4, 7, 0};  // from x 3 to 5: its corner over the step (x 4 to 6) meets it first
    CHECK(drop_to_ground(c, &box, {}));
    CHECK(std::abs(box.at[1] - 3.0f) < 1e-4f);
    // sunk into the floor: back up onto it
    box.at = {-5, 1.8f, 0};
    CHECK(drop_to_ground(c, &box, {}));
    CHECK(std::abs(box.at[1] - 2.0f) < 1e-4f);
    // nothing under it: left where it was
    box.at = {100, 5, 100};
    CHECK(!drop_to_ground(c, &box, {}) && box.at[1] == 5.0f);
}

TEST(a_shape_is_fitted_over_faces) {
    auto check_plane_on = [](const std::vector<std::array<float, 3>>& pts, const std::array<float, 3>& n) {
        const CollisionShape s = fit_shape(CollisionShape::plane, pts, n, {0, 0, 0});
        const auto tris = shape_triangles(s);
        CHECK(tris.size() == 2);
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (const auto& t : tris) {
            const auto tn = tri_normal(t);
            const float l = std::sqrt(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);
            CHECK(tn[0] / l * n[0] + tn[1] / l * n[1] + tn[2] / l * n[2] > 0.999f);  // facing the faces' way
            for (const auto& p : t)
                for (int c = 0; c < 3; ++c)
                    lo[c] = std::min(lo[c], p[c]), hi[c] = std::max(hi[c], p[c]);
        }
        float plo[3] = {1e9f, 1e9f, 1e9f}, phi[3] = {-1e9f, -1e9f, -1e9f};
        for (const auto& p : pts)
            for (int c = 0; c < 3; ++c)
                plo[c] = std::min(plo[c], p[c]), phi[c] = std::max(phi[c], p[c]);
        for (int c = 0; c < 3; ++c)  // lying where they lie, as large
            CHECK(std::abs(lo[c] - plo[c]) < 1e-3f && std::abs(hi[c] - phi[c]) < 1e-3f);
    };
    // a wall facing +x at x 5, a wall facing -z, a floor, a ceiling, a slope rising along +x (facing -x, up)
    check_plane_on({{5, 0, -1}, {5, 0, 1}, {5, 2, 1}, {5, 2, -1}}, {1, 0, 0});
    check_plane_on({{-3, 1, 7}, {3, 1, 7}, {3, 4, 7}, {-3, 4, 7}}, {0, 0, -1});
    check_plane_on({{-2, 3, -4}, {2, 3, -4}, {2, 3, 4}, {-2, 3, 4}}, {0, 1, 0});
    check_plane_on({{-2, 6, -4}, {2, 6, -4}, {2, 6, 4}, {-2, 6, 4}}, {0, -1, 0});
    const float k = 1.0f / std::sqrt(2.0f);
    check_plane_on({{0, 0, 0}, {0, 0, 3}, {2, 2, 3}, {2, 2, 0}}, {-k, k, 0});
    // a ramp over steps rising along +x: turned to rise that way, spanning them
    const CollisionShape ramp =
        fit_shape(CollisionShape::ramp, {{0, 0, 0}, {0, 0, 2}, {4, 2, 2}, {4, 2, 0}}, {0, 1, 0}, {0, 0, 0});
    CHECK(ramp.turn[1] == 90.0f && ramp.size[2] == 4.0f && ramp.size[0] == 2.0f && ramp.size[1] == 2.0f);
    // nothing selected: at the fallback point
    const CollisionShape none = fit_shape(CollisionShape::box, {}, {0, 1, 0}, {7, 8, 9});
    CHECK(none.at[0] == 7.0f && none.at[2] == 9.0f);
}

TEST(collision_edits_read_back_as_written) {
    const RoomCollision game = room();
    RoomReplacement r;
    r.world = "zz";
    r.room = 0;
    r.keep_collision = true;
    r.edits.faces[face_key(kCeiling, game.faces[kCeiling][0])].removed = true;
    FaceEdit& w = r.edits.faces[face_key(kWall, game.faces[kWall][1])];
    w.list = kGround;
    w.renamed = true;
    w.names = {"slide", "nocam"};
    CollisionShape s;
    s.kind = CollisionShape::wedge;
    s.at = {1.5f, -2, 3.25f};
    s.turn = {0, 90, 0};
    s.size = {2, 0.5f, 4};
    s.both_sides = true;
    s.names = {"gate02"};
    r.edits.shapes.push_back(s);
    r.edits.shapes.push_back(CollisionShape{});
    s.list = kCeiling;
    s.at = {123.456787f, -0.1f, 1e-5f};  // a dragged shape's place: every digit kept
    s.turn = {0, 33.3333321f, 0};
    r.edits.shapes.push_back(s);
    const auto dir = studio_test::scratch("collision_edits_text");
    const auto file = dir / "0.txt";
    studio_test::write(file, change_text(r));
    RoomReplacement back;
    std::string why;
    CHECK(read_change(file, &back, &why));
    CHECK(back.edits == r.edits);
    CHECK(back.edits.shapes[1].list == -1 && back.edits.shapes[2].list == kCeiling);
    CHECK(markers_only(back));
    CHECK(change_text(back) == change_text(r));
    // wrong lines said, with where
    studio_test::write(file, change_text(r) + "shape cone one - 0 0 0 0 0 0 1 1 1\n");
    CHECK(!read_change(file, &back, &why) && why.find("line") != std::string::npos);
    studio_test::write(file, change_text(r) + "face-remove floor 3 0 0 0 1 0 0 1 0 1\n");
    CHECK(!read_change(file, &back, &why));
}

TEST(an_edited_collision_is_saved_into_a_mod) {
    namespace fs = std::filesystem;
    const auto dir = studio_test::scratch("collision_edits_save");
    const RoomCollision game = room();
    const Bytes original = write_collision(game);
    studio_test::write(dir / "nitrofs" / "mi" / "wd" / "wd_zz", [&] {
        const Bytes f = p2({room_table(), gate_models(), original, Bytes(64, 0x11), Bytes()});
        return std::string(f.begin(), f.end());
    }());
    const fs::path mod = dir / "mods" / "Test";
    RoomReplacement r;
    r.world = "zz";
    r.room = 0;
    r.keep_collision = true;
    r.edits.faces[face_key(kCeiling, game.faces[kCeiling][0])].removed = true;
    CollisionShape ramp;
    ramp.kind = CollisionShape::ramp;
    ramp.at = {0, 0, 10};
    ramp.size = {4, 1, 4};
    r.edits.shapes.push_back(ramp);
    WorldOutcome o;
    std::string why;
    CHECK(save_change(dir / "nitrofs", dir / "dump", mod, r, {}, {}, &o, &why));
    World w;
    CHECK(load_world(mod / "files", "zz", &w, &why));
    RoomCollision made;
    CHECK(parse_collision(w.archive.entries[2].data, &made, &why));
    CHECK(made.faces[kCeiling].empty() && made.faces[kGround].size() == 2 && made.faces[kWall].size() == 2);
    CHECK(check_tree(made).empty());
    CHECK(w.archive.entries[3].data == Bytes(64, 0x11));
    std::error_code ec;
    CHECK(!fs::exists(mod / "graphics", ec));
    // what the window shows is what was saved
    World game_world;
    CHECK(load_world(dir / "nitrofs", "zz", &game_world, &why));
    RoomCollision shown;
    CHECK(kept_collision(game_world, r, &shown, nullptr, &why) && write_collision(shown) == write_collision(made));
    // given back
    CHECK(revert_change(dir / "nitrofs", dir / "dump", mod, "zz", 0, &o, &why));
    CHECK(!fs::exists(mod / "files" / "mi" / "wd" / "wd_zz", ec));
}

TEST(a_model_made_from_the_room_lines_up_with_it_again) {
    // the room's points: an L of points on the floor and up a wall
    std::vector<std::array<float, 3>> room;
    for (int i = 0; i < 20; ++i) {
        room.push_back({static_cast<float>(i) * 0.5f, 0.0f, 0.0f});
        room.push_back({0.0f, static_cast<float>(i) * 0.25f, 1.0f});
        room.push_back({3.0f, 0.0f, static_cast<float>(i) * 0.3f});
    }
    // the model: the same points, turned -90 degrees about x (as a 3D program's root node can) and in centimetres
    khdays::assets::ImportedModel model;
    khdays::assets::NeutralMesh mesh;
    for (const auto& q : room) {
        khdays::assets::NeutralVertex v;
        v.position = {q[0] * 100.0f, q[2] * 100.0f, -q[1] * 100.0f};  // y up becomes z, scaled by 100
        mesh.vertices.push_back(v);
    }
    model.model.meshes.push_back(mesh);
    const RoomAlignment a = align_with_room(room, model);
    CHECK(a.share > 0.99 && a.verdict == RoomAlignment::turned &&
          a.as_is < 0.05);  // as it is, only the origin falls on the room
    CHECK(std::fabs(a.place.scale - 0.01f) < 1e-6f);
    // under that placement the model's points are the room's
    khdays::assets::ImportedModel placed = model;
    place_model(&placed, a.place);
    CHECK(std::fabs(placed.model.meshes[0].vertices[1].position[1] - room[1][1]) < 1e-3f);
    CHECK(std::fabs(placed.model.meshes[0].vertices[1].position[2] - room[1][2]) < 1e-3f);
    // nothing in common: no share
    khdays::assets::ImportedModel other;
    khdays::assets::NeutralMesh far;
    khdays::assets::NeutralVertex v;
    v.position = {123.4f, 56.7f, 89.1f};
    far.vertices.push_back(v);
    other.model.meshes.push_back(far);
    CHECK(align_with_room(room, other).verdict == RoomAlignment::unrelated);
}
