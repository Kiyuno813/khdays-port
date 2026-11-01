#include "core/workshop/workshop.h"

#include "core/files/paths.h"
#include "core/models/model_files.h"
#include "core/rooms/kh2_source.h"

#include "khdays/assets/gltf_export.h"

extern "C" {
#include "render/png_io.h"
}

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace studio {

namespace assets = khdays::assets;

namespace {

constexpr const char* kHeading = "khdays-studio map workshop 1";
const char* const kLayerWords[kWorkLayers] = {"model", "collision"};

bool fail(std::string* why, const std::string& text) {
    if (why != nullptr)
        *why = text;
    return false;
}

// "0-12 40 77-80" from the marked indices.
std::string ranges_of(const std::vector<std::uint8_t>& marked) {
    std::ostringstream o;
    bool first = true;
    for (std::size_t i = 0; i < marked.size();) {
        if (marked[i] == 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1 < marked.size() && marked[j + 1] != 0)
            ++j;
        o << (first ? "" : " ") << i;
        if (j > i)
            o << "-" << j;
        first = false;
        i = j + 1;
    }
    return o.str();
}

bool read_ranges(std::istringstream& words, std::vector<std::uint8_t>* marked) {
    std::string w;
    while (words >> w) {
        const auto dash = w.find('-');
        char* end = nullptr;
        const unsigned long a = std::strtoul(w.c_str(), &end, 10);
        unsigned long b = a;
        if (dash != std::string::npos)
            b = std::strtoul(w.c_str() + dash + 1, &end, 10);
        if (b < a || b > 50000000UL)
            return false;
        if (marked->size() <= b)
            marked->resize(b + 1, 0);
        std::fill(marked->begin() + static_cast<std::ptrdiff_t>(a),
                  marked->begin() + static_cast<std::ptrdiff_t>(b) + 1, std::uint8_t{1});
    }
    return true;
}

}  // namespace

std::size_t triangle_count(const assets::ImportedModel& model) {
    std::size_t n = 0;
    for (const auto& mesh : model.model.meshes)
        n += mesh.indices.size() / 3;
    return n;
}

std::string workshop_text(const WorkshopProject& project) {
    std::ostringstream o;
    o << kHeading << "\n";
    for (const auto& m : project.maps) {
        o << "map " << m.map << "\n";
        o << "scale " << m.place.scale << "\n";
        o << "turn " << m.place.turn[0] << " " << m.place.turn[1] << " " << m.place.turn[2] << "\n";
        o << "move " << m.place.move[0] << " " << m.place.move[1] << " " << m.place.move[2] << "\n";
        if (!m.shown)
            o << "hidden\n";
        for (int l = 0; l < kWorkLayers; ++l) {
            const std::string r = ranges_of(m.deleted[static_cast<std::size_t>(l)]);
            if (!r.empty())
                o << "delete " << kLayerWords[l] << " " << r << "\n";
        }
    }
    return o.str();
}

bool read_workshop_text(const std::string& text, WorkshopProject* out, std::string* why) {
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line))
        return fail(why, "empty");
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != kHeading)
        return fail(why, "not a map workshop project (its first line is not \"" + std::string(kHeading) + "\")");
    WorkshopProject p;
    int number = 1;
    while (std::getline(lines, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream words(line);
        std::string key;
        if (!(words >> key) || key[0] == '#')
            continue;
        const auto bad = [&] { return fail(why, "line " + std::to_string(number) + ": " + line); };
        if (key == "map") {
            WorkshopMap m;
            if (!(words >> m.map))
                return bad();
            p.maps.push_back(std::move(m));
            continue;
        }
        if (p.maps.empty())
            return bad();
        WorkshopMap& m = p.maps.back();
        if (key == "scale") {
            if (!(words >> m.place.scale))
                return bad();
        } else if (key == "turn") {
            if (!(words >> m.place.turn[0] >> m.place.turn[1] >> m.place.turn[2]))
                return bad();
        } else if (key == "move") {
            if (!(words >> m.place.move[0] >> m.place.move[1] >> m.place.move[2]))
                return bad();
        } else if (key == "hidden") {
            m.shown = false;
        } else if (key == "delete") {
            std::string layer;
            words >> layer;
            int l = -1;
            for (int k = 0; k < kWorkLayers; ++k)
                l = layer == kLayerWords[k] ? k : l;
            if (l < 0 || !read_ranges(words, &m.deleted[static_cast<std::size_t>(l)]))
                return bad();
        } else {
            return bad();
        }
    }
    *out = std::move(p);
    return true;
}

bool save_workshop(const std::filesystem::path& file, const WorkshopProject& project, std::string* why) {
    std::error_code ec;
    if (!file.parent_path().empty())
        std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temporary = file.string() + ".tmp";
    {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        f << workshop_text(project);
        if (!f)
            return fail(why, "cannot write " + u8(file));
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return fail(why, "cannot write " + u8(file));
    }
    return true;
}

bool load_workshop(const std::filesystem::path& file, WorkshopProject* out, std::string* why) {
    std::ifstream f(file, std::ios::binary);
    if (!f)
        return fail(why, "cannot read " + u8(file));
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return read_workshop_text(text, out, why);
}

assets::ImportedModel without_deleted(const assets::ImportedModel& model, const std::vector<std::uint8_t>& deleted) {
    assets::ImportedModel out = model;
    out.model.meshes.clear();
    std::size_t t = 0;
    for (const auto& mesh : model.model.meshes) {
        assets::NeutralMesh kept = mesh;
        kept.indices.clear();
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3, ++t) {
            if (t < deleted.size() && deleted[t] != 0)
                continue;
            kept.indices.insert(kept.indices.end(), mesh.indices.begin() + static_cast<std::ptrdiff_t>(i),
                                mesh.indices.begin() + static_cast<std::ptrdiff_t>(i) + 3);
        }
        if (kept.indices.empty())
            continue;
        // only the corners still used
        std::map<std::uint32_t, std::uint32_t> renumbered;
        std::vector<assets::NeutralVertex> vertices;
        for (auto& index : kept.indices) {
            auto [it, fresh] = renumbered.emplace(index, static_cast<std::uint32_t>(vertices.size()));
            if (fresh)
                vertices.push_back(mesh.vertices[index]);
            index = it->second;
        }
        kept.vertices = std::move(vertices);
        out.model.meshes.push_back(std::move(kept));
    }
    return out;
}

assets::ImportedModel merged_models(const std::vector<assets::ImportedModel>& parts) {
    assets::ImportedModel out;
    out.model.name = "workshop";
    out.model.palette.push_back(std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    std::set<std::string> textures;
    for (const auto& part : parts) {
        // palette entry 0 is the identity every model's still meshes use: kept as one; the others after ours
        const auto palette_offset = static_cast<std::uint32_t>(out.model.palette.size() - 1);
        const auto joint_offset = static_cast<int>(out.joints.size());
        const auto node_offset = static_cast<int>(out.nodes.size());
        for (std::size_t k = 1; k < part.model.palette.size(); ++k)
            out.model.palette.push_back(part.model.palette[k]);
        for (auto mesh : part.model.meshes) {
            for (auto& v : mesh.vertices) {
                for (auto& j : v.joints)
                    j = j == 0 ? 0 : j + palette_offset;
            }
            out.model.meshes.push_back(std::move(mesh));
        }
        for (auto joint : part.joints) {
            joint.palette_index = joint.palette_index == 0 ? 0 : joint.palette_index + palette_offset;
            joint.parent = joint.parent < 0 ? -1 : joint.parent + joint_offset;
            joint.node = joint.node < 0 ? -1 : joint.node + node_offset;
            out.joints.push_back(std::move(joint));
        }
        for (auto node : part.nodes) {
            node.parent = node.parent < 0 ? -1 : node.parent + node_offset;
            out.nodes.push_back(std::move(node));
        }
        for (auto animation : part.animations) {
            for (auto& track : animation.tracks)
                track.node += node_offset;
            out.animations.push_back(std::move(animation));
        }
        for (const auto& t : part.textures) {
            if (textures.insert(t.first).second)
                out.textures.push_back(t);
        }
    }
    return out;
}

bool write_model_gltf(const assets::ImportedModel& model, const std::filesystem::path& file, std::string* why) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::string stem = u8(file.stem());
    std::map<std::string, assets::GltfTextureRef> refs;
    std::size_t n = 0;
    for (const auto& [name, decoded] : model.textures) {
        if (decoded.rgba.empty() || decoded.width <= 0 || decoded.height <= 0)
            continue;
        const std::string png = stem + "_" + std::to_string(n++) + ".png";
        const char* e = khdays_png_write(u8(file.parent_path() / path_u8(png)).c_str(), decoded.rgba.data(),
                                         decoded.width, decoded.height);
        if (e != nullptr)
            return fail(why, png + ": " + e);
        refs[name] = {png, decoded.width, decoded.height,
                      assets::texels_translucent(decoded.rgba.data(), decoded.rgba.size() / 4)};
    }
    const std::string text = assets::moves_by_itself(model) ? assets::scene_to_gltf(assets::scene_of(model, refs))
                                                            : assets::to_gltf(model.model, refs);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << text;
    if (!f)
        return fail(why, "cannot write " + u8(file));
    return true;
}

std::vector<int> connected_pieces(const assets::ImportedModel& model) {
    // corners welded by position (a thousandth of a unit)
    std::map<std::array<long long, 3>, int> corner_of;
    std::vector<int> parent;
    const auto find = [&](int x) {
        while (parent[static_cast<std::size_t>(x)] != x) {
            parent[static_cast<std::size_t>(x)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
            x = parent[static_cast<std::size_t>(x)];
        }
        return x;
    };
    std::vector<int> first_corner;
    for (const auto& mesh : model.model.meshes) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            int corners[3];
            for (int c = 0; c < 3; ++c) {
                const auto& p = mesh.vertices[mesh.indices[i + static_cast<std::size_t>(c)]].position;
                const std::array<long long, 3> key{std::llround(p[0] * 1000.0), std::llround(p[1] * 1000.0),
                                                   std::llround(p[2] * 1000.0)};
                auto [it, fresh] = corner_of.emplace(key, static_cast<int>(parent.size()));
                if (fresh)
                    parent.push_back(it->second);
                corners[c] = it->second;
            }
            const int a = find(corners[0]);
            parent[static_cast<std::size_t>(find(corners[1]))] = a;
            parent[static_cast<std::size_t>(find(corners[2]))] = a;
            first_corner.push_back(corners[0]);
        }
    }
    std::map<int, int> numbered;
    std::vector<int> out;
    out.reserve(first_corner.size());
    for (const int c : first_corner) {
        const int root = find(c);
        auto [it, fresh] = numbered.emplace(root, static_cast<int>(numbered.size()));
        out.push_back(it->second);
    }
    return out;
}

bool export_workshop(const WorkshopProject& project, const std::vector<const assets::ImportedModel*>& models,
                     const std::vector<const assets::ImportedModel*>& collisions, const std::filesystem::path& folder,
                     const std::string& name, std::vector<std::string>* written, std::string* why) {
    if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos)
        return fail(why, "the name \"" + name + "\" can't be a file's");
    for (int l = 0; l < kWorkLayers; ++l) {
        const auto& layer = l == kWorkModel ? models : collisions;
        std::vector<assets::ImportedModel> parts;
        for (std::size_t i = 0; i < project.maps.size() && i < layer.size(); ++i) {
            const WorkshopMap& m = project.maps[i];
            if (!m.shown || layer[i] == nullptr)
                continue;
            assets::ImportedModel part = without_deleted(*layer[i], m.deleted[static_cast<std::size_t>(l)]);
            place_model(&part, m.place);
            parts.push_back(std::move(part));
        }
        if (parts.empty())
            continue;
        const std::filesystem::path file =
            folder / path_u8(l == kWorkModel ? name + ".gltf" : name + "_collision.gltf");
        if (!write_model_gltf(merged_models(parts), file, why))
            return false;
        if (written != nullptr)
            written->push_back(u8(file));
    }
    if (written != nullptr && written->empty())
        return fail(why, "no map shown with anything loaded to write");
    return true;
}

bool prepare_workshop_map(const Settings& settings, const std::string& map, assets::ImportedModel* model,
                          assets::ImportedModel* collision, std::string* why) {
    if (map.empty() || map.find_first_of("/\\:*?\"<>| .") != std::string::npos)
        return fail(why, "\"" + map + "\" is not a KH2 map's name (tt08)");
    if (!take_out_kh2_map(settings, map, why))
        return false;
    std::error_code ec;
    const std::filesystem::path made = kh2_made_model(settings.root, map);
    if (!std::filesystem::is_regular_file(made, ec)) {
        Kh2Made outcome;
        if (!make_kh2_room(kh2_folder(settings.root), map, made, &outcome, why))
            return false;
    }
    if (!import_model(made, model, why))
        return false;
    *collision = assets::ImportedModel{};
    std::filesystem::path floors = made.parent_path() / path_u8(map + "_collision.gltf");
    std::string unused;
    if (!std::filesystem::is_regular_file(floors, ec) && !make_kh2_collision(settings.root, map, &floors, &unused))
        return true;  // a map without floors and walls of its own
    if (!import_model(floors, collision, &unused))
        *collision = assets::ImportedModel{};
    return true;
}

}  // namespace studio
