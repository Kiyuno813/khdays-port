// The tool's settings: saves/studio.ini beside the port's settings.ini, in
// the same `key = value` form.
#pragma once

#include <filesystem>
#include <string>

namespace studio {

struct Settings {
    std::filesystem::path root;  // the port's folder: data/, mods/, saves/ (not stored)
    std::string port;            // the port's executable, for the dumps
    std::string graphics_dump = "saves/dump/graphics";
    std::string sounds_dump = "saves/dump/sounds";
    std::string ffmpeg;        // the movie converter, empty: none
    std::string language;      // the interface's: en, fr, de, it, es; empty: the system's
    std::string mod;           // the last mod opened
    int cache_mb = 512;        // the previews' memory budget (grows with use up to it)
    bool browser_tree = true;  // the catalog as folders (else one list)
    std::string theme;         // the colour palette: kh1, kh2, kh3, days, mom, rose-pine-moon; empty: MoM's
    // the catalog's share of the window's width, dragged at the line beside it; while a room is shown, its
    // share of the left panel's height (the room's controls under it)
    float browser_width = 0.42f;
    float map_tree = 0.45f;
    // the Maps view's camera: looking around from where it stands (else orbiting the point in front of it)
    bool map_camera_fps = false;
    // KINGDOM HEARTS HD 1.5+2.5 ReMIX's folder, KH2's rooms come from; empty: the one the install made
    // its HD pieces from (setup/state.json)
    std::string collection_1525;
};

// The settings file of a port folder.
std::filesystem::path settings_file(const std::filesystem::path& root);

// Reads the settings of `root`. A missing file gives the defaults; one that
// cannot be read gives the defaults too, the file kept as studio.ini.bad and
// `note` saying so. Defaults that depend on the folder (the port's
// executable, ffmpeg on the PATH) are filled in when not set.
Settings load_settings(const std::filesystem::path& root, std::string* note);

// Writes them; false with the reason in `why`.
bool save_settings(const Settings& settings, std::string* why);

// A path setting resolved against the port's folder (absolute stays as is).
std::filesystem::path resolve(const Settings& settings, const std::string& path);

}  // namespace studio
