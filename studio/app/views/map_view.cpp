// The map editor: a room of a mission world as the game has it -- its models, collision, markers and
// exits, any face or marker picked to read what it is -- and replaced by the modder's model, its
// collision made and shown as the game will read it (docs/STUDIO.md, "Rooms").
#include "../app.h"
#include "../save_dialog.h"

#include "core/models/bones.h"
#include "core/models/game_model.h"
#include "core/models/model_files.h"
#include "core/files/paths.h"
#include "core/rooms/collision_edits.h"
#include "core/rooms/kh2_source.h"
#include "core/rooms/room_change.h"
#include "mods/room_animation.h"  // port/runtime: as the port moves a room
#include "core/rooms/room_export.h"
#include "core/mods/settings.h"
#include "core/rooms/worlds.h"
#include "../preview3d.h"

#include "khdays/vfs/filesystem.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <SDL3/SDL.h>

extern "C" {
#include "render/png_io.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>

namespace studio {

namespace {

namespace assets = khdays::assets;

constexpr float kFx = 1.0f / 4096.0f;  // the game's 20.12 to units

struct Texture {
    std::vector<std::uint8_t> rgba;
    int width = 0, height = 0;
};

struct RoomModel {
    int member = 0;
    std::string key;      // the dump's name, empty when not found
    std::string problem;  // why it is not drawn
    GameModel game;
    std::vector<Mat> palette;
};

// A replacement being made (or the mod's, opened): what the modder gave,
// placed, and the collision made from it.
struct Replacing {
    bool active = false;
    bool from_mod = false;  // the mod holds a change for this room
    RoomReplacement r;
    fs::path model_source, collision_source;   // the files as given (or the mod's copies)
    assets::ImportedModel raw, raw_collision;  // as read
    assets::ImportedModel placed;              // placed: what is drawn
    RoomPlacement placed_with;                 // where `placed` was put (drawn moved to the place now while dragged)
    SurfaceMap surfaces;                       // its floors and roofs from above, as placed_with puts them
    bool dirty = false, built_ok = false;
    RoomCollision built;
    CollisionReport report;
    std::vector<RoomWarning> warnings;
    std::string error, message;
    int view = 0;  // 0 yours, 1 the game's, 2 both
    bool next_is_collision = false;
    bool other_collision = false;  // the collision from another model, chosen (its file perhaps not yet)
    bool kh2_collision = false;    // that model the KH2 map's own floors and walls
    std::string placed_text;       // how the model was placed (lined up, as it is, or to place by hand)
    bool placed_ok = true;
    // the collision edited by hand (r.edits) over `base` (the game's kept, or the model's made): `built` is
    // base with the edits and markers; made again from base alone when only the edits changed
    RoomCollision base;
    bool edits_dirty = false;
    EditsApplied applied;
    std::map<FaceKey, FaceKey> shown_to_base;  // a face moved to another kind, as shown -> as the edit names it
    std::string edit_error;
    // what Undo and Redo step through: the hand edits, the markers moved and the exits' models moved
    struct UndoStep {
        CollisionEdits edits;
        std::map<std::string, std::array<std::int32_t, 4>> markers;
        std::map<std::pair<int, int>, std::array<std::int32_t, 3>> gate_points;
    };
    std::deque<UndoStep> undo, redo;
    std::deque<UndoStep> parked_redo;  // Redo as it was before the last step kept, given back if it is taken back
};

struct MapView {
    int item = -1;
    int mod = -1;  // the mod chosen when the room was opened
    std::string error;
    World world;
    const WorldRoom* room = nullptr;
    bool has_collision = false;
    RoomCollision collision;
    std::vector<Marker> markers;
    std::vector<RoomModel> models;
    std::map<std::string, Texture> textures;

    // as the game shows it first; the collision laid over it when asked
    bool show_models = true, show_ground = false, show_walls = false, show_ceilings = false, show_markers = true;
    float yaw = 30.0f, pitch = 35.0f, distance = 40.0f;
    std::array<float, 3> target{0, 0, 0};
    // a mission's point asked for (See spawner, See chest) ringed, breathing, for kRingSeconds (since < 0:
    // none); the camera stays where it is
    double ring_since = -1;
    std::array<float, 3> ring_at{};
    int ring_kind = 0;
    int picked_list = -1, picked_face = -1, picked_marker = -1;
    // an exit's model picked in the picture: its link key and which of its five models
    bool picked_gate = false;
    int picked_gate_key = 0, picked_gate_part = 0;
    std::unique_ptr<Preview> preview;
    Replacing rep;
    bool open_replace = false;  // the mod's replacement just opened: its panel shown open
    // the picked marker's fields while one is held (dragged or typed in): kept from frame to frame, since the
    // marker itself only moves when let go (the room is made again then)
    struct MarkerEdit {
        std::string name;
        bool held = false;
        int held_frame = -10;  // the last frame either of its fields (the panel's, the bar's) was held
        float at[3] = {0, 0, 0};
        float facing = 0;
    } marker_edit;
    // a mission's point picked in the picture (the mission open beside the room): its kind and its row
    int picked_point_kind = -1, picked_point = -1;
    // the collision editor: picking in the picture selects faces (as shown) and shapes
    bool editing = false;
    std::set<FaceKey> selected;
    int picked_shape = -1;
    // what the pointer was over in the last frame, while picking (drawn lighter): a face, and its shape
    int hover_list = -1, hover_face = -1, hover_shape = -1;
    bool box_on = false;  // a Shift+drag box being drawn from box_from
    ImVec2 box_from{0, 0};
    std::string names_text, shape_names_text;
    FaceKey names_for;  // the face names_text was filled from
    int shape_names_for = -1;
    // the picked shape's handles: the mode (0 move, 1 resize, 2 turn), the handle dragged (-1: none) and
    // what the drag started from (the shape, the pointer, the handle's pixels per unit of the game's)
    int gizmo_mode = 0;
    int gizmo_drag = -1;
    bool snap_surface = false;  // the Move square puts the shape on the surface under the pointer
    CollisionShape gizmo_start;
    ImVec2 gizmo_from{0, 0}, gizmo_last{0, 0}, gizmo_centre{0, 0};
    ImVec2 gizmo_pu[2]{};
    float gizmo_turned = 0, gizmo_sign = 1;
    // the picked marker's, exit model's or mission point's handles (shown on click, apart from the shapes'
    // modes): which handle is dragged (0-2 an axis, 3 over the ground, 20 the facing ring), what it moves
    // (0 a room marker, 1 an exit's model, 2 a mission point) and where it was when the drag began
    int point_drag = -1, point_what = -1;
    ImVec2 point_from{0, 0}, point_last{0, 0}, point_centre{0, 0};
    ImVec2 point_pu[2]{};
    float point_turned = 0, point_sign = 1;
    std::array<float, 3> point_start{};
    float point_start_facing = 0;
    bool point_gate_had = false;  // an exit's model: whether the change moved it before the drag
    std::array<std::int32_t, 3> point_gate_was{};
};

MapView m;

// KH2's rooms of the player's collection, listed and made on a thread of their own (the archives are read
// by the installer's reader in Python), what it made taken by the window once done. Kept across rooms.
struct Kh2Picker {
    bool shown = false;
    int world = -1;  // in kKh2Worlds
    std::vector<std::string> maps;
    std::map<std::string, bool> out;  // a map taken out of the copy already
    std::map<std::string, assets::DecodedTexture> radars;
    std::string typed, error;
    std::vector<Kh2Pair> pairs;  // the studio's, then the mod's
    int pairs_for = -2;          // the mod they were read for
    fs::path collection;
    std::string collection_for = "\x01";  // the setting `collection` was found for
    std::thread worker;
    std::atomic<bool> busy{false};
    std::string working;  // the map being made; empty while listing
    // left by the worker
    std::mutex mutex;
    bool listed = false, made = false;
    std::vector<std::string> found;
    std::string done_map, failed;
    fs::path model;
    Kh2Made outcome;
    ~Kh2Picker() {
        if (worker.joinable())
            worker.join();
    }
};
Kh2Picker kh2;

// The collision drawn and picked: the one made when the modder's is shown
// (the game's when it is kept).
const RoomCollision& shown() {
    return m.rep.active && m.rep.view != 1 && m.rep.built_ok ? m.rep.built : m.collision;
}

bool showing_built() {
    return m.rep.active && m.rep.view != 1 && m.rep.built_ok;
}

// The names a face or material may be given: the room's own, then the surface tags.
std::vector<std::string> name_choices() {
    std::vector<std::string> out;
    for (const auto& n : m.collision.named)
        out.push_back(n.text());
    for (const char* t : {"nohit", "nocatch", "sea", "nocam", "slide", "sea2", "sicon"}) {
        if (std::find(out.begin(), out.end(), t) == out.end())
            out.push_back(t);
    }
    return out;
}

// The materials of the model the collision is made from, as the builder names them.
std::vector<std::string> collision_materials() {
    const assets::ImportedModel& model = m.rep.collision_source.empty() ? m.rep.raw : m.rep.raw_collision;
    std::vector<std::string> out;
    for (const auto& mesh : model.model.meshes) {
        const std::string name = !mesh.material_name.empty() ? mesh.material_name : mesh.texture_name;
        if (std::find(out.begin(), out.end(), name) == out.end())
            out.push_back(name);
    }
    return out;
}

fs::path nitrofs() {
    return khdays::vfs::data_root() / "nitrofs";
}

const Texture* texture(const GameModel& g, const std::string& name) {
    const std::string key = g.label + "|" + name;
    const auto found = m.textures.find(key);
    if (found != m.textures.end())
        return found->second.rgba.empty() ? nullptr : &found->second;
    Texture t;
    const auto file = g.textures.find(name);
    if (file != g.textures.end()) {
        const char* error = nullptr;
        std::uint8_t* px = khdays_png_read(u8(file->second).c_str(), &t.width, &t.height, &error);
        if (px != nullptr) {
            t.rgba.assign(px, px + static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height) * 4u);
            khdays_png_free(px);
        }
    }
    const auto it = m.textures.emplace(key, std::move(t)).first;
    return it->second.rgba.empty() ? nullptr : &it->second;
}

// The kind a named record is, by its name.
S named_kind(const std::string& name) {
    static const char* const tags[] = {"nohit", "nocatch", "sea", "nocam", "slide", "sea2", "sicon"};
    for (const char* t : tags) {
        if (name == t)
            return S::map_kind_tag;
    }
    if (name.rfind("gate", 0) == 0)
        return S::map_kind_gate;
    if (name.rfind("col_wall", 0) == 0)
        return S::map_kind_wall;
    return S::map_kind_other;
}

// What reads a surface tag, as found in the game's code (function names: not translated).
const char* tag_readers(const std::string& name) {
    if (name == "nohit")
        return "Ov022_SettlePointOnGround, Ov022_CastMove, Ov022_MayActOnHit, Ov022_StepChargeSequence";
    if (name == "nocatch")
        return "Ov022_FindClimbTarget";
    if (name == "sea" || name == "sea2")
        return "Ov022_StepGlideState";
    if (name == "slide")
        return "Ov022_StepGlideState, Ov022_StepJumpState";
    return nullptr;
}

std::string names_of(int list, const RoomFace& f) {
    std::string out;
    for (std::size_t p = 0; p < 4; ++p) {
        const auto s = f.slots[p];
        if (s == 0xff)
            continue;
        if (!slot_names(list, p)) {
            out += (out.empty() ? "" : ", ") + std::string("kind ") + std::to_string(s);
            continue;
        }
        if (s >= shown().named.size())
            continue;
        out += (out.empty() ? "" : ", ") + shown().named[s].text();
    }
    return out;
}

constexpr double kRingSeconds = 5.0;

// A point the mission view asked to see (MissionOverlay::focus): once its room is the one shown, ringed for a
// few seconds. The camera is not moved: one look at spawner after spawner keeps the view the person chose.
void ring_focused_point() {
    MissionOverlay& mo = mission_overlay();
    if (!mo.focus || m.room == nullptr || mo.world != m.world.code || mo.room != m.room->index)
        return;
    mo.focus = false;
    m.ring_since = ImGui::GetTime();
    m.ring_at = {static_cast<float>(mo.focus_at[0]) * kFx, static_cast<float>(mo.focus_at[1]) * kFx,
                 static_cast<float>(mo.focus_at[2]) * kFx};
    m.ring_kind = mo.focus_kind;
}

void frame_camera() {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& list : shown().faces) {
        for (const auto& f : list) {
            for (std::size_t v = 0; v < f.corners && v < 4; ++v) {
                for (std::size_t c = 0; c < 3; ++c) {
                    lo[c] = std::min(lo[c], static_cast<float>(f.v[v][c]) * kFx);
                    hi[c] = std::max(hi[c], static_cast<float>(f.v[v][c]) * kFx);
                }
            }
        }
    }
    if (lo[0] > hi[0]) {  // no collision: around the models' nodes
        for (const auto& rm : m.models) {
            for (const auto& w : rm.game.rest) {
                for (std::size_t c = 0; c < 3; ++c) {
                    lo[c] = std::min(lo[c], w[12 + c]);
                    hi[c] = std::max(hi[c], w[12 + c]);
                }
            }
        }
    }
    if (lo[0] > hi[0])
        return;
    m.target = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    m.distance = std::max({hi[0] - lo[0], hi[2] - lo[2], 2.0f}) * 1.3f;
}

void open(App& app) {
    history_clear(EditOwner::room);  // the steps were of the room shown before
    const Item& item = app.catalog.items[static_cast<std::size_t>(app.selected)];
    MapView fresh;
    fresh.item = app.selected;
    fresh.mod = app.mod;
    fresh.preview = std::move(m.preview);
    m = std::move(fresh);
    // `mi/wd/wd_<code>/room/<index>`
    const auto at = item.key.rfind("/room/");
    const std::string file = item.group;
    const std::string code = file.size() > 9 ? file.substr(9) : std::string();
    if (at == std::string::npos || !load_world(nitrofs(), code, &m.world, &m.error)) {
        if (m.error.empty())
            m.error = "not a room: " + item.key;
        return;
    }
    const int index = std::atoi(item.key.c_str() + at + 6);
    if (index < 0 || static_cast<std::size_t>(index) >= m.world.rooms.size()) {
        m.error = "no room " + std::to_string(index);
        return;
    }
    m.room = &m.world.rooms[static_cast<std::size_t>(index)];
    const auto& data = m.world.archive.entries[static_cast<std::size_t>(m.room->collision_member)].data;
    if (!data.empty()) {
        std::string why;
        m.has_collision = parse_collision(data, &m.collision, &why);
        if (!m.has_collision)
            m.error = why;
        else
            m.markers = markers(m.collision);
    }
    // the room's models: its objects, as the graphics dump names them
    const fs::path dump = resolve(app.settings, app.settings.graphics_dump);
    for (const auto& o : m.room->objects) {
        RoomModel rm;
        rm.member = o.member;
        if (o.member >= m.world.archive.entries.size() || m.world.archive.entries[o.member].data.empty()) {
            rm.problem = fmt(app.language, S::map_model_empty, {std::to_string(o.member)});
            m.models.push_back(std::move(rm));
            continue;
        }
        const std::string prefix = file + "#" + std::to_string(o.member) + "/mdl/";
        for (const auto& it : app.catalog.items) {
            if (it.kind == Kind::model && it.key.rfind(prefix, 0) == 0) {
                rm.key = it.key;
                break;
            }
        }
        std::string why;
        if (rm.key.empty()) {
            rm.problem = fmt(app.language, S::map_model_missing, {prefix});
        } else if (!load_game_model(nitrofs(), dump, rm.key, &rm.game, &why)) {
            rm.problem = rm.key + ": " + why;
        } else {
            std::vector<Mat> node_world;
            pose_game_model(rm.game, nullptr, 0.0f, &node_world, &rm.palette);
        }
        m.models.push_back(std::move(rm));
    }
    frame_camera();
    // the mod's replacement of this room, opened to change it
    if (const Mod* mod = app.current_mod()) {
        const fs::path change = change_file(mod->dir, m.world.code, index);
        std::error_code ec;
        if (fs::exists(change, ec)) {
            Replacing& p = m.rep;
            std::string why;
            if (!read_change(change, &p.r, &why)) {
                p.error = why;
                return;
            }
            p.active = p.from_mod = true;
            m.open_replace = true;
            const fs::path folder = change_models(mod->dir, m.world.code, index);
            if (!markers_only(p.r)) {
                p.model_source = folder / path_u8(p.r.model);
                if (!import_model(p.model_source, &p.raw, &why))
                    p.error = why;
            }
            if (!p.r.collision.empty()) {
                p.collision_source = folder / path_u8(p.r.collision);
                if (!import_model(p.collision_source, &p.raw_collision, &why))
                    p.error = why;
                p.kh2_collision = !p.r.kh2.empty() && p.r.collision == p.r.kh2 + "_collision.gltf";
            }
            p.dirty = p.error.empty();
        }
    }
}

// The hand edits and markers over the base collision, as the save applies them.
void reapply_edits() {
    Replacing& p = m.rep;
    p.edits_dirty = false;
    p.edit_error.clear();
    p.built = p.base;
    p.applied = EditsApplied{};
    p.shown_to_base.clear();
    if (!p.r.edits.empty()) {
        std::string why;
        if (!apply_collision_edits(&p.built, m.collision, p.r.edits, p.r.ground_limit, &p.applied, &why)) {
            p.edit_error = why;
            p.built = p.base;
            p.applied = EditsApplied{};
        } else {
            const std::string problem = check_collision(p.built, m.collision, true);
            if (!problem.empty())
                p.edit_error = problem;
        }
        for (const auto& [key, fe] : p.r.edits.faces) {
            if (!fe.removed && fe.list >= 0 && fe.list != key.list) {
                FaceKey shown_key = key;
                shown_key.list = fe.list;
                p.shown_to_base[shown_key] = key;
            }
        }
    }
    for (int l = 0; l < 3; ++l) {
        auto& of = p.applied.shape_of[static_cast<std::size_t>(l)];
        if (of.size() != p.built.faces[static_cast<std::size_t>(l)].size())
            of.assign(p.built.faces[static_cast<std::size_t>(l)].size(), -1);
    }
    move_markers(&p.built, p.r.markers);
}

// The game's own floor and walls as the change keeps them, its markers moved, as the save writes them;
// the rooms sharing that collision said only when a marker moved (else nothing of it changes).
void kept_room_built() {
    Replacing& p = m.rep;
    p.base = m.collision;
    reapply_edits();
    p.report = CollisionReport{};
    p.built_ok = true;
    p.r.world = m.world.code;
    p.r.room = m.room->index;
    for (auto& more : sharing_warnings(m.world, p.r)) {
        if (more.kind != RoomWarning::collision_shared || !p.r.markers.empty())
            p.warnings.push_back(std::move(more));
    }
}

// The modder's model placed again and its collision made, as the save will.
void rebuild() {
    Replacing& p = m.rep;
    p.dirty = false;
    p.built_ok = false;
    p.error.clear();
    p.warnings.clear();
    if (p.active && p.model_source.empty()) {
        // the game's room kept as it looks and plays: only its markers (and exits' models) moved
        p.placed = {};
        p.surfaces = {};
        if (!m.has_collision) {
            p.error = "this room has no collision in the game's data, so no markers to move";
            return;
        }
        kept_room_built();
        return;
    }
    if (p.model_source.empty())
        return;
    p.placed = p.raw;
    place_model(&p.placed, p.r.place);
    p.placed_with = p.r.place;
    if (!m.has_collision) {
        p.error = "this room has no collision in the game's data to replace";
        return;
    }
    // only what lies around the room, as the save keeps it
    const RoomZone zone = p.r.zone >= 0 ? room_zone(m.collision, p.r.zone) : RoomZone{};
    if (p.r.zone >= 0)
        cut_to_zone(&p.placed, zone);
    p.surfaces = surface_map(p.placed);
    assets::ImportedModel collision = p.collision_source.empty() ? p.placed : p.raw_collision;
    if (!p.collision_source.empty()) {
        place_model(&collision, p.r.place);
        if (p.r.zone >= 0)
            cut_to_zone(&collision, zone);
    }
    if (p.r.keep_collision) {
        // only the look replaced: the game's own floor and walls are the ones shown and saved
        kept_room_built();
        return;
    }
    CollisionRules rules;
    rules.ground_limit = p.r.ground_limit;
    rules.names_by_material = p.r.names_by_material;
    rules.names_by_triangle = p.r.names_by_triangle;
    rules.drop_original = p.r.drop_original;
    rules.markers = p.r.markers;
    if (!build_collision(collision_source(collision, p.collision_source.empty()), m.collision, rules, &p.built,
                         &p.report, &p.error)) {
        return;
    }
    const std::string problem = check_collision(p.built, m.collision);
    if (!problem.empty()) {
        p.error = "the collision made is not one the game reads: " + problem;
        return;
    }
    p.built_ok = true;
    p.base = p.built;
    reapply_edits();
    p.warnings = p.report.warnings;
    p.r.world = m.world.code;
    p.r.room = m.room->index;
    for (auto& more : sharing_warnings(m.world, p.r))
        p.warnings.push_back(std::move(more));
}

// The shape a face shown was made of, -1 for the room's own.
int shape_of_shown(int list, std::size_t j) {
    if (!showing_built() || list < 0 || list > 2)
        return -1;
    const auto& of = m.rep.applied.shape_of[static_cast<std::size_t>(list)];
    return j < of.size() ? of[j] : -1;
}

// The face an edit names, from the face as shown (moved to another kind, it is named by its first).
FaceKey base_key(const FaceKey& shown_key) {
    const auto it = m.rep.shown_to_base.find(shown_key);
    return it != m.rep.shown_to_base.end() ? it->second : shown_key;
}

std::array<float, 3> corner(const RoomFace& f, std::size_t v) {
    return {static_cast<float>(f.v[v][0]) * kFx, static_cast<float>(f.v[v][1]) * kFx,
            static_cast<float>(f.v[v][2]) * kFx};
}

std::array<float, 3> face_tint(int list, const RoomFace& f) {
    bool gate = false, wall = false, tag = false;
    for (std::size_t p = 0; p < 4; ++p) {
        const auto s = f.slots[p];
        if (s == 0xff || s >= shown().named.size() || !slot_names(list, p))
            continue;
        const S kind = named_kind(shown().named[s].text());
        gate = gate || kind == S::map_kind_gate;
        wall = wall || kind == S::map_kind_wall;
        tag = tag || kind == S::map_kind_tag;
    }
    if (gate)
        return {0.95f, 0.85f, 0.2f};
    if (wall)
        return {0.9f, 0.3f, 0.9f};
    if (tag)
        return {0.3f, 0.9f, 0.9f};
    return list == kGround ? std::array<float, 3>{0.35f, 0.85f, 0.35f}
           : list == kWall ? std::array<float, 3>{0.95f, 0.55f, 0.25f}
                           : std::array<float, 3>{0.4f, 0.55f, 1.0f};
}

// Where a marker stands on the modder's room: on one of its floors, over it with nothing near above, or under
// one of its surfaces (in it); `to` the point of that floor or surface over or under it.
enum class Footing { none, on, floating, under };

Footing footing(const std::array<float, 3>& q, std::array<float, 3>* to) {
    const Replacing& p = m.rep;
    // the marker in the room as it was placed (the model may be dragged since)
    const auto b = placed_point(p.placed_with, unplaced_point(p.r.place, q));
    float below = -1e30f, above = 1e30f;
    for (const auto& h : p.surfaces.heights_at(b[0], b[2])) {
        if (h.up && h.y <= b[1] + 0.05f)
            below = std::max(below, h.y);
        if (h.y > b[1] + 0.05f)
            above = std::min(above, h.y);
    }
    const bool has_below = below > -1e29f, has_above = above < 1e29f;
    auto live = [&](float y) { return placed_point(p.r.place, unplaced_point(p.placed_with, {b[0], y, b[2]})); };
    if (has_below && b[1] - below <= 0.35f) {
        *to = live(below);
        return Footing::on;
    }
    if (has_above && (!has_below || above - b[1] < 2.0f)) {
        *to = live(above);
        return Footing::under;
    }
    if (has_below) {
        *to = live(below);
        return Footing::floating;
    }
    return Footing::none;
}

// Where vertex v is drawn when its joints are moved as `moved` says (palette entry: matrix); as it is
// when none of its joints moves.
std::array<float, 3> skinned(const std::map<std::uint32_t, Mat>& moved, const assets::NeutralVertex& v) {
    std::array<float, 3> pos{0, 0, 0};
    float total = 0.0f;
    for (std::size_t k = 0; k < 4; ++k) {
        const auto found = moved.find(v.joints[k]);
        if (v.weights[k] <= 0.0f || found == moved.end())
            continue;
        const auto q = mat_point(found->second, v.position);
        for (std::size_t c = 0; c < 3; ++c)
            pos[c] += v.weights[k] * q[c];
        total += v.weights[k];
    }
    if (total <= 0.0f)
        return v.position;
    for (auto& c : pos)
        c /= total;
    return pos;
}

// the collision editor's (below): undo, the selected faces made a shape, its edits to be applied again
void edit_push();
void edit_unpush();
void undo_field();
void start_kept_change();
void draw_convert_buttons(App& app);
void edits_changed();

void draw_room(App& app, ImVec2 size) {
    if (!m.preview)
        m.preview = std::make_unique<Preview>(app.device);
    Preview& p = *m.preview;
    p.collect();
    const int w = std::max(64, static_cast<int>(size.x)), h = std::max(64, static_cast<int>(size.y));
    ring_focused_point();
    const Mat vp = orbit_view_projection(m.target, m.yaw, m.pitch, m.distance,
                                         static_cast<float>(w) / static_cast<float>(h), app.settings.map_fov);
    p.begin(w, h, vp, orbit_eye_direction(m.yaw, m.pitch));

    const bool yours = m.rep.active && m.rep.view != 1 && !m.rep.model_source.empty();
    const bool games = !yours || m.rep.view == 2;
    if (m.show_models) {
        const auto uses = m.rep.active ? object_uses(*m.room, m.rep.r) : std::vector<std::pair<int, ObjectUse>>{};
        for (const auto& rm : m.models) {
            if (!rm.problem.empty())
                continue;
            bool kept = games;
            for (const auto& [member, use] : uses)
                kept = kept || (member == rm.member && use == ObjectUse::keep);
            if (!kept)
                continue;
            for (const auto& mesh : rm.game.model.meshes) {
                if (mesh.material_alpha <= 0.0f)
                    continue;  // the DS draws nothing of it
                const Texture* t = mesh.texture_name.empty() ? nullptr : texture(rm.game, mesh.texture_name);
                PreviewImage image;
                if (t != nullptr)
                    image = {t->rgba.data(), t->width, t->height, mesh.repeat, mesh.mirror};
                const float tw = t != nullptr ? static_cast<float>(t->width) : 1.0f;
                const float th = t != nullptr ? static_cast<float>(t->height) : 1.0f;
                auto vertex = [&](const assets::NeutralVertex& v) {
                    PreviewVertex o;
                    std::array<float, 3> pos{0, 0, 0}, nrm{0, 0, 0};
                    for (int k = 0; k < 4; ++k) {
                        const float wgt = v.weights[static_cast<std::size_t>(k)];
                        const std::uint32_t jt = v.joints[static_cast<std::size_t>(k)];
                        if (wgt <= 0.0f || jt >= rm.palette.size())
                            continue;
                        const auto q = mat_point(rm.palette[jt], v.position);
                        const auto n = mat_vec(rm.palette[jt], v.normal);
                        for (std::size_t c = 0; c < 3; ++c)
                            pos[c] += wgt * q[c], nrm[c] += wgt * n[c];
                    }
                    o.position = pos;
                    o.normal = nrm;
                    o.uv = {v.texcoord[0] / tw, v.texcoord[1] / th};
                    // the vertex colour the room is lit with, its material's alpha
                    o.colour = {v.color[0] / 255.0f, v.color[1] / 255.0f, v.color[2] / 255.0f,
                                v.color[3] / 255.0f * mesh.material_alpha};
                    return o;
                };
                for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                    p.triangle(vertex(mesh.vertices[mesh.indices[i]]), vertex(mesh.vertices[mesh.indices[i + 1]]),
                               vertex(mesh.vertices[mesh.indices[i + 2]]), image, !mesh.double_sided, {1, 1, 1});
                }
            }
        }
        if (yours) {
            // the modder's, placed: as the port draws it (its vertex colours, its textures), moving by itself
            // as the port moves it, by the studio's clock at 60 frames a second
            const assets::ImportedModel& placed = m.rep.placed;
            // while its scale, turn or move is dragged: shown where the place now puts it
            const bool moving = !same_place(m.rep.placed_with, m.rep.r.place);
            const auto frames = static_cast<std::uint64_t>(SDL_GetTicks() * 60 / 1000);
            std::map<std::uint32_t, Mat> moved;  // a joint's palette entry: its node now times its inverse bind
            if (!placed.joints.empty()) {
                const auto worlds =
                    khdays::room::node_worlds(placed.nodes, placed.animations, static_cast<double>(frames) / 60.0);
                for (const auto& j : placed.joints) {
                    const bool on_node = j.node >= 0 && static_cast<std::size_t>(j.node) < worlds.size();
                    moved[j.palette_index] =
                        mat_mul(on_node ? worlds[static_cast<std::size_t>(j.node)] : j.world, j.inverse_bind);
                }
            }
            for (const auto& mesh : placed.model.meshes) {
                std::string shown = mesh.texture_name;
                if (!mesh.texture_frames.empty()) {
                    shown = mesh.texture_frames[khdays::room::frame_at(mesh.texture_frames, mesh.texture_frames_loop,
                                                                       mesh.texture_frames_loop_from, frames)]
                                .first;
                }
                const assets::DecodedTexture* t = nullptr;
                for (const auto& [name, decoded] : placed.textures) {
                    if (name == shown && !decoded.rgba.empty())
                        t = &decoded;
                }
                PreviewImage image;
                if (t != nullptr)
                    image = {t->rgba.data(), t->width, t->height};
                const float tw = t != nullptr ? static_cast<float>(t->width) : 1.0f;
                const float th = t != nullptr ? static_cast<float>(t->height) : 1.0f;
                const auto offset = khdays::room::scroll_offset(mesh.scroll, frames, tw, th);
                auto vertex = [&](const assets::NeutralVertex& v) {
                    PreviewVertex o;
                    o.position = skinned(moved, v);
                    o.normal = v.normal;
                    if (moving)
                        o.position = placed_point(m.rep.r.place, unplaced_point(m.rep.placed_with, o.position));
                    o.uv = {(v.texcoord[0] + offset[0]) / tw, (v.texcoord[1] + offset[1]) / th};
                    o.colour = {v.color[0] / 255.0f, v.color[1] / 255.0f, v.color[2] / 255.0f, v.color[3] / 255.0f};
                    return o;
                };
                for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                    p.triangle(vertex(mesh.vertices[mesh.indices[i]]), vertex(mesh.vertices[mesh.indices[i + 1]]),
                               vertex(mesh.vertices[mesh.indices[i + 2]]), image, !mesh.double_sided, {1, 1, 1});
                }
            }
        }
    }
    const bool show[3] = {m.show_ground, m.show_walls, m.show_ceilings};
    // over the models: see-through, brought a little towards the viewer so
    // the surface it follows does not show through it in stripes
    const float alpha = m.show_models ? 0.5f : 1.0f;
    const auto eye_dir = orbit_eye_direction(m.yaw, m.pitch);
    const std::array<float, 3> eye{m.target[0] + eye_dir[0] * m.distance, m.target[1] + eye_dir[1] * m.distance,
                                   m.target[2] + eye_dir[2] * m.distance};
    const float lift = m.show_models ? m.distance * 0.002f : 0.0f;
    const bool editing_view = m.editing && showing_built();
    for (int l = 0; l < 3; ++l) {
        if (!show[l])
            continue;
        const auto& faces = shown().faces[static_cast<std::size_t>(l)];
        for (std::size_t j = 0; j < faces.size(); ++j) {
            const RoomFace& f = faces[j];
            if (f.corners < 3)
                continue;
            std::array<float, 3> tint = face_tint(l, f);
            if (l == m.picked_list && static_cast<int>(j) == m.picked_face)
                tint = {1, 1, 1};
            if (editing_view) {
                // the collision editor's: the picked shape's faces gold, the selected faces bright blue, what
                // the pointer is over lighter (a shape's every face, a face of the room's alone)
                const int from = shape_of_shown(l, j);
                const bool selected = from < 0 && !m.selected.empty() && m.selected.count(face_key(l, f)) != 0;
                const bool over =
                    from >= 0 ? from == m.hover_shape : l == m.hover_list && static_cast<int>(j) == m.hover_face;
                if (from >= 0 && from == m.picked_shape)
                    tint = over ? std::array<float, 3>{1.0f, 0.92f, 0.6f} : std::array<float, 3>{1.0f, 0.82f, 0.3f};
                else if (selected)
                    tint = over ? std::array<float, 3>{0.7f, 0.95f, 1.0f} : std::array<float, 3>{0.35f, 0.85f, 1.0f};
                else if (over)
                    tint = {std::min(1.0f, tint[0] * 0.5f + 0.5f), std::min(1.0f, tint[1] * 0.5f + 0.5f),
                            std::min(1.0f, tint[2] * 0.5f + 0.5f)};
            }
            // the face's normal from its corners, for the light
            const auto a = corner(f, 0), b = corner(f, 1), c = corner(f, 2);
            const std::array<float, 3> e0{b[0] - a[0], b[1] - a[1], b[2] - a[2]},
                e1{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const std::array<float, 3> n{e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                                         e0[0] * e1[1] - e0[1] * e1[0]};
            auto vertex = [&](std::size_t v) {
                PreviewVertex o;
                o.position = corner(f, v);
                const std::array<float, 3> to{eye[0] - o.position[0], eye[1] - o.position[1], eye[2] - o.position[2]};
                const float d = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
                if (d > 0.0f) {
                    for (std::size_t k = 0; k < 3; ++k)
                        o.position[k] += to[k] / d * lift;
                }
                o.normal = n;
                o.colour[3] = alpha;
                return o;
            };
            // a face's corners turn so that (v1-v0)x(v2-v0) points to the side the
            // player stands on (4119 of the 4124 grounds up, every ceiling down):
            // seen from behind, it is left out, as a level editor shows a room
            p.triangle(vertex(0), vertex(1), vertex(2), PreviewImage{}, true, tint);
            if (f.corners == 4)
                p.triangle(vertex(0), vertex(2), vertex(3), PreviewImage{}, true, tint);
        }
    }

    SDL_GPUTexture* tex = p.finish();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (tex != nullptr) {
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(tex))),
                     ImVec2(static_cast<float>(w), static_cast<float>(h)));
    } else {
        ImGui::Dummy(ImVec2(static_cast<float>(w), static_cast<float>(h)));
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto project = [&](const std::array<float, 3>& q, float* x, float* y) {
        if (!p.project(q, x, y))
            return false;
        *x += at.x;
        *y += at.y;
        return true;
    };

    // the markers: a dot and the way it faces; over the modder's room, coloured by where each stands on it
    // (green on its floor, yellow over it, red under or in it) with a line to that floor or surface
    int near_marker = -1;
    float best = 9.0f * ImGui::GetStyle().FontScaleDpi;
    std::vector<Marker> shown_markers = showing_built() ? markers(m.rep.built) : m.markers;
    // the marker being dragged in its fields: drawn where they put it already
    if (showing_built() && m.marker_edit.held) {
        for (auto& mk : shown_markers) {
            if (mk.name != m.marker_edit.name)
                continue;
            for (std::size_t a = 0; a < 3; ++a)
                mk.place[a] = static_cast<std::int32_t>(std::lround(m.marker_edit.at[a] * 4096.0));
            mk.extra = static_cast<std::int32_t>(
                std::lround(std::fmod(m.marker_edit.facing + 360.0f, 360.0f) * 65536.0f / 360.0f));
        }
    }
    const bool footings = yours && !m.rep.surfaces.triangles.empty();
    const ImU32 footing_colour[4] = {IM_COL32(170, 170, 170, 255), IM_COL32(80, 220, 100, 255),
                                     IM_COL32(255, 210, 60, 255), IM_COL32(255, 60, 60, 255)};
    std::array<int, 4> counted{0, 0, 0, 0};
    if (m.show_markers) {
        for (std::size_t k = 0; k < shown_markers.size(); ++k) {
            const Marker& mk = shown_markers[k];
            const std::array<float, 3> q{static_cast<float>(mk.place[0]) * kFx, static_cast<float>(mk.place[1]) * kFx,
                                         static_cast<float>(mk.place[2]) * kFx};
            ImU32 colour = IM_COL32(255, 60, 60, 255);
            std::array<float, 3> to{};
            Footing stands = Footing::none;
            if (footings) {
                stands = footing(q, &to);
                colour = footing_colour[static_cast<int>(stands)];
                ++counted[static_cast<std::size_t>(stands)];
            }
            float x, y;
            if (!project(q, &x, &y))
                continue;
            float tx, ty;
            if ((stands == Footing::floating || stands == Footing::under) && project(to, &tx, &ty)) {
                list->AddLine(ImVec2(x, y), ImVec2(tx, ty), colour, 2.0f);
                list->AddCircle(ImVec2(tx, ty), 3.0f, colour, 0, 1.5f);
            }
            const float turn = static_cast<float>(mk.extra) / 65536.0f * 6.2831853f;
            float fx, fy;
            if (project({q[0] + std::sin(turn), q[1], q[2] + std::cos(turn)}, &fx, &fy)) {
                list->AddLine(ImVec2(x, y), ImVec2(fx, fy), (colour & 0x00ffffffu) | 0xdc000000u, 2.0f);
            }
            const bool picked = static_cast<int>(k) == m.picked_marker;
            list->AddCircleFilled(ImVec2(x, y), picked ? 6.0f : 4.0f, picked ? IM_COL32(255, 255, 255, 255) : colour);
            const float d = std::hypot(x - mouse.x, y - mouse.y);
            if (hovered && d < best)
                best = d, near_marker = static_cast<int>(k);
        }
    }
    if (footings && m.show_markers && !shown_markers.empty()) {
        // how many stand where, in the picture's corner
        const S says[3] = {S::map_markers_on, S::map_markers_over, S::map_markers_under};
        ImVec2 pen(at.x + 8.0f, at.y + 6.0f);
        for (int s = 0; s < 3; ++s) {
            const std::string text =
                fmt(app.language, says[s], {std::to_string(counted[static_cast<std::size_t>(s + 1)])});
            list->AddCircleFilled(ImVec2(pen.x + 5.0f, pen.y + ImGui::GetFontSize() * 0.5f), 5.0f,
                                  footing_colour[s + 1]);
            list->AddText(ImVec2(pen.x + 14.0f, pen.y), IM_COL32(235, 235, 235, 255), text.c_str());
            pen.x += 14.0f + ImGui::CalcTextSize(text.c_str()).x + 16.0f;
        }
    }
    if (near_marker >= 0)
        ImGui::SetTooltip("%s", shown_markers[static_cast<std::size_t>(near_marker)].name.c_str());
    // a mission's points in this room: spawners green, the players' starts blue, chests gold, emblems
    // pink, labels on hover
    const MissionOverlay& mo = mission_overlay();
    // its points can be picked (and moved) while that mission is open beside the room
    const bool points_pickable = app.mission_beside >= 0 && mo.mission == app.mission_beside;
    int near_point = -1;  // in mo.points
    if (mo.world == m.world.code && mo.room == m.room->index) {
        static const ImU32 kPointColours[] = {IM_COL32(80, 255, 120, 255), IM_COL32(90, 170, 255, 255),
                                              IM_COL32(255, 200, 60, 255), IM_COL32(255, 110, 210, 255)};
        std::string tip;
        for (std::size_t k = 0; k < mo.points.size(); ++k) {
            const auto& pt = mo.points[k];
            const std::array<float, 3> q{static_cast<float>(pt.at[0]) * kFx, static_cast<float>(pt.at[1]) * kFx,
                                         static_cast<float>(pt.at[2]) * kFx};
            float x, y;
            if (!project(q, &x, &y))
                continue;
            // the way it faces (chests, players' starts): a line, as the markers have. Their number turns them
            // half a turn from the markers' (0 faces -z in the game), so it is drawn half a turn round
            if (pt.has_facing) {
                const float turn = static_cast<float>(pt.facing + 180) * 0.0174532925f;
                float fx, fy;
                if (project({q[0] + std::sin(turn), q[1], q[2] + std::cos(turn)}, &fx, &fy))
                    list->AddLine(ImVec2(x, y), ImVec2(fx, fy), (kPointColours[pt.kind] & 0x00ffffffu) | 0xdc000000u,
                                  2.0f);
            }
            const bool picked = m.picked_point_kind == static_cast<int>(pt.kind) && m.picked_point == pt.index;
            list->AddCircle(ImVec2(x, y), picked ? 9.0f : 7.0f, kPointColours[pt.kind], 0, 2.5f);
            if (picked)
                list->AddCircleFilled(ImVec2(x, y), 4.0f, IM_COL32(255, 255, 255, 255));
            const float d = std::hypot(x - mouse.x, y - mouse.y);
            // every point under the pointer (spawners often share a place), one after another
            if (hovered && d < 9.0f)
                tip += (tip.empty() ? "" : "\n\n") + pt.label;
            if (points_pickable && hovered && d < best) {
                best = d, near_marker = -1;
                near_point = static_cast<int>(k);
            }
        }
        if (!tip.empty())
            ImGui::SetTooltip("%s", tip.c_str());
        // the point asked for: a faint ring breathing around it, fading out; out of the picture, an arrow at
        // its edge pointing the way to it
        const double age = ImGui::GetTime() - m.ring_since;
        if (m.ring_since >= 0 && age < kRingSeconds) {
            const float fade = static_cast<float>(std::min(1.0, (kRingSeconds - age) / 1.5));
            const float breath = 0.5f + 0.5f * static_cast<float>(std::sin(age * 2.0 * 3.14159265 * 0.8));
            const ImU32 c = kPointColours[m.ring_kind];
            const auto with_alpha = [&](float a) {
                return (c & ~IM_COL32_A_MASK) |
                       (static_cast<ImU32>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT);
            };
            const float pw = static_cast<float>(w), ph = static_cast<float>(h);
            float x, y;
            const bool seen = project(m.ring_at, &x, &y) && x >= at.x && x <= at.x + pw && y >= at.y && y <= at.y + ph;
            if (seen) {
                const float r = 16.0f + 7.0f * breath;
                list->AddCircleFilled(ImVec2(x, y), r, with_alpha(0.10f * fade));
                list->AddCircle(ImVec2(x, y), r, with_alpha((0.35f + 0.25f * breath) * fade), 0, 2.0f);
            } else {
                // the point in the camera's terms (right, up, ahead), as orbit_view_projection makes them
                const auto back = orbit_eye_direction(m.yaw, m.pitch);
                const std::array<float, 3> f{-back[0], -back[1], -back[2]};
                std::array<float, 3> s{-f[2], 0.0f, f[0]};  // f x (0, 1, 0)
                const float sl = std::max(1e-6f, std::hypot(s[0], s[2]));
                s = {s[0] / sl, 0.0f, s[2] / sl};
                const std::array<float, 3> u{s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                                             s[0] * f[1] - s[1] * f[0]};
                std::array<float, 3> d{};
                for (std::size_t k = 0; k < 3; ++k)
                    d[k] = m.ring_at[k] - (m.target[k] + back[k] * m.distance);
                float dx = d[0] * s[0] + d[1] * s[1] + d[2] * s[2];
                float dy = -(d[0] * u[0] + d[1] * u[1] + d[2] * u[2]);  // the picture's y runs down
                const float len = std::hypot(dx, dy);
                if (len < 1e-4f)
                    dx = 0.0f, dy = 1.0f;  // straight behind: point down
                else
                    dx /= len, dy /= len;
                // from the picture's middle to its edge (kept off it), along the way to the point
                const float margin = 26.0f;
                const ImVec2 mid(at.x + pw * 0.5f, at.y + ph * 0.5f);
                const float reach = std::min(std::abs(dx) > 1e-4f ? (pw * 0.5f - margin) / std::abs(dx) : 1e9f,
                                             std::abs(dy) > 1e-4f ? (ph * 0.5f - margin) / std::abs(dy) : 1e9f);
                const ImVec2 point(mid.x + dx * reach, mid.y + dy * reach);
                const float arrow = 12.0f + 3.0f * breath;
                const ImVec2 side(-dy, dx);
                const ImVec2 base(point.x - dx * arrow * 1.6f, point.y - dy * arrow * 1.6f);
                list->AddCircleFilled(ImVec2((point.x + base.x) * 0.5f, (point.y + base.y) * 0.5f), arrow * 1.4f,
                                      with_alpha(0.12f * fade));
                list->AddTriangleFilled(point, ImVec2(base.x + side.x * arrow, base.y + side.y * arrow),
                                        ImVec2(base.x - side.x * arrow, base.y - side.y * arrow),
                                        with_alpha((0.55f + 0.3f * breath) * fade));
            }
        }
    }
    // the exits' models (the world's gate models of this room's links), where the change puts them; the one
    // nearest the pointer (nearer than any marker) can be picked
    bool near_gate = false;
    int near_gate_key = 0, near_gate_part = 0;
    if (m.show_markers) {
        for (const auto& link : m.room->links) {
            for (const auto& g : m.world.gate_models) {
                if (g.key != static_cast<std::int8_t>(link.key))
                    continue;
                for (std::size_t part = 0; part < 5; ++part) {
                    if (g.parts[part].member == 0)
                        continue;
                    auto point = g.parts[part].position;
                    const auto moved = m.rep.r.gate_points.find({g.key, static_cast<int>(part)});
                    if (m.rep.active && moved != m.rep.r.gate_points.end())
                        point = moved->second;
                    float x, y;
                    if (!project({static_cast<float>(point[0]) * kFx, static_cast<float>(point[1]) * kFx,
                                  static_cast<float>(point[2]) * kFx},
                                 &x, &y)) {
                        continue;
                    }
                    const bool picked =
                        m.picked_gate && m.picked_gate_key == g.key && m.picked_gate_part == static_cast<int>(part);
                    const float r = picked ? 7.0f : 5.0f;
                    list->AddQuadFilled(ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y),
                                        picked ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 210, 40, 230));
                    const float d = std::hypot(x - mouse.x, y - mouse.y);
                    if (hovered && d < best) {
                        best = d, near_marker = -1, near_point = -1;
                        near_gate = true, near_gate_key = g.key, near_gate_part = static_cast<int>(part);
                    }
                }
            }
        }
    }

    // the hand-made shapes' edges, where their fields put them now (drawn live while dragged)
    if (showing_built()) {
        const auto& shapes = m.rep.r.edits.shapes;
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            const bool picked = static_cast<int>(s) == m.picked_shape;
            const bool over = m.editing && static_cast<int>(s) == m.hover_shape;
            const ImU32 colour = picked ? IM_COL32(255, 220, 90, 255)
                                 : over ? IM_COL32(235, 225, 255, 255)
                                        : IM_COL32(200, 180, 255, 150);
            for (const auto& t : shape_triangles(shapes[s])) {
                float xs[3], ys[3];
                bool ok = true;
                for (std::size_t v = 0; v < 3 && ok; ++v)
                    ok = project(t[v], &xs[v], &ys[v]);
                if (!ok)
                    continue;
                for (std::size_t v = 0; v < 3; ++v) {
                    const std::size_t u = (v + 1) % 3;
                    list->AddLine(ImVec2(xs[v], ys[v]), ImVec2(xs[u], ys[u]), colour, picked || over ? 2.0f : 1.0f);
                }
            }
        }
    }
    // the picked shape's handles (the collision editor): move along an axis or over the ground, resize
    // a side, turn about the up axis; Ctrl snaps. Measured from where the drag began, so snaps are exact.
    int gizmo_hover = -1;
    bool gizmo_used = false;  // a handle took the pointer this frame
    {
        auto& shapes = m.rep.r.edits.shapes;
        const ImGuiIO& io = ImGui::GetIO();
        if (!editing_view || m.picked_shape < 0 || m.picked_shape >= static_cast<int>(shapes.size())) {
            m.gizmo_drag = -1;
        } else {
            CollisionShape& sh = shapes[static_cast<std::size_t>(m.picked_shape)];
            if (hovered && !io.WantTextInput && m.gizmo_drag < 0) {
                if (ImGui::IsKeyPressed(ImGuiKey_1, false))
                    m.gizmo_mode = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_2, false))
                    m.gizmo_mode = 1;
                if (ImGui::IsKeyPressed(ImGuiKey_3, false))
                    m.gizmo_mode = 2;
            }
            using V = std::array<float, 3>;
            auto plus = [](const V& a, const V& b, float k) {
                return V{a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k};
            };
            auto screen = [&](const V& q, ImVec2* o) {
                float x, y;
                if (!project(q, &x, &y))
                    return false;
                *o = ImVec2(x, y);
                return true;
            };
            // pixels per unit of the game's along `dir` at `from` (measured over `len` units)
            auto per_unit = [&](const V& from, const V& dir, float len, ImVec2* o) {
                ImVec2 a, b;
                if (!screen(from, &a) || !screen(plus(from, dir, len), &b))
                    return false;
                *o = ImVec2((b.x - a.x) / len, (b.y - a.y) / len);
                return true;
            };
            auto seg_dist = [](ImVec2 p, ImVec2 a, ImVec2 b) {
                const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
                const float t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
                return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
            };
            // the surface under the pointer (the room's faces and the other shapes', not this one's)
            const int picked = m.picked_shape;
            std::array<double, 3> snap_at{};
            bool snap_shown = false;
            auto surface_under = [&](std::array<double, 3>* out) {
                const auto dir = orbit_ray(m.yaw, m.pitch, mouse.x - at.x, mouse.y - at.y, static_cast<float>(w),
                                           static_cast<float>(h), app.settings.map_fov);
                RayHit hit;
                if (!cast_ray(
                        shown(), {eye[0], eye[1], eye[2]}, {dir[0], dir[1], dir[2]},
                        [&](int l, std::size_t j) { return shape_of_shown(l, j) == picked; }, &hit))
                    return false;
                *out = hit.at;
                return true;
            };
            const V axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            const ImU32 axis_colour[3] = {IM_COL32(235, 85, 85, 255), IM_COL32(120, 215, 95, 255),
                                          IM_COL32(85, 145, 245, 255)};
            const ImU32 hot = IM_COL32(255, 245, 170, 255);
            const bool plane = sh.kind == CollisionShape::plane;
            const V centre = shape_point(sh, {0, plane ? 0.0f : 0.5f, 0});
            // a Resize handle's place on the shape, in unit coordinates: the middle of the side along axis `i`
            // towards `sign`
            auto side = [&](int i, int sign) {
                V u{0, plane ? 0.0f : 0.5f, 0};
                if (i == 1)
                    u[1] = sign > 0 ? 1.0f : 0.0f;
                else
                    u[static_cast<std::size_t>(i)] = 0.5f * static_cast<float>(sign);
                return u;
            };
            ImVec2 c2;
            const bool centre_seen = screen(centre, &c2);
            // the handles of this mode, as screen shapes to draw and to point at
            struct Handle {
                int id;
                ImVec2 a, b;  // an arrow from a to b, or a box at b
                bool arrow;
                ImU32 colour;
            };
            std::vector<Handle> handles;
            std::vector<ImVec2> ring;
            float px_per_unit = 0;
            for (const auto& ax : axes) {
                ImVec2 pu;
                if (per_unit(centre, ax, 1.0f, &pu))
                    px_per_unit = std::max(px_per_unit, std::hypot(pu.x, pu.y));
            }
            const float k = ImGui::GetStyle().FontScaleDpi > 0 ? ImGui::GetStyle().FontScaleDpi : 1.0f;
            if (centre_seen && px_per_unit > 1e-4f) {
                const float reach = 75.0f * k / px_per_unit;  // the arrows' length, in units
                if (m.gizmo_mode == 0) {
                    for (int i = 0; i < 3; ++i) {
                        ImVec2 tip;
                        if (screen(plus(centre, axes[i], reach), &tip) &&
                            std::hypot(tip.x - c2.x, tip.y - c2.y) > 12.0f * k)
                            handles.push_back({i, c2, tip, true, axis_colour[i]});
                    }
                    handles.push_back({3, c2, c2, false, IM_COL32(240, 240, 240, 255)});  // over the ground
                }
                if (m.gizmo_mode == 1) {
                    for (int i = 0; i < 3; ++i) {
                        if (plane && i == 1)
                            continue;
                        for (const int sign : {-1, 1}) {
                            ImVec2 hp;
                            if (screen(shape_point(sh, side(i, sign)), &hp))
                                handles.push_back({10 + i * 2 + (sign > 0 ? 1 : 0), c2, hp, false, axis_colour[i]});
                        }
                    }
                } else {
                    // the turning ring: in Turn, and around the arrows in Move (as Blender's)
                    const float radius = 0.6f * std::max(sh.size[0], sh.size[2]) + 0.5f;
                    for (int n = 0; n <= 64; ++n) {
                        const float t = static_cast<float>(n) / 64.0f * 6.2831853f;
                        ImVec2 q;
                        if (screen(V{sh.at[0] + std::sin(t) * radius, sh.at[1], sh.at[2] + std::cos(t) * radius}, &q))
                            ring.push_back(q);
                    }
                }
            }
            // what the pointer is over
            if (hovered && m.gizmo_drag < 0 && !m.box_on) {
                float best_px = 8.0f * k;
                for (const auto& hd : handles) {
                    const float d =
                        hd.arrow ? seg_dist(mouse, hd.a, hd.b) : std::hypot(mouse.x - hd.b.x, mouse.y - hd.b.y);
                    if (d < best_px)
                        best_px = d, gizmo_hover = hd.id;
                }
                for (std::size_t n = 1; n < ring.size(); ++n) {
                    const float d = seg_dist(mouse, ring[n - 1], ring[n]);
                    if (d < best_px)
                        best_px = d, gizmo_hover = 20;
                }
            }
            // a drag begins: the shape as it was kept, and how the handle moves on the screen
            if (gizmo_hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift) {
                edit_push();
                m.gizmo_drag = gizmo_hover;
                m.gizmo_start = sh;
                m.gizmo_from = m.gizmo_last = mouse;
                m.gizmo_turned = 0;
                const int id = gizmo_hover;
                bool ok = true;
                if (id <= 2) {
                    ok = per_unit(centre, axes[id], 1.0f, &m.gizmo_pu[0]);
                } else if (id == 3) {
                    ok = per_unit(centre, axes[0], 1.0f, &m.gizmo_pu[0]) &&
                         per_unit(centre, axes[2], 1.0f, &m.gizmo_pu[1]);
                } else if (id < 20) {
                    const int i = (id - 10) / 2, sign = (id - 10) % 2 ? 1 : -1;
                    const V dir = plus(V{0, 0, 0}, shape_axis(sh, i), static_cast<float>(sign));
                    ok = per_unit(shape_point(sh, side(i, sign)), dir, 1.0f, &m.gizmo_pu[0]);
                } else {
                    // which way the ring turns on the screen: +turn goes from +z towards +x
                    ImVec2 a, b, o;
                    ok = screen(sh.at, &o) && screen(plus(sh.at, axes[2], 1.0f), &a) &&
                         screen(plus(sh.at, axes[0], 1.0f), &b);
                    m.gizmo_centre = o;
                    m.gizmo_sign = (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x) > 0 ? 1.0f : -1.0f;
                }
                if (!ok) {
                    m.gizmo_drag = -1;
                    edit_unpush();  // nothing to drag: no step kept
                }
            }
            // dragged: the shape as the drag puts it; let go: its faces made again
            if (m.gizmo_drag >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                // a right click while dragging: the shape back as it was, nothing kept to undo
                sh = m.gizmo_start;
                edit_unpush();
                m.gizmo_drag = -1;
                gizmo_used = true;
            }
            if (m.gizmo_drag >= 0) {
                gizmo_used = true;
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const ImVec2 total(mouse.x - m.gizmo_from.x, mouse.y - m.gizmo_from.y);
                    const bool snap = io.KeyCtrl;
                    auto snapped = [&](float v, float step) { return snap ? std::round(v / step) * step : v; };
                    auto along = [&](ImVec2 pu) {
                        const float l2 = pu.x * pu.x + pu.y * pu.y;
                        return l2 > 1e-8f ? (total.x * pu.x + total.y * pu.y) / l2 : 0.0f;
                    };
                    const CollisionShape& s0 = m.gizmo_start;
                    const int id = m.gizmo_drag;
                    if (id <= 2) {
                        sh.at = s0.at;
                        sh.at[static_cast<std::size_t>(id)] =
                            snapped(s0.at[static_cast<std::size_t>(id)] + along(m.gizmo_pu[0]), 0.25f);
                    } else if (id == 3 && m.snap_surface && surface_under(&snap_at)) {
                        // onto the surface under the pointer: its bottom's middle where the pointer meets it
                        sh.at = s0.at;
                        sh.at[0] = snapped(static_cast<float>(snap_at[0]), 0.25f);
                        sh.at[1] = static_cast<float>(snap_at[1]);
                        sh.at[2] = snapped(static_cast<float>(snap_at[2]), 0.25f);
                        snap_shown = true;
                    } else if (id == 3) {
                        const ImVec2 sx = m.gizmo_pu[0], sz = m.gizmo_pu[1];
                        const float det = sx.x * sz.y - sx.y * sz.x;
                        if (std::abs(det) > 1e-6f) {
                            const float dx = (total.x * sz.y - total.y * sz.x) / det;
                            const float dz = (sx.x * total.y - sx.y * total.x) / det;
                            sh.at = s0.at;
                            sh.at[0] = snapped(s0.at[0] + dx, 0.25f);
                            sh.at[2] = snapped(s0.at[2] + dz, 0.25f);
                        }
                    } else if (id < 20) {
                        const int i = (id - 10) / 2;
                        const float sign = (id - 10) % 2 ? 1.0f : -1.0f;
                        const float before = s0.size[static_cast<std::size_t>(i)];
                        float now = std::max(0.05f, before + along(m.gizmo_pu[0]));
                        if (snap)
                            now = std::max(0.25f, std::round(now / 0.25f) * 0.25f);
                        const float grown = now - before;
                        sh.size = s0.size;
                        sh.size[static_cast<std::size_t>(i)] = now;
                        // the other side stays where it was
                        const V ax = shape_axis(s0, i);
                        const float shift = i == 1 ? (sign < 0 ? -grown : 0.0f) : sign * grown / 2;
                        sh.at = plus(s0.at, ax, shift);
                    } else {
                        const float a0 =
                            std::atan2(m.gizmo_last.y - m.gizmo_centre.y, m.gizmo_last.x - m.gizmo_centre.x);
                        const float a1 = std::atan2(mouse.y - m.gizmo_centre.y, mouse.x - m.gizmo_centre.x);
                        float d = a1 - a0;
                        while (d > 3.14159265f)
                            d -= 6.2831853f;
                        while (d < -3.14159265f)
                            d += 6.2831853f;
                        m.gizmo_turned += d;
                        m.gizmo_last = mouse;
                        float yaw = snapped(s0.turn[1] + m.gizmo_sign * m.gizmo_turned * 57.2957795f, 15.0f);
                        yaw = std::fmod(yaw, 360.0f);
                        if (yaw > 180.0f)
                            yaw -= 360.0f;
                        if (yaw <= -180.0f)
                            yaw += 360.0f;
                        sh.turn = s0.turn;
                        sh.turn[1] = yaw;
                    }
                } else {
                    // let go: its faces made again, or (not moved) no step left to undo
                    m.gizmo_drag = -1;
                    if (sh == m.gizmo_start)
                        edit_unpush();
                    else
                        edits_changed();
                }
            }
            // drawn over everything, the one pointed at or dragged lit
            const int lit = m.gizmo_drag >= 0 ? m.gizmo_drag : gizmo_hover;
            for (const auto& hd : handles) {
                const ImU32 col = hd.id == lit ? hot : hd.colour;
                if (hd.arrow) {
                    list->AddLine(hd.a, hd.b, col, 3.0f * k);
                    const float dx = hd.b.x - hd.a.x, dy = hd.b.y - hd.a.y, l = std::max(1.0f, std::hypot(dx, dy));
                    const float ux = dx / l, uy = dy / l, s = 9.0f * k;
                    list->AddTriangleFilled(ImVec2(hd.b.x + ux * s, hd.b.y + uy * s),
                                            ImVec2(hd.b.x - uy * s * 0.6f, hd.b.y + ux * s * 0.6f),
                                            ImVec2(hd.b.x + uy * s * 0.6f, hd.b.y - ux * s * 0.6f), col);
                } else {
                    const float s = (hd.id == 3 ? 6.0f : 5.0f) * k;
                    list->AddRectFilled(ImVec2(hd.b.x - s, hd.b.y - s), ImVec2(hd.b.x + s, hd.b.y + s), col);
                    list->AddRect(ImVec2(hd.b.x - s, hd.b.y - s), ImVec2(hd.b.x + s, hd.b.y + s),
                                  IM_COL32(20, 20, 30, 255));
                }
            }
            if (ring.size() > 1)
                list->AddPolyline(ring.data(), static_cast<int>(ring.size()), lit == 20 ? hot : axis_colour[1], 0,
                                  (lit == 20 ? 3.5f : 2.5f) * k);
            // where the surface snap puts it: a ring on the surface
            ImVec2 sp;
            if (snap_shown && screen(V{static_cast<float>(snap_at[0]), static_cast<float>(snap_at[1]),
                                       static_cast<float>(snap_at[2])},
                                     &sp)) {
                list->AddCircle(sp, 9.0f * k, IM_COL32(255, 245, 170, 255), 0, 2.5f * k);
                list->AddCircleFilled(sp, 3.0f * k, IM_COL32(255, 245, 170, 255));
            }
            // which mode, in the picture's lower corner
            const S modes[3] = {S::map_gizmo_move, S::map_gizmo_size, S::map_gizmo_turn};
            const std::string label =
                std::string(tr(app.language, modes[m.gizmo_mode])) + "   " + tr(app.language, S::map_gizmo_keys);
            list->AddText(ImVec2(at.x + 8.0f, at.y + static_cast<float>(h) - ImGui::GetFontSize() - 6.0f),
                          IM_COL32(240, 240, 240, 230), label.c_str());
        }
    }
    // the picked room marker's, exit model's or mission point's handles: an arrow along each axis, a square to
    // move it over the ground, and a ring to turn it when it faces a way. Shown as soon as it is picked (the
    // shapes' Move / Resize / Turn do not apply); Ctrl snaps (a quarter unit, 15 degrees), a right click while
    // dragging puts it back. Each drag is one step to undo.
    int point_hover = -1;
    bool point_used = false;
    {
        using V = std::array<float, 3>;
        const ImGuiIO& io = ImGui::GetIO();
        Replacing& rp = m.rep;
        // what is picked and movable, where it is now and the way it faces
        int what = -1;
        V now_at{};
        bool has_facing = false;
        float now_facing = 0;
        const Marker* picked_mk = nullptr;
        const GateModelPart* gate_part = nullptr;
        MissionPointPlace pp;
        const auto point_kind = static_cast<MissionPoint::Kind>(m.picked_point_kind);
        if (m.picked_point_kind >= 0 && m.picked_point >= 0) {
            if (points_pickable && mission_point(app, point_kind, m.picked_point, &pp)) {
                what = 2;
                now_at = {static_cast<float>(pp.at[0]) * kFx, static_cast<float>(pp.at[1]) * kFx,
                          static_cast<float>(pp.at[2]) * kFx};
                has_facing = pp.has_facing;
                now_facing = static_cast<float>(pp.facing) + 180.0f;  // the way it faces (see the points' lines)
            }
        } else if (m.picked_gate && rp.active) {
            for (const auto& g : m.world.gate_models) {
                if (g.key == m.picked_gate_key && m.picked_gate_part >= 0 && m.picked_gate_part < 5)
                    gate_part = &g.parts[static_cast<std::size_t>(m.picked_gate_part)];
            }
            if (gate_part != nullptr) {
                what = 1;
                const auto moved = rp.r.gate_points.find({m.picked_gate_key, m.picked_gate_part});
                const auto p3 = moved != rp.r.gate_points.end() ? moved->second : gate_part->position;
                now_at = {static_cast<float>(p3[0]) * kFx, static_cast<float>(p3[1]) * kFx,
                          static_cast<float>(p3[2]) * kFx};
            }
        } else if (m.picked_marker >= 0 && showing_built() &&
                   m.picked_marker < static_cast<int>(shown_markers.size())) {
            what = 0;
            picked_mk = &shown_markers[static_cast<std::size_t>(m.picked_marker)];
            now_at = {static_cast<float>(picked_mk->place[0]) * kFx, static_cast<float>(picked_mk->place[1]) * kFx,
                      static_cast<float>(picked_mk->place[2]) * kFx};
            has_facing = true;
            now_facing = static_cast<float>(picked_mk->extra) * 360.0f / 65536.0f;
        }
        if (what < 0 || (m.point_drag >= 0 && what != m.point_what))
            m.point_drag = -1;  // nothing movable picked (or another thing): no drag goes on
        const auto wrap = [](float deg) {
            deg = std::fmod(deg, 360.0f);
            return deg < 0 ? deg + 360.0f : deg;
        };
        // it put at `to` facing `facing`: drawn there at once; `done` (let go) writes a room marker to the change
        const auto put = [&](const V& to, float facing, bool done) {
            const std::array<std::int32_t, 3> fixed{static_cast<std::int32_t>(std::lround(to[0] * 4096.0)),
                                                    static_cast<std::int32_t>(std::lround(to[1] * 4096.0)),
                                                    static_cast<std::int32_t>(std::lround(to[2] * 4096.0))};
            if (what == 2) {
                mission_point_move(app, point_kind, m.picked_point, fixed,
                                   static_cast<int>(std::lround(wrap(facing - 180.0f))));
            } else if (what == 1) {
                rp.r.gate_points[{m.picked_gate_key, m.picked_gate_part}] = fixed;
            } else if (what == 0 && picked_mk != nullptr) {
                auto& e = m.marker_edit;
                e.name = picked_mk->name;
                for (std::size_t a = 0; a < 3; ++a)
                    e.at[a] = to[a];
                e.facing = wrap(facing);
                e.held = true;
                e.held_frame = ImGui::GetFrameCount();  // its fields and the picture show it here
                if (done) {
                    rp.r.markers[picked_mk->name] = {
                        fixed[0], fixed[1], fixed[2],
                        static_cast<std::int32_t>(std::lround(wrap(facing) * 65536.0f / 360.0f))};
                    rp.dirty = true;
                }
            }
        };
        const auto keep = [&]() {  // the step to undo, as the drag begins
            if (what == 2)
                mission_point_push(app, point_kind, m.picked_point);
            else
                edit_push();
        };
        const auto unkeep = [&]() {  // no step left (put back, or not moved)
            if (what == 2)
                mission_point_unpush();
            else
                edit_unpush();
        };
        if (what >= 0) {
            auto plus = [](const V& a, const V& b, float k) {
                return V{a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k};
            };
            auto screen = [&](const V& q, ImVec2* o) {
                float x, y;
                if (!project(q, &x, &y))
                    return false;
                *o = ImVec2(x, y);
                return true;
            };
            auto per_unit = [&](const V& from, const V& dir, ImVec2* o) {
                ImVec2 a, b;
                if (!screen(from, &a) || !screen(plus(from, dir, 1.0f), &b))
                    return false;
                *o = ImVec2(b.x - a.x, b.y - a.y);
                return true;
            };
            auto seg_dist = [](ImVec2 p, ImVec2 a, ImVec2 b) {
                const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
                const float u = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
                return std::hypot(p.x - (a.x + u * dx), p.y - (a.y + u * dy));
            };
            const V axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            const ImU32 axis_colour[3] = {IM_COL32(235, 85, 85, 255), IM_COL32(120, 215, 95, 255),
                                          IM_COL32(85, 145, 245, 255)};
            const ImU32 hot = IM_COL32(255, 245, 170, 255);
            const float k = ImGui::GetStyle().FontScaleDpi > 0 ? ImGui::GetStyle().FontScaleDpi : 1.0f;
            ImVec2 c2;
            const bool seen = screen(now_at, &c2);
            float px_per_unit = 0;
            for (const auto& ax : axes) {
                ImVec2 pu;
                if (per_unit(now_at, ax, &pu))
                    px_per_unit = std::max(px_per_unit, std::hypot(pu.x, pu.y));
            }
            struct Handle {
                int id;
                ImVec2 a, b;
                bool arrow;
                ImU32 colour;
            };
            std::vector<Handle> handles;
            std::vector<ImVec2> ring;
            if (seen && px_per_unit > 1e-4f) {
                const float reach = 60.0f * k / px_per_unit;  // the arrows' length, in units (the same on screen)
                for (int i = 0; i < 3; ++i) {
                    ImVec2 tip;
                    if (screen(plus(now_at, axes[i], reach), &tip) && std::hypot(tip.x - c2.x, tip.y - c2.y) > 12.0f * k)
                        handles.push_back({i, c2, tip, true, axis_colour[i]});
                }
                handles.push_back({3, c2, c2, false, IM_COL32(240, 240, 240, 255)});
                if (has_facing) {
                    const float radius = 40.0f * k / px_per_unit;
                    for (int n = 0; n <= 48; ++n) {
                        const float a = static_cast<float>(n) / 48.0f * 6.2831853f;
                        ImVec2 q;
                        if (screen(V{now_at[0] + std::sin(a) * radius, now_at[1], now_at[2] + std::cos(a) * radius}, &q))
                            ring.push_back(q);
                    }
                }
            }
            if (hovered && m.point_drag < 0 && m.gizmo_drag < 0 && gizmo_hover < 0 && !m.box_on) {
                float best_px = 8.0f * k;
                for (const auto& hd : handles) {
                    const float d =
                        hd.arrow ? seg_dist(mouse, hd.a, hd.b) : std::hypot(mouse.x - hd.b.x, mouse.y - hd.b.y);
                    if (d < best_px)
                        best_px = d, point_hover = hd.id;
                }
                for (std::size_t n = 1; n < ring.size(); ++n) {
                    const float d = seg_dist(mouse, ring[n - 1], ring[n]);
                    if (d < best_px)
                        best_px = d, point_hover = 20;
                }
            }
            // a drag begins: where it was, and how the handle moves on the screen
            if (point_hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift) {
                bool ok = true;
                if (point_hover <= 2) {
                    ok = per_unit(now_at, axes[point_hover], &m.point_pu[0]);
                } else if (point_hover == 3) {
                    ok = per_unit(now_at, axes[0], &m.point_pu[0]) && per_unit(now_at, axes[2], &m.point_pu[1]);
                } else {
                    // which way the ring turns on the screen: +facing goes from +z towards +x
                    ImVec2 a, b;
                    ok = screen(plus(now_at, axes[2], 1.0f), &a) && screen(plus(now_at, axes[0], 1.0f), &b);
                    m.point_centre = c2;
                    m.point_sign = (a.x - c2.x) * (b.y - c2.y) - (a.y - c2.y) * (b.x - c2.x) > 0 ? 1.0f : -1.0f;
                }
                if (ok) {
                    keep();
                    m.point_drag = point_hover;
                    m.point_what = what;
                    m.point_from = m.point_last = mouse;
                    m.point_turned = 0;
                    m.point_start = now_at;
                    m.point_start_facing = now_facing;
                    if (what == 1) {
                        const auto moved = rp.r.gate_points.find({m.picked_gate_key, m.picked_gate_part});
                        m.point_gate_had = moved != rp.r.gate_points.end();
                        if (m.point_gate_had)
                            m.point_gate_was = moved->second;
                    }
                }
            }
            // a right click while dragging: back where it was, nothing kept to undo
            if (m.point_drag >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (what == 1) {
                    if (m.point_gate_had)
                        rp.r.gate_points[{m.picked_gate_key, m.picked_gate_part}] = m.point_gate_was;
                    else
                        rp.r.gate_points.erase({m.picked_gate_key, m.picked_gate_part});
                } else if (what == 2) {
                    put(m.point_start, m.point_start_facing, false);
                } else {
                    m.marker_edit.held = false;  // its fields and the picture take the room's place again
                    m.marker_edit.held_frame = -10;
                }
                unkeep();
                m.point_drag = -1;
                point_used = true;
            }
            if (m.point_drag >= 0) {
                point_used = true;
                const ImVec2 total(mouse.x - m.point_from.x, mouse.y - m.point_from.y);
                const bool snap = io.KeyCtrl;
                auto snapped = [&](float v, float step) { return snap ? std::round(v / step) * step : v; };
                auto along = [&](ImVec2 pu) {
                    const float l2 = pu.x * pu.x + pu.y * pu.y;
                    return l2 > 1e-8f ? (total.x * pu.x + total.y * pu.y) / l2 : 0.0f;
                };
                V to = m.point_start;
                float facing = m.point_start_facing;
                const int id = m.point_drag;
                if (id <= 2) {
                    to[static_cast<std::size_t>(id)] =
                        snapped(m.point_start[static_cast<std::size_t>(id)] + along(m.point_pu[0]), 0.25f);
                } else if (id == 3) {
                    const ImVec2 sx = m.point_pu[0], sz = m.point_pu[1];
                    const float det = sx.x * sz.y - sx.y * sz.x;
                    if (std::abs(det) > 1e-6f) {
                        to[0] = snapped(m.point_start[0] + (total.x * sz.y - total.y * sz.x) / det, 0.25f);
                        to[2] = snapped(m.point_start[2] + (sx.x * total.y - sx.y * total.x) / det, 0.25f);
                    }
                } else {
                    const float a0 = std::atan2(m.point_last.y - m.point_centre.y, m.point_last.x - m.point_centre.x);
                    const float a1 = std::atan2(mouse.y - m.point_centre.y, mouse.x - m.point_centre.x);
                    float d = a1 - a0;
                    while (d > 3.14159265f)
                        d -= 6.2831853f;
                    while (d < -3.14159265f)
                        d += 6.2831853f;
                    m.point_turned += d;
                    m.point_last = mouse;
                    facing = wrap(snapped(m.point_start_facing + m.point_sign * m.point_turned * 57.2957795f, 15.0f));
                }
                const bool moved = to != m.point_start || facing != m.point_start_facing;
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (moved || what == 0)
                        put(to, facing, false);
                } else {
                    // let go: kept (a room marker written to the change), or not moved and no step left
                    m.point_drag = -1;
                    if (moved) {
                        put(to, facing, true);
                    } else {
                        if (what == 0) {
                            m.marker_edit.held = false;
                            m.marker_edit.held_frame = -10;
                        }
                        unkeep();
                    }
                }
                now_at = to;
                now_facing = facing;
                screen(now_at, &c2);
            }
            // drawn over everything, the one pointed at or dragged lit (moved with it while dragged)
            const int lit = m.point_drag >= 0 ? m.point_drag : point_hover;
            if (m.point_drag >= 0 && seen && !handles.empty()) {
                const ImVec2 shift(c2.x - handles.back().b.x, c2.y - handles.back().b.y);
                for (auto& hd : handles)
                    hd.a = ImVec2(hd.a.x + shift.x, hd.a.y + shift.y), hd.b = ImVec2(hd.b.x + shift.x, hd.b.y + shift.y);
                for (auto& q : ring)
                    q = ImVec2(q.x + shift.x, q.y + shift.y);
            }
            for (const auto& hd : handles) {
                const ImU32 col = hd.id == lit ? hot : hd.colour;
                if (hd.arrow) {
                    list->AddLine(hd.a, hd.b, col, 3.0f * k);
                    const float dx = hd.b.x - hd.a.x, dy = hd.b.y - hd.a.y, l = std::max(1.0f, std::hypot(dx, dy));
                    const float ux = dx / l, uy = dy / l, s = 9.0f * k;
                    list->AddTriangleFilled(ImVec2(hd.b.x + ux * s, hd.b.y + uy * s),
                                            ImVec2(hd.b.x - uy * s * 0.6f, hd.b.y + ux * s * 0.6f),
                                            ImVec2(hd.b.x + uy * s * 0.6f, hd.b.y - ux * s * 0.6f), col);
                } else {
                    const float s = 6.0f * k;
                    list->AddRectFilled(ImVec2(hd.b.x - s, hd.b.y - s), ImVec2(hd.b.x + s, hd.b.y + s), col);
                    list->AddRect(ImVec2(hd.b.x - s, hd.b.y - s), ImVec2(hd.b.x + s, hd.b.y + s),
                                  IM_COL32(20, 20, 30, 255));
                }
            }
            if (ring.size() > 1) {
                list->AddPolyline(ring.data(), static_cast<int>(ring.size()), lit == 20 ? hot : axis_colour[1], 0,
                                  (lit == 20 ? 3.5f : 2.5f) * k);
                // the way it faces, out to the ring
                const float turn = now_facing * 0.0174532925f, radius = 40.0f * k / px_per_unit;
                ImVec2 f2;
                if (screen(V{now_at[0] + std::sin(turn) * radius, now_at[1], now_at[2] + std::cos(turn) * radius}, &f2))
                    list->AddLine(c2, f2, lit == 20 ? hot : IM_COL32(240, 240, 240, 220), 2.0f * k);
            }
            if (!handles.empty()) {
                const char* hint = tr(app.language, S::map_point_handles_keys);
                list->AddText(ImVec2(at.x + 8.0f, at.y + static_cast<float>(h) - ImGui::GetFontSize() - 6.0f),
                              IM_COL32(240, 240, 240, 230), hint);
            }
        }
    }
    const bool gizmo_owns = gizmo_used || gizmo_hover >= 0 || point_used || point_hover >= 0;
    // Shift+drag in the collision editor: the faces facing the viewer wholly inside the box selected
    bool boxed = false;
    if (editing_view && hovered && !gizmo_owns && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::GetIO().KeyShift) {
        m.box_on = true;
        m.box_from = mouse;
    }
    if (m.box_on) {
        const ImVec2 lo(std::min(m.box_from.x, mouse.x), std::min(m.box_from.y, mouse.y));
        const ImVec2 hi(std::max(m.box_from.x, mouse.x), std::max(m.box_from.y, mouse.y));
        list->AddRectFilled(lo, hi, IM_COL32(90, 200, 255, 40));
        list->AddRect(lo, hi, IM_COL32(90, 200, 255, 220), 0.0f, 0, 1.5f);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m.box_on = false;
            boxed = true;
            if (!ImGui::GetIO().KeyCtrl)
                m.selected.clear();
            for (int l = 0; l < 3; ++l) {
                if (!show[l])
                    continue;
                const auto& faces = shown().faces[static_cast<std::size_t>(l)];
                for (std::size_t j = 0; j < faces.size(); ++j) {
                    const RoomFace& f = faces[j];
                    if (f.corners < 3 || shape_of_shown(l, j) >= 0)
                        continue;
                    bool inside = true;
                    for (std::size_t v = 0; v < f.corners && inside; ++v) {
                        float x, y;
                        inside = project(corner(f, v), &x, &y) && x >= lo.x && x <= hi.x && y >= lo.y && y <= hi.y;
                    }
                    if (!inside)
                        continue;
                    const auto a = corner(f, 0), b = corner(f, 1), c2 = corner(f, 2);
                    const std::array<float, 3> e0{b[0] - a[0], b[1] - a[1], b[2] - a[2]},
                        e1{c2[0] - a[0], c2[1] - a[1], c2[2] - a[2]};
                    const std::array<float, 3> n{e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                                                 e0[0] * e1[1] - e0[1] * e1[0]};
                    if (n[0] * (eye[0] - a[0]) + n[1] * (eye[1] - a[1]) + n[2] * (eye[2] - a[2]) <= 0.0f)
                        continue;
                    m.selected.insert(face_key(l, f));
                }
            }
            m.picked_shape = -1;
        }
    }
    if (near_gate) {
        ImGui::SetTooltip("%s", fmt(app.language, S::map_gate_part,
                                    {std::to_string(near_gate_key), std::to_string(near_gate_part), "-"})
                                    .c_str());
    }
    // the nearest face of the lists shown under the pointer, facing the viewer (-1: none)
    auto face_under_pointer = [&](int* list_out, int* face_out) {
        *list_out = *face_out = -1;
        float nearest = 1e30f;
        for (int l = 0; l < 3; ++l) {
            if (!show[l])
                continue;
            const auto& faces = shown().faces[static_cast<std::size_t>(l)];
            for (std::size_t j = 0; j < faces.size(); ++j) {
                const RoomFace& f = faces[j];
                float xs[4], ys[4];
                bool ok = true;
                for (std::size_t v = 0; v < f.corners && v < 4 && ok; ++v)
                    ok = project(corner(f, v), &xs[v], &ys[v]);
                if (!ok || f.corners < 3)
                    continue;
                // not seen from behind (as drawn)
                const auto a = corner(f, 0), b = corner(f, 1), c2 = corner(f, 2);
                const std::array<float, 3> e0{b[0] - a[0], b[1] - a[1], b[2] - a[2]},
                    e1{c2[0] - a[0], c2[1] - a[1], c2[2] - a[2]};
                const std::array<float, 3> n{e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                                             e0[0] * e1[1] - e0[1] * e1[0]};
                if (n[0] * (eye[0] - a[0]) + n[1] * (eye[1] - a[1]) + n[2] * (eye[2] - a[2]) <= 0.0f)
                    continue;
                // inside the projected polygon (either winding)
                int sign = 0;
                bool inside = true;
                for (std::size_t v = 0; v < f.corners && inside; ++v) {
                    const std::size_t u = (v + 1) % f.corners;
                    const float cross = (xs[u] - xs[v]) * (mouse.y - ys[v]) - (ys[u] - ys[v]) * (mouse.x - xs[v]);
                    const int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
                    if (s != 0 && sign != 0 && s != sign)
                        inside = false;
                    if (s != 0)
                        sign = s;
                }
                if (!inside)
                    continue;
                std::array<float, 3> centre{0, 0, 0};
                for (std::size_t v = 0; v < f.corners; ++v) {
                    const auto q = corner(f, v);
                    for (std::size_t c = 0; c < 3; ++c)
                        centre[c] += q[c] / static_cast<float>(f.corners);
                }
                const float d = std::hypot(centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]);
                if (d < nearest)
                    nearest = d, *list_out = l, *face_out = static_cast<int>(j);
            }
        }
    };
    // while picking: what the pointer is over, drawn lighter next frame (nothing while over a marker or an
    // exit's model, which a click would pick instead, or while a box is drawn)
    m.hover_list = m.hover_face = m.hover_shape = -1;
    if (editing_view && hovered && !m.box_on && !gizmo_owns && near_marker < 0 && !near_gate &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        face_under_pointer(&m.hover_list, &m.hover_face);
        if (m.hover_face >= 0)
            m.hover_shape = shape_of_shown(m.hover_list, static_cast<std::size_t>(m.hover_face));
    }
    // a click picks the nearest marker or exit's model, else the nearest face under the pointer
    if (!boxed && !m.box_on && !gizmo_owns && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.0f) {
        m.picked_marker = near_marker;
        m.picked_list = m.picked_face = -1;
        m.picked_gate = near_gate;
        if (near_marker >= 0 || near_gate || near_point >= 0)
            m.picked_shape = -1;  // one thing's handles at a time
        m.picked_gate_key = near_gate_key;
        m.picked_gate_part = near_gate_part;
        MissionOverlay& picked_mo = mission_overlay();
        if (near_point >= 0 && static_cast<std::size_t>(near_point) < mo.points.size()) {
            const MissionPoint& pt = mo.points[static_cast<std::size_t>(near_point)];
            m.picked_point_kind = static_cast<int>(pt.kind);
            m.picked_point = pt.index;
            picked_mo.picked_kind = m.picked_point_kind;
            picked_mo.picked_index = m.picked_point;
            picked_mo.scroll = true;  // its row shown in the mission's summary
        } else {
            m.picked_point_kind = m.picked_point = -1;
            picked_mo.picked_kind = picked_mo.picked_index = -1;
        }
        if (near_marker < 0 && !near_gate && near_point < 0)
            face_under_pointer(&m.picked_list, &m.picked_face);
        if (editing_view) {
            // a click selects the face (Ctrl: adds it or takes it away), or the shape it was made of
            const bool add = ImGui::GetIO().KeyCtrl;
            if (m.picked_face >= 0) {
                const int from = shape_of_shown(m.picked_list, static_cast<std::size_t>(m.picked_face));
                if (from >= 0) {
                    m.picked_shape = from;
                    if (!add)
                        m.selected.clear();
                } else {
                    const FaceKey key = face_key(m.picked_list, shown().faces[static_cast<std::size_t>(m.picked_list)]
                                                                             [static_cast<std::size_t>(m.picked_face)]);
                    m.picked_shape = -1;
                    if (!add)
                        m.selected = {key};
                    else if (m.selected.erase(key) == 0)
                        m.selected.insert(key);
                }
            } else if (!add && near_marker < 0 && !near_gate) {
                m.selected.clear();
                m.picked_shape = -1;
            }
        }
    } else if (hovered && !m.box_on && !gizmo_owns && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        // orbiting: around the point in front of the camera; first person: looking around from where the
        // camera stands, the eye kept and that point moved in front of it the way it now looks
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        const auto before = orbit_eye_direction(m.yaw, m.pitch);
        const std::array<float, 3> stand{m.target[0] + before[0] * m.distance, m.target[1] + before[1] * m.distance,
                                         m.target[2] + before[2] * m.distance};
        m.yaw -= d.x * 0.4f;
        m.pitch = std::clamp(m.pitch + d.y * 0.4f, -85.0f, 85.0f);
        if (app.settings.map_camera_fps) {
            const auto after = orbit_eye_direction(m.yaw, m.pitch);
            m.target = {stand[0] - after[0] * m.distance, stand[1] - after[1] * m.distance,
                        stand[2] - after[2] * m.distance};
        }
    }
    orbit_move(m.target, m.yaw, m.pitch, m.distance, static_cast<float>(h), hovered, app.settings.map_fov);
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
        m.distance *= std::pow(0.9f, ImGui::GetIO().MouseWheel);
}

// The exit's model picked in the picture: where it stands, moved while its fields are dragged (any change of
// the room saves its exits' models' places), back to the game's place on request.
// The picked exit's model's name: its exit, its part and the model it shows.
std::string gate_name(const App& app) {
    for (const auto& g : m.world.gate_models) {
        if (g.key == m.picked_gate_key && m.picked_gate_part >= 0 && m.picked_gate_part < 5)
            return fmt(app.language, S::map_gate_part,
                       {std::to_string(m.picked_gate_key), std::to_string(m.picked_gate_part),
                        std::to_string(g.parts[static_cast<std::size_t>(m.picked_gate_part)].member)});
    }
    return {};
}

// The picked exit's model: its name (in the panel), its place in fields shared by the panel and the bar over the
// picture (bar true; both write the change at once, so both follow a drag), and putting it back.
void draw_picked_gate(App& app, bool bar) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    const GateModelPart* part = nullptr;
    for (const auto& g : m.world.gate_models) {
        if (g.key == m.picked_gate_key && m.picked_gate_part >= 0 && m.picked_gate_part < 5)
            part = &g.parts[static_cast<std::size_t>(m.picked_gate_part)];
    }
    if (part == nullptr) {
        m.picked_gate = false;
        return;
    }
    ImGui::PushID(bar ? "gate_bar" : "gate_panel");
    const std::pair<int, int> id{m.picked_gate_key, m.picked_gate_part};
    const auto moved = p.r.gate_points.find(id);
    const auto at = p.active && moved != p.r.gate_points.end() ? moved->second : part->position;
    float v[3] = {static_cast<float>(at[0]) * kFx, static_cast<float>(at[1]) * kFx, static_cast<float>(at[2]) * kFx};
    if (!bar)
        ImGui::TextUnformatted(gate_name(app).c_str());
    if (!p.active) {
        if (bar)
            ImGui::SameLine();
        ImGui::TextUnformatted(
            fmt(lang, S::map_gate_at, {std::to_string(v[0]), std::to_string(v[1]), std::to_string(v[2])}).c_str());
        if (!bar)
            ImGui::TextDisabled("%s", tr(lang, S::map_gate_move_hint));
        ImGui::PopID();
        return;
    }
    if (bar)
        ImGui::SameLine();
    ImGui::SetNextItemWidth((bar ? 15.0f : 20.0f) * ImGui::GetFontSize());
    const bool dragged =
        ImGui::DragFloat3(bar ? "##at" : tr(lang, S::map_marker_place), v, 0.02f, -100000.0f, 100000.0f, "%.2f");
    undo_field();  // the exits' models as they were, for undo
    if (dragged) {
        p.r.gate_points[id] = {static_cast<std::int32_t>(std::lround(v[0] * 4096.0)),
                               static_cast<std::int32_t>(std::lround(v[1] * 4096.0)),
                               static_cast<std::int32_t>(std::lround(v[2] * 4096.0))};
    }
    if (moved != p.r.gate_points.end()) {
        if (bar)
            ImGui::SameLine();
        else
            same_line_if_room(button_width(tr(lang, S::map_gate_reset)));
        if (ImGui::SmallButton(tr(lang, S::map_gate_reset))) {
            edit_push();
            p.r.gate_points.erase(id);
        }
    }
    ImGui::PopID();
}

// The picked room marker's place, facing and To ground, in the panel (bar false) and over the picture (bar
// true). Both show the same values: while either is held (this frame or the last), `marker_edit` holds them for
// both and for the picture; the room is made again when one is let go of.
void room_marker_fields(App& app, bool bar) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    if (!showing_built())
        return;
    const std::vector<Marker> shown_markers = markers(p.built);
    if (m.picked_marker < 0 || m.picked_marker >= static_cast<int>(shown_markers.size()))
        return;
    const Marker& mk = shown_markers[static_cast<std::size_t>(m.picked_marker)];
    auto& e = m.marker_edit;
    const int frame = ImGui::GetFrameCount();
    if (e.held_frame < frame - 1 || e.name != mk.name) {
        e.name = mk.name;
        for (std::size_t a = 0; a < 3; ++a)
            e.at[a] = static_cast<float>(mk.place[a]) * kFx;
        e.facing = static_cast<float>(mk.extra) * 360.0f / 65536.0f;
    }
    float* at = e.at;
    float& facing = e.facing;
    bool moved = false;
    ImGui::PushID(bar ? "marker_bar" : "marker_panel");
    if (!bar) {
        ImGui::TextUnformatted(mk.name.c_str());
        if (mk.name.rfind("pent", 0) == 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr(lang, S::map_marker_entry));
        }
    } else {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth((bar ? 15.0f : 20.0f) * ImGui::GetFontSize());
    ImGui::DragFloat3(bar ? "##at" : tr(lang, S::map_marker_place), at, 0.02f, -100000.0f, 100000.0f, "%.2f");
    undo_field();  // the markers as they were, for undo
    moved |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemActive())
        e.held_frame = frame;
    if (bar)
        ImGui::SameLine();
    ImGui::SetNextItemWidth((bar ? 5.0f : 8.0f) * ImGui::GetFontSize());
    ImGui::DragFloat(tr(lang, S::map_marker_facing), &facing, 1.0f, -360.0f, 360.0f, "%.0f");
    undo_field();
    moved |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemActive())
        e.held_frame = frame;
    e.held = e.held_frame >= frame - 1;
    if (bar)
        ImGui::SameLine();
    else
        same_line_if_room(button_width(tr(lang, S::map_to_ground)));
    if (ImGui::SmallButton(tr(lang, S::map_to_ground))) {
        double y = 0;
        if (ground_under(p.built, at[0] * 4096.0, at[2] * 4096.0, at[1] * 4096.0 + 4096.0, &y) ||
            ground_under(p.built, at[0] * 4096.0, at[2] * 4096.0, 1e18, &y)) {
            edit_push();
            at[1] = static_cast<float>(y / 4096.0 + 0.1);  // as the game's sit, a tenth above
            moved = true;
        }
    }
    if (moved) {
        p.r.markers[mk.name] = {
            static_cast<std::int32_t>(std::lround(at[0] * 4096.0)),
            static_cast<std::int32_t>(std::lround(at[1] * 4096.0)),
            static_cast<std::int32_t>(std::lround(at[2] * 4096.0)),
            static_cast<std::int32_t>(std::lround(std::fmod(facing + 360.0f, 360.0f) * 65536.0f / 360.0f))};
        p.dirty = true;
        e.held_frame = frame;  // shown where it was let go of until the room is made again
    }
    ImGui::PopID();
}

// The picked mission point's place and facing over the picture, and To ground; kept for undo as the field is
// taken hold of, written to the mission at once (its summary and the picture follow).
void mission_point_fields(App& app) {
    const auto lang = app.language;
    const auto kind = static_cast<MissionPoint::Kind>(m.picked_point_kind);
    MissionPointPlace pp;
    if (!mission_point(app, kind, m.picked_point, &pp)) {
        m.picked_point_kind = m.picked_point = -1;
        return;
    }
    ImGui::PushID("point_bar");
    ImGui::SameLine();
    float v[3] = {static_cast<float>(pp.at[0]) * kFx, static_cast<float>(pp.at[1]) * kFx,
                  static_cast<float>(pp.at[2]) * kFx};
    float facing = static_cast<float>(pp.facing);
    ImGui::SetNextItemWidth(15.0f * ImGui::GetFontSize());
    bool changed = ImGui::DragFloat3("##at", v, 0.02f, -100000.0f, 100000.0f, "%.2f");
    if (ImGui::IsItemActivated())
        mission_point_push(app, kind, m.picked_point);
    else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit())
        mission_point_unpush();
    if (pp.has_facing) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(5.0f * ImGui::GetFontSize());
        changed |= ImGui::DragFloat(tr(lang, S::map_marker_facing), &facing, 1.0f, -360.0f, 360.0f, "%.0f");
        if (ImGui::IsItemActivated())
            mission_point_push(app, kind, m.picked_point);
        else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit())
            mission_point_unpush();
    }
    // To ground: the floor under it in the collision shown, a tenth above (as the game's sit)
    ImGui::SameLine();
    if (ImGui::SmallButton(tr(lang, S::map_to_ground))) {
        double y = 0;
        if (ground_under(shown(), v[0] * 4096.0, v[2] * 4096.0, v[1] * 4096.0 + 4096.0, &y) ||
            ground_under(shown(), v[0] * 4096.0, v[2] * 4096.0, 1e18, &y)) {
            mission_point_push(app, kind, m.picked_point);
            v[1] = static_cast<float>(y / 4096.0 + 0.1);
            changed = true;
        }
    }
    if (changed) {
        mission_point_move(app, kind, m.picked_point,
                           {static_cast<std::int32_t>(std::lround(v[0] * 4096.0)),
                            static_cast<std::int32_t>(std::lround(v[1] * 4096.0)),
                            static_cast<std::int32_t>(std::lround(v[2] * 4096.0))},
                           static_cast<int>(std::lround(facing)));
    }
    ImGui::PopID();
}

// A row under the tools, only while something is picked: its name and its place (and facing) to drag or type,
// the same values as the left panel's. Picking a room marker or an exit's model starts a markers-only change
// of the room when there is none, so they are movable at once.
void draw_selection_bar(App& app) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    const bool point = m.picked_point_kind >= 0 && m.picked_point >= 0;
    if (!point && !m.picked_gate && m.picked_marker < 0)
        return;
    if (!point && !p.active && m.room != nullptr && m.has_collision && app.current_mod() != nullptr) {
        start_kept_change();
        m.show_markers = true;
    }
    ImGui::AlignTextToFramePadding();
    if (point) {
        MissionPointPlace pp;
        if (!mission_point(app, static_cast<MissionPoint::Kind>(m.picked_point_kind), m.picked_point, &pp)) {
            m.picked_point_kind = m.picked_point = -1;
            ImGui::NewLine();
            return;
        }
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", pp.name.c_str());
        mission_point_fields(app);
    } else if (m.picked_gate) {
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", gate_name(app).c_str());
        draw_picked_gate(app, true);
    } else {
        const std::vector<Marker> list = showing_built() ? markers(p.built) : m.markers;
        if (m.picked_marker >= static_cast<int>(list.size())) {
            ImGui::NewLine();
            return;
        }
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s",
                           list[static_cast<std::size_t>(m.picked_marker)].name.c_str());
        const Marker& mk = list[static_cast<std::size_t>(m.picked_marker)];
        ImGui::SameLine();
        if (showing_built()) {
            room_marker_fields(app, true);
        } else if (p.active && !p.built_ok && p.edit_error.empty() && p.error.empty()) {
            ImGui::TextDisabled("%s", tr(lang, S::map_marker_bar_wait));
        } else {
            // the game's room shown (no mod, or "Game's" chosen): where it is, not movable here
            ImGui::TextDisabled("%.2f  %.2f  %.2f   %.0f°", static_cast<float>(mk.place[0]) * kFx,
                                static_cast<float>(mk.place[1]) * kFx, static_cast<float>(mk.place[2]) * kFx,
                                static_cast<double>(mk.extra) * 360.0 / 65536.0);
            if (app.current_mod() == nullptr) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", tr(lang, S::choose_mod_first));
            }
        }
    }
}

void draw_details(App& app) {
    const auto lang = app.language;
    const std::vector<Marker> shown_markers = showing_built() ? markers(m.rep.built) : m.markers;
    if (m.picked_marker >= static_cast<int>(shown_markers.size()))
        m.picked_marker = -1;
    if (m.picked_gate) {
        draw_picked_gate(app, false);
    } else if (m.picked_marker >= 0 && showing_built()) {
        // moved on the new room: where the change puts it
        room_marker_fields(app, false);
    } else if (m.picked_marker >= 0) {
        const Marker& mk = m.markers[static_cast<std::size_t>(m.picked_marker)];
        ImGui::TextUnformatted(fmt(lang, S::map_marker_picked,
                                   {mk.name, std::to_string(static_cast<float>(mk.place[0]) * kFx),
                                    std::to_string(static_cast<float>(mk.place[1]) * kFx),
                                    std::to_string(static_cast<float>(mk.place[2]) * kFx),
                                    std::to_string(static_cast<double>(mk.extra) * 360.0 / 65536.0)})
                                   .c_str());
        if (mk.name.rfind("pent", 0) == 0)
            ImGui::TextDisabled("%s", tr(lang, S::map_marker_entry));
    } else if (m.picked_face >= 0) {
        const RoomFace& f =
            shown().faces[static_cast<std::size_t>(m.picked_list)][static_cast<std::size_t>(m.picked_face)];
        const S list = m.picked_list == kGround ? S::map_grounds
                       : m.picked_list == kWall ? S::map_walls
                                                : S::map_ceilings;
        const std::string names = names_of(m.picked_list, f);
        ImGui::TextUnformatted(fmt(lang, S::map_face_picked,
                                   {tr(lang, list), std::to_string(m.picked_face), std::to_string(f.corners),
                                    names.empty() ? std::string("-") : names})
                                   .c_str());
        if (showing_built()) {
            // the face of the new room: its triangles take a name
            Replacing& p = m.rep;
            const auto& lists = p.report.face_triangles[static_cast<std::size_t>(m.picked_list)];
            const std::vector<std::size_t> triangles = static_cast<std::size_t>(m.picked_face) < lists.size()
                                                           ? lists[static_cast<std::size_t>(m.picked_face)]
                                                           : std::vector<std::size_t>{};
            if (triangles.empty()) {
                ImGui::TextDisabled("%s", tr(lang, S::map_face_kept));
            } else {
                ImGui::TextDisabled("%s", fmt(lang, S::map_face_made, {std::to_string(triangles.size())}).c_str());
                const auto choices = name_choices();
                ImGui::SetNextItemWidth(10 * ImGui::GetFontSize());
                if (ImGui::BeginCombo(tr(lang, S::map_give_name), tr(lang, S::map_add_name))) {
                    for (const auto& c : choices) {
                        if (ImGui::Selectable(c.c_str())) {
                            for (const auto t : triangles) {
                                auto& given = p.r.names_by_triangle[t];
                                if (std::find(given.begin(), given.end(), c) == given.end())
                                    given.push_back(c);
                            }
                            p.dirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                same_line_if_room(button_width(tr(lang, S::map_clear_names)));
                if (ImGui::SmallButton(tr(lang, S::map_clear_names))) {
                    for (const auto t : triangles)
                        p.r.names_by_triangle.erase(t);
                    p.dirty = true;
                }
            }
        }
    } else {
        ImGui::TextDisabled("%s", tr(lang, S::map_pick_hint));
    }
}

void draw_lists(App& app) {
    const auto lang = app.language;
    if (ImGui::CollapsingHeader(fmt(lang, S::map_named_list, {std::to_string(shown().named.size())}).c_str())) {
        for (std::size_t k = 0; k < shown().named.size(); ++k) {
            const std::string name = shown().named[k].text();
            int on[3] = {0, 0, 0};
            for (int l = 0; l < 3; ++l) {
                for (const auto& f : shown().faces[static_cast<std::size_t>(l)]) {
                    for (std::size_t p = 0; p < 4; ++p)
                        on[l] += f.slots[p] == k && slot_names(l, p) ? 1 : 0;
                }
            }
            ImGui::BulletText("%s", fmt(lang, S::map_named_row,
                                        {name, tr(lang, named_kind(name)), std::to_string(on[0]), std::to_string(on[1]),
                                         std::to_string(on[2])})
                                        .c_str());
            if (const char* readers = tag_readers(name)) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", fmt(lang, S::map_read_by, {readers}).c_str());
            } else if (named_kind(name) == S::map_kind_tag || named_kind(name) == S::map_kind_other) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr(lang, S::map_not_read));
            }
        }
    }
    if (ImGui::CollapsingHeader(fmt(lang, S::map_markers_list, {std::to_string(m.markers.size())}).c_str())) {
        for (std::size_t k = 0; k < m.markers.size(); ++k) {
            const Marker& mk = m.markers[k];
            if (ImGui::Selectable((mk.name + "##mk" + std::to_string(k)).c_str(),
                                  static_cast<int>(k) == m.picked_marker)) {
                m.picked_marker = static_cast<int>(k);
                m.picked_face = m.picked_list = -1;
                m.picked_gate = false;
                m.picked_point_kind = m.picked_point = -1;
            }
        }
    }
    if (ImGui::CollapsingHeader(fmt(lang, S::map_exits_list, {std::to_string(m.room->links.size())}).c_str())) {
        // the row's target as the data holds it: which room it names is not read yet
        for (const auto& l : m.room->links) {
            ImGui::BulletText("%s",
                              fmt(lang, S::map_exit_row, {std::to_string(l.key), std::to_string(l.target)}).c_str());
        }
    }
}

void draw_gate_models(App& app);  // below

// The names (the original's exits and walls that open, the materials' names) and the exits' models.
void draw_names_and_exits(App& app) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    // names: the original's exits and walls that open, the materials' names
    if (ImGui::TreeNode(tr(lang, S::map_names_title))) {
        ImGui::TextWrapped("%s", tr(lang, S::map_names_hint));
        for (std::size_t k = 0; k < m.collision.named.size(); ++k) {
            const std::string name = m.collision.named[k].text();
            const S kind = named_kind(name);
            if (kind != S::map_kind_gate && kind != S::map_kind_wall)
                continue;
            bool yours = false;
            for (const auto& [material, names] : p.r.names_by_material)
                yours = yours || std::find(names.begin(), names.end(), name) != names.end();
            for (const auto& [t, names] : p.r.names_by_triangle)
                yours = yours || std::find(names.begin(), names.end(), name) != names.end();
            if (yours) {
                ImGui::BulletText("%s", fmt(lang, S::map_on_your_faces, {name}).c_str());
                continue;
            }
            bool keep = p.r.drop_original.count(name) == 0;
            if (ImGui::Checkbox(fmt(lang, S::map_keep_original, {name}).c_str(), &keep)) {
                if (keep)
                    p.r.drop_original.erase(name);
                else
                    p.r.drop_original.insert(name);
                p.dirty = true;
            }
        }
        const auto choices = name_choices();
        for (const auto& material : collision_materials()) {
            ImGui::PushID(material.c_str());
            auto& names = p.r.names_by_material[material];
            std::string list;
            for (const auto& n : names)
                list += (list.empty() ? "" : ", ") + n;
            ImGui::TextUnformatted(fmt(lang, S::map_material_names,
                                       {material.empty() ? std::string("-") : material,
                                        list.empty() ? std::string(tr(lang, S::map_none)) : list})
                                       .c_str());
            same_line_if_room(9 * ImGui::GetFontSize());
            ImGui::SetNextItemWidth(9 * ImGui::GetFontSize());
            if (ImGui::BeginCombo("##add", tr(lang, S::map_add_name))) {
                for (const auto& c : choices) {
                    if (ImGui::Selectable(c.c_str()) && std::find(names.begin(), names.end(), c) == names.end()) {
                        names.push_back(c);
                        p.dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (!names.empty()) {
                same_line_if_room(button_width(tr(lang, S::map_clear_names)));
                if (ImGui::SmallButton(tr(lang, S::map_clear_names))) {
                    names.clear();
                    p.dirty = true;
                }
            }
            if (names.empty())
                p.r.names_by_material.erase(material);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    draw_gate_models(app);
}

// The exits' models of this room: where they stand, moved with the change.
void draw_gate_models(App& app) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    if (ImGui::TreeNode(tr(lang, S::map_gate_models))) {
        for (const auto& link : m.room->links) {
            for (const auto& g : m.world.gate_models) {
                if (g.key != static_cast<std::int8_t>(link.key))
                    continue;
                for (std::size_t part = 0; part < 5; ++part) {
                    if (g.parts[part].member == 0)
                        continue;
                    const std::pair<int, int> id{g.key, static_cast<int>(part)};
                    const auto moved = p.r.gate_points.find(id);
                    const auto at = moved != p.r.gate_points.end() ? moved->second : g.parts[part].position;
                    float v[3] = {static_cast<float>(at[0]) * kFx, static_cast<float>(at[1]) * kFx,
                                  static_cast<float>(at[2]) * kFx};
                    ImGui::SetNextItemWidth(20 * ImGui::GetFontSize());
                    const bool dragged = ImGui::DragFloat3(
                        fmt(lang, S::map_gate_part,
                            {std::to_string(g.key), std::to_string(part), std::to_string(g.parts[part].member)})
                            .c_str(),
                        v, 0.02f, -100000.0f, 100000.0f, "%.2f");
                    undo_field();
                    if (dragged) {
                        p.r.gate_points[id] = {static_cast<std::int32_t>(std::lround(v[0] * 4096.0)),
                                               static_cast<std::int32_t>(std::lround(v[1] * 4096.0)),
                                               static_cast<std::int32_t>(std::lround(v[2] * 4096.0))};
                    }
                }
            }
        }
        ImGui::TreePop();
    }
}

// The size of what a model spans, words for a person: "12.4 wide, 3.1 high, 9.0 deep".
std::string span_text(App& app, const std::array<float, 3>& lo, const std::array<float, 3>& hi) {
    char w[32], h[32], d[32];
    std::snprintf(w, sizeof(w), "%.1f", hi[0] - lo[0]);
    std::snprintf(h, sizeof(h), "%.1f", hi[1] - lo[1]);
    std::snprintf(d, sizeof(d), "%.1f", hi[2] - lo[2]);
    return fmt(app.language, S::map_span, {w, h, d});
}

// The room's own points (its models at rest, as drawn): what a model made from it is lined up with.
std::vector<std::array<float, 3>> game_points() {
    std::vector<std::array<float, 3>> out;
    for (const auto& rm : m.models) {
        if (!rm.problem.empty())
            continue;
        for (const auto& mesh : rm.game.model.meshes) {
            for (const auto& v : mesh.vertices) {
                std::array<float, 3> pos{0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    const float wgt = v.weights[static_cast<std::size_t>(k)];
                    const std::uint32_t jt = v.joints[static_cast<std::size_t>(k)];
                    if (wgt <= 0.0f || jt >= rm.palette.size())
                        continue;
                    const auto qp = mat_point(rm.palette[jt], v.position);
                    for (std::size_t c = 0; c < 3; ++c)
                        pos[c] += wgt * qp[c];
                }
                out.push_back(pos);
            }
        }
    }
    return out;
}

// The KH2 map this room was made from (the studio's pairs, then the mod's), or null.
const Kh2Pair* room_pair(App& app) {
    if (kh2.pairs_for != app.mod) {
        const Mod* mod = app.current_mod();
        std::vector<std::string> notes;
        kh2.pairs = kh2_pairs(mod != nullptr ? mod->dir : fs::path(), &notes);
        kh2.pairs_for = app.mod;
        for (const auto& note : notes)
            app.tell(note, false);
    }
    return m.room != nullptr ? kh2_pair_of(kh2.pairs, m.world.code, m.room->index) : nullptr;
}

// The modder's model lined up with the room when it is made from it, and the person told what was done.
void line_up(App& app) {
    Replacing& p = m.rep;
    if (p.raw.model.meshes.empty())
        return;
    // the KH2 map this room was made from, where Days put it
    const Kh2Pair* pair = !p.r.kh2.empty() ? room_pair(app) : nullptr;
    if (pair != nullptr && pair->placed && pair->map == p.r.kh2) {
        p.r.place = RoomPlacement{};
        p.r.place.scale = 0.01f;
        p.r.place.turn = {0, pair->turn_y, 0};
        p.r.place.move = pair->move;
        char turn[32], move[96];
        std::snprintf(turn, sizeof(turn), "%g", pair->turn_y);
        std::snprintf(move, sizeof(move), "%.2f %.2f %.2f", pair->move[0], pair->move[1], pair->move[2]);
        p.placed_text = fmt(app.language, S::map_align_pair, {turn, move});
        p.placed_ok = true;
        p.dirty = true;
        return;
    }
    const RoomAlignment a = !p.r.kh2.empty() && m.has_collision ? kh2_alignment(game_points(), m.collision, p.raw)
                                                                : align_with_room(game_points(), p.raw);
    char share[16];
    std::snprintf(share, sizeof(share), "%.0f", a.share * 100.0);
    if (a.verdict == RoomAlignment::kh2_units) {
        p.r.place = a.place;
        char scale[32];
        std::snprintf(scale, sizeof(scale), "%g", a.place.scale);
        p.placed_text = fmt(app.language, S::map_align_kh2, {scale});
        p.placed_ok = true;
    } else if (a.verdict == RoomAlignment::turned) {
        p.r.place = a.place;
        char turn[64], scale[32];
        std::snprintf(turn, sizeof(turn), "%.0f, %.0f, %.0f", a.place.turn[0], a.place.turn[1], a.place.turn[2]);
        std::snprintf(scale, sizeof(scale), "%g", a.place.scale);
        p.placed_text = fmt(app.language, S::map_align_turned, {turn, scale, share});
        p.placed_ok = true;
    } else if (a.verdict == RoomAlignment::in_place) {
        p.r.place = RoomPlacement{};
        p.placed_text = fmt(app.language, S::map_align_in_place, {share});
        p.placed_ok = true;
    } else {
        // the sizes, to place it by hand
        std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, rlo = lo, rhi = hi;
        assets::ImportedModel base = p.raw;
        place_model(&base, RoomPlacement{});
        for (const auto& mesh : base.model.meshes) {
            for (const auto& v : mesh.vertices) {
                for (std::size_t c = 0; c < 3; ++c)
                    lo[c] = std::min(lo[c], v.position[c]), hi[c] = std::max(hi[c], v.position[c]);
            }
        }
        for (const auto& list : m.collision.faces) {
            for (const auto& f : list) {
                for (std::size_t v = 0; v < f.corners && v < 4; ++v) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        rlo[c] = std::min(rlo[c], static_cast<float>(f.v[v][c]) * kFx);
                        rhi[c] = std::max(rhi[c], static_cast<float>(f.v[v][c]) * kFx);
                    }
                }
            }
        }
        p.placed_text = fmt(app.language, S::map_align_unrelated, {span_text(app, lo, hi), span_text(app, rlo, rhi)});
        p.placed_ok = false;
    }
    p.dirty = true;
}

// The KH2 map's own floors and walls as the room's collision model; false (the reason in the room's
// error) when they cannot be made.
bool load_kh2_collision(App& app, const std::string& map) {
    Replacing& p = m.rep;
    fs::path file;
    std::string why;
    if (make_kh2_collision(app.settings.root, map, &file, &why) && import_model(file, &p.raw_collision, &why)) {
        p.collision_source = file;
        return true;
    }
    p.error = why;
    return false;
}

// The worker's results taken: its list of maps, or the model it made (offered as the room's model).
void kh2_take(App& app, const Item& item) {
    bool listed = false, made = false;
    std::string failed, map;
    fs::path model;
    Kh2Made outcome;
    {
        std::lock_guard lock(kh2.mutex);
        std::swap(listed, kh2.listed);
        std::swap(made, kh2.made);
        failed = kh2.failed;
        map = kh2.done_map;
        model = kh2.model;
        outcome = kh2.outcome;
        if (listed)
            kh2.maps = kh2.found;
    }
    if (kh2.worker.joinable() && !kh2.busy)
        kh2.worker.join();
    if (listed) {
        kh2.error = failed;
        // this room's own KH2 room first
        const Kh2Pair* pair = m.room != nullptr ? kh2_pair_of(kh2.pairs, m.world.code, m.room->index) : nullptr;
        if (pair != nullptr) {
            const auto at = std::find(kh2.maps.begin(), kh2.maps.end(), pair->map);
            if (at != kh2.maps.end())
                std::rotate(kh2.maps.begin(), at, at + 1);
        }
        kh2.out.clear();
        for (const auto& name : kh2.maps)
            kh2.out[name] = kh2_map_taken_out(app.settings.root, name);
    }
    if (!made)
        return;
    if (!failed.empty()) {
        kh2.error = failed;
        app.tell(fmt(app.language, S::save_failed, {failed}), false);
        return;
    }
    kh2.error.clear();
    kh2.out[map] = true;
    kh2.radars.erase(map);
    m.rep.next_is_collision = false;
    m.rep.r.kh2 = map;
    // the game's floor and walls kept, unless KH2's were chosen
    if (!m.rep.kh2_collision) {
        m.rep.r.keep_collision = true;
        m.rep.other_collision = false;
        m.rep.collision_source.clear();
        m.rep.raw_collision = {};
        m.rep.r.collision.clear();
    }
    room_offer(app, item, model);
    if (m.rep.kh2_collision) {
        load_kh2_collision(app, map);  // KH2's own floors and walls: the new map's
        m.rep.dirty = true;
    }
    app.tell(fmt(app.language, S::map_kh2_made,
                 {map, std::to_string(outcome.triangles), std::to_string(outcome.pictures),
                  std::to_string(outcome.pieces), std::to_string(outcome.animations)}),
             true);
    for (const auto& line : outcome.report)
        app.tell(map + ": " + line, false);
}

void kh2_start(App& app, const std::string& map, const std::string& world) {
    if (kh2.busy)
        return;
    if (kh2.worker.joinable())
        kh2.worker.join();
    kh2.busy = true;
    kh2.working = map;
    kh2.error.clear();
    const Settings settings = app.settings;
    kh2.worker = std::thread([settings, map, world] {
        std::vector<std::string> found;
        std::string why;
        Kh2Made outcome;
        const fs::path model = map.empty() ? fs::path() : kh2_made_model(settings.root, map);
        const bool ok = map.empty() ? kh2_world_maps(settings, world, &found, &why)
                                    : take_out_kh2_map(settings, map, &why) &&
                                          make_kh2_room(kh2_folder(settings.root), map, model, &outcome, &why);
        std::lock_guard lock(kh2.mutex);
        kh2.failed = ok ? std::string() : why;
        if (map.empty()) {
            kh2.found = std::move(found);
            kh2.listed = true;
        } else {
            kh2.done_map = map;
            kh2.model = model;
            kh2.outcome = std::move(outcome);
            kh2.made = true;
        }
        kh2.busy = false;
    });
}

// The KH2 rooms of the player's copy to take as the room's model: a world's, or one by its name.
void draw_kh2_picker(App& app) {
    const auto lang = app.language;
    const ImVec4 warn(0.95f, 0.8f, 0.35f, 1);
    ImGui::Indent();
    ImGui::TextWrapped("%s", tr(lang, S::map_kh2_hint));
    if (kh2.collection_for != app.settings.collection_1525) {
        kh2.collection = kh2_collection(app.settings);
        kh2.collection_for = app.settings.collection_1525;
    }
    if (kh2.collection.empty()) {
        ImGui::TextColored(warn, "%s", tr(lang, S::map_kh2_no_collection));
        ImGui::Unindent();
        return;
    }
    ImGui::TextDisabled("%s", u8(kh2.collection).c_str());
    // the KH2 room this room was made from, when it is known
    const Kh2Pair* pair = room_pair(app);
    if (pair != nullptr) {
        ImGui::TextWrapped("%s", fmt(lang, S::map_kh2_pair, {pair->map, pair->evidence}).c_str());
        ImGui::BeginDisabled(kh2.busy);
        if (ImGui::Button((std::string(tr(lang, S::map_kh2_use)) + " " + pair->map + "##pair").c_str()))
            kh2_start(app, pair->map, {});
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("%s", tr(lang, S::map_kh2_no_pair));
    }
    if (kh2.world < 0) {
        kh2.world = 0;
        for (int w = 0; w < kKh2WorldCount; ++w) {
            if (m.world.code == kKh2Worlds[w])
                kh2.world = w;
        }
    }
    ImGui::BeginDisabled(kh2.busy);
    ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
    if (ImGui::Combo(tr(lang, S::map_kh2_world), &kh2.world, kKh2Worlds, kKh2WorldCount))
        kh2.maps.clear();
    ImGui::SameLine();
    if (ImGui::Button(tr(lang, S::map_kh2_list)))
        kh2_start(app, {}, kKh2Worlds[kh2.world]);
    ImGui::SetNextItemWidth(8 * ImGui::GetFontSize());
    ImGui::InputText(tr(lang, S::map_kh2_by_name), &kh2.typed);
    ImGui::SameLine();
    if (ImGui::Button((std::string(tr(lang, S::map_kh2_use)) + "##typed").c_str()) && !kh2.typed.empty())
        kh2_start(app, kh2.typed, {});
    ImGui::EndDisabled();
    if (kh2.busy) {
        ImGui::TextDisabled("%s", kh2.working.empty() ? tr(lang, S::map_kh2_list)
                                                      : fmt(lang, S::map_kh2_working, {kh2.working}).c_str());
    }
    if (!kh2.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1));
        ImGui::TextWrapped("%s", kh2.error.c_str());
        ImGui::PopStyleColor();
    }
    const float side = 4 * ImGui::GetFontSize();
    for (const auto& name : kh2.maps) {
        ImGui::PushID(name.c_str());
        auto radar = kh2.radars.find(name);
        if (radar == kh2.radars.end() && kh2.out[name]) {
            assets::DecodedTexture picture;
            kh2_radar(app.settings.root, name, &picture);
            radar = kh2.radars.emplace(name, std::move(picture)).first;
        }
        if (radar != kh2.radars.end() && !radar->second.rgba.empty()) {
            const GpuImage& image = app.images->pixels("kh2-radar/" + name, 1, radar->second.rgba, radar->second.width,
                                                       radar->second.height);
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(image.texture))),
                         ImVec2(side, side));
        } else {
            ImGui::Dummy(ImVec2(side, side));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (pair != nullptr && pair->map == name)
            ImGui::Text("%s  (%s)", name.c_str(), tr(lang, S::map_kh2_pair_mark));
        else
            ImGui::TextUnformatted(name.c_str());
        if (!kh2.out[name])
            ImGui::TextDisabled("%s", tr(lang, S::map_kh2_not_out));
        ImGui::BeginDisabled(kh2.busy);
        if (ImGui::SmallButton(tr(lang, S::map_kh2_use)))
            kh2_start(app, name, {});
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::Unindent();
}

// The modder's room raised so that most of the room's markers stand on one of its floors, and the person
// told how many do.
void raise_to_room_markers(App& app) {
    Replacing& p = m.rep;
    if (p.dirty)
        rebuild();
    std::vector<std::array<float, 3>> feet;
    for (const auto& mk : showing_built() ? markers(p.built) : m.markers) {
        feet.push_back({static_cast<float>(mk.place[0]) * kFx, static_cast<float>(mk.place[1]) * kFx,
                        static_cast<float>(mk.place[2]) * kFx});
    }
    std::size_t on = 0;
    const float raise = raise_to_markers(p.surfaces, feet, &on);
    char by[32];
    std::snprintf(by, sizeof(by), "%+.2f", raise);
    if (on > 0) {
        p.r.place.move[1] += raise;
        p.placed_text = fmt(app.language, S::map_raised, {by, std::to_string(on), std::to_string(feet.size())});
        p.placed_ok = on * 2 >= feet.size();
        p.dirty = true;
    } else {
        p.placed_text = tr(app.language, S::map_raised_none);
        p.placed_ok = false;
    }
}

void choose_room_export(App& app, const Item& item) {
    choose_save_file(app, export_name(item), "glTF", "gltf", app.dialog_room_exports);
}

// Above the picture while the room is being replaced: which room it shows,
// and why it is not yours when yours could not be built.
void draw_shown_room(App& app) {
    const auto lang = app.language;
    const Replacing& p = m.rep;
    if (!p.active)
        return;
    const char* views[3] = {tr(lang, S::map_show_new), tr(lang, S::map_show_game), tr(lang, S::map_show_both)};
    ImGui::SetNextItemWidth(12 * ImGui::GetFontSize());
    ImGui::Combo(tr(lang, S::map_show), &m.rep.view, views, 3);
    if (!p.built_ok && !p.dirty) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1));
        ImGui::TextWrapped("%s",
                           fmt(lang, S::map_yours_not_shown, {p.error.empty() ? std::string("-") : p.error}).c_str());
        ImGui::PopStyleColor();
    }
}

// Steps 2 and 3 of replacing a room by a model, and its advanced settings; whether a setting was let go of.
bool draw_model_steps(App& app, const Item& item) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    const ImVec4 warn(0.95f, 0.8f, 0.35f, 1), good(0.55f, 0.9f, 0.55f, 1);
    bool changed = false;
    // 2. where it sits: lined up by the tool when it can, the person's hand otherwise
    ImGui::SeparatorText(tr(lang, S::map_step_place));
    if (!p.placed_text.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, p.placed_ok ? good : warn);
        ImGui::TextWrapped("%s", p.placed_text.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button(tr(lang, S::map_align_button)))
        line_up(app);
    // the height that puts most of the room's markers on one of its floors
    same_line_if_room(button_width(tr(lang, S::map_raise_to_markers)));
    if (ImGui::Button(tr(lang, S::map_raise_to_markers)) && p.built_ok)
        raise_to_room_markers(app);
    ImGui::SetNextItemWidth(8 * ImGui::GetFontSize());
    ImGui::DragFloat(tr(lang, S::map_scale), &p.r.place.scale, 0.01f, 0.001f, 1000.0f, "%.3f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(20 * ImGui::GetFontSize());
    ImGui::DragFloat3(tr(lang, S::map_turn), p.r.place.turn.data(), 0.5f, -360.0f, 360.0f, "%.1f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(20 * ImGui::GetFontSize());
    ImGui::DragFloat3(tr(lang, S::map_move), p.r.place.move.data(), 0.05f, -100000.0f, 100000.0f, "%.2f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    if (p.r.place.scale <= 0.0f)
        p.r.place.scale = 0.001f;
    // only what lies around the room: a KH2 map is a whole place, the room a part of it
    bool cut = p.r.zone >= 0;
    if (ImGui::Checkbox(tr(lang, S::map_zone), &cut)) {
        p.r.zone = cut ? kZoneMargin : -1.0f;
        changed = true;
    }
    if (cut) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
        ImGui::DragFloat(tr(lang, S::map_zone_margin), &p.r.zone, 0.25f, 0.0f, 1000.0f, "%.1f");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        if (p.r.zone < 0)
            p.r.zone = 0;
    }

    // 3. the floor and walls: the game's kept, made from the model, or from a simpler one
    ImGui::SeparatorText(tr(lang, S::map_step_collision));
    int source = p.r.keep_collision                                 ? 0
                 : p.kh2_collision                                  ? 3
                 : p.other_collision || !p.collision_source.empty() ? 2
                                                                    : 1;
    const int before = source;
    ImGui::RadioButton(tr(lang, S::map_collision_keep), &source, 0);
    ImGui::RadioButton(tr(lang, S::map_collision_mine), &source, 1);
    ImGui::RadioButton(tr(lang, S::map_collision_other), &source, 2);
    if (!p.r.kh2.empty())
        ImGui::RadioButton(tr(lang, S::map_collision_kh2), &source, 3);
    if (source == 2) {
        ImGui::SameLine();
        ImGui::TextUnformatted(p.collision_source.empty() ? "-" : u8(p.collision_source.filename()).c_str());
        same_line_if_room(button_width(tr(lang, S::map_choose_collision)));
        if (ImGui::SmallButton(tr(lang, S::map_choose_collision))) {
            p.next_is_collision = true;
            choose_file(app, item);
        }
    }
    if (source != before) {
        p.r.keep_collision = source == 0;
        p.other_collision = source == 2 || source == 3;
        p.kh2_collision = source == 3;
        if (source != 2) {
            p.collision_source.clear();
            p.raw_collision = {};
            p.r.collision.clear();
        }
        if (source == 3 && !load_kh2_collision(app, p.r.kh2))
            p.kh2_collision = p.other_collision = false;
        p.dirty = true;
    }

    // advanced: the steepest ground, the room's other models, names, the exits' models
    if (ImGui::TreeNode(tr(lang, S::map_advanced))) {
        float degrees = std::acos(std::clamp(p.r.ground_limit, -1.0f, 1.0f)) * 57.2957795f;
        ImGui::SetNextItemWidth(8 * ImGui::GetFontSize());
        if (ImGui::DragFloat(tr(lang, S::map_ground_limit), &degrees, 0.1f, 1.0f, 89.0f, "%.1f")) {
            p.r.ground_limit = std::cos(std::clamp(degrees, 1.0f, 89.0f) / 57.2957795f);
        }
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        const char* uses[3] = {tr(lang, S::map_use_model), tr(lang, S::map_use_hide), tr(lang, S::map_use_keep)};
        for (const auto& [member, use] : object_uses(*m.room, p.r)) {
            std::string name = std::to_string(member);
            for (const auto& rm : m.models) {
                if (rm.member == member && !rm.key.empty())
                    name = rm.key.substr(rm.key.rfind('/') + 1);
            }
            int choice = use == ObjectUse::model ? 0 : use == ObjectUse::hide ? 1 : 2;
            ImGui::SetNextItemWidth(10 * ImGui::GetFontSize());
            if (ImGui::Combo(fmt(lang, S::map_object, {std::to_string(member), name}).c_str(), &choice, uses, 3)) {
                p.r.objects[member] = choice == 0 ? ObjectUse::model : choice == 1 ? ObjectUse::hide : ObjectUse::keep;
                p.dirty = true;
            }
        }
        draw_names_and_exits(app);
        ImGui::TreePop();
    }
    return changed;
}

// A change of the room that keeps the game's look and floor (markers, exits' models and hand edits only).
void start_kept_change() {
    Replacing& p = m.rep;
    p = Replacing{};
    history_clear(EditOwner::room);
    p.active = true;
    p.r.world = m.world.code;
    p.r.room = m.room->index;
    p.r.keep_collision = true;
    p.dirty = true;
}

// --- The collision editor ---------------------------------------------------------------------------------

// The shapes in the order the lists and buttons show them.
constexpr std::pair<S, CollisionShape::Kind> kShapeKinds[4] = {{S::map_shape_plane, CollisionShape::plane},
                                                               {S::map_shape_ramp, CollisionShape::ramp},
                                                               {S::map_shape_box, CollisionShape::box},
                                                               {S::map_shape_wedge, CollisionShape::wedge}};

// The collision editor on or off (the panel's header and the bar's button): on, picking in the picture is on,
// the floors and walls shown, and a room with no change yet starts one keeping the game's room.
void set_editing(bool on) {
    m.editing = on;
    if (!on) {
        m.gizmo_drag = -1;
        m.box_on = false;
        return;
    }
    m.show_ground = m.show_walls = true;
    if (!m.rep.active && m.room != nullptr && m.has_collision)
        start_kept_change();
    if (m.rep.view == 1)
        m.rep.view = 0;
}

Replacing::UndoStep undo_state() {
    return {m.rep.r.edits, m.rep.r.markers, m.rep.r.gate_points};
}

// Before each change of the hand edits, a marker or an exit's model: all of them as they were, for undo.
void edit_push() {
    Replacing& p = m.rep;
    p.undo.push_back(undo_state());
    if (p.undo.size() > 200)
        p.undo.pop_front();
    p.parked_redo = std::move(p.redo);
    p.redo.clear();
    history_push(EditOwner::room);
}

// The step edit_push just kept taken back (a drag cancelled, or a field let go of unchanged), Redo as it was.
void edit_unpush() {
    Replacing& p = m.rep;
    if (p.undo.empty())
        return;
    p.undo.pop_back();
    p.redo = std::move(p.parked_redo);
    p.parked_redo.clear();
    history_unpush(EditOwner::room);
}

// After a field that changes the room's change: its state kept for undo when taken hold of, and given back
// when let go of with nothing changed (a click into a field leaves no step to undo).
void undo_field() {
    if (ImGui::IsItemActivated())
        edit_push();
    else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit())
        edit_unpush();
}

void edits_changed() {
    m.rep.edits_dirty = true;
    m.rep.message.clear();
    m.shape_names_for = -1;  // the picked shape's names field filled again (shapes may have moved or changed)
}

bool edit_undo(bool redo) {
    Replacing& p = m.rep;
    auto& from = redo ? p.redo : p.undo;
    auto& to = redo ? p.undo : p.redo;
    if (from.empty())
        return false;
    to.push_back(undo_state());
    const bool markers_moved = from.back().markers != p.r.markers;
    p.r.edits = std::move(from.back().edits);
    p.r.markers = std::move(from.back().markers);
    p.r.gate_points = std::move(from.back().gate_points);
    from.pop_back();
    if (m.picked_shape >= static_cast<int>(p.r.edits.shapes.size()))
        m.picked_shape = -1;
    m.marker_edit.held = false;  // the picked marker's fields show where it is now
    m.marker_edit.held_frame = -10;
    edits_changed();
    if (markers_moved)
        p.dirty = true;  // a model's collision holds its markers: made again
    return true;
}

// The shape picked in the picture or the list, or null.
CollisionShape* picked_shape() {
    auto& shapes = m.rep.r.edits.shapes;
    if (m.picked_shape < 0 || m.picked_shape >= static_cast<int>(shapes.size()))
        return nullptr;
    return &shapes[static_cast<std::size_t>(m.picked_shape)];
}

// The picked shape lowered or raised onto the floor under it (its own faces left out).
void drop_picked_shape(App& app) {
    CollisionShape* picked = picked_shape();
    if (!showing_built() || picked == nullptr)
        return;
    const int index = m.picked_shape;
    CollisionShape s = *picked;
    if (!drop_to_ground(shown(), &s, [&](int l, std::size_t j) { return shape_of_shown(l, j) == index; })) {
        app.tell(tr(app.language, S::map_drop_none), false);
        return;
    }
    edit_push();
    *picked = std::move(s);
    edits_changed();
}

// The picked shape turned a quarter about the up axis, copied beside itself (the copy picked), or deleted.
void turn_picked_shape() {
    CollisionShape* s = picked_shape();
    if (s == nullptr)
        return;
    edit_push();
    s->turn[1] = std::fmod(s->turn[1] + 90.0f, 360.0f);
    edits_changed();
}

void copy_picked_shape() {
    CollisionShape* s = picked_shape();
    if (s == nullptr)
        return;
    edit_push();
    CollisionShape copy = *s;
    copy.at[0] += 1.0f;
    auto& shapes = m.rep.r.edits.shapes;
    shapes.push_back(std::move(copy));
    m.picked_shape = static_cast<int>(shapes.size()) - 1;
    edits_changed();
}

void delete_picked_shape() {
    if (picked_shape() == nullptr)
        return;
    edit_push();
    auto& shapes = m.rep.r.edits.shapes;
    shapes.erase(shapes.begin() + m.picked_shape);
    m.picked_shape = -1;
    edits_changed();
}

// The names a face gives (its gate, wall that opens, surface tags), as shown.
std::vector<std::string> face_names(int list, const RoomFace& f) {
    std::vector<std::string> out;
    for (std::size_t s = 0; s < 4; ++s) {
        if (f.slots[s] != 0xff && slot_names(list, s) && f.slots[s] < shown().named.size())
            out.push_back(shown().named[f.slots[s]].text());
    }
    return out;
}

const RoomFace* shown_face(const FaceKey& k) {
    if (k.list < 0 || k.list > 2)
        return nullptr;
    for (const auto& f : shown().faces[static_cast<std::size_t>(k.list)]) {
        if (face_key(k.list, f) == k)
            return &f;
    }
    return nullptr;
}

// The selected faces given another kind, removed, or named.
void selected_to_kind(int to) {
    edit_push();
    std::set<FaceKey> now;
    for (const auto& k : m.selected) {
        const FaceKey b = base_key(k);
        FaceEdit& fe = m.rep.r.edits.faces[b];
        fe.list = to == b.list ? -1 : to;
        if (!fe.removed && fe.list < 0 && !fe.renamed)
            m.rep.r.edits.faces.erase(b);
        FaceKey shown_key = k;
        shown_key.list = to;
        now.insert(shown_key);
    }
    m.selected = std::move(now);
    edits_changed();
}

void remove_selected() {
    if (m.selected.empty())
        return;
    edit_push();
    for (const auto& k : m.selected)
        m.rep.r.edits.faces[base_key(k)].removed = true;
    m.selected.clear();
    edits_changed();
}

// Names typed apart by commas or spaces, and written back that way.
std::vector<std::string> names_from_text(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char ch : text + ",") {
        if (ch == ',' || ch == ' ') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    return out;
}

std::string names_line(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

// The names field of the selected faces: filled from the first of them whenever it changes.
void sync_names_text() {
    if (m.selected.empty())
        return;
    const FaceKey& first = *m.selected.begin();
    if (m.names_for == first)
        return;
    m.names_for = first;
    const RoomFace* f = shown_face(first);
    m.names_text = f != nullptr ? names_line(face_names(first.list, *f)) : std::string();
}

void apply_names_to_selected() {
    edit_push();
    const auto names = names_from_text(m.names_text);
    for (const auto& k : m.selected) {
        FaceEdit& fe = m.rep.r.edits.faces[base_key(k)];
        fe.renamed = true;
        fe.names = names;
    }
    edits_changed();
}

// The selected faces' corners and their normals summed (as shown), for a shape fitted over them.
void selection_points(std::vector<std::array<float, 3>>* points, std::array<float, 3>* normal) {
    *normal = {0, 0, 0};
    for (const auto& k : m.selected) {
        const RoomFace* f = shown_face(k);
        if (f == nullptr || f->corners < 3)
            continue;
        for (std::size_t v = 0; v < f->corners; ++v)
            points->push_back(corner(*f, v));
        const auto a = corner(*f, 0), b = corner(*f, 1), c = corner(*f, 2);
        const std::array<float, 3> e0{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e1{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        std::array<float, 3> n{e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                               e0[0] * e1[1] - e0[1] * e1[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0)
            for (std::size_t q = 0; q < 3; ++q)
                (*normal)[q] += n[q] / l;
    }
}

// A new shape: fitted over the selected faces (a plane lying on them, a ramp or wedge rising the way they
// rise), else at the middle of the view.
void add_shape(CollisionShape::Kind kind) {
    edit_push();
    std::vector<std::array<float, 3>> points;
    std::array<float, 3> normal{};
    selection_points(&points, &normal);
    m.rep.r.edits.shapes.push_back(fit_shape(kind, points, normal, m.target));
    m.picked_shape = static_cast<int>(m.rep.r.edits.shapes.size()) - 1;
    edits_changed();
}

// The selected faces made one shape: fitted over them, given their kind (a plane's or ramp's, when all are one)
// and names (when all share them), and the faces removed -- one step to undo.
void convert_selection(CollisionShape::Kind kind) {
    if (m.selected.empty())
        return;
    std::vector<std::array<float, 3>> points;
    std::array<float, 3> normal{};
    selection_points(&points, &normal);
    if (points.empty())
        return;
    edit_push();
    CollisionShape s = fit_shape(kind, points, normal, m.target);
    bool one_list = true, one_names = true, first = true;
    int list = -1;
    std::vector<std::string> names;
    for (const auto& k : m.selected) {
        const RoomFace* f = shown_face(k);
        if (f == nullptr)
            continue;
        auto these = face_names(k.list, *f);
        if (first) {
            list = k.list, names = std::move(these), first = false;
        } else {
            one_list = one_list && k.list == list;
            one_names = one_names && these == names;
        }
    }
    if (one_list && (kind == CollisionShape::plane || kind == CollisionShape::ramp))
        s.list = list;  // walls stay walls, a steep ground stays a ground
    if (one_names)
        s.names = std::move(names);
    for (const auto& k : m.selected)
        m.rep.r.edits.faces[base_key(k)].removed = true;
    m.selected.clear();
    m.rep.r.edits.shapes.push_back(std::move(s));
    m.picked_shape = static_cast<int>(m.rep.r.edits.shapes.size()) - 1;
    edits_changed();
}

// --- The picked shape's fields, in the panel (labelled, one a row) or in the bar under the picture (across) ---

void shape_kind_combo(App& app, CollisionShape& s, bool bar) {
    const auto lang = app.language;
    const char* kinds[4];
    for (std::size_t i = 0; i < 4; ++i)
        kinds[i] = tr(lang, kShapeKinds[i].first);
    int kind = static_cast<int>(s.kind);
    ImGui::SetNextItemWidth((bar ? 6 : 10) * ImGui::GetFontSize());
    if (ImGui::Combo(bar ? "##shape" : tr(lang, S::map_shape_kind), &kind, kinds, 4) &&
        kind != static_cast<int>(s.kind)) {
        edit_push();
        s.kind = static_cast<CollisionShape::Kind>(kind);
        edits_changed();
    }
}

// what its faces are: each by its slope, or all one kind
void shape_list_combo(App& app, CollisionShape& s, bool bar) {
    const auto lang = app.language;
    const char* lists[4] = {tr(lang, S::map_shape_by_slope), tr(lang, S::map_grounds), tr(lang, S::map_walls),
                            tr(lang, S::map_ceilings)};
    int list = s.list + 1;
    ImGui::SetNextItemWidth((bar ? 8 : 10) * ImGui::GetFontSize());
    if (ImGui::Combo(bar ? "##list" : tr(lang, S::map_shape_list), &list, lists, 4) && list - 1 != s.list) {
        edit_push();
        s.list = list - 1;
        edits_changed();
    }
}

// Its place, turn and size: dragged or typed, the outline follows at once and the faces are made again when
// let go.
void shape_fields(App& app, CollisionShape& s, bool bar) {
    const auto lang = app.language;
    const float w = ImGui::GetFontSize();
    auto field = [&](S panel_label, S bar_label, std::array<float, 3>& v, float speed, const char* format) {
        if (bar) {
            same_line_if_room(16 * w);
            ImGui::TextUnformatted(tr(lang, bar_label));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(12 * w);
            ImGui::PushID(static_cast<int>(bar_label));
            ImGui::DragFloat3("##v", v.data(), speed, -100000.0f, 100000.0f, format);
            ImGui::PopID();
        } else {
            ImGui::SetNextItemWidth(20 * w);
            ImGui::DragFloat3(tr(lang, panel_label), v.data(), speed, -100000.0f, 100000.0f, format);
        }
        undo_field();
        if (ImGui::IsItemDeactivatedAfterEdit())
            edits_changed();
    };
    field(S::map_shape_at, S::map_tb_place, s.at, 0.02f, "%.2f");
    field(S::map_shape_turn, S::map_tb_turn, s.turn, 0.5f, "%.1f");
    field(S::map_shape_size, S::map_tb_size, s.size, 0.02f, "%.2f");
    for (auto& c : s.size)
        c = std::max(c, 0.01f);
}

// stood on from both sides: a plane's or a ramp's
void shape_both_check(App& app, CollisionShape& s, bool bar) {
    if (s.kind != CollisionShape::plane && s.kind != CollisionShape::ramp)
        return;
    const char* label = tr(app.language, S::map_shape_both);
    if (bar)
        same_line_if_room(check_width(label));
    bool both = s.both_sides;
    if (ImGui::Checkbox(label, &both)) {
        edit_push();
        s.both_sides = both;
        edits_changed();
    }
}

// Turn 90, (Drop, in the bar,) Copy and Delete. Drawn last: Copy and Delete change the list the shape is in.
void shape_buttons(App& app, bool bar) {
    const auto lang = app.language;
    auto button = [&](S label, bool first) {
        if (bar)
            same_line_if_room(button_width(tr(lang, label)));
        else if (!first)
            ImGui::SameLine();
        return ImGui::Button(tr(lang, label));
    };
    if (button(S::map_shape_turn90, true))
        turn_picked_shape();
    if (bar && button(S::map_drop, false))
        drop_picked_shape(app);
    const bool copy = button(S::map_shape_copy, false);
    const bool remove = button(S::map_shape_delete, false);
    if (copy)
        copy_picked_shape();
    else if (remove)
        delete_picked_shape();
}

void draw_shape_fields(App& app) {
    const auto lang = app.language;
    CollisionShape* picked = picked_shape();
    if (picked == nullptr)
        return;
    CollisionShape& s = *picked;
    ImGui::PushID("shape");
    // the handles in the picture: which ones (also 1, 2, 3 over the picture)
    ImGui::RadioButton(tr(lang, S::map_gizmo_move), &m.gizmo_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(tr(lang, S::map_gizmo_size), &m.gizmo_mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton(tr(lang, S::map_gizmo_turn), &m.gizmo_mode, 2);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(lang, S::map_gizmo_hint));
    shape_kind_combo(app, s, false);
    shape_fields(app, s, false);
    shape_list_combo(app, s, false);
    shape_both_check(app, s, false);
    if (m.shape_names_for != m.picked_shape) {
        m.shape_names_for = m.picked_shape;
        m.shape_names_text = names_line(s.names);
    }
    ImGui::SetNextItemWidth(14 * ImGui::GetFontSize());
    ImGui::InputText(tr(lang, S::map_edit_names), &m.shape_names_text);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        edit_push();
        s.names = names_from_text(m.shape_names_text);
        edits_changed();
    }
    shape_buttons(app, false);
    ImGui::PopID();
}

// What to do with the selected faces: delete them, give them another kind, select none, or convert them to a
// shape.
void draw_selection_actions(App& app) {
    const auto lang = app.language;
    if (ImGui::Button(tr(lang, S::map_edit_delete)))
        remove_selected();
    const S kinds[3] = {S::map_edit_make_ground, S::map_edit_make_wall, S::map_edit_make_ceiling};
    for (int l = 0; l < 3; ++l) {
        same_line_if_room(button_width(tr(lang, kinds[l])));
        if (ImGui::Button(tr(lang, kinds[l])))
            selected_to_kind(l);
    }
    same_line_if_room(button_width(tr(lang, S::map_edit_select_none)));
    if (ImGui::Button(tr(lang, S::map_edit_select_none)))
        m.selected.clear();
    draw_convert_buttons(app);
}

// "Add:" and a button per shape.
void draw_add_buttons(App& app) {
    ImGui::TextUnformatted(tr(app.language, S::map_edit_add));
    for (const auto& [label, kind] : kShapeKinds) {
        ImGui::SameLine();
        if (ImGui::Button(tr(app.language, label)))
            add_shape(kind);
    }
}

void draw_collision_editor(App& app) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    const ImVec4 warn(0.95f, 0.8f, 0.35f, 1);
    // open while editing: the same switch as the bar's "Edit collision" (picking in the picture on with it)
    ImGui::SetNextItemOpen(m.editing, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(tr(lang, S::map_tb_edit));
    if (open != m.editing)
        set_editing(open);
    if (!open)
        return;
    if (app.current_mod() == nullptr) {
        ImGui::TextColored(warn, "%s", tr(lang, S::choose_mod_first));
        return;
    }
    if (!m.has_collision) {
        ImGui::TextDisabled("%s", tr(lang, S::map_no_collision));
        return;
    }
    if (!p.active)
        return;  // started by set_editing
    if (p.view == 1)
        p.view = 0;  // the edits are on yours
    ImGui::TextDisabled("%s", tr(lang, S::map_edit_help));
    // the keys, while no field is typed in or held and no handle is dragged (each keeps its own undo step)
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() && m.gizmo_drag < 0 && m.point_drag < 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))
            drop_picked_shape(app);
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !m.selected.empty())
            remove_selected();
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            delete_picked_shape();
    }
    if (!p.edit_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1));
        ImGui::TextWrapped("%s", fmt(lang, S::map_edit_error, {p.edit_error}).c_str());
        ImGui::PopStyleColor();
    }

    // the selected faces
    ImGui::SeparatorText(fmt(lang, S::map_edit_selected, {std::to_string(m.selected.size())}).c_str());
    if (!m.selected.empty())
        draw_selection_actions(app);
    if (!m.selected.empty()) {
        // names: those of the first selected face to start from (still selected after the actions above)
        sync_names_text();
        ImGui::SetNextItemWidth(14 * ImGui::GetFontSize());
        ImGui::InputText(tr(lang, S::map_edit_names), &m.names_text);
        same_line_if_room(button_width(tr(lang, S::map_edit_apply_names)));
        if (ImGui::Button(tr(lang, S::map_edit_apply_names)))
            apply_names_to_selected();
    }
    // what the hand edits do to the room's own faces
    if (!p.r.edits.faces.empty()) {
        std::size_t removed = 0, kinds = 0, renamed = 0;
        for (const auto& [key, fe] : p.r.edits.faces) {
            removed += fe.removed ? 1 : 0;
            kinds += !fe.removed && fe.list >= 0 && fe.list != key.list ? 1 : 0;
            renamed += !fe.removed && fe.renamed ? 1 : 0;
        }
        ImGui::TextUnformatted(
            fmt(lang, S::map_edit_counts, {std::to_string(removed), std::to_string(kinds), std::to_string(renamed)})
                .c_str());
        same_line_if_room(button_width(tr(lang, S::map_edit_bring_back)));
        if (ImGui::SmallButton(tr(lang, S::map_edit_bring_back))) {
            edit_push();
            p.r.edits.faces.clear();
            edits_changed();
        }
    }
    if (p.applied.unmatched > 0)
        ImGui::TextColored(warn, "%s", fmt(lang, S::map_edit_unmatched, {std::to_string(p.applied.unmatched)}).c_str());

    // the shapes
    ImGui::SeparatorText(fmt(lang, S::map_edit_shapes, {std::to_string(p.r.edits.shapes.size())}).c_str());
    draw_add_buttons(app);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(lang, S::map_edit_add_hint));
    for (std::size_t s = 0; s < p.r.edits.shapes.size(); ++s) {
        const auto& sh = p.r.edits.shapes[s];
        char at[3][16];
        for (std::size_t c = 0; c < 3; ++c)
            std::snprintf(at[c], sizeof(at[c]), "%.2f", static_cast<double>(sh.at[c]));
        const std::string row =
            fmt(lang, S::map_shape_row,
                {std::to_string(s + 1), tr(lang, kShapeKinds[static_cast<std::size_t>(sh.kind)].first), at[0], at[1],
                 at[2]}) +
            "##shape" + std::to_string(s);
        if (ImGui::Selectable(row.c_str(), static_cast<int>(s) == m.picked_shape))
            m.picked_shape = static_cast<int>(s);
    }
    draw_shape_fields(app);
}

// The room's change saved into the mod: "Save into the mod" in the panel, "Save room" over the picture.
void save_room(App& app) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    const Mod* mod = app.current_mod();
    if (mod == nullptr || !p.built_ok || !p.edit_error.empty())
        return;
    const fs::path dump = resolve(app.settings, app.settings.graphics_dump);

        WorldOutcome o;
        std::string why;
        p.r.world = m.world.code;
        p.r.room = m.room->index;
        if (save_change(nitrofs(), dump, mod->dir, p.r, p.model_source, p.collision_source, &o, &why)) {
            // from now on, the mod's copies
            const fs::path folder = change_models(mod->dir, m.world.code, m.room->index);
            if (!p.model_source.empty())
                p.model_source = folder / p.model_source.filename();
            if (!p.collision_source.empty())
                p.collision_source = folder / p.collision_source.filename();
            p.from_mod = true;
            p.message = fmt(lang, S::map_saved, {mod->name});
            app.tell(p.message + "  " + tr(lang, S::restart_note), true);
        } else {
            p.error = why;
            app.tell(fmt(lang, S::save_failed, {why}), false);
        }
}

// --- The bars over and under the picture: the collision editor's tools laid out across ---------------------

// "Convert to:" and a button per shape: the selected faces made that shape (convert_selection).
void draw_convert_buttons(App& app) {
    const auto lang = app.language;
    ImGui::PushID("convert");
    same_line_if_room(ImGui::CalcTextSize(tr(lang, S::map_convert)).x + 4 * button_width(tr(lang, S::map_shape_wedge)));
    ImGui::TextUnformatted(tr(lang, S::map_convert));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(lang, S::map_convert_hint));
    for (const auto& [label, kind] : kShapeKinds) {
        ImGui::SameLine();
        if (ImGui::Button(tr(lang, label)))
            convert_selection(kind);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr(lang, S::map_convert_hint));
    }
    ImGui::PopID();
}

// A button shown pressed while `on`.
bool toggle_button(const char* label, bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    }
    const bool clicked = ImGui::Button(label);
    if (on)
        ImGui::PopStyleColor(3);
    return clicked;
}

// a gap between groups of tools: a faint bar
void bar_gap() {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

// Over the picture: start editing, pick on/off, the handles' mode, shapes to add, undo, the selection's actions.
void draw_map_tools(App& app) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    if (app.current_mod() == nullptr || !m.has_collision)
        return;
    ImGui::PushID("map_toolbar");
    // undo and redo first, over the picture's left corner: the room's change and the mission's points
    if (p.active || history_can(false) || history_can(true)) {
        ImGui::BeginDisabled(!history_can(false));
        if (ImGui::Button(tr(lang, S::map_edit_undo)))
            history_step(app, false, true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!history_can(true));
        if (ImGui::Button(tr(lang, S::map_edit_redo)))
            history_step(app, true, true);
        ImGui::EndDisabled();
        bar_gap();
    }
    if (toggle_button(tr(lang, S::map_tb_edit), m.editing))
        set_editing(!m.editing);
    if (m.editing) {
        bar_gap();
        ImGui::TextUnformatted(tr(lang, S::map_tb_mode));
        const S modes[3] = {S::map_gizmo_move, S::map_gizmo_size, S::map_gizmo_turn};
        for (int i = 0; i < 3; ++i) {
            ImGui::SameLine();
            if (toggle_button(tr(lang, modes[i]), m.gizmo_mode == i))
                m.gizmo_mode = i;
        }
        bar_gap();
        ImGui::Checkbox(tr(lang, S::map_snap_surface), &m.snap_surface);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr(lang, S::map_snap_surface_hint));
        ImGui::SameLine();
        ImGui::BeginDisabled(picked_shape() == nullptr);
        if (ImGui::Button(tr(lang, S::map_drop)))
            drop_picked_shape(app);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr(lang, S::map_drop_hint));
        bar_gap();
        draw_add_buttons(app);
        if (!m.selected.empty()) {
            bar_gap();
            draw_selection_actions(app);
        }
    }
    ImGui::PopID();
}

// "FOV:" and the camera's field of view: dragged, or typed after a double-click (or Ctrl+click); a right-click
// puts back the default. Kept in the settings when let go.
float fov_field_width() {
    return 4.0f * ImGui::GetFontSize();
}

float fov_width(const App& app) {
    return ImGui::CalcTextSize(tr(app.language, S::map_fov)).x + ImGui::GetStyle().ItemSpacing.x + fov_field_width();
}

void draw_fov_field(App& app) {
    const auto lang = app.language;
    ImGui::PushID("fov");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(lang, S::map_fov));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(lang, S::map_fov_hint));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fov_field_width());
    ImGui::DragFloat("##fov", &app.settings.map_fov, 0.25f, kOrbitFovMin, kOrbitFovMax, "%.1f",
                     ImGuiSliderFlags_AlwaysClamp);
    bool save = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        ImGui::SetTooltip("%s", tr(lang, S::map_fov_hint));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && app.settings.map_fov != kOrbitFov) {
        app.settings.map_fov = kOrbitFov;
        save = true;
    }
    if (save) {
        std::string why;
        if (!save_settings(app.settings, &why))
            app.tell(fmt(lang, S::save_failed, {why}), false);
    }
    ImGui::PopID();
}

// The bar over the picture: the collision editor's tools, then (at its right end) the camera: orbiting the
// point in front of it, or looking around from where it stands, with its field of view over it at the end of
// the view's checkboxes' row (all kept in the settings).
void draw_map_toolbar(App& app) {
    const auto lang = app.language;
    // the row before the bar (the view's checkboxes, its last one just drawn): its top and where it ends
    const float above_y = ImGui::GetItemRectMin().y, above_end = ImGui::GetItemRectMax().x;
    draw_map_tools(app);
    const float space = ImGui::GetStyle().ItemSpacing.x;
    const char* label = tr(lang, S::map_camera);
    float width = ImGui::CalcTextSize(label).x + button_width(tr(lang, S::map_camera_orbit)) +
                  button_width(tr(lang, S::map_camera_fps)) + 2 * space;
    const float right = ImGui::GetWindowContentRegionMax().x, right_x = ImGui::GetWindowPos().x + right;
    const float fov_w = fov_width(app);
    // the FOV field over the camera when the checkboxes' row has room at its end, else before "Camera:"
    const bool fov_above = above_end + space + fov_w <= right_x;
    if (!fov_above)
        width += fov_w + space;
    // at the right end of the tools' last row when it has room, else a row of its own (also when there are
    // no tools: the row it would share is the checkboxes', the FOV field's)
    ImGui::SameLine();
    const bool on_above_row = std::abs(ImGui::GetCursorScreenPos().y - above_y) < 0.5f;
    if (ImGui::GetCursorPosX() + width > right || (fov_above && on_above_row))
        ImGui::NewLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - width));
    ImGui::PushID("camera");
    if (!fov_above) {
        draw_fov_field(app);
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(lang, S::map_camera_hint));
    bool fps = app.settings.map_camera_fps;
    ImGui::SameLine();
    if (toggle_button(tr(lang, S::map_camera_orbit), !fps))
        fps = false;
    ImGui::SameLine();
    if (toggle_button(tr(lang, S::map_camera_fps), app.settings.map_camera_fps))
        fps = true;
    if (fps != app.settings.map_camera_fps) {
        app.settings.map_camera_fps = fps;
        std::string why;
        if (!save_settings(app.settings, &why))
            app.tell(fmt(lang, S::save_failed, {why}), false);
    }
    if (fov_above) {
        // over the camera, right-aligned with it, then back to where the bar ends
        const ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(right_x - fov_w, above_y));
        draw_fov_field(app);
        ImGui::SetCursorScreenPos(back);
    }
    ImGui::PopID();
}

// Under the picture: the picked shape's fields across, or the selected faces' names, or what to do.
void draw_map_bottom_bar(App& app) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    ImGui::PushID("map_bottom_bar");
    CollisionShape* picked = m.editing && p.active ? picked_shape() : nullptr;
    if (picked != nullptr) {
        CollisionShape& s = *picked;
        ImGui::Text("%d.", m.picked_shape + 1);
        ImGui::SameLine();
        shape_kind_combo(app, s, true);
        ImGui::SameLine();
        shape_list_combo(app, s, true);
        shape_fields(app, s, true);
        shape_both_check(app, s, true);
        shape_buttons(app, true);
    } else if (m.editing && p.active && !m.selected.empty()) {
        sync_names_text();
        ImGui::TextUnformatted(fmt(lang, S::map_edit_selected, {std::to_string(m.selected.size())}).c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(tr(lang, S::map_edit_names));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(14 * ImGui::GetFontSize());
        ImGui::InputText("##names", &m.names_text);
        ImGui::SameLine();
        if (ImGui::Button(tr(lang, S::map_edit_apply_names)))
            apply_names_to_selected();
    } else if (m.editing && p.active) {
        ImGui::TextDisabled("%s", tr(lang, S::map_tb_nothing));
    }
    if (!p.edit_error.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1), "%s", fmt(lang, S::map_edit_error, {p.edit_error}).c_str());
    ImGui::TextDisabled("%s", tr(lang, S::map_controls));
    ImGui::PopID();
}

void draw_replace(App& app, const Item& item) {
    const auto lang = app.language;
    Replacing& p = m.rep;
    const ImVec4 warn(0.95f, 0.8f, 0.35f, 1), good(0.55f, 0.9f, 0.55f, 1);
    if (m.open_replace) {
        ImGui::SetNextItemOpen(true);
        m.open_replace = false;
    }
    if (!ImGui::CollapsingHeader(tr(lang, S::map_replace), p.active ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        return;
    const Mod* mod = app.current_mod();

    // 1. the model: the person's, or the game's room exported to start from
    ImGui::SeparatorText(tr(lang, S::map_step_model));
    ImGui::TextWrapped("%s", tr(lang, S::map_replace_hint));
    kh2_take(app, item);
    if (ImGui::Button(tr(lang, S::choose_file))) {
        p.next_is_collision = false;
        p.r.kh2.clear();  // the modder's own model, not a KH2 room
        choose_file(app, item);
    }
    same_line_if_room(button_width(tr(lang, S::map_export)));
    if (ImGui::Button(tr(lang, S::map_export)))
        choose_room_export(app, item);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(lang, S::map_export_hint));
    same_line_if_room(button_width(tr(lang, S::map_kh2_source)));
    if (ImGui::Button(tr(lang, S::map_kh2_source)))
        kh2.shown = !kh2.shown;
    if (kh2.shown)
        draw_kh2_picker(app);
    if (mod == nullptr) {
        ImGui::TextColored(warn, "%s", tr(lang, S::choose_mod_first));
        return;
    }
    if (!p.active) {
        // or no model at all: the game's room kept, only its markers and exits' models moved
        if (ImGui::Button(tr(lang, S::map_markers_only)) && m.room != nullptr && m.has_collision) {
            start_kept_change();
            m.show_markers = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr(lang, S::map_markers_only_hint));
        return;
    }
    if (!p.r.kh2.empty())
        ImGui::TextColored(warn, "%s %s", p.r.kh2.c_str(), tr(lang, S::map_kh2_note));
    ImGui::TextUnformatted(
        fmt(lang, S::map_model_file, {p.model_source.empty() ? std::string("-") : u8(p.model_source.filename())})
            .c_str());

    if (markers_only(p.r)) {
        // the game's room as it looks and plays: its markers moved by picking one, its exits' models below
        ImGui::PushStyleColor(ImGuiCol_Text, good);
        ImGui::TextWrapped("%s", tr(lang, S::map_markers_only_note));
        ImGui::PopStyleColor();
        draw_gate_models(app);
    } else if (draw_model_steps(app, item)) {
        p.dirty = true;
    }
    if (p.dirty)
        rebuild();

    if (p.built_ok && !p.r.keep_collision) {
        ImGui::TextDisabled("%s", fmt(lang, S::map_built,
                                      {std::to_string(p.report.faces[kGround]), std::to_string(p.report.faces[kWall]),
                                       std::to_string(p.report.faces[kCeiling]), std::to_string(p.report.nodes),
                                       std::to_string(p.report.bytes)})
                                      .c_str());
    } else if (p.built_ok && !markers_only(p.r)) {
        ImGui::TextDisabled("%s", tr(lang, S::map_collision_kept_note));
    }
    for (const auto& w : p.warnings) {
        // in RoomWarning::Kind's order
        static const S words[] = {S::map_warn_dropped,         S::map_warn_too_big,          S::map_warn_no_ground,
                                  S::map_warn_gate_unplaced,   S::map_warn_wall_unplaced,    S::map_warn_name_kept,
                                  S::map_warn_marker_floating, S::map_warn_collision_shared, S::map_warn_model_shared,
                                  S::map_warn_passable,        S::map_warn_marker_in_wall};
        static_assert(sizeof(words) / sizeof(words[0]) == RoomWarning::marker_in_wall + 1, "a word per warning");
        std::string text = tr(lang, words[w.kind]);
        for (std::size_t k = 0; k < w.args.size(); ++k) {
            const std::string key = "{" + std::to_string(k) + "}";
            for (auto at = text.find(key); at != std::string::npos; at = text.find(key))
                text.replace(at, key.size(), w.args[k]);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, warn);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }
    if (!p.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1));
        ImGui::TextWrapped("%s", p.error.c_str());
        ImGui::PopStyleColor();
    }

    // 4. saved into the mod (or the room given back to the game)
    ImGui::SeparatorText(tr(lang, S::map_step_save));
    const fs::path dump = resolve(app.settings, app.settings.graphics_dump);
    ImGui::BeginDisabled(!p.built_ok || !p.edit_error.empty());
    if (ImGui::Button(tr(lang, S::map_save)))
        save_room(app);
    ImGui::EndDisabled();
    if (p.from_mod) {
        same_line_if_room(button_width(tr(lang, S::map_revert)));
        if (ImGui::Button(tr(lang, S::map_revert))) {
            WorldOutcome o;
            std::string why;
            if (revert_change(nitrofs(), dump, mod->dir, m.world.code, m.room->index, &o, &why)) {
                m.rep = Replacing{};
                history_clear(EditOwner::room);
                m.rep.message = fmt(lang, S::map_reverted, {mod->name});
                app.tell(m.rep.message, true);
            } else {
                p.error = why;
                app.tell(fmt(lang, S::save_failed, {why}), false);
            }
        }
    }
    if (!m.rep.message.empty())
        ImGui::TextDisabled("%s", m.rep.message.c_str());
}

}  // namespace

void room_offer(App& app, const Item& item, const fs::path& file) {
    (void)item;
    if (m.item != app.selected || m.mod != app.mod)
        open(app);
    if (m.room == nullptr)
        return;
    Replacing& p = m.rep;
    const bool collision = p.next_is_collision;
    p.next_is_collision = false;
    assets::ImportedModel model;
    std::string why;
    if (!import_model(file, &model, &why)) {
        p.error = u8(file.filename()) + ": " + why;
        return;
    }
    if (collision && p.active) {
        p.collision_source = file;
        p.raw_collision = std::move(model);
        p.other_collision = true;
    } else {
        p.model_source = file;
        p.raw = std::move(model);
        p.r.model = u8(file.filename());  // no longer only markers: the room takes this model
    }
    if (!p.active) {
        p.active = true;
        p.r.world = m.world.code;
        p.r.room = m.room->index;
    }
    p.message.clear();
    p.dirty = true;
    if (!collision)
        line_up(app);
}

void room_export_to(App& app, const fs::path& file) {
    if (m.room == nullptr)
        return;
    std::vector<std::string> notes;
    std::string why;
    fs::path out = file;
    if (lower(u8(out.extension())) != ".gltf")
        out += ".gltf";
    if (!export_room(nitrofs(), resolve(app.settings, app.settings.graphics_dump), m.world, m.room->index, out, &notes,
                     &why)) {
        app.tell(fmt(app.language, S::save_failed, {why}), false);
        return;
    }
    app.tell(fmt(app.language, S::map_exported, {u8(out)}), true);
    for (const auto& n : notes)
        app.tell(n, false);
}

void draw_map_view(App& app, const Item& item) {
    (void)item;
    if (m.item != app.selected || m.mod != app.mod)
        open(app);
    const auto lang = app.language;
    if (!m.error.empty() && m.room == nullptr) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1), "%s", m.error.c_str());
        return;
    }
    if (m.rep.dirty)
        rebuild();  // the modder's room made before it is counted and drawn
    else if (m.rep.edits_dirty)
        reapply_edits();
    ImGui::TextUnformatted(
        fmt(lang, S::map_room, {std::to_string(m.room->index), std::to_string(m.room->id), world_path(m.world.code)})
            .c_str());
    draw_shown_room(app);
    if (m.has_collision) {
        ImGui::TextDisabled(
            "%s", fmt(lang, S::map_counts,
                      {std::to_string(shown().faces[kGround].size()), std::to_string(shown().faces[kWall].size()),
                       std::to_string(shown().faces[kCeiling].size()), std::to_string(m.markers.size()),
                       std::to_string(shown().named.size())})
                      .c_str());
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "%s",
                           m.error.empty() ? tr(lang, S::map_no_collision) : m.error.c_str());
    }
    for (const auto& rm : m.models) {
        if (!rm.problem.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "%s", rm.problem.c_str());
    }
    ImGui::Checkbox(tr(lang, S::map_models), &m.show_models);
    same_line_if_room(check_width(tr(lang, S::map_grounds)));
    ImGui::Checkbox(tr(lang, S::map_grounds), &m.show_ground);
    same_line_if_room(check_width(tr(lang, S::map_walls)));
    ImGui::Checkbox(tr(lang, S::map_walls), &m.show_walls);
    same_line_if_room(check_width(tr(lang, S::map_ceilings)));
    ImGui::Checkbox(tr(lang, S::map_ceilings), &m.show_ceilings);
    same_line_if_room(check_width(tr(lang, S::map_markers)));
    ImGui::Checkbox(tr(lang, S::map_markers), &m.show_markers);
    // the bar over the picture: the collision editor's tools, then what is picked
    draw_map_toolbar(app);
    draw_selection_bar(app);
    // the picture takes what the bar under it leaves (two rows, three when its fields wrap); the room's
    // other controls are in the left panel, under the catalog (draw_map_controls)
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float row = ImGui::GetFrameHeightWithSpacing();
    const float bottom = (m.editing ? 3.0f : 1.0f) * row + ImGui::GetStyle().WindowPadding.y;
    draw_room(app, ImVec2(avail.x, std::max(160.0f, avail.y - bottom)));
    ImGui::BeginChild("##map_bottom_bar", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    draw_map_bottom_bar(app);
    ImGui::EndChild();
}

void draw_map_controls(App& app, const Item& item, bool panels) {
    if (m.item != app.selected || m.mod != app.mod)
        open(app);
    if (m.room == nullptr)
        return;
    // Undo / Redo of everything the room's change holds (hand edits, markers, exits' models), whatever is
    // open, while no field is typed in and nothing is being dragged
    const ImGuiIO& io = ImGui::GetIO();
    if ((m.rep.active || history_can(false) || history_can(true)) && !io.WantTextInput && m.gizmo_drag < 0 &&
        m.point_drag < 0 &&
        !ImGui::IsAnyItemActive()) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            history_step(app, io.KeyShift, true);
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            history_step(app, true, true);
    }
    if (m.rep.dirty)
        rebuild();
    else if (m.rep.edits_dirty)
        reapply_edits();
    if (!panels)
        return;
    draw_replace(app, item);
    draw_collision_editor(app);
    draw_details(app);
    draw_lists(app);
}

// --- One history for Undo and Redo: the room's change and the mission's points, in the order they were made

namespace {
std::vector<EditOwner> history_undo, history_redo, history_parked;
}  // namespace

void history_push(EditOwner owner) {
    history_undo.push_back(owner);
    history_parked = std::move(history_redo);
    history_redo.clear();
}

void history_unpush(EditOwner owner) {
    for (auto it = history_undo.rbegin(); it != history_undo.rend(); ++it) {
        if (*it == owner) {
            history_undo.erase(std::next(it).base());
            break;
        }
    }
    history_redo = std::move(history_parked);
    history_parked.clear();
}

void history_clear(EditOwner owner) {
    for (auto* list : {&history_undo, &history_redo, &history_parked})
        list->erase(std::remove(list->begin(), list->end(), owner), list->end());
}

bool history_can(bool redo) {
    return !(redo ? history_redo : history_undo).empty();
}

void history_step(App& app, bool redo, bool room_shown) {
    auto& from = redo ? history_redo : history_undo;
    auto& to = redo ? history_undo : history_redo;
    // the last step first; one the owner no longer holds (its oldest dropped, or read again) is passed over
    for (auto it = from.rbegin(); it != from.rend();) {
        const EditOwner owner = *it;
        if (owner == EditOwner::room && !room_shown) {
            ++it;
            continue;
        }
        const bool done = owner == EditOwner::room ? edit_undo(redo) : mission_point_undo(app, redo);
        it = std::reverse_iterator(from.erase(std::next(it).base()));
        if (done) {
            to.push_back(owner);
            return;
        }
    }
}

}  // namespace studio
