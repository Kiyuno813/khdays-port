// The mission database's files beside the mission files (mdb.z, eid.z, evi, trbox), held as read until
// their fields are named from their readers (docs/FORMATS.md, "Missions").
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace studio {

// Records of a fixed size after a count word.
struct CountedRecords {
    std::size_t record_size = 0;
    std::vector<std::vector<std::uint8_t>> records;
};
bool parse_counted_records(const std::vector<std::uint8_t>& d, std::size_t record_size, CountedRecords* out,
                           std::string* why);
std::vector<std::uint8_t> write_counted_records(const CountedRecords& r);

// A count, that many offsets (from the file's start), then the bytes they
// point into; record i runs from its offset to the next larger one (the last
// to the end).
struct OffsetRecords {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint8_t> body;  // the bytes after the offsets
    std::vector<std::uint8_t> record(std::size_t i) const;
};
bool parse_offset_records(const std::vector<std::uint8_t>& d, OffsetRecords* out, std::string* why);
std::vector<std::uint8_t> write_offset_records(const OffsetRecords& r);

// What the field reads of `trbox` (the chests) and `evi` (the emblems): rows of 0x14 bytes per mission,
// keyed by its number -- slot or bit, kind, keys, flags, a heading, a point (docs/FORMATS.md,
// "Missions").
struct PlacedRow {
    std::int8_t b0 = 0, b1 = 0;
    std::int16_t h2 = 0, h4 = 0, h6 = 0;
    std::array<std::int32_t, 3> at{};
};
struct PlacedSet {
    int mission = 0;
    std::vector<PlacedRow> rows;
    std::size_t at = 0;  // where the set starts in its file
};
bool parse_placed_sets(const std::vector<std::uint8_t>& d, std::vector<PlacedSet>* out, std::string* why);
// The set of `mission`, or null (the first, as Ov002_FindHandlerByKey takes it).
const PlacedSet* placed_set(const std::vector<PlacedSet>& sets, int mission);
// mi/mi/<name> read and its sets parsed (trbox, evi).
bool load_placed_sets(const std::filesystem::path& nitrofs, const std::string& name, std::vector<PlacedSet>* out,
                      std::string* why);

// The story's Holo-Missions list (UI/cm/msl_<lang>.msi.z, Ov025): a u16 count, then records each starting
// with its u16 size -- +2 the mission file it starts (mi/mi/<file>), +4 the number the game shows, +6 and
// +8 the first and last day it is on the list (Ov025_MissionList_HasVisibleMissionInDays) -- its UTF-16
// texts from +0x30 (name, briefing, ...; 0x01 and 0x02 mark highlights). A mission has several records
// (briefing variants); files from 10000 are the story days' scripts.
struct MissionListEntry {
    int file = 0, number = 0, first_day = 0, last_day = 0;
    std::string name;  // UTF-8, its highlight marks left out
};
bool parse_mission_list(const std::vector<std::uint8_t>& d, std::vector<MissionListEntry>* out, std::string* why);
// UI/cm/msl_<language>.msi.z read (LZ undone) and parsed; `language` en, fr, de, it or es.
bool load_mission_list(const std::filesystem::path& nitrofs, const std::string& language,
                       std::vector<MissionListEntry>* out, std::string* why);

// The worlds' names (UI/cm/str/world_id_<lang>.s.z): a word, a u32 count, then each name as a u32 size (its
// own four bytes included) and UTF-16 -- in the order of the world codes (world_code: tt aw he al eh nm hb zz
// pp bb tw pi), the index a script's LoadWorldLinkResources names.
bool parse_world_names(const std::vector<std::uint8_t>& d, std::vector<std::string>* out, std::string* why);
bool load_world_names(const std::filesystem::path& nitrofs, const std::string& language, std::vector<std::string>* out,
                      std::string* why);

// The four files' names under mi/mi, and the check of one: read (LZ
// undone for .z), parsed, written back the same bytes.
std::vector<std::string> mission_db_files();
bool check_mission_db_file(const std::filesystem::path& nitrofs, const std::string& name, std::string* why);

}  // namespace studio
