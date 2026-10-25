// The studio's colour palettes (the Themes window): the five of the maintainer's
// MoM viewer, in the colours of the Kingdom Hearts games' menus. Only the
// studio's look.
#pragma once

#include "strings.h"

#include "imgui.h"

#include <string>

namespace studio {

// How many palettes there are.
int theme_count();

// A palette's id as studio.ini keeps it ("kh1", "kh2", "kh3", "days", "mom", "rose-pine-moon").
const char* theme_id(int index);

// A palette's name in the list (the games' own names).
const char* theme_label(int index, Language lang);

// The index of `id`; MoM's when it names none (an empty setting), as the viewer.
int theme_index(const std::string& id);

// Sets ImGui's colours and roundings to the palette `id` (sizes scaled by the
// style's FontScaleDpi).
void apply_theme(const std::string& id);

// The page behind the panels (the studio's main window), of the palette applied.
ImVec4 theme_page();

// A button that saves into the mod drawn green, for the palette applied: push before the button, pop after
// (four colours: the button, under the pointer, pressed, and its text).
void push_save_colours();
void pop_save_colours();

}  // namespace studio
