// The Map workshop tab (core/workshop/workshop.h): KH2 maps of the player's collection side by side, placed,
// their triangles picked and deleted, the project saved anywhere and written out as one glTF.
#include "../app.h"
#include "../preview3d.h"

#include "core/files/paths.h"
#include "core/models/bones.h"
#include "core/rooms/kh2_source.h"
#include "core/workshop/workshop.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace studio {

namespace {

namespace fs = std::filesystem;
namespace assets = khdays::assets;
using V3 = std::array<float, 3>;

// A map read once and shared by every place it is put (a map can be put twice).
struct Geometry {
    std::array<assets::ImportedModel, kWorkLayers> layer;
    std::array<std::vector<V3>, kWorkLayers> rest;            // 3 corners a triangle, as the file poses it
    std::array<std::vector<int>, kWorkLayers> pieces;         // a triangle's piece
    std::array<std::vector<int>, kWorkLayers> mesh_of;        // a triangle's mesh
    std::array<std::vector<std::size_t>, kWorkLayers> first;  // a mesh's first triangle
    // the model's textures as drawn: opaque materials all opaque, a mask's texels whole or gone (the port's)
    std::map<std::string, std::vector<std::uint8_t>> shaped;
};

// A map put in the project: its geometry, where its triangles are now, what of them is picked.
struct Placed {
    std::shared_ptr<Geometry> geometry;
    RoomPlacement placed_with{-1.0f, {0, 0, 0}, {0, 0, 0}};  // what `world` was made for
    std::array<std::vector<V3>, kWorkLayers> world;
    std::array<std::vector<std::uint8_t>, kWorkLayers> selected;
};

enum Tool { kPiece, kTriangle, kBox, kTexture };
enum PickIn { kInModel, kInCollision, kInBoth };

struct Workshop {
    WorkshopProject project;
    std::vector<Placed> placed;  // the project's maps' order
    std::map<std::string, std::shared_ptr<Geometry>> geometries;
    bool dirty = false;
    std::string project_file;
    std::string export_folder, export_name = "open_world";
    std::string chosen_map;  // the collection's map picked in the tree, for Add
    int current = -1;        // the map whose place is shown and moved
    // the camera
    V3 target{0, 0, 0};
    float yaw = 30.0f, pitch = 25.0f, distance = 6000.0f;
    bool framed = false;
    // the tools
    int tool = kPiece, pick_in = kInModel;
    bool pick_deleted = false;  // the tools pick the deleted (faint) triangles, to restore them
    bool show_models = true, show_collision = false, show_deleted = false, show_skies = true;
    bool box_on = false;
    ImVec2 box_from{0, 0};
    std::vector<WorkshopProject> undo, redo;
    bool place_dragging = false;
    std::unique_ptr<Preview> preview;
    // the maps being read, and what the dialogs chose, left by their threads
    std::mutex mutex;
    std::deque<std::string> to_load;
    std::thread loader;
    std::atomic<bool> loading{false};
    std::string loading_map;
    std::map<std::string, std::shared_ptr<Geometry>> loaded;
    std::map<std::string, std::string> failed;
    std::vector<std::string> chosen_save, chosen_open, chosen_folder;
    // the collection's maps by world, listed on their own thread the first time a world is opened in the tree
    std::string collection_for = "\x01";
    fs::path collection;
    std::deque<std::string> to_list;
    std::thread lister;
    std::atomic<bool> listing{false};
    std::string listing_world;
    std::map<std::string, std::vector<std::string>> world_maps;
    std::map<std::string, std::string> list_failed;
    std::thread exporter;
    std::atomic<bool> exporting{false};
    bool exported = false, export_ok = false;
    std::string export_message;
    ~Workshop() {
        if (loader.joinable())
            loader.join();
        if (lister.joinable())
            lister.join();
        if (exporter.joinable())
            exporter.join();
    }
};
Workshop w;

V3 sub(const V3& a, const V3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(const V3& a, const V3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// A geometry made ready on the loader's thread: the files read, each triangle's corners, piece and mesh.
std::shared_ptr<Geometry> read_geometry(const Settings& settings, const std::string& map, std::string* why) {
    auto g = std::make_shared<Geometry>();
    if (!prepare_workshop_map(settings, map, &g->layer[kWorkModel], &g->layer[kWorkCollision], why))
        return nullptr;
    for (int l = 0; l < kWorkLayers; ++l) {
        const assets::ImportedModel& model = g->layer[static_cast<std::size_t>(l)];
        auto& rest = g->rest[static_cast<std::size_t>(l)];
        for (std::size_t mi = 0; mi < model.model.meshes.size(); ++mi) {
            const auto& mesh = model.model.meshes[mi];
            g->first[static_cast<std::size_t>(l)].push_back(rest.size() / 3);
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                for (std::size_t c = 0; c < 3; ++c) {
                    // posed as the file poses it: its skin's palette at rest (entry 0 the still meshes' identity)
                    const auto& v = mesh.vertices[mesh.indices[i + c]];
                    V3 p{0, 0, 0};
                    float total = 0;
                    for (std::size_t k = 0; k < 4; ++k) {
                        if (v.weights[k] <= 0 || v.joints[k] >= model.model.palette.size())
                            continue;
                        const auto q = mat_point(model.model.palette[v.joints[k]], v.position);
                        for (std::size_t a = 0; a < 3; ++a)
                            p[a] += v.weights[k] * q[a];
                        total += v.weights[k];
                    }
                    rest.push_back(total > 0 ? V3{p[0] / total, p[1] / total, p[2] / total} : v.position);
                }
                g->mesh_of[static_cast<std::size_t>(l)].push_back(static_cast<int>(mi));
            }
        }
        g->pieces[static_cast<std::size_t>(l)] = connected_pieces(model);
    }
    for (const auto& mesh : g->layer[kWorkModel].model.meshes) {
        const bool opaque = mesh.alpha_mode == assets::NeutralMesh::AlphaMode::opaque;
        const bool mask = mesh.alpha_mode == assets::NeutralMesh::AlphaMode::mask;
        if (!opaque && !mask)
            continue;
        const std::string key = mesh.texture_name + (opaque ? "@opaque" : "@mask" + std::to_string(mesh.alpha_cutoff));
        if (g->shaped.count(key) != 0)
            continue;
        for (const auto& [name, decoded] : g->layer[kWorkModel].textures) {
            if (name != mesh.texture_name || decoded.rgba.empty())
                continue;
            std::vector<std::uint8_t> px = decoded.rgba;
            const auto least =
                static_cast<std::uint8_t>(std::fmin(255.0f, std::fmax(0.0f, std::ceil(mesh.alpha_cutoff * 255.0f))));
            for (std::size_t i = 3; i < px.size(); i += 4)
                px[i] = opaque || px[i] >= least ? 255 : 0;
            g->shaped.emplace(key, std::move(px));
        }
    }
    return g;
}

void start_loader(const Settings& settings) {
    if (w.loading || w.to_load.empty())
        return;
    if (w.loader.joinable())
        w.loader.join();
    w.loading_map = w.to_load.front();
    w.to_load.pop_front();
    w.loading = true;
    w.loader = std::thread([settings, map = w.loading_map] {
        std::string why;
        auto g = read_geometry(settings, map, &why);
        std::lock_guard lock(w.mutex);
        if (g != nullptr)
            w.loaded[map] = std::move(g);
        else
            w.failed[map] = why;
        w.loading = false;
    });
}

// The project's maps given their geometry (asked for when not read yet) and their places' lists sized.
void sync_placed(const Settings& settings) {
    {
        std::lock_guard lock(w.mutex);
        for (auto& [map, g] : w.loaded)
            w.geometries[map] = std::move(g);
        w.loaded.clear();
    }
    w.placed.resize(w.project.maps.size());
    for (std::size_t i = 0; i < w.project.maps.size(); ++i) {
        WorkshopMap& m = w.project.maps[i];
        Placed& p = w.placed[i];
        const auto g = w.geometries.find(m.map);
        if (g == w.geometries.end()) {
            p.geometry = nullptr;
            const bool queued = std::find(w.to_load.begin(), w.to_load.end(), m.map) != w.to_load.end();
            std::lock_guard lock(w.mutex);
            if (!queued && w.loading_map != m.map && w.failed.count(m.map) == 0)
                w.to_load.push_back(m.map);
            continue;
        }
        if (p.geometry != g->second) {
            p.geometry = g->second;
            p.placed_with.scale = -1.0f;
        }
        for (int l = 0; l < kWorkLayers; ++l) {
            const std::size_t n = p.geometry->rest[static_cast<std::size_t>(l)].size() / 3;
            if (m.deleted[static_cast<std::size_t>(l)].size() < n)
                m.deleted[static_cast<std::size_t>(l)].resize(n, 0);
            p.selected[static_cast<std::size_t>(l)].resize(n, 0);
        }
        const RoomPlacement& now = m.place;
        if (p.placed_with.scale != now.scale || p.placed_with.turn != now.turn || p.placed_with.move != now.move) {
            for (int l = 0; l < kWorkLayers; ++l) {
                const auto& rest = p.geometry->rest[static_cast<std::size_t>(l)];
                auto& world = p.world[static_cast<std::size_t>(l)];
                world.resize(rest.size());
                for (std::size_t k = 0; k < rest.size(); ++k)
                    world[k] = placed_point(now, rest[k]);
            }
            p.placed_with = now;
        }
    }
    start_loader(settings);
}

// The next world asked for in the tree listed: its maps in the player's collection.
void start_lister(const Settings& settings) {
    if (w.listing || w.to_list.empty())
        return;
    if (w.lister.joinable())
        w.lister.join();
    w.listing_world = w.to_list.front();
    w.to_list.pop_front();
    w.listing = true;
    w.lister = std::thread([settings, world = w.listing_world] {
        std::vector<std::string> maps;
        std::string why;
        const bool ok = kh2_world_maps(settings, world, &maps, &why);
        std::lock_guard lock(w.mutex);
        if (ok)
            w.world_maps[world] = std::move(maps);
        else
            w.list_failed[world] = why.empty() ? std::string("?") : why;
        w.listing = false;
    });
}

void remember() {
    w.undo.push_back(w.project);
    if (w.undo.size() > 100)
        w.undo.erase(w.undo.begin());
    w.redo.clear();
    w.dirty = true;
}

void step_history(bool forward) {
    auto& from = forward ? w.redo : w.undo;
    auto& to = forward ? w.undo : w.redo;
    if (from.empty())
        return;
    to.push_back(w.project);
    w.project = std::move(from.back());
    from.pop_back();
    for (auto& p : w.placed)
        for (auto& s : p.selected)
            std::fill(s.begin(), s.end(), std::uint8_t{0});
    w.dirty = true;
}

std::size_t count_marked(const std::vector<std::uint8_t>& marks) {
    return static_cast<std::size_t>(std::count_if(marks.begin(), marks.end(), [](std::uint8_t v) { return v != 0; }));
}

std::size_t selected_total() {
    std::size_t n = 0;
    for (const auto& p : w.placed)
        for (const auto& s : p.selected)
            n += count_marked(s);
    return n;
}

void clear_selection() {
    for (auto& p : w.placed)
        for (auto& s : p.selected)
            std::fill(s.begin(), s.end(), std::uint8_t{0});
}

// The selected triangles deleted (or with `restore`, brought back).
void delete_selected(bool restore) {
    if (selected_total() == 0)
        return;
    remember();
    for (std::size_t i = 0; i < w.placed.size() && i < w.project.maps.size(); ++i) {
        for (int l = 0; l < kWorkLayers; ++l) {
            auto& sel = w.placed[i].selected[static_cast<std::size_t>(l)];
            auto& del = w.project.maps[i].deleted[static_cast<std::size_t>(l)];
            for (std::size_t t = 0; t < sel.size() && t < del.size(); ++t) {
                if (sel[t] != 0)
                    del[t] = restore ? 0 : 1;
            }
        }
    }
    clear_selection();
}

bool layer_pickable(int l) {
    if (w.pick_in == kInBoth)
        return l == kWorkModel ? w.show_models : w.show_collision;
    return w.pick_in == (l == kWorkModel ? kInModel : kInCollision);
}

// Whether the tools may pick `t` of map `map`'s layer `l`: a kept triangle, or with "Pick from: Deleted" only a
// deleted one (drawn faint then, whatever the "Deleted" box says); skies only while shown.
bool triangle_pickable(std::size_t map, int l, std::size_t t) {
    const auto& del = w.project.maps[map].deleted[static_cast<std::size_t>(l)];
    const bool deleted = t < del.size() && del[t] != 0;
    if (deleted != w.pick_deleted)
        return false;
    if (l == kWorkModel && !w.show_skies) {
        const Geometry& g = *w.placed[map].geometry;
        if (g.layer[kWorkModel].model.meshes[static_cast<std::size_t>(g.mesh_of[kWorkModel][t])].sky)
            return false;
    }
    return true;
}

// `t` of map `map`'s layer `l` picked by the tool: itself, its piece or its texture's triangles; `mode` 0 sets,
// 1 adds, 2 takes away.
void pick(std::size_t map, int l, std::size_t t, int mode) {
    if (mode == 0)
        clear_selection();
    Placed& p = w.placed[map];
    const Geometry& g = *p.geometry;
    auto& sel = p.selected[static_cast<std::size_t>(l)];
    const std::uint8_t value = mode == 2 ? 0 : 1;
    const auto& pieces = g.pieces[static_cast<std::size_t>(l)];
    const auto& meshes = g.mesh_of[static_cast<std::size_t>(l)];
    const auto& model = g.layer[static_cast<std::size_t>(l)].model;
    const auto& mesh = model.meshes[static_cast<std::size_t>(meshes[t])];
    for (std::size_t k = 0; k < sel.size(); ++k) {
        bool take = k == t;
        if (w.tool == kPiece)
            take = pieces[k] == pieces[t];
        else if (w.tool == kTexture) {
            const auto& other = model.meshes[static_cast<std::size_t>(meshes[k])];
            take =
                l == kWorkModel ? other.texture_name == mesh.texture_name : other.material_name == mesh.material_name;
        }
        if (take && triangle_pickable(map, l, k))
            sel[k] = value;
    }
}

std::string place_label(const std::string& map) {
    const char* name = kh2_place_name(map);
    return name != nullptr && name[0] != '\0' ? map + " - " + name : map;
}

// The dialogs, on their own threads: what was chosen kept for the next frame.
void ask_save_file(App& app) {
    static const SDL_DialogFileFilter filters[] = {{"Map workshop project", "khmap"}};
    SDL_ShowSaveFileDialog(
        [](void*, const char* const* files, int) {
            if (files == nullptr || files[0] == nullptr)
                return;
            std::lock_guard lock(w.mutex);
            w.chosen_save.emplace_back(files[0]);
        },
        nullptr, app.window, filters, 1, w.project_file.empty() ? nullptr : w.project_file.c_str());
}

void ask_open_file(App& app) {
    static const SDL_DialogFileFilter filters[] = {{"Map workshop project", "khmap"}};
    SDL_ShowOpenFileDialog(
        [](void*, const char* const* files, int) {
            if (files == nullptr || files[0] == nullptr)
                return;
            std::lock_guard lock(w.mutex);
            w.chosen_open.emplace_back(files[0]);
        },
        nullptr, app.window, filters, 1, nullptr, false);
}

void ask_folder(App& app) {
    SDL_ShowOpenFolderDialog(
        [](void*, const char* const* files, int) {
            if (files == nullptr || files[0] == nullptr)
                return;
            std::lock_guard lock(w.mutex);
            w.chosen_folder.emplace_back(files[0]);
        },
        nullptr, app.window, w.export_folder.empty() ? nullptr : w.export_folder.c_str(), false);
}

void save_to(App& app, const std::string& file) {
    std::string why;
    std::string path = file;
    if (u8(path_u8(path).extension()).empty())
        path += ".khmap";
    if (save_workshop(path_u8(path), w.project, &why)) {
        w.project_file = path;
        w.dirty = false;
        app.tell(fmt(app.language, S::ws_saved, {path}), true);
    } else {
        app.tell(why, false);
    }
}

void open_from(App& app, const std::string& file) {
    WorkshopProject p;
    std::string why;
    if (!load_workshop(path_u8(file), &p, &why)) {
        app.tell(why, false);
        return;
    }
    w.project = std::move(p);
    w.placed.clear();
    w.undo.clear();
    w.redo.clear();
    w.project_file = file;
    w.dirty = false;
    w.current = w.project.maps.empty() ? -1 : 0;
    w.framed = false;
    std::lock_guard lock(w.mutex);
    w.failed.clear();
}

void take_chosen(App& app) {
    std::vector<std::string> save, open, folder;
    {
        std::lock_guard lock(w.mutex);
        save.swap(w.chosen_save);
        open.swap(w.chosen_open);
        folder.swap(w.chosen_folder);
    }
    if (!save.empty())
        save_to(app, save.back());
    if (!open.empty())
        open_from(app, open.back());
    if (!folder.empty())
        w.export_folder = folder.back();
}

void start_export(App& app) {
    if (w.exporting)
        return;
    if (w.exporter.joinable())
        w.exporter.join();
    std::vector<std::shared_ptr<Geometry>> keep;
    for (const auto& p : w.placed)
        keep.push_back(p.geometry);
    w.exporting = true;
    w.exported = false;
    w.exporter =
        std::thread([project = w.project, keep, folder = w.export_folder, name = w.export_name, lang = app.language] {
            std::vector<const assets::ImportedModel*> models, collisions;
            for (const auto& g : keep) {
                models.push_back(g != nullptr ? &g->layer[kWorkModel] : nullptr);
                collisions.push_back(g != nullptr && !g->layer[kWorkCollision].model.meshes.empty()
                                         ? &g->layer[kWorkCollision]
                                         : nullptr);
            }
            std::vector<std::string> written;
            std::string why;
            const bool ok = export_workshop(project, models, collisions, path_u8(folder), name, &written, &why);
            std::string said;
            for (const auto& f : written)
                said += (said.empty() ? "" : ", ") + f;
            std::lock_guard lock(w.mutex);
            w.export_ok = ok;
            w.export_message = ok ? fmt(lang, S::ws_exported, {said}) : why;
            w.exported = true;
            w.exporting = false;
        });
}

void add_map(const std::string& map) {
    remember();
    WorkshopMap m;
    m.map = map;
    w.project.maps.push_back(m);
    w.current = static_cast<int>(w.project.maps.size()) - 1;
    std::lock_guard lock(w.mutex);
    w.failed.erase(map);
}

bool in_project(const std::string& map) {
    return std::any_of(w.project.maps.begin(), w.project.maps.end(),
                       [&](const WorkshopMap& m) { return m.map == map; });
}

// KH2's worlds of the player's collection and their maps as a tree, named as the catalog names them (the game's
// world names, KH2's place names): a map picked, then put in the project with Add or a double click.
void draw_library(App& app) {
    const auto lang = app.language;
    ImGui::SeparatorText(tr(lang, S::ws_library));
    if (w.collection_for != app.settings.collection_1525) {
        w.collection = kh2_collection(app.settings);
        w.collection_for = app.settings.collection_1525;
    }
    if (w.collection.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.8f, 0.35f, 1));
        ImGui::TextWrapped("%s", tr(lang, S::map_kh2_no_collection));
        ImGui::PopStyleColor();
        return;
    }
    ImGui::BeginChild("##ws_library", ImVec2(0, 10 * ImGui::GetTextLineHeightWithSpacing()),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY, ImGuiWindowFlags_HorizontalScrollbar);
    for (int k = 0; k < kKh2WorldCount; ++k) {
        const std::string world = kKh2Worlds[k];
        const std::string name = world_folder_label(app, "wd_" + world);
        const std::string label = world + (name.empty() ? std::string() : " - " + name) + "###" + world;
        if (!ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
            continue;
        std::vector<std::string> maps;
        std::string failed;
        bool listed = false, busy = false;
        {
            std::lock_guard lock(w.mutex);
            if (const auto f = w.world_maps.find(world); f != w.world_maps.end()) {
                maps = f->second;
                listed = true;
            } else if (const auto e = w.list_failed.find(world); e != w.list_failed.end()) {
                failed = e->second;
            }
            busy = w.listing && w.listing_world == world;
        }
        if (!failed.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1));
            ImGui::TextWrapped("%s", failed.c_str());
            ImGui::PopStyleColor();
        } else if (!listed) {
            if (!busy && std::find(w.to_list.begin(), w.to_list.end(), world) == w.to_list.end())
                w.to_list.push_back(world);
            ImGui::TextDisabled("%s", tr(lang, S::ws_listing));
        } else if (maps.empty()) {
            ImGui::TextDisabled("%s", tr(lang, S::ws_world_none));
        }
        for (const auto& map : maps) {
            const bool added = in_project(map);
            ImGui::PushID(map.c_str());
            ImGui::TreeNodeEx(
                "##map",
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                    (w.chosen_map == map ? ImGuiTreeNodeFlags_Selected : 0),
                "%s%s%s", place_label(map).c_str(), added ? "  " : "", added ? tr(lang, S::ws_in_project) : "");
            if (ImGui::IsItemClicked()) {
                w.chosen_map = map;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    add_map(map);
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(w.chosen_map.empty());
    if (ImGui::Button(tr(lang, S::ws_add)))
        add_map(w.chosen_map);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr(lang, S::ws_add_hint));
    if (!w.chosen_map.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", place_label(w.chosen_map).c_str());
    }
}

// Map `from` moved to stand before the map now at `to` (the list's size: last), as a step to undo; the picked
// map stays picked.
void move_map(int from, int to) {
    const int count = static_cast<int>(w.project.maps.size());
    if (from < 0 || from >= count || to < 0 || to > count)
        return;
    const int dest = to > from ? to - 1 : to;
    if (dest == from)
        return;
    remember();
    auto shift = [&](auto& list) {
        if (static_cast<int>(list.size()) != count)
            return;
        auto item = std::move(list[static_cast<std::size_t>(from)]);
        list.erase(list.begin() + from);
        list.insert(list.begin() + dest, std::move(item));
    };
    shift(w.placed);
    shift(w.project.maps);
    if (w.current == from)
        w.current = dest;
    else if (from < w.current && dest >= w.current)
        --w.current;
    else if (from > w.current && dest <= w.current)
        ++w.current;
}

// The project's maps as layers: a click on a row picks the map the fields under the list move, its box shows or
// hides it. The list keeps its height (the modder drags its bottom edge), so the fields stay where they are.
void draw_layers(App& app) {
    const auto lang = app.language;
    ImGui::SeparatorText(tr(lang, S::ws_maps));
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row = 2 * ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    ImGui::BeginChild("##ws_layers", ImVec2(0, 4 * (row + style.ItemSpacing.y) + 2 * style.WindowPadding.y),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY, ImGuiWindowFlags_HorizontalScrollbar);
    int remove = -1, move_from = -1, move_to = -1;
    for (std::size_t i = 0; i < w.project.maps.size(); ++i) {
        WorkshopMap& m = w.project.maps[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float full = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##layer", w.current == static_cast<int>(i), ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(full, row)))
            w.current = static_cast<int>(i);
        if (ImGui::IsItemHovered() && ImGui::GetDragDropPayload() == nullptr)
            ImGui::SetTooltip("%s", tr(lang, S::ws_layers_hint));
        // dragged up or down to reorder: a line shows where it lands
        if (ImGui::BeginDragDropSource()) {
            const int from = static_cast<int>(i);
            ImGui::SetDragDropPayload("ws_layer", &from, sizeof(from));
            ImGui::TextUnformatted(place_label(m.map).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    "ws_layer", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                const bool below = ImGui::GetIO().MousePos.y > start.y + 0.5f * row;
                const float y =
                    below ? start.y + row + 0.5f * style.ItemSpacing.y : start.y - 0.5f * style.ItemSpacing.y;
                ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x, y), ImVec2(start.x + full, y),
                                                    ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
                if (payload->IsDelivery()) {
                    move_from = *static_cast<const int*>(payload->Data);
                    move_to = static_cast<int>(i) + (below ? 1 : 0);
                }
            }
            ImGui::EndDragDropTarget();
        }
        const ImVec2 next = ImGui::GetCursorScreenPos();
        // drawn over the row: its box, its name and what it holds, its Remove at the right
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 0.5f * (row - ImGui::GetFrameHeight())));
        bool shown = m.shown;
        if (ImGui::Checkbox("##shown", &shown)) {
            remember();
            m.shown = shown;
        }
        ImGui::SameLine();
        const float text_x = ImGui::GetCursorScreenPos().x;
        ImGui::SetCursorScreenPos(ImVec2(text_x, start.y));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(place_label(m.map).c_str());
        const Placed* p = i < w.placed.size() ? &w.placed[i] : nullptr;
        if (p != nullptr && p->geometry != nullptr) {
            const auto& g = *p->geometry;
            ImGui::TextDisabled("%s", fmt(lang, S::ws_triangles,
                                          {std::to_string(g.rest[kWorkModel].size() / 3),
                                           std::to_string(count_marked(m.deleted[kWorkModel])),
                                           std::to_string(g.rest[kWorkCollision].size() / 3),
                                           std::to_string(count_marked(m.deleted[kWorkCollision]))})
                                          .c_str());
        } else {
            std::lock_guard lock(w.mutex);
            const auto f = w.failed.find(m.map);
            if (f != w.failed.end())
                ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1), "%s", f->second.c_str());
            else
                ImGui::TextDisabled("%s", fmt(lang, S::ws_loading, {m.map}).c_str());
        }
        ImGui::EndGroup();
        const char* remove_word = tr(lang, S::ws_remove);
        const float remove_w = ImGui::CalcTextSize(remove_word).x + 2 * style.FramePadding.x;
        ImGui::SetCursorScreenPos(ImVec2(std::max(text_x, start.x + full - remove_w), start.y));
        if (ImGui::SmallButton(remove_word))
            remove = static_cast<int>(i);
        ImGui::SetCursorScreenPos(next);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();
    if (move_from >= 0)
        move_map(move_from, move_to);
    if (remove >= 0) {
        remember();
        w.project.maps.erase(w.project.maps.begin() + remove);
        if (static_cast<std::size_t>(remove) < w.placed.size())
            w.placed.erase(w.placed.begin() + remove);
        // the picked map kept picked when another goes
        if (remove < w.current)
            --w.current;
        w.current = std::min(w.current, static_cast<int>(w.project.maps.size()) - 1);
    }
}

// --- The panel at the left ------------------------------------------------------------------------------------

void draw_panel(App& app) {
    const auto lang = app.language;
    // the project
    ImGui::SeparatorText(tr(lang, S::ws_project));
    ImGui::TextDisabled("%s%s", w.project_file.empty() ? "-" : w.project_file.c_str(),
                        w.dirty ? (std::string("  ") + tr(lang, S::ws_unsaved)).c_str() : "");
    if (ImGui::Button(tr(lang, S::ws_new))) {
        remember();
        w.project = WorkshopProject{};
        w.placed.clear();
        w.project_file.clear();
        w.current = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(lang, S::ws_open)))
        ask_open_file(app);
    ImGui::SameLine();
    if (ImGui::Button(tr(lang, S::ws_save))) {
        if (w.project_file.empty())
            ask_save_file(app);
        else
            save_to(app, w.project_file);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(lang, S::ws_save_as)))
        ask_save_file(app);

    draw_library(app);
    draw_layers(app);

    // where the map picked in the list sits: the fields always here, moving whichever map is picked
    const bool has = w.current >= 0 && static_cast<std::size_t>(w.current) < w.project.maps.size();
    {
        static WorkshopMap none;  // what the fields show while no map is picked
        WorkshopMap& m = has ? w.project.maps[static_cast<std::size_t>(w.current)] : none;
        ImGui::SeparatorText(has ? (std::string(tr(lang, S::ws_place)) + ": " + place_label(m.map)).c_str()
                                 : tr(lang, S::ws_place));
        ImGui::BeginDisabled(!has);
        RoomPlacement place = m.place;
        bool changed = false, started = false, ended = false;
        auto watch = [&](bool c) {
            changed = changed || c;
            started = started || ImGui::IsItemActivated();
            ended = ended || ImGui::IsItemDeactivatedAfterEdit();
        };
        watch(ImGui::DragFloat3(tr(lang, S::ws_move), place.move.data(), 5.0f, 0, 0, "%.1f"));
        watch(ImGui::DragFloat(tr(lang, S::ws_turn), &place.turn[1], 0.5f, -360.0f, 360.0f, "%.1f"));
        watch(ImGui::DragFloat(tr(lang, S::ws_scale), &place.scale, 0.005f, 0.01f, 100.0f, "%.3f"));
        if (started && !w.place_dragging) {
            remember();
            w.place_dragging = true;
        }
        if (changed && has)
            m.place = place;
        if (ended || (!ImGui::IsAnyItemActive() && w.place_dragging))
            w.place_dragging = false;
        if (ImGui::Button(tr(lang, S::ws_look)) && has && static_cast<std::size_t>(w.current) < w.placed.size() &&
            w.placed[static_cast<std::size_t>(w.current)].geometry != nullptr) {
            // the middle of its still model, but its skies
            const Placed& p = w.placed[static_cast<std::size_t>(w.current)];
            V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            const auto& g = *p.geometry;
            for (std::size_t t = 0; t < g.mesh_of[kWorkModel].size(); ++t) {
                if (g.layer[kWorkModel].model.meshes[static_cast<std::size_t>(g.mesh_of[kWorkModel][t])].sky)
                    continue;
                for (std::size_t c = 0; c < 3; ++c) {
                    const V3& q = p.world[kWorkModel][t * 3 + c];
                    if (std::fabs(q[1]) > 50000.0f)
                        continue;  // a piece waiting far down for its event
                    for (std::size_t a = 0; a < 3; ++a)
                        lo[a] = std::min(lo[a], q[a]), hi[a] = std::max(hi[a], q[a]);
                }
            }
            if (lo[0] <= hi[0]) {
                w.target = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
                w.distance = std::max(500.0f, std::max(hi[0] - lo[0], hi[2] - lo[2]));
            }
        }
        ImGui::EndDisabled();
        if (!has)
            ImGui::TextDisabled("%s", tr(lang, S::ws_place_none));
    }

    // picking and deleting
    ImGui::SeparatorText(tr(lang, S::ws_tools));
    const S tools[4] = {S::ws_tool_piece, S::ws_tool_triangle, S::ws_tool_box, S::ws_tool_texture};
    for (int i = 0; i < 4; ++i) {
        if (i > 0)
            ImGui::SameLine();
        if (ImGui::RadioButton(tr(lang, tools[i]), w.tool == i))
            w.tool = i;
    }
    ImGui::TextUnformatted(tr(lang, S::ws_layer));
    const S layers[3] = {S::ws_layer_model, S::ws_layer_collision, S::ws_layer_both};
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine();
        if (ImGui::RadioButton(tr(lang, layers[i]), w.pick_in == i)) {
            w.pick_in = i;
            if (i != kInModel)
                w.show_collision = true;
        }
    }
    ImGui::TextUnformatted(tr(lang, S::ws_from));
    const S froms[2] = {S::ws_from_kept, S::ws_from_deleted};
    for (int i = 0; i < 2; ++i) {
        ImGui::SameLine();
        if (ImGui::RadioButton(tr(lang, froms[i]), w.pick_deleted == (i == 1)) && w.pick_deleted != (i == 1)) {
            w.pick_deleted = i == 1;
            clear_selection();  // what was picked is of the other kind
        }
    }
    ImGui::TextWrapped("%s", tr(lang, S::ws_tools_hint));
    const std::size_t picked = selected_total();
    ImGui::TextUnformatted(fmt(lang, S::ws_selected, {std::to_string(picked)}).c_str());
    ImGui::BeginDisabled(picked == 0 || w.pick_deleted);
    if (ImGui::Button(tr(lang, S::ws_delete)))
        delete_selected(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(picked == 0 || !w.pick_deleted);
    if (ImGui::Button(tr(lang, S::ws_restore)))
        delete_selected(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(picked == 0);
    if (ImGui::Button(tr(lang, S::ws_clear)))
        clear_selection();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(w.undo.empty());
    if (ImGui::Button(tr(lang, S::map_edit_undo)))
        step_history(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(w.redo.empty());
    if (ImGui::Button(tr(lang, S::ws_redo)))
        step_history(true);
    ImGui::EndDisabled();

    // writing it out
    ImGui::SeparatorText(tr(lang, S::ws_export));
    ImGui::SetNextItemWidth(-6 * ImGui::GetFontSize());
    ImGui::InputText(tr(lang, S::ws_export_folder), &w.export_folder);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr(lang, S::ws_browse)))
        ask_folder(app);
    ImGui::SetNextItemWidth(-6 * ImGui::GetFontSize());
    ImGui::InputText(tr(lang, S::ws_export_name), &w.export_name);
    ImGui::BeginDisabled(w.exporting || w.export_folder.empty() || w.project.maps.empty());
    if (ImGui::Button(w.exporting ? tr(lang, S::ws_exporting) : tr(lang, S::ws_export)))
        start_export(app);
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", tr(lang, S::ws_export_hint));
}

// --- The picture ----------------------------------------------------------------------------------------------

void draw_picture(App& app, ImVec2 size) {
    if (!w.preview)
        w.preview = std::make_unique<Preview>(app.device);
    Preview& p = *w.preview;
    p.collect();
    const int width = std::max(64, static_cast<int>(size.x)), height = std::max(64, static_cast<int>(size.y));
    const float fov = app.settings.map_fov;
    const Mat vp = orbit_view_projection(w.target, w.yaw, w.pitch, w.distance,
                                         static_cast<float>(width) / static_cast<float>(height), fov);
    p.begin(width, height, vp, orbit_eye_direction(w.yaw, w.pitch));
    const std::array<float, 3> plain{1, 1, 1}, picked{1.0f, 0.35f, 0.3f};
    for (std::size_t i = 0; i < w.placed.size() && i < w.project.maps.size(); ++i) {
        const Placed& pl = w.placed[i];
        const WorkshopMap& m = w.project.maps[i];
        if (!m.shown || pl.geometry == nullptr)
            continue;
        const Geometry& g = *pl.geometry;
        if (w.show_models) {
            const auto& model = g.layer[kWorkModel];
            for (std::size_t mi = 0; mi < model.model.meshes.size(); ++mi) {
                const auto& mesh = model.model.meshes[mi];
                if (mesh.sky && !w.show_skies)
                    continue;
                const assets::DecodedTexture* t = nullptr;
                for (const auto& [name, decoded] : model.textures)
                    if (name == mesh.texture_name && !decoded.rgba.empty())
                        t = &decoded;
                const bool opaque = mesh.alpha_mode == assets::NeutralMesh::AlphaMode::opaque;
                PreviewImage image;
                if (t != nullptr) {
                    const auto s = g.shaped.find(mesh.texture_name +
                                                 (opaque ? "@opaque" : "@mask" + std::to_string(mesh.alpha_cutoff)));
                    image = {s != g.shaped.end() ? s->second.data() : t->rgba.data(), t->width, t->height, mesh.repeat,
                             mesh.mirror};
                }
                const float tw = t != nullptr ? static_cast<float>(t->width) : 1.0f;
                const float th = t != nullptr ? static_cast<float>(t->height) : 1.0f;
                const std::size_t first = g.first[kWorkModel][mi];
                for (std::size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
                    const std::size_t tri = first + k / 3;
                    const bool deleted = m.deleted[kWorkModel][tri] != 0;
                    const bool chosen = pl.selected[kWorkModel][tri] != 0;
                    if (deleted && !w.show_deleted && !w.pick_deleted)
                        continue;
                    PreviewVertex c[3];
                    for (std::size_t n = 0; n < 3; ++n) {
                        const auto& v = mesh.vertices[mesh.indices[k + n]];
                        c[n].position = pl.world[kWorkModel][tri * 3 + n];
                        c[n].uv = {v.texcoord[0] / tw, v.texcoord[1] / th};
                        c[n].colour = {v.color[0] / 255.0f, v.color[1] / 255.0f, v.color[2] / 255.0f,
                                       deleted  ? (chosen ? 0.65f : 0.2f)
                                       : opaque ? 1.0f
                                                : v.color[3] / 255.0f};
                    }
                    p.triangle(c[0], c[1], c[2], image, !mesh.double_sided, chosen ? picked : plain);
                }
            }
        }
        if (w.show_collision) {
            const auto& world = pl.world[kWorkCollision];
            const float alpha = w.show_models ? 0.55f : 1.0f;
            for (std::size_t tri = 0; tri * 3 + 2 < world.size(); ++tri) {
                const bool deleted = m.deleted[kWorkCollision][tri] != 0;
                const bool chosen = pl.selected[kWorkCollision][tri] != 0;
                if (deleted && !w.show_deleted && !w.pick_deleted)
                    continue;
                const V3 n = cross(sub(world[tri * 3 + 1], world[tri * 3]), sub(world[tri * 3 + 2], world[tri * 3]));
                const float len = std::sqrt(dot(n, n));
                const float up = len > 0 ? n[1] / len : 0;
                // as the Maps view colours them: grounds green, walls orange, ceilings blue
                std::array<float, 4> colour = up > 0.5f    ? std::array<float, 4>{0.35f, 0.85f, 0.4f, alpha}
                                              : up < -0.5f ? std::array<float, 4>{0.35f, 0.55f, 0.95f, alpha}
                                                           : std::array<float, 4>{0.95f, 0.6f, 0.25f, alpha};
                if (deleted)
                    colour[3] = chosen ? 0.6f : 0.15f;
                PreviewVertex c[3];
                for (std::size_t k = 0; k < 3; ++k) {
                    c[k].position = world[tri * 3 + k];
                    c[k].colour = colour;
                }
                p.triangle(c[0], c[1], c[2], PreviewImage{}, false, chosen ? picked : plain);
            }
        }
    }
    SDL_GPUTexture* tex = p.finish();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (tex != nullptr)
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(tex))),
                     ImVec2(static_cast<float>(width), static_cast<float>(height)));
    else
        ImGui::Dummy(ImVec2(static_cast<float>(width), static_cast<float>(height)));
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const int mode = io.KeyShift ? 1 : io.KeyCtrl ? 2 : 0;
    ImDrawList* list = ImGui::GetWindowDrawList();

    // a box dragged: what lies wholly in it picked
    if (w.tool == kBox && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        w.box_on = true;
        w.box_from = io.MousePos;
    }
    if (w.box_on) {
        const ImVec2 a(std::min(w.box_from.x, io.MousePos.x), std::min(w.box_from.y, io.MousePos.y));
        const ImVec2 b(std::max(w.box_from.x, io.MousePos.x), std::max(w.box_from.y, io.MousePos.y));
        list->AddRectFilled(a, b, IM_COL32(255, 120, 90, 40));
        list->AddRect(a, b, IM_COL32(255, 120, 90, 220));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            w.box_on = false;
            if (mode == 0)
                clear_selection();
            for (std::size_t i = 0; i < w.placed.size() && i < w.project.maps.size(); ++i) {
                Placed& pl = w.placed[i];
                if (!w.project.maps[i].shown || pl.geometry == nullptr)
                    continue;
                for (int l = 0; l < kWorkLayers; ++l) {
                    if (!layer_pickable(l))
                        continue;
                    const auto& world = pl.world[static_cast<std::size_t>(l)];
                    auto& sel = pl.selected[static_cast<std::size_t>(l)];
                    for (std::size_t t = 0; t * 3 + 2 < world.size(); ++t) {
                        if (!triangle_pickable(i, l, t))
                            continue;
                        bool inside = true;
                        for (std::size_t c = 0; c < 3 && inside; ++c) {
                            float x = 0, y = 0;
                            inside = p.project(world[t * 3 + c], &x, &y) && x + at.x >= a.x && x + at.x <= b.x &&
                                     y + at.y >= a.y && y + at.y <= b.y;
                        }
                        if (inside)
                            sel[t] = mode == 2 ? 0 : 1;
                    }
                }
            }
        }
    } else if (w.tool != kBox && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
               ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x == 0.0f &&
               ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y == 0.0f) {
        // a click: the nearest triangle under the pointer
        const auto dir_eye = orbit_eye_direction(w.yaw, w.pitch);
        const V3 eye{w.target[0] + dir_eye[0] * w.distance, w.target[1] + dir_eye[1] * w.distance,
                     w.target[2] + dir_eye[2] * w.distance};
        const V3 ray = orbit_ray(w.yaw, w.pitch, io.MousePos.x - at.x, io.MousePos.y - at.y, static_cast<float>(width),
                                 static_cast<float>(height), fov);
        float best = 1e30f;
        std::size_t best_map = 0, best_tri = 0;
        int best_layer = -1;
        for (std::size_t i = 0; i < w.placed.size() && i < w.project.maps.size(); ++i) {
            const Placed& pl = w.placed[i];
            if (!w.project.maps[i].shown || pl.geometry == nullptr)
                continue;
            for (int l = 0; l < kWorkLayers; ++l) {
                if (!layer_pickable(l) || (l == kWorkModel && !w.show_models) ||
                    (l == kWorkCollision && !w.show_collision))
                    continue;
                const auto& world = pl.world[static_cast<std::size_t>(l)];
                for (std::size_t t = 0; t * 3 + 2 < world.size(); ++t) {
                    if (!triangle_pickable(i, l, t))
                        continue;
                    // Moller-Trumbore, both faces
                    const V3 e1 = sub(world[t * 3 + 1], world[t * 3]), e2 = sub(world[t * 3 + 2], world[t * 3]);
                    const V3 h = cross(ray, e2);
                    const float det = dot(e1, h);
                    if (std::fabs(det) < 1e-9f)
                        continue;
                    const float inv = 1.0f / det;
                    const V3 s = sub(eye, world[t * 3]);
                    const float u = dot(s, h) * inv;
                    if (u < 0 || u > 1)
                        continue;
                    const V3 q = cross(s, e1);
                    const float v = dot(ray, q) * inv;
                    if (v < 0 || u + v > 1)
                        continue;
                    const float d = dot(e2, q) * inv;
                    if (d > 0 && d < best) {
                        best = d;
                        best_map = i;
                        best_tri = t;
                        best_layer = l;
                    }
                }
            }
        }
        if (best_layer >= 0)
            pick(best_map, best_layer, best_tri, mode);
        else if (mode == 0)
            clear_selection();
    }
    // the camera: left drag (with Alt in the box tool) or middle drag turns, right drag and W A S D Q E move
    const bool left_turns = (w.tool != kBox || io.KeyAlt) && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    if (hovered && !w.box_on && (left_turns || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        // orbiting: around the point in front of the camera; first person: looking around from where the
        // camera stands, the eye kept and that point moved in front of it the way it now looks
        const auto before = orbit_eye_direction(w.yaw, w.pitch);
        const V3 stand{w.target[0] + before[0] * w.distance, w.target[1] + before[1] * w.distance,
                       w.target[2] + before[2] * w.distance};
        w.yaw -= io.MouseDelta.x * 0.4f;
        w.pitch = std::clamp(w.pitch + io.MouseDelta.y * 0.4f, -85.0f, 85.0f);
        if (app.settings.map_camera_fps) {
            const auto after = orbit_eye_direction(w.yaw, w.pitch);
            w.target = {stand[0] - after[0] * w.distance, stand[1] - after[1] * w.distance,
                        stand[2] - after[2] * w.distance};
        }
    }
    orbit_move(w.target, w.yaw, w.pitch, w.distance, static_cast<float>(height), hovered, fov, false);
    if (hovered && io.MouseWheel != 0.0f)
        w.distance = std::max(10.0f, w.distance * std::pow(0.9f, io.MouseWheel));
    // the keys: Delete deletes, R restores (picking from the deleted), Ctrl+Z / Ctrl+Y step back and forth, Escape lets go of what is picked
    if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !w.pick_deleted)
            delete_selected(false);
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.KeyCtrl && w.pick_deleted)
            delete_selected(true);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            clear_selection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            step_history(io.KeyShift);
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            step_history(true);
    }
}

}  // namespace

void draw_workshop(App& app, float height) {
    const auto lang = app.language;
    take_chosen(app);
    sync_placed(app.settings);
    start_lister(app.settings);
    {
        std::lock_guard lock(w.mutex);
        if (w.exported) {
            w.exported = false;
            app.tell(w.export_message, w.export_ok);
        }
    }
    // the first map read: the camera over it
    if (!w.framed) {
        for (std::size_t i = 0; i < w.placed.size(); ++i) {
            if (w.placed[i].geometry != nullptr) {
                w.current = static_cast<int>(i);
                w.framed = true;
                break;
            }
        }
    }
    // the panel takes a share of the width the modder drags at the line beside it (each side at least 200
    // wide), as the catalog does in the other tab
    const float width = ImGui::GetContentRegionAvail().x;
    const float narrowest = std::min(200.0f, 0.5f * (width - kSplitLineGrip));
    const float widest = std::max(narrowest, width - kSplitLineGrip - narrowest);
    float left = std::clamp(app.settings.workshop_width * width, narrowest, widest);
    ImGui::BeginChild("##workshop_panel", ImVec2(left, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    draw_panel(app);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);
    const bool let_go = split_line("##workshop_split", false, height, &left, narrowest, widest, 0.3f * width, false);
    if (width > 0.0f && (let_go || ImGui::IsItemActive()))
        app.settings.workshop_width = std::clamp(left / width, 0.01f, 1.0f);
    if (let_go)
        save_now(app);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##workshop_picture", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::Checkbox(tr(lang, S::ws_show_models), &w.show_models);
    ImGui::SameLine();
    ImGui::Checkbox(tr(lang, S::ws_show_collision), &w.show_collision);
    ImGui::SameLine();
    ImGui::Checkbox(tr(lang, S::ws_show_skies), &w.show_skies);
    ImGui::SameLine();
    ImGui::Checkbox(tr(lang, S::ws_show_deleted), &w.show_deleted);
    if (w.loading) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", fmt(lang, S::ws_loading, {w.loading_map}).c_str());
    }
    // the camera at the row's right end (the Maps view's: one field of view and one camera for both)
    const float controls = camera_controls_width(app);
    ImGui::SameLine();
    if (ImGui::GetCursorPosX() + controls > ImGui::GetWindowContentRegionMax().x)
        ImGui::NewLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - controls));
    draw_camera_controls(app);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (w.project.maps.empty()) {
        ImGui::TextWrapped("%s", tr(lang, S::ws_empty));
    } else {
        draw_picture(app, ImVec2(avail.x, avail.y - ImGui::GetTextLineHeightWithSpacing()));
        ImGui::TextDisabled("%s", tr(lang, S::ws_camera_hint));
    }
    ImGui::EndChild();
}

}  // namespace studio
