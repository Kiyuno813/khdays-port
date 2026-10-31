#include "core/rooms/room_change.h"

#include "core/files/file_write.h"
#include "core/models/model_files.h"
#include "core/files/paths.h"

#include "khdays/assets/gltf.h"
#include "khdays/assets/gltf_export.h"

extern "C" {
#include "render/png_io.h"
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace studio {

namespace {

namespace assets = khdays::assets;

constexpr const char* kHeading = "khdays-studio room 1";

bool fail_with(std::string* why, const std::string& reason) {
    if (why != nullptr)
        *why = reason;
    return false;
}

std::string use_word(ObjectUse u) {
    return u == ObjectUse::model ? "model" : u == ObjectUse::hide ? "hide" : "keep";
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names)
        out += (out.empty() ? "" : ",") + n;
    return out;
}

std::vector<std::string> split_names(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char ch : text) {
        if (ch == ',') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

using M4 = std::array<double, 16>;  // column-major

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[static_cast<std::size_t>(k * 4 + row)] * b[static_cast<std::size_t>(c * 4 + k)];
            r[static_cast<std::size_t>(c * 4 + row)] = s;
        }
    }
    return r;
}

M4 identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// scale, then turned about x, y, z (degrees), then moved
M4 placement_matrix(const RoomPlacement& p) {
    const double k = 3.14159265358979323846 / 180.0;
    const double ax = p.turn[0] * k, ay = p.turn[1] * k, az = p.turn[2] * k;
    const M4 rx{1, 0, 0, 0, 0, std::cos(ax), std::sin(ax), 0, 0, -std::sin(ax), std::cos(ax), 0, 0, 0, 0, 1};
    const M4 ry{std::cos(ay), 0, -std::sin(ay), 0, 0, 1, 0, 0, std::sin(ay), 0, std::cos(ay), 0, 0, 0, 0, 1};
    const M4 rz{std::cos(az), std::sin(az), 0, 0, -std::sin(az), std::cos(az), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    M4 s = identity();
    s[0] = s[5] = s[10] = p.scale;
    M4 t = identity();
    t[12] = p.move[0];
    t[13] = p.move[1];
    t[14] = p.move[2];
    return mul(t, mul(rz, mul(ry, mul(rx, s))));
}

M4 to_m4(const std::array<float, 16>& f) {
    M4 m{};
    for (std::size_t k = 0; k < 16; ++k)
        m[k] = f[k];
    return m;
}

std::array<float, 16> to_floats(const M4& m) {
    std::array<float, 16> f{};
    for (std::size_t k = 0; k < 16; ++k)
        f[k] = static_cast<float>(m[k]);
    return f;
}

std::array<float, 3> apply(const M4& m, const std::array<float, 3>& v, double w) {
    std::array<float, 3> r{};
    for (int row = 0; row < 3; ++row) {
        r[static_cast<std::size_t>(row)] =
            static_cast<float>(m[static_cast<std::size_t>(row)] * v[0] + m[static_cast<std::size_t>(4 + row)] * v[1] +
                               m[static_cast<std::size_t>(8 + row)] * v[2] + m[static_cast<std::size_t>(12 + row)] * w);
    }
    return r;
}

std::string member_label(const std::string& code, int member) {
    return world_path(code) + "#" + std::to_string(member);
}

// The texture files a written room model uses: `<model>-room-<n>.png`.
std::string texture_file(const std::string& model, std::size_t n) {
    return model + "-room-" + std::to_string(n) + ".png";
}

// What the studio wrote for a world's rooms (paths under the mod, '/'
// separators), so that only those are ever removed -- never a model the
// modder replaced by hand.
std::filesystem::path written_list(const std::filesystem::path& mod_dir, const std::string& code) {
    return rooms_folder(mod_dir, code) / "written.list";
}

std::set<std::string> read_written(const std::filesystem::path& mod_dir, const std::string& code) {
    std::set<std::string> out;
    std::ifstream in(written_list(mod_dir, code), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            out.insert(line);
    }
    return out;
}

// Removes a written file and the folders it leaves empty under the mod.
void remove_written(const std::filesystem::path& mod_dir, const std::string& relative) {
    std::error_code ec;
    const auto file = mod_dir / path_u8(relative);
    if (!inside(mod_dir, file))
        return;
    std::filesystem::remove(file, ec);
    for (auto f = file.parent_path(); f != mod_dir && inside(mod_dir, f); f = f.parent_path()) {
        if (!std::filesystem::is_directory(f, ec) || !std::filesystem::is_empty(f, ec))
            break;
        std::filesystem::remove(f, ec);
    }
}

// Writes `model` (placed) as the glTF replacing `key`'s model, its textures beside it.
bool write_room_model(const std::filesystem::path& mod_dir, const std::string& key, const assets::ImportedModel& model,
                      std::set<std::string>* written, std::string* why) {
    const std::filesystem::path gltf = mod_dir / "graphics" / path_u8(key + ".gltf");
    const std::string name = key.substr(key.rfind('/') + 1);
    std::map<std::string, assets::GltfTextureRef> refs;
    std::size_t n = 0;
    for (const auto& [texture, decoded] : model.textures) {
        if (decoded.rgba.empty() || decoded.width <= 0 || decoded.height <= 0)
            continue;
        const std::string file = texture_file(name, n++);
        const auto result = write_into_mod(
            mod_dir, gltf.parent_path() / path_u8(file),
            [&](const std::filesystem::path& temporary) -> std::string {
                const char* e =
                    khdays_png_write(u8(temporary).c_str(), decoded.rgba.data(), decoded.width, decoded.height);
                return e != nullptr ? std::string(e) : std::string();
            },
            [](const std::filesystem::path&) { return std::string(); });
        if (!result.ok)
            return fail_with(why, file + ": " + result.why);
        written->insert("graphics/" + key.substr(0, key.rfind('/') + 1) + file);
        refs[texture] = {file, decoded.width, decoded.height,
                         assets::texels_translucent(decoded.rgba.data(), decoded.rgba.size() / 4)};
    }
    // a model that moves by itself written as its scene, its skeleton and animation kept
    const std::string text = assets::moves_by_itself(model) ? assets::scene_to_gltf(assets::scene_of(model, refs))
                                                            : assets::to_gltf(model.model, refs);
    const auto result = write_into_mod(
        mod_dir, gltf,
        [&](const std::filesystem::path& temporary) -> std::string {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << text;
            out.close();  // what the buffer still holds written now, its failure seen
            return out ? std::string() : std::string("could not be written");
        },
        [&](const std::filesystem::path& temporary) -> std::string {
            // read as the port reads it; the textures are beside the destination, not the temporary
            try {
                (void)assets::import_gltf(temporary);
            } catch (const std::exception& e) {
                return std::string("the port cannot read it: ") + e.what();
            }
            return {};
        });
    if (!result.ok)
        return fail_with(why, key + ".gltf: " + result.why);
    written->insert("graphics/" + key + ".gltf");
    return true;
}

// A model's joints at rest: each palette entry its joint's global times its inverse bind.
std::map<std::uint32_t, M4> rest_palette(const assets::ImportedModel& m) {
    std::map<std::uint32_t, M4> palette;
    for (const auto& j : m.joints)
        palette[j.palette_index] = mul(to_m4(j.world), to_m4(j.inverse_bind));
    return palette;
}

// Where a vertex is drawn: through its joints when the model keeps its skeleton, else as it is.
std::array<float, 3> posed(const std::map<std::uint32_t, M4>& palette, const assets::NeutralVertex& v) {
    if (palette.empty())
        return v.position;
    std::array<float, 3> pos{0, 0, 0};
    float total = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        const auto found = palette.find(v.joints[k]);
        if (v.weights[k] <= 0 || found == palette.end())
            continue;
        const auto q = apply(found->second, v.position, 1);
        for (std::size_t c = 0; c < 3; ++c)
            pos[c] += v.weights[k] * q[c];
        total += v.weights[k];
    }
    if (total <= 0)
        return v.position;
    for (auto& c : pos)
        c /= total;
    return pos;
}

}  // namespace

std::filesystem::path rooms_folder(const std::filesystem::path& mod_dir, const std::string& code) {
    return mod_dir / "studio" / "rooms" / path_u8(code);
}

std::filesystem::path change_file(const std::filesystem::path& mod_dir, const std::string& code, int room) {
    return rooms_folder(mod_dir, code) / (std::to_string(room) + ".txt");
}

std::filesystem::path change_models(const std::filesystem::path& mod_dir, const std::string& code, int room) {
    return rooms_folder(mod_dir, code) / std::to_string(room);
}

std::string change_text(const RoomReplacement& r) {
    std::ostringstream o;
    o << kHeading << "\n";
    o << "world " << r.world << "\n";
    o << "room " << r.room << "\n";
    if (!r.model.empty())
        o << "model " << r.model << "\n";
    if (!r.kh2.empty())
        o << "kh2 " << r.kh2 << "\n";
    if (!r.collision.empty())
        o << "collision " << r.collision << "\n";
    if (r.keep_collision)
        o << "collision-kept\n";
    o << "scale " << r.place.scale << "\n";
    o << "turn " << r.place.turn[0] << " " << r.place.turn[1] << " " << r.place.turn[2] << "\n";
    o << "move " << r.place.move[0] << " " << r.place.move[1] << " " << r.place.move[2] << "\n";
    if (r.zone >= 0)
        o << "zone " << r.zone << "\n";
    o << "ground-limit " << r.ground_limit << "\n";
    for (const auto& [member, use] : r.objects)
        o << "object " << member << " " << use_word(use) << "\n";
    for (const auto& [material, names] : r.names_by_material)
        o << "material-names " << joined(names) << " " << material << "\n";
    for (const auto& [triangle, names] : r.names_by_triangle)
        o << "triangle-names " << triangle << " " << joined(names) << "\n";
    for (const auto& name : r.drop_original)
        o << "drop-original " << name << "\n";
    for (const auto& [name, at] : r.markers)
        o << "marker " << name << " " << at[0] << " " << at[1] << " " << at[2] << " " << at[3] << "\n";
    for (const auto& [key, at] : r.gate_points) {
        o << "gate-point " << key.first << " " << key.second << " " << at[0] << " " << at[1] << " " << at[2] << "\n";
    }
    o << edits_text(r.edits);
    return o.str();
}

bool read_change(const std::filesystem::path& file, RoomReplacement* out, std::string* why) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return fail_with(why, "cannot open " + u8(file));
    RoomReplacement r;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (number == 1) {
            if (line != kHeading)
                return fail_with(why, u8(file) + ": not a room change of this studio");
            continue;
        }
        if (line.empty() || line[0] == '#')
            continue;
        const auto space = line.find(' ');
        const std::string word = line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
        std::istringstream v(rest);
        const std::string at = u8(file) + " line " + std::to_string(number);
        if (word == "world")
            r.world = rest;
        else if (word == "room")
            v >> r.room;
        else if (word == "model")
            r.model = rest;
        else if (word == "kh2")
            r.kh2 = rest;
        else if (word == "collision")
            r.collision = rest;
        else if (word == "collision-kept")
            r.keep_collision = true;
        else if (word == "scale")
            v >> r.place.scale;
        else if (word == "turn")
            v >> r.place.turn[0] >> r.place.turn[1] >> r.place.turn[2];
        else if (word == "move")
            v >> r.place.move[0] >> r.place.move[1] >> r.place.move[2];
        else if (word == "zone")
            v >> r.zone;
        else if (word == "ground-limit")
            v >> r.ground_limit;
        else if (word == "object") {
            int member = -1;
            std::string use;
            v >> member >> use;
            if (use != "model" && use != "hide" && use != "keep")
                return fail_with(why, at + ": an object is model, hide or keep");
            r.objects[member] = use == "model" ? ObjectUse::model : use == "hide" ? ObjectUse::hide : ObjectUse::keep;
        } else if (word == "material-names") {
            const auto gap = rest.find(' ');
            if (gap == std::string::npos)
                return fail_with(why, at + ": names, then the material");
            r.names_by_material[rest.substr(gap + 1)] = split_names(rest.substr(0, gap));
        } else if (word == "triangle-names") {
            std::size_t t = 0;
            std::string names;
            v >> t >> names;
            r.names_by_triangle[t] = split_names(names);
        } else if (word == "drop-original") {
            r.drop_original.insert(rest);
        } else if (word == "marker") {
            std::string name;
            std::array<std::int32_t, 4> place{};
            v >> name >> place[0] >> place[1] >> place[2] >> place[3];
            r.markers[name] = place;
        } else if (word == "gate-point") {
            int key = 0, part = 0;
            std::array<std::int32_t, 3> place{};
            v >> key >> part >> place[0] >> place[1] >> place[2];
            if (part < 0 || part > 4)
                return fail_with(why, at + ": a gate model's point is 0 to 4");
            r.gate_points[{key, part}] = place;
        } else {
            std::string problem;
            const int edit = read_edit_line(word, rest, &r.edits, &problem);
            if (edit < 0)
                return fail_with(why, at + ": " + problem);
            if (edit == 0)
                return fail_with(why, at + ": \"" + word + "\" is not known");
            continue;  // read whole, numbers and words
        }
        if (v.fail() && word != "world" && word != "model" && word != "collision" && word != "collision-kept" &&
            word != "material-names" && word != "drop-original") {
            return fail_with(why, at + ": not a number where one goes");
        }
    }
    if (r.world.empty() || r.room < 0)
        return fail_with(why, u8(file) + ": no world or room");
    // no model: the game's room kept, only its markers and exits' models moved -- its floor and walls too
    if (r.model.empty() && (!r.keep_collision || !r.collision.empty() || !r.kh2.empty()))
        return fail_with(why, u8(file) + ": no model, yet the room's floor and walls are not the game's");
    if (!(r.place.scale > 0))
        return fail_with(why, u8(file) + ": the scale must be above 0");
    *out = std::move(r);
    return true;
}

std::vector<RoomReplacement> changes_of(const std::filesystem::path& mod_dir, const std::string& code,
                                        std::vector<std::string>* problems) {
    std::vector<RoomReplacement> out;
    std::error_code ec;
    const auto folder = rooms_folder(mod_dir, code);
    if (!std::filesystem::is_directory(folder, ec))
        return out;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
        if (!e.is_regular_file() || lower(u8(e.path().extension())) != ".txt")
            continue;
        RoomReplacement r;
        std::string why;
        if (read_change(e.path(), &r, &why)) {
            out.push_back(std::move(r));
        } else if (problems != nullptr) {
            problems->push_back(why);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.room < b.room; });
    return out;
}

std::vector<std::string> changed_worlds(const std::filesystem::path& mod_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    const auto folder = mod_dir / "studio" / "rooms";
    if (!std::filesystem::is_directory(folder, ec))
        return out;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
        if (e.is_directory())
            out.push_back(u8(e.path().filename()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

RoomAlignment align_with_room(const std::vector<std::array<float, 3>>& room_points,
                              const assets::ImportedModel& model) {
    RoomAlignment best;
    if (room_points.empty())
        return best;
    // the model's points as the file places them, a sample of them
    assets::ImportedModel base = model;
    place_model(&base, RoomPlacement{});
    std::vector<std::array<float, 3>> points;
    std::size_t total = 0;
    for (const auto& mesh : base.model.meshes)
        total += mesh.vertices.size();
    const std::size_t step = std::max<std::size_t>(1, total / 4000);
    std::size_t at = 0;
    for (const auto& mesh : base.model.meshes) {
        for (const auto& v : mesh.vertices) {
            if (at++ % step == 0)
                points.push_back(v.position);
        }
    }
    if (points.empty())
        return best;
    // the room's points in cells of a hundredth of a unit (the game's own places: within 1/4096)
    const double cell = 0.01;
    auto key = [&](double x, double y, double z) {
        const auto ix = static_cast<std::int64_t>(std::floor(x / cell)),
                   iy = static_cast<std::int64_t>(std::floor(y / cell)),
                   iz = static_cast<std::int64_t>(std::floor(z / cell));
        return (ix * 73856093) ^ (iy * 19349663) ^ (iz * 83492791);
    };
    std::set<std::int64_t> cells;
    for (const auto& q : room_points)
        cells.insert(key(q[0], q[1], q[2]));
    auto on_room = [&](const std::array<float, 3>& q) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (cells.count(key(q[0] + dx * cell, q[1] + dy * cell, q[2] + dz * cell)) != 0)
                        return true;
                }
            }
        }
        return false;
    };
    // every quarter-turn orientation once (24), as the turns that make it
    std::vector<std::array<float, 3>> turns;
    std::set<std::array<int, 9>> seen;
    for (int x = 0; x < 360; x += 90) {
        for (int y = 0; y < 360; y += 90) {
            for (int z = 0; z < 360; z += 90) {
                RoomPlacement p;
                p.turn = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
                const M4 m = placement_matrix(p);
                std::array<int, 9> r{};
                for (int c = 0; c < 3; ++c) {
                    for (int row = 0; row < 3; ++row)
                        r[static_cast<std::size_t>(c * 3 + row)] =
                            static_cast<int>(std::lround(m[static_cast<std::size_t>(c * 4 + row)]));
                }
                if (seen.insert(r).second)
                    turns.push_back(p.turn);
            }
        }
    }
    for (const double scale : {1.0, 0.01, 100.0, 0.1, 10.0}) {
        for (const auto& turn : turns) {
            RoomPlacement p;
            p.scale = static_cast<float>(scale);
            p.turn = turn;
            const M4 m = placement_matrix(p);
            std::size_t hits = 0;
            for (const auto& q : points)
                hits += on_room(apply(m, q, 1)) ? 1 : 0;
            const double share = static_cast<double>(hits) / static_cast<double>(points.size());
            if (scale == 1.0 && turn[0] == 0 && turn[1] == 0 && turn[2] == 0)
                best.as_is = share;
            if (share > best.share + 1e-9) {
                best.share = share;
                best.place = p;
            }
        }
    }
    const bool moved =
        best.place.scale != 1.0f || best.place.turn[0] != 0 || best.place.turn[1] != 0 || best.place.turn[2] != 0;
    if (moved && best.share >= 0.05 && best.share >= 2 * best.as_is) {
        best.verdict = RoomAlignment::turned;
    } else if (best.as_is >= 0.03) {
        best.verdict = RoomAlignment::in_place;
        best.place = RoomPlacement{};
        best.share = best.as_is;
    } else {
        best.verdict = RoomAlignment::unrelated;
    }
    return best;
}

RoomAlignment kh2_alignment(const std::vector<std::array<float, 3>>& room_points, const RoomCollision& room,
                            const assets::ImportedModel& kh2) {
    RoomAlignment a = align_with_room(room_points, kh2);
    if (a.verdict != RoomAlignment::unrelated)
        return a;
    a = RoomAlignment{};
    a.verdict = RoomAlignment::kh2_units;
    a.place.scale = 0.01f;  // KH2's units are a hundredth of the game's (the Usual Spot, room 0 of Twilight Town)
    // left where KH2 has it, not moved: Days kept KH2's units and axes, so a room made from a map is that map
    // at 0 0 0 (Station Plaza's rooms on their KH2 map exactly). Matching the two middles and floors moved it by
    // what the map holds past the room (its far streets, the hills), numbers that meant nothing.
    (void)room;
    return a;
}

std::array<float, 3> placed_point(const RoomPlacement& place, const std::array<float, 3>& v) {
    return apply(placement_matrix(place), v, 1);
}

std::array<float, 3> unplaced_point(const RoomPlacement& place, const std::array<float, 3>& v) {
    // the matrix's columns are the turned axes times the scale: back along each, over the scale squared
    const M4 m = placement_matrix(place);
    const double d[3] = {v[0] - m[12], v[1] - m[13], v[2] - m[14]};
    const double s2 = static_cast<double>(place.scale) * place.scale;
    std::array<float, 3> out{};
    for (std::size_t c = 0; c < 3; ++c)
        out[c] = static_cast<float>((m[c * 4] * d[0] + m[c * 4 + 1] * d[1] + m[c * 4 + 2] * d[2]) / s2);
    return out;
}

bool same_place(const RoomPlacement& a, const RoomPlacement& b) {
    return a.scale == b.scale && a.turn == b.turn && a.move == b.move;
}

bool placed_model(const std::filesystem::path& model, const RoomPlacement& place, assets::ImportedModel* out,
                  std::string* why) {
    assets::ImportedModel m;
    if (!import_model(model, &m, why))
        return false;
    place_model(&m, place);
    *out = std::move(m);
    return true;
}

void place_model(assets::ImportedModel* model, const RoomPlacement& place) {
    assets::ImportedModel& m = *model;
    if (!m.animations.empty()) {
        // a model with its own animation keeps its skeleton: the place goes on its roots and joints,
        // the meshes loading made still are placed
        const M4 p = placement_matrix(place);
        for (auto& node : m.nodes) {
            if (node.parent < 0)
                node.local = to_floats(mul(p, to_m4(node.local)));
        }
        std::set<std::uint32_t> skinned;
        for (auto& j : m.joints) {
            j.world = to_floats(mul(p, to_m4(j.world)));
            skinned.insert(j.palette_index);
        }
        for (auto& mesh : m.model.meshes) {
            for (auto& v : mesh.vertices) {
                bool on_joint = false;
                for (std::size_t k = 0; k < 4; ++k)
                    on_joint = on_joint || (v.weights[k] > 0 && skinned.count(v.joints[k]) != 0);
                if (!on_joint) {
                    v.position = apply(p, v.position, 1);
                    v.normal = apply(p, v.normal, 0);
                }
            }
        }
        return;
    }
    // the palette as the file poses it: each joint's global times its inverse bind
    std::map<std::uint32_t, M4> palette;
    for (const auto& j : m.joints) {
        M4 world{}, ibm{};
        for (std::size_t k = 0; k < 16; ++k)
            world[k] = j.world[k], ibm[k] = j.inverse_bind[k];
        palette[j.palette_index] = mul(world, ibm);
    }
    const M4 p = placement_matrix(place);
    for (auto& mesh : m.model.meshes) {
        for (auto& v : mesh.vertices) {
            std::array<float, 3> pos{0, 0, 0}, nrm{0, 0, 0};
            float total = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                if (v.weights[k] <= 0)
                    continue;
                const auto found = palette.find(v.joints[k]);
                const M4& s = found != palette.end() ? found->second : identity();
                const auto q = apply(s, v.position, 1), n = apply(s, v.normal, 0);
                for (std::size_t c = 0; c < 3; ++c)
                    pos[c] += v.weights[k] * q[c], nrm[c] += v.weights[k] * n[c];
                total += v.weights[k];
            }
            if (total <= 0) {
                pos = v.position;
                nrm = v.normal;
            } else if (std::fabs(total - 1.0f) > 1e-4f) {
                for (auto& c : pos)
                    c /= total;
            }
            v.position = apply(p, pos, 1);
            v.normal = apply(p, nrm, 0);
            v.joints = {0, 0, 0, 0};
            v.weights = {1, 0, 0, 0};
        }
    }
    m.joints.clear();
    m.model.palette.clear();
    m.model.palette_bones.clear();
    m.model.object_matrices.clear();
    m.model.object_names.clear();
    m.model.object_parents.clear();
    m.model.skinning = nullptr;
}

CollisionSource collision_source(const assets::ImportedModel& placed, bool visible) {
    CollisionSource s;
    // a model that keeps its skeleton (its own animation) at rest: each vertex through its joints
    const auto palette = rest_palette(placed);
    std::vector<assets::NeutralMesh> at_rest;
    if (!palette.empty()) {
        at_rest = placed.model.meshes;
        for (auto& mesh : at_rest) {
            for (auto& v : mesh.vertices)
                v.position = posed(palette, v);
        }
    }
    for (const auto& mesh : palette.empty() ? placed.model.meshes : at_rest) {
        const std::string material = !mesh.material_name.empty() ? mesh.material_name : mesh.texture_name;
        const char passable = visible && mesh.alpha_mode == assets::NeutralMesh::AlphaMode::blend ? 1 : 0;
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const auto& a = mesh.vertices[mesh.indices[i]];
            const auto& b = mesh.vertices[mesh.indices[i + 1]];
            const auto& c = mesh.vertices[mesh.indices[i + 2]];
            s.triangles.push_back({a.position, b.position, c.position});
            s.materials.push_back(material);
            s.passable.push_back(passable);
            // drawn both sides (every FBX mesh is): a steep face stops from both
            // sides too; a floor's other side would be a ceiling under it, so not
            const float e0[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                                 b.position[2] - a.position[2]};
            const float e1[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                                 c.position[2] - a.position[2]};
            const float n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                                e0[0] * e1[1] - e0[1] * e1[0]};
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (mesh.double_sided && length > 0 && std::fabs(n[1] / length) < 0.5f) {
                s.triangles.push_back({a.position, c.position, b.position});
                s.materials.push_back(material);
                s.passable.push_back(passable);
            }
        }
    }
    return s;
}

bool RoomZone::holds(float x, float z) const {
    const auto i = static_cast<std::int64_t>(std::floor(x / cell)) - x0;
    const auto k = static_cast<std::int64_t>(std::floor(z / cell)) - z0;
    if (i < 0 || k < 0 || i >= width || k >= depth)
        return false;
    return in[static_cast<std::size_t>(k * width + i)] != 0;
}

namespace {

// The cells of `zone` that a room's faces cover from above: their edges (a wall from above is one) and
// the cells whose middles they hold.
std::vector<char> covered_cells(const RoomCollision& room, const RoomZone& zone) {
    const float c = zone.cell;
    const int w = zone.width, d = zone.depth;
    std::vector<char> covered(static_cast<std::size_t>(w) * static_cast<std::size_t>(d), 0);
    auto mark = [&](double x, double z) {
        const auto i = static_cast<std::int64_t>(std::floor(x / c)) - zone.x0;
        const auto k = static_cast<std::int64_t>(std::floor(z / c)) - zone.z0;
        if (i >= 0 && k >= 0 && i < w && k < d)
            covered[static_cast<std::size_t>(k * w + i)] = 1;
    };
    for (const auto& list : room.faces) {
        for (const auto& f : list) {
            const std::size_t n = std::min<std::size_t>(f.corners, 4);
            if (n < 3)
                continue;
            double px[4], pz[4];
            for (std::size_t v = 0; v < n; ++v)
                px[v] = f.v[v][0] / 4096.0, pz[v] = f.v[v][2] / 4096.0;
            for (std::size_t v = 0; v < n; ++v) {
                const std::size_t u = (v + 1) % n;
                const double length = std::hypot(px[u] - px[v], pz[u] - pz[v]);
                const int steps = static_cast<int>(length / (c / 2)) + 1;
                for (int s = 0; s <= steps; ++s) {
                    const double t = static_cast<double>(s) / steps;
                    mark(px[v] + t * (px[u] - px[v]), pz[v] + t * (pz[u] - pz[v]));
                }
            }
            double flo[2] = {px[0], pz[0]}, fhi[2] = {px[0], pz[0]};
            for (std::size_t v = 1; v < n; ++v) {
                flo[0] = std::min(flo[0], px[v]), fhi[0] = std::max(fhi[0], px[v]);
                flo[1] = std::min(flo[1], pz[v]), fhi[1] = std::max(fhi[1], pz[v]);
            }
            for (auto i = static_cast<std::int64_t>(std::floor(flo[0] / c)); i * c <= fhi[0]; ++i) {
                for (auto k = static_cast<std::int64_t>(std::floor(flo[1] / c)); k * c <= fhi[1]; ++k) {
                    const double x = (static_cast<double>(i) + 0.5) * c, z = (static_cast<double>(k) + 0.5) * c;
                    bool above = false, below = false;
                    for (std::size_t v = 0; v < n; ++v) {
                        const std::size_t u = (v + 1) % n;
                        const double side = (px[u] - px[v]) * (z - pz[v]) - (pz[u] - pz[v]) * (x - px[v]);
                        above = above || side > 0;
                        below = below || side < 0;
                    }
                    if (!(above && below))
                        mark(x, z);
                }
            }
        }
    }
    return covered;
}

// The cells outside what the covered ones close in: those reached from the zone's edges.
std::vector<char> outside_cells(const std::vector<char>& covered, int w, int d) {
    std::vector<char> outside(covered.size(), 0);
    std::vector<std::size_t> stack;
    auto reach = [&](int i, int k) {
        const auto at = static_cast<std::size_t>(k) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i);
        if (covered[at] == 0 && outside[at] == 0) {
            outside[at] = 1;
            stack.push_back(at);
        }
    };
    for (int i = 0; i < w; ++i)
        reach(i, 0), reach(i, d - 1);
    for (int k = 0; k < d; ++k)
        reach(0, k), reach(w - 1, k);
    while (!stack.empty()) {
        const std::size_t at = stack.back();
        stack.pop_back();
        const int i = static_cast<int>(at % static_cast<std::size_t>(w)),
                  k = static_cast<int>(at / static_cast<std::size_t>(w));
        if (i > 0)
            reach(i - 1, k);
        if (i + 1 < w)
            reach(i + 1, k);
        if (k > 0)
            reach(i, k - 1);
        if (k + 1 < d)
            reach(i, k + 1);
    }
    return outside;
}

// Each cell's distance (in cells) to the nearest cell not outside, in two sweeps.
std::vector<float> distance_cells(const std::vector<char>& outside, int w, int d) {
    const float far = 1e30f;
    std::vector<float> dist(outside.size(), far);
    for (std::size_t at = 0; at < outside.size(); ++at) {
        if (outside[at] == 0)
            dist[at] = 0;
    }
    const float diagonal = std::sqrt(2.0f);
    auto relax = [&](int i, int k, int di, int dk, float step) {
        const int ni = i + di, nk = k + dk;
        if (ni < 0 || nk < 0 || ni >= w || nk >= d)
            return;
        float& here = dist[static_cast<std::size_t>(k) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i)];
        here = std::min(
            here,
            dist[static_cast<std::size_t>(nk) * static_cast<std::size_t>(w) + static_cast<std::size_t>(ni)] + step);
    };
    for (int k = 0; k < d; ++k) {
        for (int i = 0; i < w; ++i) {
            relax(i, k, -1, 0, 1), relax(i, k, 0, -1, 1), relax(i, k, -1, -1, diagonal), relax(i, k, 1, -1, diagonal);
        }
    }
    for (int k = d - 1; k >= 0; --k) {
        for (int i = w - 1; i >= 0; --i) {
            relax(i, k, 1, 0, 1), relax(i, k, 0, 1, 1), relax(i, k, 1, 1, diagonal), relax(i, k, -1, 1, diagonal);
        }
    }
    return dist;
}

}  // namespace

RoomZone room_zone(const RoomCollision& room, float margin) {
    RoomZone zone;
    const float c = zone.cell;
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    for (const auto& list : room.faces) {
        for (const auto& f : list) {
            for (std::size_t v = 0; v < f.corners && v < 4; ++v) {
                const double x = f.v[v][0] / 4096.0, z = f.v[v][2] / 4096.0;
                lo[0] = std::min(lo[0], x), hi[0] = std::max(hi[0], x);
                lo[1] = std::min(lo[1], z), hi[1] = std::max(hi[1], z);
            }
        }
    }
    if (lo[0] > hi[0])
        return zone;
    const double pad = std::max(0.0f, margin) + 2 * c;
    zone.x0 = static_cast<std::int64_t>(std::floor((lo[0] - pad) / c));
    zone.z0 = static_cast<std::int64_t>(std::floor((lo[1] - pad) / c));
    zone.width = static_cast<int>(std::floor((hi[0] + pad) / c) - zone.x0 + 1);
    zone.depth = static_cast<int>(std::floor((hi[1] + pad) / c) - zone.z0 + 1);
    // the cells the room covers or closes in, and those within the margin of them
    const auto dist =
        distance_cells(outside_cells(covered_cells(room, zone), zone.width, zone.depth), zone.width, zone.depth);
    zone.in.assign(dist.size(), 0);
    const float reach_cells = std::max(0.0f, margin) / c;
    for (std::size_t at = 0; at < dist.size(); ++at)
        zone.in[at] = dist[at] <= reach_cells ? 1 : 0;
    return zone;
}

namespace {

// The model's drawn triangles (its skies left out), each corner where it is drawn at rest.
std::vector<std::array<std::array<float, 3>, 3>> drawn_triangles(const assets::ImportedModel& placed) {
    const auto palette = rest_palette(placed);
    std::vector<std::array<std::array<float, 3>, 3>> out;
    for (const auto& mesh : placed.model.meshes) {
        if (mesh.sky)
            continue;
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            out.push_back({posed(palette, mesh.vertices[mesh.indices[i]]),
                           posed(palette, mesh.vertices[mesh.indices[i + 1]]),
                           posed(palette, mesh.vertices[mesh.indices[i + 2]])});
        }
    }
    return out;
}

}  // namespace

SurfaceMap surface_map(const assets::ImportedModel& placed) {
    SurfaceMap s;
    for (const auto& t : drawn_triangles(placed)) {
        const float e0[3] = {t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]};
        const float e1[3] = {t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]};
        const float n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                            e0[0] * e1[1] - e0[1] * e1[0]};
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(length > 0) || std::fabs(n[1] / length) <= 0.3f)
            continue;
        const std::size_t at = s.triangles.size();
        s.triangles.push_back(t);
        s.up.push_back(n[1] > 0 ? 1 : 0);
        const float lo_x = std::min({t[0][0], t[1][0], t[2][0]}), hi_x = std::max({t[0][0], t[1][0], t[2][0]});
        const float lo_z = std::min({t[0][2], t[1][2], t[2][2]}), hi_z = std::max({t[0][2], t[1][2], t[2][2]});
        if (hi_x - lo_x > 64 || hi_z - lo_z > 64) {
            s.wide.push_back(at);  // a ground far and wide: asked every time, not cell by cell
            continue;
        }
        for (auto i = static_cast<std::int64_t>(std::floor(lo_x)); i <= static_cast<std::int64_t>(std::floor(hi_x));
             ++i) {
            for (auto k = static_cast<std::int64_t>(std::floor(lo_z)); k <= static_cast<std::int64_t>(std::floor(hi_z));
                 ++k)
                s.cells[{i, k}].push_back(at);
        }
    }
    return s;
}

std::vector<SurfaceMap::Height> SurfaceMap::heights_at(float x, float z) const {
    std::vector<Height> out;
    const auto cell = cells.find({static_cast<std::int64_t>(std::floor(x)), static_cast<std::int64_t>(std::floor(z))});
    std::vector<std::size_t> asked = wide;
    if (cell != cells.end())
        asked.insert(asked.end(), cell->second.begin(), cell->second.end());
    for (const std::size_t i : asked) {
        const auto& t = triangles[i];
        // where (x, z) falls on it from above, by its corners' weights
        const double d = (t[1][2] - t[2][2]) * (t[0][0] - t[2][0]) + (t[2][0] - t[1][0]) * (t[0][2] - t[2][2]);
        if (std::fabs(d) < 1e-12)
            continue;
        const double a = ((t[1][2] - t[2][2]) * (x - t[2][0]) + (t[2][0] - t[1][0]) * (z - t[2][2])) / d;
        const double b = ((t[2][2] - t[0][2]) * (x - t[2][0]) + (t[0][0] - t[2][0]) * (z - t[2][2])) / d;
        const double c = 1 - a - b;
        if (a < -1e-6 || b < -1e-6 || c < -1e-6)
            continue;
        out.push_back({static_cast<float>(a * t[0][1] + b * t[1][1] + c * t[2][1]), up[i] != 0});
    }
    return out;
}

float raise_to_markers(const SurfaceMap& surfaces, const std::vector<std::array<float, 3>>& markers, std::size_t* on) {
    // per marker, each raise that would put a floor under it a tenth below its feet
    std::vector<std::vector<float>> raises;
    for (const auto& q : markers) {
        std::vector<float> r;
        for (const auto& h : surfaces.heights_at(q[0], q[2])) {
            const float d = q[1] - 0.1f - h.y;
            if (h.up && std::fabs(d) <= 50.0f)
                r.push_back(d);
        }
        raises.push_back(std::move(r));
    }
    // the raise the most markers agree on (within 0.15), the smallest of equals, then their mean
    const float near = 0.15f;
    float best = 0;
    std::size_t most = 0;
    for (const auto& r : raises) {
        for (const float d : r) {
            std::size_t count = 0;
            for (const auto& other : raises) {
                bool agrees = false;
                for (const float e : other)
                    agrees = agrees || std::fabs(e - d) <= near;
                count += agrees ? 1 : 0;
            }
            if (count > most || (count == most && std::fabs(d) < std::fabs(best))) {
                most = count;
                best = d;
            }
        }
    }
    if (most > 0) {
        float sum = 0;
        for (const auto& r : raises) {
            float closest = 1e30f;
            for (const float e : r) {
                if (std::fabs(e - best) < std::fabs(closest - best))
                    closest = e;
            }
            if (std::fabs(closest - best) <= near)
                sum += closest;
        }
        best = sum / static_cast<float>(most);
    }
    if (on != nullptr)
        *on = most;
    return best;
}

std::size_t cut_to_zone(assets::ImportedModel* placed, const RoomZone& zone) {
    assets::ImportedModel& m = *placed;
    // a model that keeps its skeleton: each vertex where its joints put it
    const auto palette = rest_palette(m);
    auto where = [&](const assets::NeutralVertex& v) { return posed(palette, v); };
    std::size_t dropped = 0;
    std::vector<assets::NeutralMesh> kept;
    for (auto& mesh : m.model.meshes) {
        if (!mesh.sky) {
            std::vector<std::array<float, 3>> at(mesh.vertices.size());
            for (std::size_t v = 0; v < mesh.vertices.size(); ++v)
                at[v] = where(mesh.vertices[v]);
            std::vector<std::uint32_t> indices;
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const auto& a = at[mesh.indices[i]];
                const auto& b = at[mesh.indices[i + 1]];
                const auto& c = at[mesh.indices[i + 2]];
                const bool over = zone.holds(a[0], a[2]) || zone.holds(b[0], b[2]) || zone.holds(c[0], c[2]) ||
                                  zone.holds((a[0] + b[0] + c[0]) / 3, (a[2] + b[2] + c[2]) / 3);
                if (over)
                    indices.insert(indices.end(), {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]});
                else
                    ++dropped;
            }
            // only the vertices still used
            std::map<std::uint32_t, std::uint32_t> renumbered;
            std::vector<assets::NeutralVertex> vertices;
            for (auto& index : indices) {
                const auto found = renumbered.emplace(index, static_cast<std::uint32_t>(vertices.size()));
                if (found.second)
                    vertices.push_back(mesh.vertices[index]);
                index = found.first->second;
            }
            mesh.indices = std::move(indices);
            mesh.vertices = std::move(vertices);
        }
        if (!mesh.indices.empty())
            kept.push_back(std::move(mesh));
    }
    m.model.meshes = std::move(kept);
    std::set<std::string> used;
    for (const auto& mesh : m.model.meshes) {
        used.insert(mesh.texture_name);
        for (const auto& frame : mesh.texture_frames)
            used.insert(frame.first);
    }
    m.textures.erase(
        std::remove_if(m.textures.begin(), m.textures.end(), [&](const auto& t) { return used.count(t.first) == 0; }),
        m.textures.end());
    return dropped;
}

std::vector<std::pair<int, ObjectUse>> object_uses(const WorldRoom& room, const RoomReplacement& r) {
    std::vector<std::pair<int, ObjectUse>> out;
    bool first = true;
    for (const auto& o : room.objects) {
        if (markers_only(r)) {
            out.push_back({o.member, ObjectUse::keep});  // nothing of the modder's to draw
            continue;
        }
        const auto set = r.objects.find(o.member);
        out.push_back({o.member, set != r.objects.end() ? set->second : first ? ObjectUse::model : ObjectUse::hide});
        first = false;
    }
    return out;
}

std::vector<std::string> member_models(const std::filesystem::path& graphics_dump, const std::string& code,
                                       int member) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::string label = member_label(code, member);
    const auto folder = graphics_dump / path_u8(label) / "mdl";
    if (!std::filesystem::is_directory(folder, ec))
        return out;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
        if (lower(u8(e.path().extension())) == ".gltf")
            out.push_back(label + "/mdl/" + u8(e.path().stem()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int> rooms_sharing(const World& w, int room, int member, bool collision) {
    std::vector<int> out;
    for (const auto& r : w.rooms) {
        if (r.index == room)
            continue;
        bool uses = collision ? r.collision_member == member : false;
        for (const auto& o : r.objects)
            uses = uses || (!collision && o.member == member);
        if (uses)
            out.push_back(r.index);
    }
    return out;
}

bool build_room(const World& w, const std::filesystem::path& mod_dir, const RoomReplacement& r, BuiltRoom* out,
                std::string* why) {
    if (r.room < 0 || static_cast<std::size_t>(r.room) >= w.rooms.size())
        return fail_with(why, "no room " + std::to_string(r.room));
    const WorldRoom& room = w.rooms[static_cast<std::size_t>(r.room)];
    const auto& data = w.archive.entries[static_cast<std::size_t>(room.collision_member)].data;
    if (data.empty())
        return fail_with(why, "room " + std::to_string(r.room) + " has no collision in the game's data to replace");
    RoomCollision original;
    if (!parse_collision(data, &original, why))
        return false;
    const auto folder = change_models(mod_dir, r.world, r.room);
    assets::ImportedModel placed;
    if (!placed_model(folder / path_u8(r.collision.empty() ? r.model : r.collision), r.place, &placed, why))
        return false;
    if (r.zone >= 0)
        cut_to_zone(&placed, room_zone(original, r.zone));
    CollisionRules rules;
    rules.ground_limit = r.ground_limit;
    rules.names_by_material = r.names_by_material;
    rules.names_by_triangle = r.names_by_triangle;
    rules.drop_original = r.drop_original;
    rules.markers = r.markers;
    BuiltRoom b;
    if (!build_collision(collision_source(placed, r.collision.empty()), original, rules, &b.collision, &b.report,
                         why)) {
        return false;
    }
    if (!r.edits.empty() && !apply_collision_edits(&b.collision, original, r.edits, r.ground_limit, nullptr, why)) {
        return false;
    }
    const std::string problem = check_collision(b.collision, original, true);
    if (!problem.empty())
        return fail_with(why, "the collision made is not one the game reads: " + problem);
    b.warnings = b.report.warnings;
    for (auto& more : sharing_warnings(w, r))
        b.warnings.push_back(std::move(more));
    *out = std::move(b);
    return true;
}

std::string only_markers_differ(const RoomCollision& c, const std::vector<std::uint8_t>& game) {
    const auto bytes = write_collision(c);
    RoomCollision back, original;
    std::string why;
    if (!parse_collision(bytes, &back, &why))
        return "it does not read back: " + why;
    if (write_collision(back) != bytes)
        return "it does not write back the same";
    if (!parse_collision(game, &original, &why))
        return "the game's does not read: " + why;
    set_markers(&back, markers(original));
    if (write_collision(back) != game)
        return "more than its markers changed";
    return {};
}

bool kept_collision(const World& w, const RoomReplacement& r, RoomCollision* out, std::size_t* moved,
                    std::string* why) {
    if (r.room < 0 || static_cast<std::size_t>(r.room) >= w.rooms.size())
        return fail_with(why, "no room " + std::to_string(r.room));
    const WorldRoom& room = w.rooms[static_cast<std::size_t>(r.room)];
    const auto& data = w.archive.entries[static_cast<std::size_t>(room.collision_member)].data;
    if (data.empty())
        return fail_with(why, "room " + std::to_string(r.room) + " has no collision in the game's data");
    RoomCollision original;
    if (!parse_collision(data, &original, why))
        return false;
    RoomCollision c = original;
    if (!r.edits.empty() && !apply_collision_edits(&c, original, r.edits, r.ground_limit, nullptr, why))
        return false;
    const std::size_t n = move_markers(&c, r.markers);
    // unedited, only the markers' bytes may differ from the game's; edited, the game's own faces are taken as
    // they are (some are not what a made face must be, and the game reads them), the rest checked as made
    const std::string problem = r.edits.empty() ? only_markers_differ(c, data) : check_collision(c, original, true);
    if (!problem.empty())
        return fail_with(why, "the collision as changed: " + problem);
    if (moved != nullptr)
        *moved = n;
    *out = std::move(c);
    return true;
}

void apply_gate_points(World* w, const RoomReplacement& r) {
    for (const auto& [key, at] : r.gate_points) {
        for (auto& g : w->gate_models) {
            if (g.key == key.first)
                g.parts[static_cast<std::size_t>(key.second)].position = at;
        }
    }
}

std::vector<RoomWarning> sharing_warnings(const World& w, const RoomReplacement& r) {
    std::vector<RoomWarning> out;
    if (r.room < 0 || static_cast<std::size_t>(r.room) >= w.rooms.size())
        return out;
    const WorldRoom& room = w.rooms[static_cast<std::size_t>(r.room)];
    for (const int other : rooms_sharing(w, r.room, room.collision_member, true)) {
        out.push_back({RoomWarning::collision_shared, {std::to_string(other)}});
    }
    for (const auto& [member, use] : object_uses(room, r)) {
        if (use == ObjectUse::keep)
            continue;
        for (const int other : rooms_sharing(w, r.room, member, false)) {
            out.push_back({RoomWarning::model_shared, {std::to_string(other), std::to_string(member)}});
        }
    }
    return out;
}

namespace {

// At most this many rooms made at once: each holds its model and its pictures decoded, hundreds of megabytes
// for a KH2 map.
constexpr unsigned kMostAtOnce = 6;

// `work(i)` for every i below `n`, on up to kMostAtOnce threads (the calling one among them); returns when all
// are done. What `work` throws is kept as the text of its index's failure in `failed` (empty: none).
void each_at_once(std::size_t n, const std::function<void(std::size_t)>& work, std::vector<std::string>* failed) {
    failed->assign(n, std::string());
    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (std::size_t i = next++; i < n; i = next++) {
            try {
                work(i);
            } catch (const std::exception& e) {
                (*failed)[i] = e.what();
            } catch (...) {
                (*failed)[i] = "an unknown failure";
            }
        }
    };
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t threads = std::min<std::size_t>(n, std::min(cores, kMostAtOnce));
    std::vector<std::thread> pool;
    for (std::size_t k = 1; k < threads; ++k)
        pool.emplace_back(run);
    run();
    for (auto& thread : pool)
        thread.join();
}

// FNV-1a, 64 bits: what a save was made from, told apart.
struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    Fnv& add(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
        return *this;
    }
    Fnv& add(const std::string& s) {
        add(s.data(), s.size());
        return add("\0", 1);
    }
    Fnv& add(std::uint64_t v) {
        return add(&v, sizeof(v));
    }
};

std::string hex(std::uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
    return text;
}

// The hash of a file's bytes (read a piece at a time), on `f`.
void add_file(Fnv& f, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> piece(1 << 20);
    std::uint64_t size = 0;
    while (in) {
        in.read(piece.data(), static_cast<std::streamsize>(piece.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        f.add(piece.data(), got);
        size += got;
    }
    f.add(size);
}

std::string file_bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A file's size and last change, "" when it is not there.
std::string file_mark(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec)
        return {};
    const auto when = std::filesystem::last_write_time(file, ec);
    if (ec)
        return {};
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(when.time_since_epoch().count()));
}

}  // namespace

// The program saving, by its file's size and last change: another build makes everything again (what a room
// becomes is the program's doing too). Empty when it can't be told: nothing is kept then.
#ifdef _WIN32
extern "C"
    __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* module, wchar_t* name, unsigned long size);
#endif
namespace {
std::string program_mark() {
    std::filesystem::path self;
#ifdef _WIN32
    std::wstring name(32768, L'\0');
    const unsigned long n = GetModuleFileNameW(nullptr, name.data(), static_cast<unsigned long>(name.size()));
    if (n == 0 || n >= name.size())
        return {};
    name.resize(n);
    self = name;
#else
    std::error_code ec;
    self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
#endif
    return file_mark(self);
}

// What the last save of a world made each changed room and member from, and what it gave
// (studio/rooms/<code>/save.cache): a room made from the same is taken from the last save's outputs.
struct SaveCache {
    struct Room {
        std::string from, entry;  // what it was made from; its collision entry's hash in the archive written
        bool untouched = false;
        std::vector<std::string> lines;
        std::string zone;  // the line of its model cut to the room, if any
        bool has_zone = false;
    };
    struct Member {
        std::string from;
        std::vector<std::string> lines;
        std::vector<std::pair<std::string, std::string>> files;  // relative to the mod, and their marks
    };
    std::map<int, Room> rooms;
    std::map<int, Member> members;
};
// 2: an end line (the hash of all before it); a cache without it, cut short, is not trusted
constexpr const char* kCacheHeading = "khdays-studio save cache 2";

std::filesystem::path cache_file(const std::filesystem::path& mod_dir, const std::string& code) {
    return rooms_folder(mod_dir, code) / "save.cache";
}

// Split at tabs into at most `parts` (the last keeps the rest).
std::vector<std::string> fields(const std::string& line, std::size_t parts) {
    std::vector<std::string> out;
    std::size_t from = 0;
    while (out.size() + 1 < parts) {
        const auto tab = line.find('\t', from);
        if (tab == std::string::npos)
            break;
        out.push_back(line.substr(from, tab - from));
        from = tab + 1;
    }
    out.push_back(line.substr(from));
    return out;
}

// The text before a cache's end line when that line holds its hash: the whole cache, else "" (cut short,
// damaged or not there).
std::string whole_cache(const std::string& text) {
    const auto end = text.rfind("end\t");
    if (end == std::string::npos || (end != 0 && text[end - 1] != '\n'))
        return {};
    const std::string body = text.substr(0, end);
    return text.substr(end) == "end\t" + hex(Fnv().add(body).h) + "\n" ? body : std::string();
}

// The cache of `code`'s last save when it was made by this program over this copy of the world (`stamp`) and
// is whole; empty otherwise.
SaveCache read_cache(const std::filesystem::path& mod_dir, const std::string& code, const std::string& stamp) {
    SaveCache c;
    std::istringstream in(whole_cache(file_bytes(cache_file(mod_dir, code))));
    std::string line;
    if (!std::getline(in, line) || line != kCacheHeading || !std::getline(in, line) || line != "stamp\t" + stamp)
        return c;
    while (std::getline(in, line)) {
        const auto f = fields(line, 2);
        if (f.size() < 2)
            continue;
        const std::string& kind = f[0];
        if (kind == "room") {
            const auto r = fields(f[1], 4);
            if (r.size() == 4)
                c.rooms[std::atoi(r[0].c_str())] = {r[1], r[3], r[2] == "1", {}, {}, false};
        } else if (kind == "rline" || kind == "zone") {
            const auto r = fields(f[1], 2);
            if (r.size() == 2 && c.rooms.count(std::atoi(r[0].c_str())) != 0) {
                SaveCache::Room& room = c.rooms[std::atoi(r[0].c_str())];
                if (kind == "rline") {
                    room.lines.push_back(r[1]);
                } else {
                    room.zone = r[1];
                    room.has_zone = true;
                }
            }
        } else if (kind == "member") {
            const auto r = fields(f[1], 2);
            if (r.size() == 2)
                c.members[std::atoi(r[0].c_str())].from = r[1];
        } else if (kind == "mline" || kind == "mfile") {
            const auto r = fields(f[1], kind == "mline" ? 2 : 3);
            const int m = std::atoi(r[0].c_str());
            if (c.members.count(m) == 0)
                continue;
            if (kind == "mline" && r.size() == 2)
                c.members[m].lines.push_back(r[1]);
            else if (kind == "mfile" && r.size() == 3)
                c.members[m].files.emplace_back(r[2], r[1]);
        }
    }
    return c;
}

}  // namespace

bool write_world_outputs(const std::filesystem::path& nitrofs, const std::filesystem::path& graphics_dump,
                         const std::filesystem::path& mod_dir, const std::string& code, WorldOutcome* out,
                         std::string* why, const SaveProgressFn& progress) {
    WorldOutcome o;
    World w;
    if (!load_world(nitrofs, code, &w, why))
        return false;  // the player's copy, never the mod's
    std::vector<std::string> problems;
    const auto changes = changes_of(mod_dir, code, &problems);
    if (!problems.empty())
        return fail_with(why, problems.front());
    std::error_code ec;
    const auto archive = mod_dir / "files" / path_u8(world_path(code));

    const std::set<std::string> before = read_written(mod_dir, code);
    if (changes.empty()) {
        for (const auto& file : before)
            remove_written(mod_dir, file);
        std::filesystem::remove(written_list(mod_dir, code), ec);
        std::filesystem::remove(cache_file(mod_dir, code), ec);
        std::filesystem::remove(rooms_folder(mod_dir, code), ec);  // only when empty
        o.removed = true;
        o.lines.push_back(world_path(code) + ": no room replaced; its outputs removed");
        if (out != nullptr)
            *out = std::move(o);
        return true;
    }

    // what the last save made, kept when it was made by this program over this copy of the world: each room
    // made from the same change and model files as then is taken from the last save's outputs, not made again
    const std::string program = program_mark();
    const std::string stamp = program.empty()
                                  ? std::string()
                                  : hex(Fnv().add(program).add(file_bytes(nitrofs / path_u8(world_path(code)))).h);
    const SaveCache cache = stamp.empty() ? SaveCache{} : read_cache(mod_dir, code, stamp);
    World last;  // the archive the last save wrote, its collisions taken back
    bool has_last = false;
    if (!cache.rooms.empty()) {
        const auto bytes = file_bytes(archive);
        std::string unused;
        has_last =
            !bytes.empty() && parse_world(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), code, &last, &unused);
    }
    // per change: its file and its models' files, by what they hold (an edit keeping a file's size and time is
    // seen too); the changes at once, hundreds of megabytes read for a world of KH2 rooms
    std::vector<std::string> made_from(changes.size());
    std::vector<std::string> unread;
    each_at_once(
        changes.size(),
        [&](std::size_t i) {
            Fnv f;
            f.add(file_bytes(change_file(mod_dir, code, changes[i].room)));
            std::error_code walk;
            std::vector<std::filesystem::path> files;
            const auto folder = change_models(mod_dir, code, changes[i].room);
            for (auto it = std::filesystem::recursive_directory_iterator(folder, walk);
                 !walk && it != std::filesystem::recursive_directory_iterator(); it.increment(walk)) {
                if (it->is_regular_file(walk))
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                f.add(u8(std::filesystem::relative(file, folder, walk)));
                add_file(f, file);
            }
            made_from[i] = hex(f.h);
        },
        &unread);
    for (std::size_t i = 0; i < changes.size(); ++i) {
        if (!unread[i].empty())
            made_from[i].clear();  // nothing matches it: made again
    }
    const auto index_of = [&](const RoomReplacement* r) { return static_cast<std::size_t>(r - changes.data()); };
    const auto entry_hash = [](const std::vector<std::uint8_t>& data) {
        return hex(Fnv().add(data.data(), data.size()).h);
    };

    // what each changed room's models become (the modder's, nothing, or the game's), and the steps: a room's
    // collision each, the archive, a member's models each, the list of what was written
    std::map<int, std::pair<const RoomReplacement*, ObjectUse>> use_of;
    for (const auto& r : changes) {
        for (const auto& [member, use] : object_uses(w.rooms[static_cast<std::size_t>(r.room)], r))
            use_of[member] = {&r, use};
    }
    SaveProgress step;
    step.total = static_cast<int>(changes.size()) + 2;
    for (const auto& [member, change] : use_of)
        step.total += change.second == ObjectUse::keep ? 0 : 1;
    std::mutex stepping;  // the rooms' threads report too
    const auto report = [&](SaveProgress::Stage stage, int room) {
        std::lock_guard lock(stepping);
        step.stage = stage;
        step.room = room;
        if (progress)
            progress(step);
        ++step.done;
    };

    // the collisions: each room's made at once (they share nothing), then taken in the changes' order -- the
    // lines, the first failure and two rooms on one collision as one room after another would find them
    struct MadeCollision {
        bool untouched = false;  // the game's own, nothing moved: nothing to write
        bool kept = false;       // the game's own, markers moved
        bool reused = false;     // taken from the last save
        int member = -1;
        std::vector<std::uint8_t> bytes;
        std::vector<std::string> lines;
        std::string problem;
    };
    std::vector<MadeCollision> made(changes.size());
    std::vector<std::string> thrown;
    each_at_once(
        changes.size(),
        [&](std::size_t i) {
            const RoomReplacement& r = changes[i];
            MadeCollision& c = made[i];
            report(SaveProgress::building, r.room);
            const std::string room = "room " + std::to_string(r.room) + ": ";
            // made from the same as the last save: its collision taken from the archive that save wrote
            if (const auto was = cache.rooms.find(r.room);
                !made_from[i].empty() && was != cache.rooms.end() && was->second.from == made_from[i]) {
                const int member = w.rooms[static_cast<std::size_t>(r.room)].collision_member;
                if (was->second.untouched) {
                    c.untouched = c.reused = true;
                    c.lines = was->second.lines;
                    return;
                }
                if (has_last && member >= 0 && static_cast<std::size_t>(member) < last.archive.entries.size() &&
                    entry_hash(last.archive.entries[static_cast<std::size_t>(member)].data) == was->second.entry) {
                    c.member = member;
                    c.bytes = last.archive.entries[static_cast<std::size_t>(member)].data;
                    c.kept = r.keep_collision && r.edits.empty();
                    c.lines = was->second.lines;
                    c.reused = true;
                    return;
                }
            }
            if (r.keep_collision && r.markers.empty() && r.edits.empty()) {
                c.untouched = true;
                c.lines.push_back(room + "the game's own grounds, walls and ceilings kept");
                return;
            }
            c.member = w.rooms[static_cast<std::size_t>(r.room)].collision_member;
            if (r.keep_collision) {
                // the game's own floor and walls, its markers moved: only those bytes change
                RoomCollision kept_room;
                std::size_t moved = 0;
                if (!kept_collision(w, r, &kept_room, &moved, &c.problem))
                    return;
                c.bytes = write_collision(kept_room);
                c.kept = r.edits.empty();  // else checked as an edited collision
                c.lines.push_back(
                    room + "the game's own grounds, walls and ceilings kept" +
                    (r.edits.empty() ? std::string()
                                     : ", edited (" + std::to_string(kept_room.faces[kGround].size()) + " grounds, " +
                                           std::to_string(kept_room.faces[kWall].size()) + " walls, " +
                                           std::to_string(kept_room.faces[kCeiling].size()) + " ceilings)") +
                    ", " + std::to_string(moved) + " of " + std::to_string(r.markers.size()) + " markers moved");
                for (const int other : rooms_sharing(w, r.room, c.member, true))
                    c.lines.push_back(room + warning_text({RoomWarning::collision_shared, {std::to_string(other)}}));
                return;
            }
            BuiltRoom built;
            if (!build_room(w, mod_dir, r, &built, &c.problem))
                return;
            c.bytes = write_collision(built.collision);
            c.lines.push_back(room + std::to_string(built.report.faces[kGround]) + " grounds, " +
                              std::to_string(built.report.faces[kWall]) + " walls, " +
                              std::to_string(built.report.faces[kCeiling]) + " ceilings, " +
                              std::to_string(built.report.nodes) + " nodes, " + std::to_string(built.report.bytes) +
                              " bytes");
            for (const auto& warning : built.warnings)
                c.lines.push_back(room + warning_text(warning));
        },
        &thrown);
    std::vector<std::vector<std::string>> room_lines(changes.size());  // for the cache
    for (std::size_t i = 0; i < changes.size(); ++i)
        room_lines[i] = made[i].lines;
    std::map<int, std::vector<std::uint8_t>> collisions;
    std::map<int, int> collision_of;  // member -> the room that made it
    std::set<int> kept;               // members that are the game's own collision, markers moved
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const RoomReplacement& r = changes[i];
        MadeCollision& c = made[i];
        if (!thrown[i].empty())
            c.problem = thrown[i];
        if (!c.problem.empty())
            return fail_with(why, "room " + std::to_string(r.room) + ": " + c.problem);
        if (!c.untouched) {
            if (collision_of.count(c.member) != 0) {
                return fail_with(why, "rooms " + std::to_string(collision_of[c.member]) + " and " +
                                          std::to_string(r.room) + " share one collision: replace only one of them");
            }
            collision_of[c.member] = r.room;
            collisions[c.member] = std::move(c.bytes);
            if (c.kept)
                kept.insert(c.member);
        }
        for (auto& line : c.lines)
            o.lines.push_back(std::move(line));
    }

    // the archive, checked before it replaces anything: the gate models' points moved too
    report(SaveProgress::archive, -1);
    World moved = w;
    for (const auto& r : changes)
        apply_gate_points(&moved, r);
    const auto bytes = write_world(moved, collisions);
    const auto gate_entry = write_gate_models(moved);
    const auto result = write_into_mod(
        mod_dir, archive,
        [&](const std::filesystem::path& temporary) -> std::string {
            std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            f.close();
            return f ? std::string() : std::string("could not be written");
        },
        [&](const std::filesystem::path& temporary) -> std::string {
            std::ifstream f(temporary, std::ios::binary);
            const std::vector<std::uint8_t> back((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            World again;
            std::string problem;
            if (!parse_world(back, code, &again, &problem))
                return "it does not read back: " + problem;
            if (again.archive.entries.size() != w.archive.entries.size())
                return "it holds another number of entries";
            for (std::size_t e = 0; e < w.archive.entries.size(); ++e) {
                const auto made = collisions.find(static_cast<int>(e));
                if (e == 1) {
                    if (again.archive.entries[e].data != gate_entry)
                        return "the gate models are not as moved";
                    continue;
                }
                if (made == collisions.end()) {
                    if (again.archive.entries[e].data != w.archive.entries[e].data)
                        return "entry " + std::to_string(e) + " changed";
                    continue;
                }
                RoomCollision c, original;
                if (!parse_collision(again.archive.entries[e].data, &c, &problem))
                    return "entry " + std::to_string(e) + ": " + problem;
                if (kept.count(static_cast<int>(e)) != 0) {
                    problem = only_markers_differ(c, w.archive.entries[e].data);
                    if (!problem.empty())
                        return "entry " + std::to_string(e) + ": " + problem;
                    continue;
                }
                if (!parse_collision(w.archive.entries[e].data, &original, &problem))
                    return problem;
                problem = check_collision(c, original, true);
                if (!problem.empty())
                    return "entry " + std::to_string(e) + ": " + problem;
            }
            return {};
        });
    if (!result.ok)
        return fail_with(why, world_path(code) + ": " + result.why);
    std::set<std::string> now{"files/" + world_path(code)};
    o.lines.push_back(u8(archive) + " written");

    // the models: the modder's, nothing, or the game's. A change's members together (they share its placed
    // model), the changes at once; then taken in the members' order -- the lines and the first failure as one
    // member after another would find them
    struct MemberOut {
        int member = -1;
        const RoomReplacement* r = nullptr;
        ObjectUse use = ObjectUse::keep;
        bool no_models = false;  // the dump has none of it
        std::string from;        // what it was made from (its change's, its use, its keys)
        std::vector<std::string> lines;
        std::set<std::string> written;
        std::string problem;
    };
    struct ChangeOut {
        std::vector<std::size_t> members;  // into `outs`, in the members' order
        bool placed_tried = false;
        std::string zone_line, problem;  // of placing its model
    };
    std::vector<MemberOut> outs;
    std::map<std::string, std::size_t> change_index;  // by world/room
    std::vector<ChangeOut> change_outs;
    for (const auto& [member, change] : use_of) {
        if (change.second == ObjectUse::keep)
            continue;
        const std::string id = change.first->world + "/" + std::to_string(change.first->room);
        auto found = change_index.find(id);
        if (found == change_index.end()) {
            found = change_index.emplace(id, change_outs.size()).first;
            change_outs.emplace_back();
        }
        change_outs[found->second].members.push_back(outs.size());
        MemberOut m;
        m.member = member;
        m.r = change.first;
        m.use = change.second;
        outs.push_back(std::move(m));
    }
    each_at_once(
        change_outs.size(),
        [&](std::size_t c) {
            ChangeOut& change = change_outs[c];
            assets::ImportedModel placed;
            for (const std::size_t at : change.members) {
                MemberOut& m = outs[at];
                const RoomReplacement& r = *m.r;
                report(SaveProgress::models, r.room);
                const auto keys = member_models(graphics_dump, code, m.member);
                if (keys.empty()) {
                    m.no_models = true;
                    return;  // as one after another: nothing past this member
                }
                Fnv f;
                f.add(made_from[index_of(&r)]).add(static_cast<std::uint64_t>(m.use));
                for (const auto& key : keys)
                    f.add(key);
                m.from = hex(f.h);
                // made from the same as the last save, its files still as that save left them: kept
                const auto was = cache.members.find(m.member);
                const auto room_was = cache.rooms.find(r.room);
                if (was != cache.members.end() && was->second.from == m.from &&
                    (m.use != ObjectUse::model || r.zone < 0 ||
                     (room_was != cache.rooms.end() && room_was->second.has_zone))) {
                    // every model of it listed, each listed file still as written
                    bool same = true;
                    for (const auto& key : keys) {
                        const std::string gltf = "graphics/" + key + ".gltf";
                        same = same && std::any_of(was->second.files.begin(), was->second.files.end(),
                                                   [&](const auto& file) { return file.first == gltf; });
                    }
                    for (const auto& [file, mark] : was->second.files)
                        same = same && file_mark(mod_dir / path_u8(file)) == mark;
                    if (same) {
                        m.lines = was->second.lines;
                        for (const auto& [file, mark] : was->second.files)
                            m.written.insert(file);
                        if (m.use == ObjectUse::model && r.zone >= 0 && !change.placed_tried)
                            change.zone_line = room_was->second.zone;
                        continue;
                    }
                }
                if (m.use == ObjectUse::model && !change.placed_tried) {
                    change.placed_tried = true;
                    if (!placed_model(change_models(mod_dir, r.world, r.room) / path_u8(r.model), r.place, &placed,
                                      &change.problem))
                        return;
                    if (r.zone >= 0) {
                        // only what lies around the room
                        const WorldRoom& room = w.rooms[static_cast<std::size_t>(r.room)];
                        const auto& data = w.archive.entries[static_cast<std::size_t>(room.collision_member)].data;
                        RoomCollision floor;
                        std::string problem;
                        if (data.empty() || !parse_collision(data, &floor, &problem)) {
                            change.problem = "room " + std::to_string(r.room) + ": no floor to cut the model to";
                            return;
                        }
                        change.zone_line = "room " + std::to_string(r.room) + ": " +
                                           std::to_string(cut_to_zone(&placed, room_zone(floor, r.zone))) +
                                           " triangles away from the room left out";
                    }
                }
                const assets::ImportedModel nothing;
                const assets::ImportedModel& model = m.use == ObjectUse::model ? placed : nothing;
                for (const auto& key : keys) {
                    if (!write_room_model(mod_dir, key, model, &m.written, &m.problem))
                        return;
                    m.lines.push_back(key + ".gltf written" + (m.use == ObjectUse::hide ? " (nothing drawn)" : ""));
                }
            }
        },
        &thrown);
    std::vector<bool> zone_said(change_outs.size(), false);
    for (std::size_t c = 0; c < change_outs.size(); ++c) {
        if (!thrown[c].empty())
            change_outs[c].problem = thrown[c];
    }
    for (std::size_t at = 0; at < outs.size(); ++at) {
        MemberOut& m = outs[at];
        const std::size_t c = change_index.at(m.r->world + "/" + std::to_string(m.r->room));
        const ChangeOut& change = change_outs[c];
        if (m.no_models) {
            return fail_with(why, "the graphics dump has no model of " + member_label(code, m.member) +
                                      ": make the dump first");
        }
        if (m.use == ObjectUse::model && !zone_said[c]) {
            zone_said[c] = true;
            if (!change.problem.empty())
                return fail_with(why, change.problem);
            if (!change.zone_line.empty())
                o.lines.push_back(change.zone_line);
        } else if (!change.problem.empty() && m.lines.empty() && m.problem.empty()) {
            return fail_with(why, change.problem);  // a member after a failed one: never made
        }
        if (!m.problem.empty())
            return fail_with(why, m.problem);
        now.insert(m.written.begin(), m.written.end());
        for (const auto& line : m.lines)
            o.lines.push_back(line);  // copied: kept for the cache
    }
    // what an earlier save wrote and this one did not: removed; the list kept
    for (const auto& file : before) {
        if (now.count(file) == 0)
            remove_written(mod_dir, file);
    }
    report(SaveProgress::finishing, -1);
    std::string list;
    for (const auto& file : now)
        list += file + "\n";
    const auto saved = write_into_mod(
        mod_dir, written_list(mod_dir, code),
        [&](const std::filesystem::path& temporary) -> std::string {
            std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
            f << list;
            f.close();
            return f ? std::string() : std::string("could not be written");
        },
        [](const std::filesystem::path&) { return std::string(); });
    if (!saved.ok)
        return fail_with(why, saved.why);
    // what this save made each room and member from, for the next (a cache: without it all is made again)
    if (!stamp.empty()) {
        World written_world;
        std::string unused;
        const bool readable = parse_world(bytes, code, &written_world, &unused);
        std::ostringstream c;
        c << kCacheHeading << "\nstamp\t" << stamp << "\n";
        for (std::size_t i = 0; i < changes.size(); ++i) {
            const RoomReplacement& r = changes[i];
            const int member = w.rooms[static_cast<std::size_t>(r.room)].collision_member;
            const bool untouched = r.keep_collision && r.markers.empty() && r.edits.empty();
            std::string entry = "-";
            if (!untouched && readable && member >= 0 &&
                static_cast<std::size_t>(member) < written_world.archive.entries.size())
                entry = entry_hash(written_world.archive.entries[static_cast<std::size_t>(member)].data);
            c << "room\t" << r.room << "\t" << made_from[i] << "\t" << (untouched ? 1 : 0) << "\t" << entry << "\n";
            for (const auto& line : room_lines[i])
                c << "rline\t" << r.room << "\t" << line << "\n";
        }
        for (std::size_t k = 0; k < change_outs.size(); ++k) {
            const ChangeOut& change = change_outs[k];
            if (!change.zone_line.empty() && !change.members.empty())
                c << "zone\t" << outs[change.members.front()].r->room << "\t" << change.zone_line << "\n";
        }
        for (const MemberOut& m : outs) {
            if (m.from.empty())
                continue;
            c << "member\t" << m.member << "\t" << m.from << "\n";
            for (const auto& line : m.lines)
                c << "mline\t" << m.member << "\t" << line << "\n";
            for (const auto& file : m.written)
                c << "mfile\t" << m.member << "\t" << file_mark(mod_dir / path_u8(file)) << "\t" << file << "\n";
        }
        std::string text = c.str();
        text += "end\t" + hex(Fnv().add(text).h) + "\n";
        // not kept is no failure: the next save makes everything again
        (void)write_into_mod(
            mod_dir, cache_file(mod_dir, code),
            [&](const std::filesystem::path& temporary) -> std::string {
                std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
                f << text;
                f.close();
                return f ? std::string() : std::string("could not be written");
            },
            [](const std::filesystem::path&) { return std::string(); });
    }
    if (out != nullptr)
        *out = std::move(o);
    return true;
}

bool save_change(const std::filesystem::path& nitrofs, const std::filesystem::path& graphics_dump,
                 const std::filesystem::path& mod_dir, RoomReplacement r, const std::filesystem::path& model_source,
                 const std::filesystem::path& collision_source_file, WorldOutcome* out, std::string* why,
                 const SaveProgressFn& progress) {
    std::error_code ec;
    const auto folder = change_models(mod_dir, r.world, r.room);
    if (progress)
        progress(SaveProgress{});  // the model files copied first
    // the model files and what they need, beside the change
    auto bring = [&](const std::filesystem::path& source, std::string* name) {
        if (source.empty())
            return true;
        *name = u8(source.filename());
        const auto destination = folder / source.filename();
        if (std::filesystem::equivalent(source, destination, ec))
            return true;
        std::filesystem::create_directories(folder, ec);
        std::string problem = copy_bytes(source, destination);
        if (!problem.empty())
            return fail_with(why, problem);
        std::vector<std::string> missing;
        for (const auto& c : model_companions(source, &missing)) {
            const auto to = folder / path_u8(c.relative);
            std::filesystem::create_directories(to.parent_path(), ec);
            problem = copy_bytes(c.source, to);
            if (!problem.empty())
                return fail_with(why, problem);
        }
        return true;
    };
    if (!bring(model_source, &r.model))
        return false;
    if (collision_source_file.empty())
        r.collision.clear();
    else if (!bring(collision_source_file, &r.collision))
        return false;
    // the change, then everything made from it
    const std::string text = change_text(r);
    const auto result = write_into_mod(
        mod_dir, change_file(mod_dir, r.world, r.room),
        [&](const std::filesystem::path& temporary) -> std::string {
            std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
            f << text;
            f.close();
            return f ? std::string() : std::string("could not be written");
        },
        [&](const std::filesystem::path& temporary) -> std::string {
            RoomReplacement back;
            std::string problem;
            return read_change(temporary, &back, &problem) ? std::string() : problem;
        });
    if (!result.ok)
        return fail_with(why, result.why);
    return write_world_outputs(nitrofs, graphics_dump, mod_dir, r.world, out, why, progress);
}

bool revert_change(const std::filesystem::path& nitrofs, const std::filesystem::path& graphics_dump,
                   const std::filesystem::path& mod_dir, const std::string& code, int room, WorldOutcome* out,
                   std::string* why, const SaveProgressFn& progress) {
    std::error_code ec;
    const auto file = change_file(mod_dir, code, room);
    if (!std::filesystem::exists(file, ec))
        return fail_with(why, "room " + std::to_string(room) + " of " + code + " is not replaced in this mod");
    // the change and its models set aside (not a .txt: no longer read as a change), the world made without
    // them, and only then removed; when the world can't be made they are put back, the mod as it was
    const auto folder = change_models(mod_dir, code, room);
    auto aside_file = file, aside_folder = folder;
    aside_file += ".giving-back";
    aside_folder += ".giving-back";
    std::filesystem::remove(aside_file, ec);  // left by a studio stopped half way
    std::filesystem::remove_all(aside_folder, ec);
    std::filesystem::rename(file, aside_file, ec);
    if (ec)
        return fail_with(why, "cannot set " + u8(file) + " aside: " + ec.message());
    const bool has_folder = std::filesystem::exists(folder, ec);
    if (has_folder) {
        std::filesystem::rename(folder, aside_folder, ec);
        if (ec) {
            const std::string problem = ec.message();
            std::filesystem::rename(aside_file, file, ec);
            return fail_with(why, "cannot set " + u8(folder) + " aside: " + problem);
        }
    }
    if (!write_world_outputs(nitrofs, graphics_dump, mod_dir, code, out, why, progress)) {
        std::filesystem::rename(aside_file, file, ec);
        if (has_folder)
            std::filesystem::rename(aside_folder, folder, ec);
        return false;
    }
    std::filesystem::remove(aside_file, ec);
    std::filesystem::remove_all(aside_folder, ec);
    std::filesystem::remove(rooms_folder(mod_dir, code), ec);  // only when empty: no room replaced any more
    return true;
}

}  // namespace studio
