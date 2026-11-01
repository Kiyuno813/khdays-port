// The map workshop's data (the Map workshop tab, views/workshop_view.cpp): KINGDOM HEARTS II maps of the
// player's collection put side by side, each placed and with triangles of its model and of its floors and walls
// deleted, saved as a project file anywhere and written out as one glTF (and one collision glTF) to make a room
// of, or to open elsewhere. A project names maps and triangles only: none of the game is in it.
#pragma once

#include "core/mods/settings.h"
#include "core/rooms/room_change.h"

#include "khdays/assets/model_import.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace studio {

// What of a map is shown, picked and deleted: its model, or its floors and walls (KH2's own collision).
enum WorkLayer { kWorkModel = 0, kWorkCollision = 1, kWorkLayers = 2 };

struct WorkshopMap {
    std::string map;  // the KH2 map: tt08
    RoomPlacement place;
    bool shown = true;
    // per layer, per triangle in the layer's order (its meshes in turn, each mesh's in turn): 1 deleted;
    // shorter than the layer's triangles while not loaded (the rest kept)
    std::array<std::vector<std::uint8_t>, kWorkLayers> deleted;
};

struct WorkshopProject {
    std::vector<WorkshopMap> maps;
};

// The triangles of `model`, all its meshes': what WorkshopMap::deleted counts.
std::size_t triangle_count(const khdays::assets::ImportedModel& model);

// A project file: its maps, places and deleted triangles (as ranges, "0-12 40 77-80"). False with the reason.
bool save_workshop(const std::filesystem::path& file, const WorkshopProject& project, std::string* why);
bool load_workshop(const std::filesystem::path& file, WorkshopProject* out, std::string* why);
// The same as text (what the file holds).
std::string workshop_text(const WorkshopProject& project);
bool read_workshop_text(const std::string& text, WorkshopProject* out, std::string* why);

// `model` without the triangles `deleted` marks; meshes left with none dropped. Skeleton, nodes, animations and
// textures kept.
khdays::assets::ImportedModel without_deleted(const khdays::assets::ImportedModel& model,
                                              const std::vector<std::uint8_t>& deleted);

// Models made one: their meshes in turn, their textures by name (the first of a name kept), their skeletons,
// nodes and animations side by side (each still moving its own meshes).
khdays::assets::ImportedModel merged_models(const std::vector<khdays::assets::ImportedModel>& parts);

// `model` written as the glTF `file`, its textures beside it as <file's name>_<n>.png (a model that moves by itself
// as its scene: skeleton and animations kept). False with the reason.
bool write_model_gltf(const khdays::assets::ImportedModel& model, const std::filesystem::path& file, std::string* why);

// Per triangle (the layer's order), the piece it belongs to: triangles sharing a corner (by position) are one
// piece. The pieces numbered from 0.
std::vector<int> connected_pieces(const khdays::assets::ImportedModel& model);

// The workshop's output: every shown map placed, its deleted triangles left out, made one and written as
// `<folder>/<name>.gltf` and `<folder>/<name>_collision.gltf`; `models` and `collisions` the maps' loaded layers
// (the project's order; a map without one is left out of that file). False with the reason.
bool export_workshop(const WorkshopProject& project, const std::vector<const khdays::assets::ImportedModel*>& models,
                     const std::vector<const khdays::assets::ImportedModel*>& collisions,
                     const std::filesystem::path& folder, const std::string& name, std::vector<std::string>* written,
                     std::string* why);

// A KH2 map of the player's collection ready for the workshop: taken out and made the first time (its model and
// its floors and walls beside it, kh2_made_model()'s folder), then both read as the port reads them. `collision`
// left empty when the map has none. False with the reason.
bool prepare_workshop_map(const Settings& settings, const std::string& map, khdays::assets::ImportedModel* model,
                          khdays::assets::ImportedModel* collision, std::string* why);

}  // namespace studio
