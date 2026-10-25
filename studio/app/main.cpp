// khdays-studio: the window, its frame loop, and the command line
// when it is given one.
#include "app.h"
#include "cli.h"
#include "views/console.h"
#include "theme.h"

#include "core/files/paths.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdio>

namespace studio {
namespace {

void apply_language(App& app) {
    app.language =
        app.settings.language.empty() ? system_language() : language_from_code(app.settings.language, Language::en);
}

void take_dialog_files(App& app) {
    std::vector<std::string> files, clips, exports, grids, rooms, models, sheet_folders, texture_folders,
        texture_exports;
    {
        std::lock_guard lock(app.dialog_mutex);
        files.swap(app.dialog_files);
        clips.swap(app.dialog_clips);
        exports.swap(app.dialog_exports);
        grids.swap(app.dialog_grids);
        rooms.swap(app.dialog_room_exports);
        models.swap(app.dialog_model_exports);
        sheet_folders.swap(app.dialog_sheet_folders);
        texture_folders.swap(app.dialog_texture_folders);
        texture_exports.swap(app.dialog_texture_exports);
    }
    if (!sheet_folders.empty())
        sheets_export_to(app, path_u8(sheet_folders.front()));
    if (!texture_folders.empty())
        textures_export_to(app, path_u8(texture_folders.front()));
    if (!texture_exports.empty())
        texture_export_to(app, path_u8(texture_exports.front()));
    if (!rooms.empty())
        room_export_to(app, path_u8(rooms.front()));
    if (!models.empty())
        model_export_to(app, path_u8(models.front()));
    if (!files.empty())
        app.offer(path_u8(files.front()));
    if (!clips.empty())
        clip_offer(app, path_u8(clips.front()));
    if (!exports.empty())
        sheet_export_to(app, path_u8(exports.front()));
    if (!grids.empty())
        sheet_grid_export_to(app, path_u8(grids.front()));
}

void draw_notices(App& app);  // below

// The settings written now (a split let go of), a failure told.
void save_now(App& app) {
    std::string why;
    if (!save_settings(app.settings, &why))
        app.tell(fmt(app.language, S::save_failed, {why}), false);
}

constexpr float kSplitGrip = 8.0f;  // a split line's thickness to grab

// A line the modder drags between two panels, `length` long: `*at` (pixels) moved by the drag within
// `lo`..`hi`, back to `first` on a double click. `across`: a line across (dragged up and down), else upright.
// `always_drawn`: else only while pointed at or dragged. True when let go or reset: the time to save it.
bool split_line(const char* id, bool across, float length, float* at, float lo, float hi, float first,
                bool always_drawn) {
    const ImVec2 size =
        across ? ImVec2(std::max(1.0f, length), kSplitGrip) : ImVec2(kSplitGrip, std::max(1.0f, length));
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const bool reset = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (hovered || active)
        ImGui::SetMouseCursor(across ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
    if (active) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        *at = std::clamp(*at + (across ? d.y : d.x), lo, hi);
    }
    if (reset)
        *at = std::clamp(first, lo, hi);
    if (always_drawn || hovered || active) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImU32 colour = ImGui::GetColorU32(active    ? ImGuiCol_SeparatorActive
                                                : hovered ? ImGuiCol_SeparatorHovered
                                                          : ImGuiCol_Separator);
        const float thickness = hovered || active ? 3.0f : 1.0f;
        if (across) {
            const float mid = 0.5f * (a.y + b.y);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, mid), ImVec2(b.x, mid), colour, thickness);
        } else {
            const float mid = 0.5f * (a.x + b.x);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(mid, a.y), ImVec2(mid, b.y), colour, thickness);
        }
    }
    return ImGui::IsItemDeactivated() || reset;
}

// The mission kept open beside the shown room (See room), or null: let go once something else than a room
// is selected.
const Item* mission_beside(App& app) {
    const int n = static_cast<int>(app.catalog.items.size());
    const bool room = app.selected >= 0 && app.selected < n &&
                      app.catalog.items[static_cast<std::size_t>(app.selected)].kind == Kind::room;
    if (!room || app.mission_beside < 0 || app.mission_beside >= n ||
        app.catalog.items[static_cast<std::size_t>(app.mission_beside)].kind != Kind::mission) {
        app.mission_beside = -1;
        return nullptr;
    }
    return &app.catalog.items[static_cast<std::size_t>(app.mission_beside)];
}

// Under the catalog while a mission's room is shown: "Map editor" (the room's tools, folded at first), then
// the mission. Opened, the tools take the share of the height above a line the modder drags (this session).
void draw_room_and_mission(App& app, const Item& room, const Item& mission) {
    static float tools_share = 0.5f;
    const bool tools = ImGui::CollapsingHeader(tr(app.language, S::map_tools_header));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr(app.language, S::map_tools_hint));
    if (!tools) {
        draw_map_controls(app, room, false);  // its keys still work
    } else {
        const float height = ImGui::GetContentRegionAvail().y;
        const float lowest = std::min(120.0f, 0.5f * (height - kSplitGrip));
        const float highest = std::max(lowest, height - kSplitGrip - lowest);
        float top = std::clamp(tools_share * height, lowest, highest);
        ImGui::BeginChild("##map_tools", ImVec2(0, top), ImGuiChildFlags_None);
        draw_map_controls(app, room);
        ImGui::EndChild();
        split_line("##map_tools_split", true, ImGui::GetContentRegionAvail().x, &top, lowest, highest, 0.5f * height,
                   true);
        if (height > 0.0f && ImGui::IsItemActive())
            tools_share = std::clamp(top / height, 0.01f, 1.0f);
    }
    ImGui::Separator();
    ImGui::PushID("mission_beside");
    ImGui::BeginChild("##mission", ImVec2(0, 0), ImGuiChildFlags_None);
    draw_mission_view(app, mission);
    ImGui::EndChild();
    ImGui::PopID();
}

void draw_frame(App& app, bool* settings_open, bool* themes_open, bool* console_open) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme_page());  // the page behind the panels
    ImGui::Begin("##studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    map_save_poll(app);
    draw_mod_bar(app);
    // the themes and the settings at the right end of the same row
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons =
        ImGui::CalcTextSize(tr(app.language, S::console)).x + ImGui::CalcTextSize(tr(app.language, S::themes)).x +
        ImGui::CalcTextSize(tr(app.language, S::settings)).x + style.FramePadding.x * 6 + style.ItemSpacing.x * 2;
    ImGui::SameLine();
    const float room = ImGui::GetContentRegionAvail().x;
    if (room > buttons)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - buttons);
    if (ImGui::Button(tr(app.language, S::console)))
        *console_open = true;
    ImGui::SameLine();
    if (ImGui::Button(tr(app.language, S::themes)))
        *themes_open = true;
    ImGui::SameLine();
    if (ImGui::Button(tr(app.language, S::settings)))
        *settings_open = true;
    ImGui::Separator();

    const float status_h = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // the catalog takes a share of the width the modder drags at the line between the panels, each panel at
    // least 200 wide
    const float narrowest = std::min(200.0f, 0.5f * (avail.x - kSplitGrip));
    const float widest = std::max(narrowest, avail.x - kSplitGrip - narrowest);
    float left = std::clamp(app.settings.browser_width * avail.x, narrowest, widest);
    // what is wider than a panel can be scrolled to, never hidden past its edge
    ImGui::BeginChild("##left", ImVec2(left, avail.y - status_h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    mission_beside(app);  // let go when no room is selected any more
    const Item* shown_room = app.selected >= 0 && app.selected < static_cast<int>(app.catalog.items.size()) &&
                                     app.catalog.items[static_cast<std::size_t>(app.selected)].kind == Kind::room
                                 ? &app.catalog.items[static_cast<std::size_t>(app.selected)]
                                 : nullptr;
    if (shown_room == nullptr) {
        // the same child as with a room shown: ImGui keeps the folders' open state per window, so the catalog
        // keeps its open folders whatever is shown
        ImGui::BeginChild("##catalog", ImVec2(0, 0), ImGuiChildFlags_None);
        draw_browser(app);
        ImGui::EndChild();
    } else {
        // a room shown: the catalog over its controls, the line between them dragged, each at least 120 high
        const float height = ImGui::GetContentRegionAvail().y;
        const float lowest = std::min(120.0f, 0.5f * (height - kSplitGrip));
        const float highest = std::max(lowest, height - kSplitGrip - lowest);
        float top = std::clamp(app.settings.map_tree * height, lowest, highest);
        const int room_selected = app.selected;
        ImGui::BeginChild("##catalog", ImVec2(0, top), ImGuiChildFlags_None);
        draw_browser(app);
        ImGui::EndChild();
        // the share kept only when the modder moves the line (a small window does not shrink it for good)
        const bool let_go = split_line("##map_tree_split", true, ImGui::GetContentRegionAvail().x, &top, lowest,
                                       highest, 0.45f * height, true);
        if (height > 0.0f && (let_go || ImGui::IsItemActive()))
            app.settings.map_tree = std::clamp(top / height, 0.01f, 1.0f);
        if (let_go)
            save_now(app);
        // the room's controls, unless the catalog picked something else this frame (shown from the next); with
        // a mission kept beside it, the mission under the room's tools, which start folded away
        ImGui::BeginChild("##room_controls", ImVec2(0, 0), ImGuiChildFlags_None);
        if (app.selected == room_selected) {
            const Item* mission = mission_beside(app);
            if (mission == nullptr) {
                draw_map_controls(app, *shown_room);
            } else {
                draw_room_and_mission(app, *shown_room, *mission);
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    // drawn only while pointed at or dragged: the panels' borders already show where it is
    ImGui::SameLine(0.0f, 0.0f);
    const bool let_go =
        split_line("##browser_split", false, avail.y - status_h, &left, narrowest, widest, 0.42f * avail.x, false);
    if (avail.x > 0.0f && (let_go || ImGui::IsItemActive()))
        app.settings.browser_width = std::clamp(left / avail.x, 0.01f, 1.0f);
    if (let_go)
        save_now(app);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##right", ImVec2(0, avail.y - status_h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    draw_item(app);
    ImGui::EndChild();
    ImGui::TextUnformatted(app.status.c_str());
    ImGui::End();
    draw_notices(app);

    if (*settings_open)
        draw_settings(app, settings_open);
    if (*themes_open)
        draw_themes(app, themes_open);
    if (*console_open)
        draw_mods_console(app, console_open);
}

// The results a person must see (App::tell), over the window: a success in
// green for six seconds, a failure in red until it is closed.
void draw_notices(App& app) {
    const std::uint64_t now = SDL_GetTicks();
    app.notices.erase(std::remove_if(app.notices.begin(), app.notices.end(),
                                     [&](const App::Notice& n) { return n.ok && now - n.since > 6000; }),
                      app.notices.end());
    if (app.notices.empty())
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float k = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 48.0f * k), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(vp->WorkSize.x * 0.7f, vp->WorkSize.y * 0.5f));
    ImGui::Begin("##notices", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    for (std::size_t i = 0; i < app.notices.size();) {
        const App::Notice& n = app.notices[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, n.ok ? ImVec4(0.10f, 0.42f, 0.18f, 1) : ImVec4(0.55f, 0.12f, 0.12f, 1));
        ImGui::BeginChild("##n", ImVec2(vp->WorkSize.x * 0.6f, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 90.0f * k);
        ImGui::TextUnformatted(n.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f * k);
        const bool close = ImGui::Button(tr(app.language, S::close));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (close) {
            app.notices.erase(app.notices.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    ImGui::End();
}

int run_window(App& app) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "khdays-studio: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) > 0
                            ? SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay())
                            : 1.0f;
    app.window = SDL_CreateWindow("khdays-studio", static_cast<int>(1400 * scale), static_cast<int>(860 * scale),
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (app.window == nullptr) {
        std::fprintf(stderr, "khdays-studio: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowPosition(app.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(app.window);
    // the formats the port's own device asks for (port/host/render/gpu_present.c)
    app.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL,
                                     false, nullptr);
    if (app.device == nullptr || !SDL_ClaimWindowForGPUDevice(app.device, app.window)) {
        const std::string why = SDL_GetError();
        std::fprintf(stderr, "khdays-studio: no graphics device: %s\n", why.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "khdays-studio", why.c_str(), app.window);
        return 1;
    }
    SDL_SetGPUSwapchainParameters(app.device, app.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // nothing written outside the mod and studio.ini
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    apply_theme(app.settings.theme);  // the palette chosen in the Themes window
    style.FontSizeBase = 16.0f;
    io.Fonts->AddFontDefaultVector();
    ImGui_ImplSDL3_InitForSDLGPU(app.window);
    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device = app.device;
    init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(app.device, app.window);
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);

    app.images = std::make_unique<GpuImages>(app.device, static_cast<std::size_t>(app.settings.cache_mb) << 20);
    app.refresh_mods();
    app.start_catalog();

    bool settings_open = false;
    bool themes_open = false;
    bool console_open = false;
    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(app.window))
                done = true;
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr)
                app.offer(path_u8(event.drop.data));
        }
        if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        app.images->collect();
        app.take_catalog();
        take_dialog_files(app);

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw_frame(app, &settings_open, &themes_open, &console_open);
        ImGui::Render();

        ImDrawData* draw = ImGui::GetDrawData();
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(app.device);
        SDL_GPUTexture* swapchain = nullptr;
        SDL_WaitAndAcquireGPUSwapchainTexture(cmd, app.window, &swapchain, nullptr, nullptr);
        if (swapchain != nullptr && draw->DisplaySize.x > 0 && draw->DisplaySize.y > 0) {
            ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
            SDL_GPUColorTargetInfo target{};
            target.texture = swapchain;
            target.clear_color = SDL_FColor{0.08f, 0.08f, 0.10f, 1.0f};
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    stop_sounds();
    app.catalog_cancel = true;
    if (app.catalog_thread.joinable())
        app.catalog_thread.join();
    app.job.reset();
    app.bulk_export.reset();  // stopped and waited for
    app.clear_candidate();
    movie_view_leave();
    SDL_WaitForGPUIdle(app.device);
    model_view_shutdown();
    app.images.reset();
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(app.device, app.window);
    SDL_DestroyGPUDevice(app.device);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}

}  // namespace
}  // namespace studio

int main(int argc, char** argv) {
    using namespace studio;
    attach_parent_console();  // a terminal run still shows the command line's output
    std::filesystem::path root = std::filesystem::current_path();
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    if (!take_port_argument(args, &root)) {
        std::fprintf(stderr, "khdays-studio: --port needs a folder\n");
        return 2;
    }
    if (!args.empty())
        return run_command_line(root, args);

    App app;
    std::string note;
    app.settings = load_settings(root, &note);
    app.mods_root = root / "mods";
    apply_language(app);
    if (!note.empty())
        app.tell(fmt(app.language, S::settings_bad, {note}), false);
    return run_window(app);
}
