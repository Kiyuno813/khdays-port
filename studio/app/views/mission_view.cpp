// A mission. "Summary": what it is in plain words -- where, the start, the time limit, the enemies and
// where they come from, chests, emblems, what ends it -- each read value editable and saved into the
// mod. "Scripts": its entries and scripts, each step by the function its group's table gives it and its
// words as read; what a function does is shown only once read from the game.
#include "../app.h"

#include "core/missions/command_meanings.h"
#include "core/missions/command_names.h"
#include "core/missions/enemy_names.h"
#include "core/missions/mission_db.h"
#include "core/missions/mission_edit.h"
#include "core/missions/missions.h"
#include "core/rooms/kh2_source.h"
#include "core/rooms/room_collision.h"
#include "core/rooms/worlds.h"
#include "core/files/paths.h"

#include "khdays/vfs/filesystem.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <tuple>

namespace studio {

namespace {

struct MissionView {
    int item = -1;
    std::string mod;  // the mod the edit was read with
    MissionEdit edit;
    std::string error;
    int entry = -1, script = -1;
    int table = -1;  // an enemy table's entry shown instead of a script
    std::string filter;
    std::size_t scripts = 0, steps = 0;
    int changes = 0;  // values changed since read or saved
};

MissionView v;

// The mission's points as they were before each change (see mission_point_push): undone and redone one by one.
struct PointStep {
    MissionPoint::Kind kind = MissionPoint::spawner;
    int index = -1;
    std::array<std::int32_t, 3> at{};
    int facing = 0;
};
std::vector<PointStep> point_undo, point_redo, point_parked;

fs::path nitrofs() {
    return khdays::vfs::data_root() / "nitrofs";
}

std::string lower_text(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The catalog's index of `item` (an item of app.catalog.items).
int index_of(const App& app, const Item& item) {
    return static_cast<int>(&item - app.catalog.items.data());
}

void open(App& app, const Item& item) {
    v = MissionView{};
    point_undo.clear();  // the steps were of the mission read before
    point_redo.clear();
    point_parked.clear();
    history_clear(EditOwner::mission);
    v.item = index_of(app, item);
    const Mod* m = app.current_mod();
    v.mod = m != nullptr ? m->name : std::string();
    const auto slash = item.key.find_last_of('/');
    const int number = std::atoi(item.key.c_str() + (slash == std::string::npos ? 0 : slash + 1));
    if (!load_mission_edit(nitrofs(), m != nullptr ? m->dir : fs::path(), number, &v.edit, &v.error))
        return;
    for (const auto& e : v.edit.mission.entries) {
        v.scripts += e.archive.scripts.size();
        for (const auto& s : e.archive.scripts)
            v.steps += s.steps.size();
    }
}

// Where the mission's scripts go: the first world they load and every room any of them puts in a
// slot (for the room under a point), and every spawner and start with the script holding it.
struct Placed {
    ScriptPlaces all;
    struct Ref {
        std::size_t entry, script, step;
    };
    std::vector<std::pair<Ref, ScriptPlaces::Spawner>> spawners;
    std::vector<std::pair<Ref, ScriptPlaces::Start>> starts;
};

Placed placed() {
    Placed out;
    const auto& entries = v.edit.mission.entries;
    for (std::size_t e = 0; e < entries.size(); ++e) {
        for (std::size_t i = 0; i < entries[e].archive.scripts.size(); ++i) {
            const ScriptPlaces p = script_places(entries[e].archive.scripts[i]);
            if (out.all.world < 0)
                out.all.world = p.world;
            if (p.world == out.all.world || p.world < 0) {
                for (const auto& r : p.rooms) {
                    if (std::find(out.all.rooms.begin(), out.all.rooms.end(), r) == out.all.rooms.end())
                        out.all.rooms.push_back(r);
                }
            }
            for (const auto& sp : p.spawners) {
                out.spawners.push_back({{e, i, sp.step}, sp});
                out.all.spawners.push_back(sp);
            }
            for (const auto& st : p.starts) {
                out.starts.push_back({{e, i, st.step}, st});
                out.all.starts.push_back(st);
            }
        }
    }
    return out;
}

ScriptStep& step_of(const Placed::Ref& r) {
    return v.edit.mission.entries[r.entry].archive.scripts[r.script].steps[r.step];
}
Script& script_of(const Placed::Ref& r) {
    return v.edit.mission.entries[r.entry].archive.scripts[r.script];
}

// The game's number for a language (GetLanguage: 1 en, 2 fr, 3 de, 4 it,
// 5 es -- the window's own order, from 0).
int game_language(Language language) {
    return static_cast<int>(language) + 1;
}

// What a step does, in the window's language (empty when its handler is not read).
std::string meaning_of(App& app, const Script& script, const ScriptStep& s) {
    const StepMeaning m = step_meaning(s, &script, game_language(app.language));
    // in StepMeaning::Kind's order
    static const S words[] = {
        S::mission_meaning,        S::meaning_assign,           S::meaning_load_world,      S::meaning_load_room,
        S::meaning_load_enemies,   S::meaning_enemy_at,         S::meaning_spawner,         S::meaning_go_to,
        S::meaning_branch_unless,  S::meaning_pending_jump,     S::meaning_no_pending_jump, S::meaning_notice,
        S::meaning_on_event,       S::meaning_on_event_start,   S::meaning_event_when,      S::meaning_hud_on,
        S::meaning_time_limit,     S::meaning_time_limit_shown, S::meaning_time_limit_add,  S::meaning_time_limit_set,
        S::meaning_time_limit_sub, S::meaning_clock_restart,    S::meaning_player_at,       S::meaning_player_at_marker,
        S::meaning_chests,         S::meaning_emblems};
    static_assert(sizeof(words) / sizeof(words[0]) == StepMeaning::emblems + 1, "a word per meaning");
    if (m.kind == StepMeaning::none)
        return {};
    std::string text = tr(app.language, words[m.kind]);
    for (std::size_t k = 0; k < m.args.size(); ++k) {
        const std::string key = "{" + std::to_string(k) + "}";
        for (auto at = text.find(key); at != std::string::npos; at = text.find(key))
            text.replace(at, key.size(), m.args[k]);
    }
    return text;
}

std::string function_of(App& app, const ScriptStep& s) {
    const CommandName* n = command_name(s.group, s.command);
    if (n == nullptr)
        return tr(app.language, S::mission_unnamed);
    if (n->handler[0] == '\0')
        return tr(app.language, S::mission_skipped);
    return n->handler;
}

// The room of the world whose ground is under a point, among the rooms a
// script loads (by the collision: a point is in the room that has ground
// under it); -1 when none.
int room_under(const World& w, const std::vector<std::pair<int, int>>& rooms, const std::array<std::int32_t, 3>& at) {
    // each room's collision read once (the summary asks for every point at every frame)
    static std::string cached_world;
    static std::map<int, std::pair<bool, RoomCollision>> cache;
    if (cached_world != w.code) {
        cache.clear();
        cached_world = w.code;
    }
    for (const auto& [slot, room] : rooms) {
        (void)slot;
        if (room < 0 || static_cast<std::size_t>(room) >= w.rooms.size())
            continue;
        auto it = cache.find(room);
        if (it == cache.end()) {
            const auto& data =
                w.archive.entries[static_cast<std::size_t>(w.rooms[static_cast<std::size_t>(room)].collision_member)]
                    .data;
            RoomCollision c;
            const bool ok = !data.empty() && parse_collision(data, &c, nullptr);
            it = cache.emplace(room, std::make_pair(ok, std::move(c))).first;
        }
        if (!it->second.first)
            continue;
        double y = 0;
        if (ground_under(it->second.second, at[0], at[2], at[1] + 4096.0 * 4, &y))
            return room;
    }
    return -1;
}

// The world the mission's scripts load (read once per world).
const World& mission_world(int index) {
    static World world;
    const char* code = world_code(index);
    if (code == nullptr) {
        static const World none;
        return none;
    }
    if (world.code != code) {
        std::string why;
        if (!load_world(nitrofs(), code, &world, &why))
            world = World{};
    }
    return world;
}

const std::vector<PlacedRow>* chest_rows() {
    const PlacedSet* s = placed_set(v.edit.trbox_sets, v.edit.number);
    return s != nullptr ? &s->rows : nullptr;
}
const std::vector<PlacedRow>* emblem_rows() {
    const PlacedSet* s = placed_set(v.edit.evi_sets, v.edit.number);
    return s != nullptr ? &s->rows : nullptr;
}

// The mission's enemy table (em0), false when it has none or it cannot be read.
bool mission_enemies(EnemyTable* t) {
    const int em = v.edit.mission.entry("em0");
    return em >= 0 && parse_enemy_table(v.edit.mission.p2.entries[static_cast<std::size_t>(em)].data, t, nullptr);
}

// A spawner's words over the map: its number in the summary, its group, and the group's enemies with how many.
std::string spawner_label(App& app, std::size_t number, int group, const EnemyTable* t) {
    const auto lang = app.language;
    std::string text = fmt(lang, S::mission_tip_spawner, {std::to_string(number), std::to_string(group)});
    if (t == nullptr || group < 0 || static_cast<std::size_t>(group) >= t->groups.size())
        return text;
    bool any = false;
    for (const auto& part : t->groups[static_cast<std::size_t>(group)].parts) {
        const char* name = part.row < t->kinds.size() ? enemy_name(t->kinds[part.row].id()) : nullptr;
        text += "\n" + fmt(lang, S::mission_tip_enemy, {std::to_string(part.spawned()), name != nullptr ? name : "?"});
        any = true;
    }
    if (!any)
        text += "\n" + std::string(tr(lang, S::mission_tip_no_enemies));
    return text;
}

// An emblem's words: its number and its kind (evi's byte 1, the kind Ov002_SpawnStateSpots hands
// Ov002_SpawnSpot; what each kind is has not been read).
std::string emblem_label(App& app, std::size_t number, const PlacedRow& r) {
    return fmt(app.language, S::mission_tip_emblem, {std::to_string(number), std::to_string(r.b1)});
}

std::string chest_label(App& app, std::size_t number, const PlacedRow& r) {
    const auto lang = app.language;
    return fmt(lang, S::mission_tip_chest, {std::to_string(number)}) + "\n" +
           fmt(lang, S::mission_tip_chest_info, {std::to_string(r.h2), std::to_string(r.h6)});
}

// The mission's points in `room` (spawners, starts, chests, emblems) put in the map's overlay, from the values
// as they are now.
void fill_overlay(App& app, const World& world, const ScriptPlaces& p, int room, MissionOverlay& mo) {
    const auto lang = app.language;
    mo.points.clear();
    EnemyTable t;
    const bool enemies = mission_enemies(&t);
    const auto add = [&](const std::array<std::int32_t, 3>& at, std::string label, MissionPoint::Kind kind,
                         std::size_t index, bool has_facing, int facing) {
        MissionPoint pt;
        pt.at = at;
        pt.label = std::move(label);
        pt.kind = kind;
        pt.index = static_cast<int>(index);
        pt.has_facing = has_facing;
        pt.facing = facing;
        mo.points.push_back(std::move(pt));
    };
    for (std::size_t i = 0; i < p.spawners.size(); ++i) {
        const auto& sp = p.spawners[i];
        if (room_under(world, p.rooms, sp.at) == room)
            add(sp.at, spawner_label(app, i + 1, sp.group, enemies ? &t : nullptr), MissionPoint::spawner, i, false, 0);
    }
    for (std::size_t i = 0; i < p.starts.size(); ++i) {
        const auto& st = p.starts[i];
        if (st.marker.empty() && room_under(world, p.rooms, st.at) == room)
            add(st.at, fmt(lang, S::mission_start_short, {std::to_string(st.player)}), MissionPoint::start, i, true,
                st.heading);
    }
    if (const auto* rows = chest_rows()) {
        for (std::size_t i = 0; i < rows->size(); ++i) {
            if (room_under(world, p.rooms, (*rows)[i].at) == room)
                add((*rows)[i].at, chest_label(app, i + 1, (*rows)[i]), MissionPoint::chest, i, true, (*rows)[i].h6);
        }
    }
    if (const auto* rows = emblem_rows()) {
        for (std::size_t i = 0; i < rows->size(); ++i) {
            if (room_under(world, p.rooms, (*rows)[i].at) == room)
                add((*rows)[i].at, emblem_label(app, i + 1, (*rows)[i]), MissionPoint::emblem, i, false, 0);
        }
    }
}

// The room shown with every point of the mission in it, the mission kept open under the catalog.
void see_in_room(App& app, const World& world, const ScriptPlaces& p, int room) {
    MissionOverlay& mo = mission_overlay();
    mo = MissionOverlay{};
    mo.world = world.code;
    mo.room = room;
    mo.mission = v.item;
    fill_overlay(app, world, p, room, mo);
    const std::string key = world_path(world.code) + "/room/" + std::to_string(room);
    for (std::size_t i = 0; i < app.catalog.items.size(); ++i) {
        if (app.catalog.items[i].kind == Kind::room && app.catalog.items[i].key == key) {
            app.mission_beside = v.item;
            app.selected = static_cast<int>(i);
        }
    }
}

std::string point_text(const std::array<std::int32_t, 3>& at) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", at[0] / 4096.0, at[1] / 4096.0, at[2] / 4096.0);
    return buf;
}

// An enemy by its table row: its name and id.
std::string enemy_text(App& app, const EnemyTable& t, std::size_t row) {
    if (row >= t.kinds.size())
        return "?";
    const int id = t.kinds[row].id();
    const char* name = enemy_name(id);
    return fmt(app.language, S::mission_enemy_named,
               {name != nullptr ? name : "?", std::to_string(id), std::to_string(row)});
}

// After a field of a mission's point: the point kept for undo when the field is taken hold of, the step given
// back when it is let go of unchanged.
void point_undo_field(App& app, MissionPoint::Kind kind, int index) {
    if (ImGui::IsItemActivated())
        mission_point_push(app, kind, index);
    else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit())
        mission_point_unpush();
}

// A point edited as three numbers (the game's units, 20.12 written back); true when it changed. Kept for undo
// as the point `kind`, `index`.
bool edit_point(App& app, const char* id, std::array<std::int32_t, 3>* at, MissionPoint::Kind kind, int index) {
    float xyz[3] = {static_cast<float>((*at)[0] / 4096.0), static_cast<float>((*at)[1] / 4096.0),
                    static_cast<float>((*at)[2] / 4096.0)};
    ImGui::SetNextItemWidth(20 * ImGui::GetFontSize());
    const bool changed = ImGui::DragFloat3(id, xyz, 0.05f, 0, 0, "%.2f");
    point_undo_field(app, kind, index);
    if (!changed)
        return false;
    for (int k = 0; k < 3; ++k)
        (*at)[static_cast<std::size_t>(k)] = static_cast<std::int32_t>(std::lround(xyz[k] * 4096.0));
    return true;
}

// A line's button showing the room under a point (or a bullet when none is found).
void room_button(App& app, const World& world, const ScriptPlaces& all, const std::array<std::int32_t, 3>& at) {
    const int room = world.code.empty() ? -1 : room_under(world, all.rooms, at);
    if (room >= 0) {
        if (ImGui::SmallButton(fmt(app.language, S::mission_see_room_n, {std::to_string(room)}).c_str()))
            see_in_room(app, world, all, room);
    } else {
        ImGui::TextDisabled("%s", tr(app.language, S::mission_no_room));
    }
}

// "See spawner" / "See chest" beside a line's room button: its room shown (or kept) and the point ringed for a
// few seconds; the camera stays where it is.
void focus_button(App& app, const World& world, const ScriptPlaces& all, const std::array<std::int32_t, 3>& at,
                  MissionPoint::Kind kind) {
    const int room = world.code.empty() ? -1 : room_under(world, all.rooms, at);
    if (room < 0)
        return;
    ImGui::SameLine();
    if (ImGui::SmallButton(
            tr(app.language, kind == MissionPoint::chest ? S::mission_see_chest : kind == MissionPoint::emblem ? S::mission_see_emblem : S::mission_see_spawner))) {
        see_in_room(app, world, all, room);
        MissionOverlay& mo = mission_overlay();
        mo.focus = true;
        mo.focus_at = at;
        mo.focus_kind = kind;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(app.language, S::mission_see_point_hint));
}

// Where a step is set, in a few words: "start · _i" (the start's entry, its script), "area 11 · e0".
std::string where_text(App& app, const Placed::Ref& r) {
    const auto& names = v.edit.mission.entry_names;
    std::string entry = r.entry < names.size() ? names[r.entry] : std::string();
    const auto dot = entry.find('.');
    if (dot != std::string::npos)
        entry = entry.substr(0, dot);
    const std::string script = script_of(r).name;
    const std::string place =
        entry.rfind("_s", 0) == 0 ? std::string(tr(app.language, S::mission_sum_at_start))
        : !entry.empty() && std::all_of(entry.begin(), entry.end(), [](char c) { return c >= '0' && c <= '9'; })
            ? fmt(app.language, S::mission_sum_in_area, {entry})
            : entry;
    return "(" + place + " · " + script + ")";
}

void section_note(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

// --- the summary ------------------------------------------------------------------------

// A section of the summary that folds (open at first; ImGui keeps whether it is open). Opened when the map has
// just picked one of its points (`kinds`, a bit for each MissionPoint::Kind), so that point's row can be shown.
bool fold_opens_for_pick(unsigned kinds) {
    const MissionOverlay& mo = mission_overlay();
    return mo.scroll && mo.mission == v.item && mo.picked_kind >= 0 && (kinds & (1u << mo.picked_kind)) != 0;
}

bool section(const char* label, unsigned kinds = 0) {
    if (fold_opens_for_pick(kinds))
        ImGui::SetNextItemOpen(true);
    return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
}

// A part of a section that folds on its own (Spawners, Enemies put one by one); TreePop() after it when open.
bool subsection(const char* label, unsigned kinds = 0) {
    if (fold_opens_for_pick(kinds))
        ImGui::SetNextItemOpen(true);
    return ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
}

void draw_texts(App& app) {
    const auto lang = app.language;
    // the notices (Ov002_ShowNoticeText: the text shown on the screen), grouped by what they show:
    // the same text in several steps is one text to change
    struct Use {
        Placed::Ref ref;
        int mode;
    };
    struct Shown {
        std::vector<std::string> texts;  // by language 1-5 (mode 0x40), or one
        std::vector<Use> uses;
    };
    std::vector<Shown> shown;
    auto& entries = v.edit.mission.entries;
    for (std::size_t e = 0; e < entries.size(); ++e) {
        for (std::size_t i = 0; i < entries[e].archive.scripts.size(); ++i) {
            const Script& s = entries[e].archive.scripts[i];
            for (std::size_t k = 0; k < s.steps.size(); ++k) {
                const ScriptStep& st = s.steps[k];
                if (step_meaning(st, &s).kind != StepMeaning::notice)
                    continue;
                const OperandSlot o = operand_at(st, 0);
                std::vector<std::string> texts;
                if (o.mode == 0x40) {
                    for (int l = 1; l <= 5; ++l)
                        texts.push_back(script_text(s, static_cast<std::uint32_t>(o.value), l));
                } else {
                    texts.push_back(script_string(s, static_cast<std::uint32_t>(o.value)));
                }
                auto it = std::find_if(shown.begin(), shown.end(), [&](const Shown& x) { return x.texts == texts; });
                if (it == shown.end()) {
                    shown.push_back({texts, {}});
                    it = shown.end() - 1;
                }
                it->uses.push_back({{e, i, k}, o.mode});
            }
        }
    }
    if (shown.empty() || !section(tr(lang, S::mission_sum_texts)))
        return;
    section_note(tr(lang, S::mission_sum_texts_note));
    static const char* const names[] = {"English", "Français", "Deutsch", "Italiano", "Español"};
    for (std::size_t n = 0; n < shown.size(); ++n) {
        ImGui::PushID(static_cast<int>(n));
        if (shown[n].uses.size() > 1)
            ImGui::TextDisabled("%s",
                                fmt(lang, S::mission_sum_text_uses, {std::to_string(shown[n].uses.size())}).c_str());
        const bool per_language = shown[n].texts.size() == 5;
        for (std::size_t l = 0; l < shown[n].texts.size(); ++l) {
            std::string text = shown[n].texts[l];
            ImGui::PushID(static_cast<int>(l));
            ImGui::SetNextItemWidth(-10 * ImGui::GetFontSize());
            if (ImGui::InputText(per_language ? names[l] : tr(lang, S::mission_sum_all_languages), &text,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                // once per script and place (steps of one script may share a table)
                std::vector<std::tuple<std::size_t, std::size_t, std::int32_t>> done;
                for (const auto& u : shown[n].uses) {
                    ScriptStep& st = step_of(u.ref);
                    const auto key = std::make_tuple(u.ref.entry, u.ref.script, operand_at(st, 0).value);
                    if (u.mode == 0x40 && std::find(done.begin(), done.end(), key) != done.end())
                        continue;
                    if (set_step_text(script_of(u.ref), st, 0, per_language ? static_cast<int>(l) + 1 : 0, text))
                        done.push_back(key);
                }
                if (!done.empty())
                    ++v.changes;
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    section_note(tr(lang, S::mission_sum_enter));
}

// A point's title in the summary: lit when the map's picked point, scrolled to once.
void point_title(const std::string& text, MissionPoint::Kind kind, int index) {
    MissionOverlay& mo = mission_overlay();
    const bool picked = mo.mission == v.item && mo.picked_kind == static_cast<int>(kind) && mo.picked_index == index;
    if (picked)
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", text.c_str());
    else
        ImGui::TextUnformatted(text.c_str());
    if (picked && mo.scroll) {
        ImGui::SetScrollHereY(0.3f);
        mo.scroll = false;
    }
}

void draw_starts(App& app, const Placed& p, const World& world) {
    const auto lang = app.language;
    if (p.starts.empty() || !section(tr(lang, S::mission_sum_starts), 1u << MissionPoint::start))
        return;
    section_note(tr(lang, S::mission_sum_starts_note));
    int id = 0;
    for (const auto& [ref, st] : p.starts) {
        ImGui::PushID(id++);
        if (!st.marker.empty()) {
            ImGui::BulletText("%s",
                              fmt(lang, S::mission_sum_start_marker, {std::to_string(st.player), st.marker}).c_str());
            ImGui::PopID();
            continue;
        }
        const int index = id - 1;
        point_title(fmt(lang, S::mission_sum_start, {std::to_string(st.player)}), MissionPoint::start, index);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", where_text(app, ref).c_str());
        ImGui::SameLine();
        room_button(app, world, p.all, st.at);
        ImGui::Indent();
        std::array<std::int32_t, 3> at = st.at;
        if (edit_point(app, "##at", &at, MissionPoint::start, index)) {
            ScriptStep& step = step_of(ref);
            set_fx(step, 3, at[0]);
            set_fx(step, 4, at[1]);
            set_fx(step, 5, at[2]);
            ++v.changes;
        }
        ImGui::SameLine();
        int heading = st.heading;
        ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
        const bool turned = ImGui::InputInt(tr(lang, S::mission_sum_facing), &heading, 0, 0);
        point_undo_field(app, MissionPoint::start, index);
        if (turned) {
            set_int(step_of(ref), 6, ((heading % 360) + 360) % 360);
            ++v.changes;
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
}

void draw_time(App& app) {
    const auto lang = app.language;
    if (!section(tr(lang, S::mission_sum_time)))
        return;
    bool any = false;
    auto& entries = v.edit.mission.entries;
    for (std::size_t e = 0; e < entries.size(); ++e) {
        for (std::size_t i = 0; i < entries[e].archive.scripts.size(); ++i) {
            Script& s = entries[e].archive.scripts[i];
            for (std::size_t k = 0; k < s.steps.size(); ++k) {
                ScriptStep& st = s.steps[k];
                const auto kind = step_meaning(st, &s).kind;
                S label;
                switch (kind) {
                case StepMeaning::time_limit: label = S::mission_sum_time_limit; break;
                case StepMeaning::time_limit_add: label = S::mission_sum_time_add; break;
                case StepMeaning::time_limit_set: label = S::mission_sum_time_set; break;
                case StepMeaning::time_limit_sub: label = S::mission_sum_time_sub; break;
                default: continue;
                }
                any = true;
                ImGui::PushID(static_cast<int>(e * 100000 + i * 1000 + k));
                int seconds = operand_at(st, 4).value / 1000;
                ImGui::SetNextItemWidth(8 * ImGui::GetFontSize());
                if (ImGui::InputInt(tr(lang, label), &seconds, 5, 30)) {
                    set_int(st, 2, std::max(0, seconds) * 1000);
                    ++v.changes;
                }
                ImGui::PopID();
            }
        }
    }
    if (!any)
        ImGui::TextDisabled("%s", tr(lang, S::mission_sum_no_time));
}

void draw_enemies(App& app, const Placed& p, const World& world) {
    const auto lang = app.language;
    const int em = v.edit.mission.entry("em0");
    if (!section(tr(lang, S::mission_sum_enemies), 1u << MissionPoint::spawner))
        return;
    if (em < 0) {
        ImGui::TextDisabled("%s", tr(lang, S::mission_sum_no_enemies));
        return;
    }
    auto& data = v.edit.mission.p2.entries[static_cast<std::size_t>(em)].data;
    EnemyTable t;
    std::string why;
    if (!parse_enemy_table(data, &t, &why)) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1), "%s", why.c_str());
        return;
    }
    bool table_changed = false;
    int id = 0;
    const bool spawners_open = subsection(
        fmt(lang, S::mission_sum_spawners, {std::to_string(p.spawners.size())}).c_str(), 1u << MissionPoint::spawner);
    if (spawners_open)
        section_note(tr(lang, S::mission_sum_enemies_note));
    for (const auto& [ref, sp] : p.spawners) {
        if (!spawners_open)
            break;
        ImGui::PushID(id++);
        point_title(fmt(lang, S::mission_sum_spawner, {std::to_string(id)}), MissionPoint::spawner, id - 1);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", where_text(app, ref).c_str());
        ImGui::SameLine();
        room_button(app, world, p.all, sp.at);
        focus_button(app, world, p.all, sp.at, MissionPoint::spawner);
        ImGui::Indent();
        std::array<std::int32_t, 3> at = sp.at;
        if (edit_point(app, "##at", &at, MissionPoint::spawner, id - 1)) {
            ScriptStep& step = step_of(ref);
            set_fx(step, 5, at[0]);
            set_fx(step, 6, at[1]);
            set_fx(step, 7, at[2]);
            ++v.changes;
        }
        // which group it makes, and the group's enemies
        int group = sp.group;
        ImGui::SetNextItemWidth(14 * ImGui::GetFontSize());
        if (ImGui::BeginCombo(tr(lang, S::mission_sum_group),
                              fmt(lang, S::mission_sum_group_n, {std::to_string(group)}).c_str())) {
            for (std::size_t g = 0; g < t.groups.size(); ++g) {
                std::string who;
                for (const auto& part : t.groups[g].parts) {
                    who +=
                        (who.empty() ? "" : ", ") + std::to_string(part.spawned()) + " " + enemy_text(app, t, part.row);
                }
                if (ImGui::Selectable((fmt(lang, S::mission_sum_group_n, {std::to_string(g)}) + ": " + who).c_str(),
                                      static_cast<int>(g) == group)) {
                    set_int(step_of(ref), 0, static_cast<std::int32_t>(g));
                    ++v.changes;
                }
            }
            ImGui::EndCombo();
        }
        if (group >= 0 && static_cast<std::size_t>(group) < t.groups.size()) {
            auto& parts = t.groups[static_cast<std::size_t>(group)].parts;
            for (std::size_t k = 0; k < parts.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                auto& part = parts[k];
                int how_many = part.spawned();
                ImGui::SetNextItemWidth(7 * ImGui::GetFontSize());
                if (ImGui::InputInt(tr(lang, S::mission_sum_how_many), &how_many, 1, 5)) {
                    how_many = std::clamp(how_many, 0, 254);
                    part.count = static_cast<std::uint8_t>(how_many);
                    if (part.limit != 0xff)
                        part.limit = static_cast<std::uint8_t>(how_many);
                    table_changed = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(18 * ImGui::GetFontSize());
                if (ImGui::BeginCombo("##enemy", enemy_text(app, t, part.row).c_str())) {
                    for (std::size_t r = 0; r < t.kinds.size(); ++r) {
                        if (ImGui::Selectable(enemy_text(app, t, r).c_str(), r == part.row)) {
                            part.row = static_cast<std::uint8_t>(r);
                            table_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            int users = 0;
            for (const auto& other : p.spawners)
                users += other.second.group == group ? 1 : 0;
            if (users > 1)
                ImGui::TextDisabled("%s", fmt(lang, S::mission_sum_group_shared, {std::to_string(users)}).c_str());
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (spawners_open) {
        if (p.spawners.empty())
            ImGui::TextDisabled("%s", tr(lang, S::mission_sum_no_spawners));
        ImGui::TreePop();
    }
    // the enemies put one by one at a source the script made (Ov002_CreateActorFromMarker(source, row))
    auto& entries = v.edit.mission.entries;
    const auto one_by_one = [&](const ScriptStep& st, const Script& sc) {
        return step_meaning(st, &sc).kind == StepMeaning::enemy_at && operand_at(st, 2).mode == 1;
    };
    bool any_one_by_one = false;
    for (const auto& en : entries) {
        for (const auto& sc : en.archive.scripts) {
            for (const auto& st : sc.steps)
                any_one_by_one = any_one_by_one || one_by_one(st, sc);
        }
    }
    const bool marker_open = any_one_by_one && subsection(tr(lang, S::mission_sum_marker_enemies));
    if (marker_open)
        section_note(tr(lang, S::mission_sum_marker_enemies_note));
    for (std::size_t e = 0; marker_open && e < entries.size(); ++e) {
        for (std::size_t i = 0; i < entries[e].archive.scripts.size(); ++i) {
            Script& sc = entries[e].archive.scripts[i];
            for (std::size_t k = 0; k < sc.steps.size(); ++k) {
                ScriptStep& st = sc.steps[k];
                if (!one_by_one(st, sc))
                    continue;
                const OperandSlot source = operand_at(st, 0), row = operand_at(st, 2);
                ImGui::PushID(static_cast<int>(100000 + e * 10000 + i * 1000 + k));
                ImGui::SetNextItemWidth(18 * ImGui::GetFontSize());
                if (ImGui::BeginCombo("##marker", enemy_text(app, t, static_cast<std::size_t>(row.value)).c_str())) {
                    for (std::size_t r = 0; r < t.kinds.size(); ++r) {
                        if (ImGui::Selectable(enemy_text(app, t, r).c_str(), static_cast<int>(r) == row.value)) {
                            set_int(st, 1, static_cast<std::int32_t>(r));
                            ++v.changes;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(fmt(lang, S::mission_sum_marker_enemy, {slot_text(source)}).c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", where_text(app, {e, i, k}).c_str());
                ImGui::PopID();
            }
        }
    }
    if (marker_open)
        ImGui::TreePop();
    if (table_changed) {
        if (write_enemy_table_over(data, t, &why)) {
            ++v.changes;
        } else {
            app.tell(why, false);
        }
    }
    section_note(tr(lang, S::mission_sum_enemies_kinds));
}

void draw_placed(App& app, const Placed& p, const World& world, bool chests) {
    const auto lang = app.language;
    PlacedSet* set = placed_set_of(chests ? v.edit.trbox_sets : v.edit.evi_sets, v.edit.number);
    if (!section(tr(lang, chests ? S::mission_sum_chests : S::mission_sum_emblems),
                 1u << (chests ? MissionPoint::chest : MissionPoint::emblem)))
        return;
    if (set == nullptr || set->rows.empty()) {
        ImGui::TextDisabled("%s", tr(lang, chests ? S::mission_sum_no_chests : S::mission_sum_no_emblems));
        return;
    }
    section_note(tr(lang, chests ? S::mission_sum_chests_note : S::mission_sum_emblems_note));
    bool changed = false;
    for (std::size_t i = 0; i < set->rows.size(); ++i) {
        PlacedRow& r = set->rows[i];
        const MissionPoint::Kind kind = chests ? MissionPoint::chest : MissionPoint::emblem;
        const int index = static_cast<int>(i);
        ImGui::PushID(index);
        point_title(fmt(lang, chests ? S::mission_sum_chest : S::mission_sum_emblem, {std::to_string(i + 1)}), kind,
                    index);
        if (!chests) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", fmt(lang, S::mission_emblem_kind, {std::to_string(r.b1)}).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr(lang, S::mission_emblem_kind_hint));
        }
        ImGui::SameLine();
        room_button(app, world, p.all, r.at);
        focus_button(app, world, p.all, r.at, kind);
        ImGui::Indent();
        changed |= edit_point(app, "##at", &r.at, kind, index);
        if (chests) {
            ImGui::SameLine();
            int contents = r.h2;
            ImGui::SetNextItemWidth(7 * ImGui::GetFontSize());
            if (ImGui::InputInt(tr(lang, S::mission_sum_contents), &contents, 0, 0)) {
                r.h2 = static_cast<std::int16_t>(std::clamp(contents, -32768, 32767));
                changed = true;
            }
            ImGui::SameLine();
            int heading = r.h6;
            ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
            const bool turned = ImGui::InputInt(tr(lang, S::mission_sum_facing), &heading, 0, 0);
            point_undo_field(app, kind, index);
            if (turned) {
                r.h6 = static_cast<std::int16_t>(((heading % 360) + 360) % 360);
                changed = true;
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (changed) {
        std::string why;
        if (write_placed_set_over(chests ? v.edit.trbox : v.edit.evi, *set, &why)) {
            ++v.changes;
        } else {
            app.tell(why, false);
        }
    }
}

void draw_objective(App& app) {
    const auto lang = app.language;
    if (!section(tr(lang, S::mission_sum_objective)))
        return;
    section_note(tr(lang, S::mission_sum_objective_note));
    bool any = false;
    for (const auto& e : v.edit.mission.entries) {
        for (const auto& s : e.archive.scripts) {
            for (const auto& st : s.steps) {
                const auto kind = step_meaning(st, &s).kind;
                if (kind != StepMeaning::event_when && kind != StepMeaning::on_event &&
                    kind != StepMeaning::on_event_start)
                    continue;
                ImGui::BulletText("%s  (%s)", meaning_of(app, s, st).c_str(), s.name.c_str());
                any = true;
            }
        }
    }
    if (!any)
        ImGui::TextDisabled("%s", tr(lang, S::mission_sum_no_objective));
}

void draw_summary(App& app) {
    const auto lang = app.language;
    const Placed p = placed();
    const World& world = mission_world(p.all.world);
    ImGui::BeginChild("##summary", ImVec2(0, 0));
    // where it happens
    ImGui::PushID("where");
    if (section(tr(lang, S::mission_sum_where))) {
        std::string rooms;
        for (const auto& [slot, room] : p.all.rooms)
            rooms += (rooms.empty() ? "" : ", ") + std::to_string(room);
        const char* code = world_code(p.all.world);
        ImGui::TextUnformatted(
            fmt(lang, S::mission_sum_world, {code != nullptr ? code : "?", rooms.empty() ? std::string("-") : rooms})
                .c_str());
    }
    ImGui::PopID();
    // each section in an ID space of its own (their rows number from 0 alike)
    ImGui::PushID("texts");
    draw_texts(app);
    ImGui::PopID();
    ImGui::PushID("starts");
    draw_starts(app, p, world);
    ImGui::PopID();
    ImGui::PushID("time");
    draw_time(app);
    ImGui::PopID();
    ImGui::PushID("enemies");
    draw_enemies(app, p, world);
    ImGui::PopID();
    ImGui::PushID("chests");
    draw_placed(app, p, world, true);
    ImGui::PopID();
    ImGui::PushID("emblems");
    draw_placed(app, p, world, false);
    ImGui::PopID();
    draw_objective(app);
    ImGui::EndChild();
}

// --- the scripts --------------------------------------------------------------------------

void draw_places(App& app, const Script& s) {
    const auto lang = app.language;
    const ScriptPlaces p = script_places(s);
    if (p.world < 0 && p.spawners.empty() && p.starts.empty())
        return;
    if (!ImGui::CollapsingHeader(tr(lang, S::mission_places), ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const World& world = mission_world(p.world);
    const char* code = world_code(p.world);
    if (code != nullptr) {
        std::string rooms;
        for (const auto& [slot, room] : p.rooms)
            rooms += (rooms.empty() ? "" : ", ") + std::to_string(room);
        ImGui::TextUnformatted(
            fmt(lang, S::mission_places_world, {code, rooms.empty() ? std::string("-") : rooms}).c_str());
    }
    int id = 0;
    // a line of the list: the button first (a long line never hides it), then the text, wrapped
    const auto place_line = [&](int room, const std::string& text) {
        ImGui::PushID(id++);
        if (room >= 0) {
            if (ImGui::SmallButton(tr(lang, S::mission_see_in_room)))
                see_in_room(app, world, p, room);
        } else {
            ImGui::Bullet();
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopID();
    };
    for (const auto& st : p.starts) {
        if (!st.marker.empty()) {
            place_line(-1, fmt(lang, S::mission_start_marker_line, {std::to_string(st.player), st.marker}));
            continue;
        }
        const int room = world.code.empty() ? -1 : room_under(world, p.rooms, st.at);
        place_line(room, fmt(lang, S::mission_start_line,
                             {std::to_string(st.player), point_text(st.at), std::to_string(st.heading),
                              room < 0 ? std::string("?") : std::to_string(room)}));
    }
    EnemyTable table;
    const int em = v.edit.mission.entry("em0");
    if (em >= 0)
        parse_enemy_table(v.edit.mission.p2.entries[static_cast<std::size_t>(em)].data, &table, nullptr);
    for (const auto& sp : p.spawners) {
        std::string who;
        if (sp.group >= 0 && static_cast<std::size_t>(sp.group) < table.groups.size()) {
            for (const auto& part : table.groups[static_cast<std::size_t>(sp.group)].parts) {
                who += (who.empty() ? "" : " + ") + std::to_string(part.spawned()) + " x " +
                       enemy_text(app, table, part.row);
            }
        }
        const int room = world.code.empty() ? -1 : room_under(world, p.rooms, sp.at);
        place_line(room, fmt(lang, S::mission_spawner_line,
                             {std::to_string(sp.group), who.empty() ? std::string("?") : who, point_text(sp.at),
                              room < 0 ? std::string("?") : std::to_string(room)}));
    }
}

void draw_scripts(App& app) {
    const auto lang = app.language;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr(lang, S::mission_hint));
    ImGui::PopTextWrapPos();
    const MissionFile& mission = v.edit.mission;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // the entries and their scripts
    ImGui::BeginChild("##scripts", ImVec2(avail.x * 0.24f, avail.y), ImGuiChildFlags_Borders);
    for (std::size_t e = 0; e < mission.entries.size(); ++e) {
        const auto& entry = mission.entries[e];
        const std::string name = e < mission.entry_names.size() ? mission.entry_names[e] : std::string();
        if (entry.kind != MissionEntry::script_archive) {
            const std::string label = name + "  (" + std::to_string(mission.p2.entries[e].data.size()) + " B)";
            if (name == "em0" || name == "em0m") {
                if (ImGui::Selectable((label + "##table" + std::to_string(e)).c_str(),
                                      v.table == static_cast<int>(e))) {
                    v.table = static_cast<int>(e);
                    v.entry = v.script = -1;
                }
            } else {
                ImGui::TextDisabled("%s", label.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "%s", fmt(lang, entry.kind == MissionEntry::empty ? S::mission_entry_empty : S::mission_entry_table,
                              {std::to_string(e), std::to_string(mission.p2.entries[e].data.size())})
                              .c_str());
            }
            continue;
        }
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        const bool open = ImGui::TreeNode(
            (name + "  (" + std::to_string(entry.archive.scripts.size()) + ")##e" + std::to_string(e)).c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", fmt(lang, S::mission_entry_named,
                                        {std::to_string(e), name, std::to_string(entry.archive.scripts.size())})
                                        .c_str());
        }
        if (open) {
            if (name.rfind("_s", 0) == 0)
                ImGui::TextDisabled("%s", tr(lang, S::mission_start));
            for (std::size_t i = 0; i < entry.archive.scripts.size(); ++i) {
                const Script& s = entry.archive.scripts[i];
                const std::string label = (s.name.empty() ? std::string("-") : s.name) + "  (" +
                                          std::to_string(s.steps.size()) + ")##" + std::to_string(e) + ":" +
                                          std::to_string(i);
                if (ImGui::Selectable(label.c_str(),
                                      v.entry == static_cast<int>(e) && v.script == static_cast<int>(i))) {
                    v.entry = static_cast<int>(e);
                    v.script = static_cast<int>(i);
                    v.table = -1;
                }
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // the chosen script's steps
    ImGui::BeginChild("##steps", ImVec2(0, avail.y), ImGuiChildFlags_Borders);
    if (v.table >= 0) {
        EnemyTable t;
        std::string why;
        const std::string name = mission.entry_names[static_cast<std::size_t>(v.table)];
        if (!parse_enemy_table(mission.p2.entries[static_cast<std::size_t>(v.table)].data, &t, &why)) {
            ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1), "%s", why.c_str());
        } else {
            ImGui::TextUnformatted(fmt(lang, S::mission_enemy_table,
                                       {name, std::to_string(t.kinds.size()), std::to_string(t.groups.size())})
                                       .c_str());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", tr(lang, S::mission_enemy_hint));
            ImGui::PopTextWrapPos();
            for (std::size_t r = 0; r < t.kinds.size(); ++r) {
                const EnemyKind& k = t.kinds[r];
                ImGui::BulletText("%s  %s", enemy_text(app, t, r).c_str(),
                                  fmt(lang, S::mission_enemy_row,
                                      {std::to_string(r), std::to_string(k.id()), std::to_string(k.kind()),
                                       std::to_string(k.high()), std::to_string(k.raw[2]), std::to_string(k.raw[3]),
                                       std::to_string(static_cast<std::int8_t>(k.raw[4])), std::to_string(k.param()),
                                       std::to_string(k.drop_key()), std::to_string(k.drop_chance())})
                                      .c_str());
            }
            ImGui::Separator();
            for (std::size_t g = 0; g < t.groups.size(); ++g) {
                std::string parts;
                for (const auto& part : t.groups[g].parts) {
                    parts += (parts.empty() ? "" : " + ") + std::to_string(part.spawned()) + " x " +
                             enemy_text(app, t, part.row);
                }
                ImGui::BulletText("%s", fmt(lang, S::mission_enemy_group, {std::to_string(g), parts}).c_str());
            }
        }
        ImGui::EndChild();
        return;
    }
    if (v.entry < 0 || v.script < 0) {
        ImGui::TextDisabled("%s", tr(lang, S::mission_choose_script));
        ImGui::EndChild();
        return;
    }
    const Script& s =
        mission.entries[static_cast<std::size_t>(v.entry)].archive.scripts[static_cast<std::size_t>(v.script)];
    ImGui::TextUnformatted(
        fmt(lang, S::mission_script_title,
            {s.name, std::to_string(v.entry), std::to_string(v.script), std::to_string(s.steps.size())})
            .c_str());
    draw_places(app, s);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", tr(lang, S::mission_filter), &v.filter);
    // the steps shown: each with its byte offset in the script
    std::vector<std::size_t> shown, offsets(s.steps.size());
    std::size_t at = 4;
    const std::string want = lower_text(v.filter);
    for (std::size_t k = 0; k < s.steps.size(); ++k) {
        offsets[k] = at;
        at += 4 * s.steps[k].length();
        if (want.empty() ||
            lower_text(function_of(app, s.steps[k]) + " " + operands_text(s.steps[k], &s, game_language(lang)) + " " +
                       meaning_of(app, s, s.steps[k]))
                    .find(want) != std::string::npos) {
            shown.push_back(k);
        }
    }
    const ImGuiTableFlags flags =
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##steptable", 5, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("g.c", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);  // the handlers' names are long
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr(lang, S::mission_meaning), ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const std::size_t k = shown[static_cast<std::size_t>(row)];
                const ScriptStep& st = s.steps[k];
                const CommandName* n = command_name(st.group, st.command);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%05zx", offsets[k]);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d.%d", st.group, st.command);
                ImGui::TableSetColumnIndex(2);
                if (n == nullptr || n->handler[0] == '\0') {
                    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "%s", function_of(app, st).c_str());
                } else {
                    ImGui::TextUnformatted(n->handler);
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(operands_text(st, &s, game_language(lang)).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(meaning_of(app, s, st).c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

}  // namespace

MissionOverlay& mission_overlay() {
    static MissionOverlay overlay;
    return overlay;
}

bool mission_point(const App& app, MissionPoint::Kind kind, int index, MissionPointPlace* out) {
    if (v.item < 0 || !v.error.empty() || index < 0)
        return false;
    const auto lang = app.language;
    const auto i = static_cast<std::size_t>(index);
    MissionPointPlace pp;
    switch (kind) {
    case MissionPoint::spawner: {
        const Placed p = placed();
        if (i >= p.spawners.size())
            return false;
        pp.at = p.spawners[i].second.at;
        pp.name = fmt(lang, S::mission_tip_spawner, {std::to_string(i + 1), std::to_string(p.spawners[i].second.group)});
        break;
    }
    case MissionPoint::start: {
        const Placed p = placed();
        if (i >= p.starts.size() || !p.starts[i].second.marker.empty())
            return false;
        pp.at = p.starts[i].second.at;
        pp.has_facing = true;
        pp.facing = p.starts[i].second.heading;
        pp.name = fmt(lang, S::mission_sum_start, {std::to_string(p.starts[i].second.player)});
        break;
    }
    case MissionPoint::chest:
    case MissionPoint::emblem: {
        const auto* rows = kind == MissionPoint::chest ? chest_rows() : emblem_rows();
        if (rows == nullptr || i >= rows->size())
            return false;
        const PlacedRow& r = (*rows)[i];
        pp.at = r.at;
        if (kind == MissionPoint::chest) {
            pp.has_facing = true;
            pp.facing = r.h6;
            pp.name = fmt(lang, S::mission_tip_chest, {std::to_string(i + 1)});
        } else {
            pp.name = fmt(lang, S::mission_tip_emblem, {std::to_string(i + 1), std::to_string(r.b1)});
        }
        break;
    }
    }
    *out = std::move(pp);
    return true;
}

void mission_point_move(App& app, MissionPoint::Kind kind, int index, const std::array<std::int32_t, 3>& at,
                        int facing) {
    if (v.item < 0 || !v.error.empty() || index < 0)
        return;
    const auto i = static_cast<std::size_t>(index);
    const int degrees = ((facing % 360) + 360) % 360;
    switch (kind) {
    case MissionPoint::spawner: {
        const Placed p = placed();
        if (i >= p.spawners.size())
            return;
        ScriptStep& step = step_of(p.spawners[i].first);
        set_fx(step, 5, at[0]);
        set_fx(step, 6, at[1]);
        set_fx(step, 7, at[2]);
        break;
    }
    case MissionPoint::start: {
        const Placed p = placed();
        if (i >= p.starts.size() || !p.starts[i].second.marker.empty())
            return;
        ScriptStep& step = step_of(p.starts[i].first);
        set_fx(step, 3, at[0]);
        set_fx(step, 4, at[1]);
        set_fx(step, 5, at[2]);
        set_int(step, 6, degrees);
        break;
    }
    case MissionPoint::chest:
    case MissionPoint::emblem: {
        const bool chests = kind == MissionPoint::chest;
        PlacedSet* set = placed_set_of(chests ? v.edit.trbox_sets : v.edit.evi_sets, v.edit.number);
        if (set == nullptr || i >= set->rows.size())
            return;
        set->rows[i].at = at;
        if (chests)
            set->rows[i].h6 = static_cast<std::int16_t>(degrees);
        std::string why;
        if (!write_placed_set_over(chests ? v.edit.trbox : v.edit.evi, *set, &why)) {
            app.tell(why, false);
            return;
        }
        break;
    }
    }
    ++v.changes;
}

void mission_point_push(const App& app, MissionPoint::Kind kind, int index) {
    MissionPointPlace now;
    if (!mission_point(app, kind, index, &now))
        return;
    point_undo.push_back({kind, index, now.at, now.facing});
    if (point_undo.size() > 200)
        point_undo.erase(point_undo.begin());
    point_parked = std::move(point_redo);
    point_redo.clear();
    history_push(EditOwner::mission);
}

void mission_point_unpush() {
    if (point_undo.empty())
        return;
    point_undo.pop_back();
    point_redo = std::move(point_parked);
    point_parked.clear();
    history_unpush(EditOwner::mission);
}

bool mission_point_undo(App& app, bool redo) {
    auto& from = redo ? point_redo : point_undo;
    auto& to = redo ? point_undo : point_redo;
    while (!from.empty()) {
        const PointStep s = from.back();
        from.pop_back();
        MissionPointPlace now;
        if (!mission_point(app, s.kind, s.index, &now))
            continue;  // no longer there (the mission read again)
        to.push_back({s.kind, s.index, now.at, now.facing});
        mission_point_move(app, s.kind, s.index, s.at, s.facing);
        return true;
    }
    return false;
}

namespace {

// What the game says of a mission file: its record in the story's mission list (when it has one) and the
// worlds its scripts load, with the words the catalog and the mission's header show.
struct MissionInfo {
    bool listed = false, story = false;
    int number = 0, first_day = 0, last_day = 0;
    std::string name;
    std::vector<int> worlds;  // world_code's indices
    std::string days;         // "Days 51-54", "Mission Mode", empty when not listed
    std::string worlds_text;  // "Twilight Town", "4 worlds", empty when none
    std::string worlds_all;   // every world's name: "Wonderland, Halloween Town, ..."
    std::string label;        // the catalog's: days · name · world
};

const char* const kLanguageCodes[] = {"en", "fr", "de", "it", "es"};

// Every mission file's info in the interface's language: the list and the worlds' names read once per
// language (English when that language's cannot be read), the worlds the scripts load once (the game's own
// files; about 0.2 s for all of them).
const std::map<int, MissionInfo>& mission_infos(const App& app) {
    static int made_for = -1;
    static std::map<int, MissionInfo> infos;
    static std::map<int, std::vector<int>> worlds;
    static bool worlds_read = false;
    const auto lang = app.language;
    if (made_for == static_cast<int>(lang))
        return infos;
    made_for = static_cast<int>(lang);
    infos.clear();
    if (!worlds_read) {
        worlds_read = true;
        for (const int number : mission_numbers(nitrofs())) {
            MissionFile mf;
            if (load_mission(nitrofs(), number, &mf, nullptr))
                worlds[number] = mission_worlds(mf);
        }
    }
    const std::string code = kLanguageCodes[static_cast<std::size_t>(lang)];
    std::vector<MissionListEntry> list;
    if (!load_mission_list(nitrofs(), code, &list, nullptr))
        load_mission_list(nitrofs(), "en", &list, nullptr);
    std::vector<std::string> world_names;
    if (!load_world_names(nitrofs(), code, &world_names, nullptr))
        load_world_names(nitrofs(), "en", &world_names, nullptr);
    const auto world_name = [&](int index) -> std::string {
        if (index >= 0 && static_cast<std::size_t>(index) < world_names.size())
            return world_names[static_cast<std::size_t>(index)];
        const char* c = world_code(index);
        return c != nullptr ? c : "?";
    };
    for (const auto& [number, its_worlds] : worlds) {
        MissionInfo& in = infos[number];
        in.worlds = its_worlds;
        for (const int w : its_worlds)
            in.worlds_all += (in.worlds_all.empty() ? "" : ", ") + world_name(w);
        in.worlds_text = its_worlds.size() > 1 ? fmt(lang, S::mission_tree_worlds, {std::to_string(its_worlds.size())})
                                               : in.worlds_all;
    }
    std::map<int, std::vector<const MissionListEntry*>> by_file;
    for (const auto& e : list)
        by_file[e.file].push_back(&e);
    constexpr int kStoryDays = 10000, kMissionModeDay = 400;
    const auto days = [&](int first, int last) {
        return first == last ? fmt(lang, S::mission_tree_day, {std::to_string(first)})
                             : fmt(lang, S::mission_tree_days, {std::to_string(first), std::to_string(last)});
    };
    for (const auto& [file, records] : by_file) {
        MissionInfo& in = infos[file];
        const MissionListEntry& e = *records.front();  // the first record, as the game lists it
        in.listed = true;
        in.number = e.number;
        in.first_day = e.first_day;
        in.last_day = e.last_day;
        if (file >= kStoryDays) {
            in.story = true;
            for (const auto* r : records) {
                in.first_day = std::min(in.first_day, r->first_day);
                in.last_day = std::max(in.last_day, r->last_day);
            }
            in.name = tr(lang, S::mission_tree_story);
            in.days = days(in.first_day, in.last_day);
        } else if (e.first_day >= kMissionModeDay) {
            in.name = e.name;
            in.days = tr(lang, S::mission_tree_mode);
        } else {
            in.name = e.name;
            in.days = days(e.first_day, e.last_day);
        }
    }
    for (auto& [number, in] : infos) {
        for (const std::string* part : {&in.days, &in.name, &in.worlds_text}) {
            if (!part->empty())
                in.label += (in.label.empty() ? "" : " · ") + *part;
        }
    }
    return infos;
}

const MissionInfo* mission_info(const App& app, int number) {
    const auto& infos = mission_infos(app);
    const auto found = infos.find(number);
    return found != infos.end() ? &found->second : nullptr;
}

int number_of(const Item& item) {
    const auto slash = item.key.find_last_of('/');
    return std::atoi(item.key.c_str() + (slash == std::string::npos ? 0 : slash + 1));
}

// Over the mission's view: its number and name, then its days, its worlds and how many spawners, chests and
// emblems it has now (the edit's, so they follow what is changed).
void draw_header(App& app) {
    const auto lang = app.language;
    const MissionInfo* in = mission_info(app, v.edit.number);
    const ImVec4 title_colour = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    if (in != nullptr && in->listed && !in->story) {
        ImGui::TextColored(title_colour, "%s",
                           fmt(lang, S::mission_head_title, {std::to_string(in->number), in->name}).c_str());
    } else if (in != nullptr && in->story) {
        ImGui::TextColored(title_colour, "%s", in->name.c_str());
    } else {
        ImGui::TextColored(title_colour, "%s", tr(lang, S::mission_head_unlisted));
    }
    std::string line;
    const auto add = [&](const std::string& part) {
        if (!part.empty())
            line += (line.empty() ? "" : " · ") + part;
    };
    if (in != nullptr) {
        add(in->days);
        std::string where = in->worlds_all;
        if (in->worlds.size() == 1 && world_code(in->worlds[0]) != nullptr)
            where += std::string(" (") + world_code(in->worlds[0]) + ")";
        add(where);
    }
    const Placed p = placed();
    const auto* chests = chest_rows();
    const auto* emblems = emblem_rows();
    add(fmt(lang, S::mission_head_counts,
            {std::to_string(p.spawners.size()), std::to_string(chests != nullptr ? chests->size() : 0),
             std::to_string(emblems != nullptr ? emblems->size() : 0)}));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopTextWrapPos();
}

}  // namespace

std::string world_folder_label(const App& app, const std::string& folder) {
    if (folder == "wd")
        return tr(app.language, S::tree_worlds);
    if (folder.size() != 5 || folder.rfind("wd_", 0) != 0)
        return {};
    // the game's own names of its worlds, in the interface's language (English when it has none)
    static int read_for = -1;
    static std::vector<std::string> names;
    if (read_for != static_cast<int>(app.language)) {
        read_for = static_cast<int>(app.language);
        names.clear();
        if (!load_world_names(nitrofs(), kLanguageCodes[static_cast<std::size_t>(app.language)], &names, nullptr))
            load_world_names(nitrofs(), "en", &names, nullptr);
    }
    const std::string code = folder.substr(3);
    for (int i = 0; world_code(i) != nullptr; ++i) {
        if (code == world_code(i))
            return static_cast<std::size_t>(i) < names.size() ? names[static_cast<std::size_t>(i)] : std::string();
    }
    return {};
}

const std::string& room_list_label(const App& app, const Item& item) {
    static const std::string none;
    if (item.kind != Kind::room)
        return none;
    // the pairs (the studio's, then the mod's), read again when the mod or the language changes
    static std::string read_for = "?";
    static std::map<std::string, std::string> labels;  // key -> label
    const Mod* mod = app.current_mod();
    const std::string now = (mod != nullptr ? u8(mod->dir) : std::string()) + "|" +
                            std::to_string(static_cast<int>(app.language));
    if (read_for != now) {
        read_for = now;
        labels.clear();
        for (const Kh2Pair& p : kh2_pairs(mod != nullptr ? mod->dir : fs::path(), nullptr)) {
            // only rooms compared with their map, and maps KH2 names
            const char* name = kh2_place_name(p.map);
            if (name == nullptr || !kh2_pair_seen_alike(p))
                continue;
            labels["mi/wd/wd_" + p.world + "/room/" + std::to_string(p.room)] =
                fmt(app.language, S::tree_room_kh2, {name, p.map});
        }
    }
    const auto found = labels.find(item.key);
    return found != labels.end() ? found->second : none;
}

const std::string& mission_list_label(const App& app, const Item& item) {
    static const std::string none;
    if (item.kind != Kind::mission)
        return none;
    const MissionInfo* in = mission_info(app, number_of(item));
    return in != nullptr ? in->label : none;
}

void draw_mission_view(App& app, const Item& item) {
    const Mod* m = app.current_mod();
    const std::string mod_name = m != nullptr ? m->name : std::string();
    // read again when another mission or mod is chosen (an unsaved change kept while the same is shown)
    if (v.item != index_of(app, item) || v.mod != mod_name)
        open(app, item);
    const auto lang = app.language;
    if (!v.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1), "%s", v.error.c_str());
        return;
    }
    // the map's points of this mission follow its values as they are changed, whichever tab is shown
    {
        MissionOverlay& mo = mission_overlay();
        const Placed p = placed();
        const World& world = mission_world(p.all.world);
        if (mo.mission == v.item && mo.room >= 0 && mo.world == world.code)
            fill_overlay(app, world, p.all, mo.room, mo);
    }
    // Undo / Redo of the points while the mission is shown alone (beside a room, the map's keys do it)
    const ImGuiIO& io = ImGui::GetIO();
    if (app.selected == v.item && !io.WantTextInput && !ImGui::IsAnyItemActive() && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
            history_step(app, io.KeyShift, false);
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            history_step(app, true, false);
    }
    draw_header(app);
    ImGui::Separator();
    ImGui::TextDisabled("%s", fmt(lang, S::mission_header,
                                  {mission_path(v.edit.number), std::to_string(v.edit.mission.entries.size()),
                                   std::to_string(v.scripts), std::to_string(v.steps)})
                                  .c_str());
    // whose version is shown, and saving it
    const bool from_mod = v.edit.mission_from_mod;
    ImGui::TextUnformatted(from_mod ? fmt(lang, S::mission_from_mod, {mod_name}).c_str()
                                    : tr(lang, S::mission_from_game));
    if (m == nullptr) {
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1), "%s", tr(lang, S::choose_mod_first));
    } else {
        ImGui::BeginDisabled(v.changes == 0);
        if (ImGui::Button(fmt(lang, S::mission_save, {std::to_string(v.changes)}).c_str())) {
            std::vector<std::string> written;
            std::string why;
            if (save_mission_edit(nitrofs(), m->dir, v.edit, &written, &why)) {
                std::string list;
                for (const auto& w : written)
                    list += (list.empty() ? "" : ", ") + w;
                app.tell(fmt(lang, S::mission_saved, {list.empty() ? std::string("-") : list}) + "  " +
                             tr(lang, S::restart_note),
                         true);
                v.item = -1;  // read again from what the mod now holds
            } else {
                app.tell(fmt(lang, S::save_failed, {why}), false);
            }
        }
        ImGui::EndDisabled();
        if (v.changes > 0) {
            same_line_if_room(button_width(tr(lang, S::mission_undo)));
            if (ImGui::Button(tr(lang, S::mission_undo)))
                v.item = -1;  // read again: the changes dropped
        }
        // a new mission made from this one, in the slot of another
        same_line_if_room(button_width(tr(lang, S::mission_copy)));
        if (ImGui::Button(tr(lang, S::mission_copy)))
            ImGui::OpenPopup("###copy");
        if (ImGui::BeginPopupModal((std::string(tr(lang, S::mission_copy)) + "###copy").c_str(), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            static int target = -1;
            static std::vector<int> numbers;
            if (numbers.empty())
                numbers = mission_numbers(nitrofs());
            ImGui::PushTextWrapPos(40 * ImGui::GetFontSize());
            ImGui::TextUnformatted(fmt(lang, S::mission_copy_note, {mission_path(v.edit.number)}).c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetNextItemWidth(14 * ImGui::GetFontSize());
            if (ImGui::BeginCombo(tr(lang, S::mission_copy_to), target < 0 ? "-" : mission_path(target).c_str())) {
                for (const int n : numbers) {
                    if (n == v.edit.number)
                        continue;
                    if (ImGui::Selectable(mission_path(n).c_str(), n == target))
                        target = n;
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(target < 0);
            if (ImGui::Button(tr(lang, S::mission_copy_do))) {
                std::vector<std::string> written;
                std::string why;
                bool sets = false;
                if (copy_mission_to(nitrofs(), m->dir, v.edit, target, &written, &sets, &why)) {
                    app.tell(fmt(lang, S::mission_copied, {mission_path(v.edit.number), mission_path(target)}) +
                                 (sets ? std::string() : "  " + std::string(tr(lang, S::mission_copied_sets_kept))) +
                                 "  " + tr(lang, S::restart_note),
                             true);
                    // the new mission shown
                    const std::string key = mission_path(target);
                    for (std::size_t i = 0; i < app.catalog.items.size(); ++i) {
                        if (app.catalog.items[i].kind == Kind::mission && app.catalog.items[i].key == key)
                            app.selected = static_cast<int>(i);
                    }
                    v.item = -1;
                    ImGui::CloseCurrentPopup();
                } else {
                    app.tell(fmt(lang, S::save_failed, {why}), false);
                }
            }
            ImGui::EndDisabled();
            same_line_if_room(button_width(tr(lang, S::cancel)));
            if (ImGui::Button(tr(lang, S::cancel)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (from_mod) {
            same_line_if_room(button_width(tr(lang, S::mission_give_back)));
            if (ImGui::Button(tr(lang, S::mission_give_back))) {
                std::string why;
                if (give_back_mission(nitrofs(), m->dir, v.edit.number, &why)) {
                    app.tell(fmt(lang, S::mission_given_back, {mission_path(v.edit.number), m->name}), true);
                    v.item = -1;
                } else {
                    app.tell(fmt(lang, S::save_failed, {why}), false);
                }
            }
        }
    }
    if (ImGui::BeginTabBar("##missiontabs")) {
        if (ImGui::BeginTabItem(tr(lang, S::mission_tab_summary))) {
            draw_summary(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr(lang, S::mission_tab_scripts))) {
            draw_scripts(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace studio
