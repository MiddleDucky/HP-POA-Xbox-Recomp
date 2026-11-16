// The options panel on F10, after SCDA-XBERecomp's (src/app/menu.cpp): its graphics settings on one ImGui page,
// applied at once and saved to hppoa.toml. Alt+Enter flips windowed and borderless. The fps counter draws here too.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// xboxrecomp: marks written from F10's Debug tab; hppoa_Marks lets the game's debug logs start at one
static std::atomic<int> g_marks{0};
extern "C" int hppoa_Marks(void) { return g_marks.load(); }

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"

extern "C" int hppoa_QuitState(void);  // hle_quit.c: 0 nothing, 1 the prompt, 2 the box
extern "C" int hppoa_PcSettings(int* page, int* sel, int* top);  // hle_quit.c: PC Settings is up, and where
// below: PC Settings's pages, for hle_quit.c and DrawPcSettings
extern "C" int app_GameOptRows(int page);
extern "C" const char* app_GameOptLabel(int page, int row);
extern "C" const char* app_GameOptValue(int page, int row);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern "C" void xbox_FramebufferWindowSetMsgHook(int (*fn)(void*, unsigned, uintptr_t, intptr_t));

namespace app {

namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_start{false};  // xboxrecomp: the start menu is up, the title waits for it
std::atomic<bool> g_ui{false};  // ImGui is set up

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"Game's", "2x", "4x", "8x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"Off", "2x", "4x", "8x"};
const char* const kDisplayNames[] = {"Windowed", "Borderless", "Fullscreen"};
const char* const kAspectNames[] = {"4:3", "16:9", "Stretch"};

template <size_t N>
int IndexOf(const uint32_t (&values)[N], uint32_t v) {
    for (size_t i = 0; i < N; i++) {
        if (values[i] == v) return int(i);
    }
    return 0;
}

// the monitor's biggest size. EnumDisplaySettings lists the real modes; GetSystemMetrics is scaled by Windows' DPI
// setting for a program that is not DPI-aware (a 4K screen at 150% reads 2560 x 1440 there), so it only stands in
std::pair<uint32_t, uint32_t> ScreenMax() {
    static const std::pair<uint32_t, uint32_t> max = [] {
        uint32_t w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        auto take = [&] {
            if (uint64_t(dm.dmPelsWidth) * dm.dmPelsHeight > uint64_t(w) * h) {
                w = dm.dmPelsWidth;
                h = dm.dmPelsHeight;
            }
        };
        for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++) take();
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) take();
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    return max;
}

// internal resolution: the lines the 3D is drawn at, 0 follows the window. Steps of the Xbox's 480 up to the screen's
// height, and that height itself; a saved value off the list (set in the toml) stays on it
struct InternalChoice {
    uint32_t lines;
    std::string name;
};

std::vector<InternalChoice> InternalChoices(uint32_t screen_h, uint32_t cur) {
    static const uint32_t kSteps[] = {480, 720, 960, 1080, 1440, 1920, 2160, 2880, 3240, 3840, 4320};
    std::vector<uint32_t> v;
    for (uint32_t l : kSteps)
        if (l <= screen_h) v.push_back(l);
    if (screen_h >= 480) v.push_back(screen_h);
    if (cur) v.push_back(cur);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::vector<InternalChoice> out{{0, "Follow window"}};
    for (uint32_t l : v) {
        uint32_t x = (l * 100 + 240) / 480;  // the scale over the Xbox's lines, in hundredths
        std::string scale = std::to_string(x / 100);
        if (x % 100) scale += "." + std::to_string(x % 100 / 10) + (x % 10 ? std::to_string(x % 10) : "");
        std::string name = std::to_string(l) + " (" + (l == 480 ? "Xbox, " : "") + scale + "x";
        if (l == screen_h) name += ", screen";
        out.push_back({l, name + ")"});
    }
    return out;
}

// the primary monitor's modes, biggest first, each size once
const std::vector<std::pair<uint32_t, uint32_t>>& Modes() {
    static std::vector<std::pair<uint32_t, uint32_t>> modes = [] {
        std::vector<std::pair<uint32_t, uint32_t>> m;
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++) {
            if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) m.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
        }
        for (auto s : {std::pair<uint32_t, uint32_t>{1280, 960}, {1440, 1080}, {1920, 1080}, {2560, 1440}})
            m.push_back(s);
        std::sort(m.begin(), m.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
        m.erase(std::unique(m.begin(), m.end()), m.end());
        return m;
    }();
    return modes;
}

// xboxrecomp: the sizes for an aspect ratio, biggest first: the monitor's own modes of that shape and the common ones
// that fit the desktop; stretch takes every mode
bool Shaped(uint32_t w, uint32_t h, AspectMode a) {
    if (a == AspectMode::Stretch) return true;
    float r = float(w) / float(h), want = a == AspectMode::Wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    return r > want - 0.02f && r < want + 0.02f;
}

// the biggest size of a shape inside w x h, even on both sides (a 3440 x 1440 screen holds 2560 x 1440 at 16:9)
std::pair<uint32_t, uint32_t> FitShape(uint32_t w, uint32_t h, AspectMode a) {
    if (a == AspectMode::Stretch) return {w, h};
    uint32_t rw = a == AspectMode::Wide ? 16 : 4, rh = a == AspectMode::Wide ? 9 : 3, k = std::min(w / rw, h / rh) & ~1u;
    return {k * rw, k * rh};
}

// up to the screen's biggest mode (ScreenMax): the screen's own size on top when it has another shape, the picture
// keeping its shape in it
std::vector<std::pair<uint32_t, uint32_t>> ModesFor(AspectMode a) {
    static const std::pair<uint32_t, uint32_t> kCommon[] = {
        {640, 480},   {800, 600},   {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440},
        {2048, 1536}, {2880, 2160}, {1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
        {3840, 2160}, {5120, 2880}, {7680, 4320}, {3840, 2880}, {5760, 4320}};
    auto [sw, sh] = ScreenMax();
    std::vector<std::pair<uint32_t, uint32_t>> m;
    for (auto [w, h] : Modes())
        if (Shaped(w, h, a) && w <= sw && h <= sh) m.emplace_back(w, h);
    auto fit = FitShape(sw, sh, a);
    if (fit.first >= 640) m.push_back(fit);
    for (auto [w, h] : kCommon)
        if (Shaped(w, h, a) && w <= sw && h <= sh) m.emplace_back(w, h);
    std::sort(m.begin(), m.end(), [](auto x, auto y) { return x.first * x.second > y.first * y.second; });
    m.erase(std::unique(m.begin(), m.end()), m.end());
    if (!Shaped(sw, sh, a)) m.insert(m.begin(), {sw, sh});
    return m;
}

// a setting changed: applied by the renderer next frame, kept in hppoa.toml
void Changed(bool display = false) {
    Save();
    if (display) ApplyDisplay();
}

bool Combo(const char* label, int* cur, const char* const* names, int n) {
    return ImGui::Combo(label, cur, names, n);
}

void Row(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

// xboxrecomp: the Controls tab: keyboard and mouse bindings (click, then press a key or a mouse button; Escape clears),
// the controller's remapping, sticks, camera and vibration
void DrawControls(Settings& s) {
    ImGuiIO& io = ImGui::GetIO();
    uint32_t vk;
    if (int b = KeyCaptureBinding(); b >= 0 && TakeCapturedKey(&vk)) {
        s.keys[b] = vk;
        Changed();
    }
    Row("Keyboard and mouse");
    if (ImGui::Checkbox("Keyboard and mouse play as controller 1", &s.kbm)) Changed();
    // xboxrecomp (hppoa): the arrows (the camera keys) move through the menus too (by request, 2026-11-15). input.cpp
    if (ImGui::Checkbox("Camera keys move through menus", &s.kbm_arrows_menu)) Changed();
    ImGui::TextDisabled("In the pause menu, the inventory and the front end the camera keys (the arrows) also act");
    ImGui::TextDisabled("as the D-pad; in play they turn the camera as before.");
    int ms = int(s.mouse_sensitivity);
    if (ImGui::SliderInt("Mouse sensitivity", &ms, 1, 100)) s.mouse_sensitivity = uint32_t(ms);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::Checkbox("Invert mouse Y", &s.mouse_invert_y)) Changed();
    ImGui::BeginDisabled(!s.kbm);
    if (ImGui::TreeNode("Key bindings")) {
        if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (uint32_t i = 0; i < kKeyBinds; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kKeyBindNames[i]);
                ImGui::TableNextColumn();
                ImGui::PushID(int(i));
                bool waiting = KeyCaptureBinding() == int(i);
                std::string label = waiting ? "Press a key or mouse button (Esc clears)" : KeyName(s.keys[i]);
                if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
                    if (waiting) CancelKeyCapture();
                    else StartKeyCapture(int(i));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Default keys")) {
            Settings d;
            std::copy(std::begin(d.keys), std::end(d.keys), std::begin(s.keys));
            Changed();
        }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();

    Row("Controller");
    if (ImGui::Checkbox("Use controller", &s.controller)) Changed();
    if (!s.controller) ImGui::TextDisabled("The game ignores the controller; keyboard and mouse still play.");
    int dl = int(s.deadzone_left), dr = int(s.deadzone_right), sl = int(s.sensitivity_left), sr = int(s.sensitivity_right);
    if (ImGui::SliderInt("Left stick deadzone %", &dl, 0, 50)) s.deadzone_left = uint32_t(dl);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Left stick sensitivity %", &sl, 25, 200)) s.sensitivity_left = uint32_t(sl);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Right stick deadzone %", &dr, 0, 50)) s.deadzone_right = uint32_t(dr);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Right stick sensitivity %", &sr, 25, 200)) s.sensitivity_right = uint32_t(sr);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::Checkbox("Invert camera X", &s.invert_x)) Changed();
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert camera Y", &s.invert_y)) Changed();
    if (ImGui::Checkbox("Vibration", &s.vibration)) Changed();
    ImGui::BeginDisabled(!s.vibration);
    int vs = int(s.vibration_strength);
    if (ImGui::SliderInt("Vibration strength %", &vs, 0, 100)) s.vibration_strength = uint32_t(vs);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    ImGui::EndDisabled();
    if (ImGui::TreeNode("Button remapping")) {
        ImGui::TextDisabled("The pad button each game button reads (a 360/One pad's LB is White, RB Black).");
        ImGui::PushItemWidth(160 * io.FontGlobalScale);
        for (uint32_t i = 0; i < kPadInputs; i++) {
            int m = int(std::min(s.pad_map[i], kPadInputs - 1));
            std::string label = std::string("Game ") + kPadInputNames[i];
            if (ImGui::Combo(label.c_str(), &m, kPadInputNames, int(kPadInputs))) {
                s.pad_map[i] = uint32_t(m);
                Changed();
            }
        }
        ImGui::PopItemWidth();
        if (ImGui::Button("Default buttons")) {
            for (uint32_t i = 0; i < kPadInputs; i++) s.pad_map[i] = i;
            Changed();
        }
        ImGui::TreePop();
    }
}

// xboxrecomp: the Debug tab (as HP:CoS's): where this run's log is, a marker line in it for "it happened here", the
// shaders made ahead, a frame capture, the renderer's debug keys and an audio trace
void DrawDebug(Settings& s) {
    static char log_name[MAX_PATH];
    if (!log_name[0] && !GetEnvironmentVariableA("HPPOA_RUN_LOG", log_name, sizeof log_name))
        std::snprintf(log_name, sizeof log_name, "hppoa_run.log (logs\\last_run.log when started by run_win.sh)");
#ifndef HPPOA_NO_TRAINER  // no trainer unless built with HPPOA_TRAINER (never a release): neither F6 nor this button
    Row("Trainer");
    if (ImGui::Button("Open the trainer (F6)")) ToggleTrainer();
    ImGui::SameLine();
    ImGui::TextDisabled("god mode, items, game speed, the title's debug menu and variables");
#endif

    Row("Logs");
    ImGui::TextWrapped("This run's log: %s", log_name);
    if (ImGui::Button("Open logs folder")) {
        char dir[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, dir, sizeof dir);
        char* slash = n ? std::strrchr(dir, '\\') : nullptr;
        if (slash) {
            // run_win.sh writes port\logs beside run\; the launcher writes hppoa_run.log beside the exe
            std::snprintf(slash + 1, size_t(dir + sizeof dir - slash - 1), "..\\logs");
            if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) *slash = 0;
            ShellExecuteA(nullptr, "open", dir, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Mark this moment in the log")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        LOGI("[MARK] %d at %02u:%02u:%02u.%03u (marked from F10's Debug tab)", ++g_marks, t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds);
    }
    if (g_marks) ImGui::TextDisabled("%d mark%s written: search the log for [MARK]", g_marks.load(), g_marks == 1 ? "" : "s");
    ImGui::TextDisabled("Mark right after something goes wrong, then give the mark's number with your report.");

    Row("Shaders");
    ImGui::Text("Pipelines recorded: %u (shadercache\\pipelines_list.bin)", gpu::g_pipelines_recorded.load());
    if (uint32_t total = gpu::g_prebuild_total)
        ImGui::Text("Made ahead at start: %u / %u, %u could not be", gpu::g_prebuild_done.load(), total,
                    gpu::g_prebuild_failed.load());
    else
        ImGui::TextDisabled("Nothing to make ahead at start (an empty list, or RECOMP_PREBUILD=0)");

    Row("Renderer");
#ifdef HPPOA_RELEASE
    if (ImGui::Button("Capture the next frame")) gpu::g_capture = true;  // no F12 in a release build
#else
    if (ImGui::Button("Capture the next frame (F12)")) gpu::g_capture = true;
#endif
    ImGui::SameLine();
    ImGui::TextDisabled("saved under captures\\");
    if (ImGui::Checkbox("Debug keys", &s.debug_keys)) Changed();
    ImGui::TextDisabled("F7 depth test always passes, F8 log depth for all draws, F9 alpha test off");

    Row("Audio");
    ImGui::Checkbox("Audio trace", &s.audio_trace);
    ImGui::TextDisabled("Logs every sound started and every DirectSound call; on only while reproducing.");
}

// xboxrecomp: F10's Defaults resets the page it is on, not every setting (the aspect ratio, a boot setting, stays)
static void ResetPage(Settings& s, const char* page) {
    const Settings d;
    if (!std::strcmp(page, "Graphics")) {
        std::memcpy(&s.aniso, &d.aniso, sizeof(s.aniso));
        std::memcpy(&s.brightness, &d.brightness, sizeof(s.brightness));
        std::memcpy(&s.contrast, &d.contrast, sizeof(s.contrast));
        std::memcpy(&s.display_mode, &d.display_mode, sizeof(s.display_mode));
        std::memcpy(&s.game_bloom, &d.game_bloom, sizeof(s.game_bloom));
        std::memcpy(&s.sharpen, &d.sharpen, sizeof(s.sharpen));
        std::memcpy(&s.saturation, &d.saturation, sizeof(s.saturation));
        std::memcpy(&s.vibrance, &d.vibrance, sizeof(s.vibrance));
        std::memcpy(&s.warmth, &d.warmth, sizeof(s.warmth));
        std::memcpy(&s.vignette, &d.vignette, sizeof(s.vignette));
        std::memcpy(&s.grain, &d.grain, sizeof(s.grain));
        std::memcpy(&s.fov, &d.fov, sizeof(s.fov));
        std::memcpy(&s.fps_counter, &d.fps_counter, sizeof(s.fps_counter));
        std::memcpy(&s.frame_rate, &d.frame_rate, sizeof(s.frame_rate));
        std::memcpy(&s.height, &d.height, sizeof(s.height));
        std::memcpy(&s.internal_height, &d.internal_height, sizeof(s.internal_height));
        std::memcpy(&s.msaa, &d.msaa, sizeof(s.msaa));
        std::memcpy(&s.render_max_height, &d.render_max_height, sizeof(s.render_max_height));
        std::memcpy(&s.render_percent, &d.render_percent, sizeof(s.render_percent));
        std::memcpy(&s.smaa, &d.smaa, sizeof(s.smaa));
        std::memcpy(&s.vsync, &d.vsync, sizeof(s.vsync));
        std::memcpy(&s.width, &d.width, sizeof(s.width));
    }
    if (!std::strcmp(page, "Audio")) {
        std::memcpy(&s.speakers, &d.speakers, sizeof(s.speakers));
    }
    if (!std::strcmp(page, "Controls")) {
        std::memcpy(&s.controller, &d.controller, sizeof(s.controller));
        std::memcpy(&s.deadzone_left, &d.deadzone_left, sizeof(s.deadzone_left));
        std::memcpy(&s.deadzone_right, &d.deadzone_right, sizeof(s.deadzone_right));
        std::memcpy(&s.invert_x, &d.invert_x, sizeof(s.invert_x));
        std::memcpy(&s.invert_y, &d.invert_y, sizeof(s.invert_y));
        std::memcpy(&s.kbm, &d.kbm, sizeof(s.kbm));
        std::memcpy(&s.keys, &d.keys, sizeof(s.keys));
        std::memcpy(&s.mouse_invert_y, &d.mouse_invert_y, sizeof(s.mouse_invert_y));
        std::memcpy(&s.mouse_sensitivity, &d.mouse_sensitivity, sizeof(s.mouse_sensitivity));
        std::memcpy(&s.pad_map, &d.pad_map, sizeof(s.pad_map));
        std::memcpy(&s.sensitivity_left, &d.sensitivity_left, sizeof(s.sensitivity_left));
        std::memcpy(&s.sensitivity_right, &d.sensitivity_right, sizeof(s.sensitivity_right));
        std::memcpy(&s.vibration, &d.vibration, sizeof(s.vibration));
        std::memcpy(&s.vibration_strength, &d.vibration_strength, sizeof(s.vibration_strength));
    }
    if (!std::strcmp(page, "Enhancements")) {
        std::memcpy(&s.button_prompts, &d.button_prompts, sizeof(s.button_prompts));
        std::memcpy(&s.tutorials_once, &d.tutorials_once, sizeof(s.tutorials_once));
        std::memcpy(&s.quiet_turn_in, &d.quiet_turn_in, sizeof(s.quiet_turn_in));
        std::memcpy(&s.reparo_sound, &d.reparo_sound, sizeof(s.reparo_sound));
        std::memcpy(&s.smooth_textures, &d.smooth_textures, sizeof(s.smooth_textures));
        std::memcpy(&s.hide_save_icon, &d.hide_save_icon, sizeof(s.hide_save_icon));
        std::memcpy(&s.cutscene_bars_hidden, &d.cutscene_bars_hidden, sizeof(s.cutscene_bars_hidden));
        std::memcpy(&s.skip_intro, &d.skip_intro, sizeof(s.skip_intro));
        std::memcpy(&s.turn_response, &d.turn_response, sizeof(s.turn_response));
        std::memcpy(&s.instant_step, &d.instant_step, sizeof(s.instant_step));
        std::memcpy(&s.turn_full_speed, &d.turn_full_speed, sizeof(s.turn_full_speed));
    }
    if (!std::strcmp(page, "Accessibility")) {
        std::memcpy(&s.colorblind, &d.colorblind, sizeof(s.colorblind));
        std::memcpy(&s.colorblind_strength, &d.colorblind_strength, sizeof(s.colorblind_strength));
    }
    if (!std::strcmp(page, "Debug")) {
        std::memcpy(&s.audio_trace, &d.audio_trace, sizeof(s.audio_trace));
        std::memcpy(&s.debug_keys, &d.debug_keys, sizeof(s.debug_keys));
    }
}

// start: the start menu (the aspect ratio applies at once, Start game instead of Close); otherwise F10's panel
void DrawPanel(bool start) {
    Settings& s = Get();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520 * io.FontGlobalScale, 0), ImGuiCond_Appearing);
    bool open = true;
    const char* title = start ? "Harry Potter and the Prisoner of Azkaban - Settings###start" : "Options (F10)";
    if (!ImGui::Begin(title, start ? nullptr : &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!open) g_open = false;
        return;
    }
    ImGui::PushItemWidth(260 * io.FontGlobalScale);

    bool tabs = ImGui::BeginTabBar("##tabs");
    static const char* page = "Graphics";  // the open page, for Defaults
    if (tabs && ImGui::BeginTabItem("Graphics")) {
        page = "Graphics";
    // xboxrecomp: the launcher's page before the game, on a tab as well as in the bar below (2026-11-15: a player who
    // had turned it off found no way back but holding Shift)
    Row("Startup");
    if (ImGui::Checkbox("Show the launcher at startup##graphics", &s.show_launcher)) Changed();
    ImGui::SameLine();
    ImGui::TextDisabled("its settings page before the game starts (or hold Shift as the launcher starts)");
    Row("Display");
    // the aspect ratio first: the sizes below follow it
    int asp = int(s.aspect);
    if (Combo("Aspect ratio", &asp, kAspectNames, 3)) {
        s.aspect = AspectMode(asp);
        // a window or mode of the old shape: the biggest of the new one
        if (s.width && s.height && !Shaped(s.width, s.height, s.aspect)) {
            auto m = ModesFor(s.aspect);
            if (!m.empty()) {
                s.width = m.front().first;
                s.height = m.front().second;
            }
        }
        if (start) SetBootAspect(s.aspect);
        Changed(true);
    }
    if (!start && s.aspect != BootAspect())
        ImGui::TextDisabled("Aspect ratio applies at the next start.");
    int dm = int(s.display_mode);
    if (Combo("Display mode", &dm, kDisplayNames, 3)) {
        s.display_mode = DisplayMode(dm);
        Changed(true);
    }
    {
        const auto modes = ModesFor(s.aspect);
        std::string cur = s.width && s.height ? std::to_string(s.width) + " x " + std::to_string(s.height) : "Desktop";
        if (ImGui::BeginCombo(s.display_mode == DisplayMode::Windowed ? "Window size" : "Resolution", cur.c_str())) {
            if (ImGui::Selectable("Desktop", !(s.width && s.height))) {
                s.width = s.height = 0;
                Changed(true);
            }
            for (auto [w, h] : modes) {
                std::string name = std::to_string(w) + " x " + std::to_string(h);
                if (std::pair{w, h} == ScreenMax()) name += "  (screen)";
                if (ImGui::Selectable(name.c_str(), w == s.width && h == s.height)) {
                    s.width = w;
                    s.height = h;
                    Changed(true);
                }
            }
            ImGui::EndCombo();
        }
        if (s.display_mode == DisplayMode::Borderless)
            ImGui::TextDisabled("Borderless covers the monitor; the size is for windowed / fullscreen.");
    }
    if (ImGui::Checkbox("Vsync", &s.vsync)) Changed();
    // xboxrecomp (hppoa): 60 fps, or the game's own 30 (hle_clock.c keeps its clock at real time)
    int fr = s.frame_rate >= 60 ? 0 : 1;
    const char* const kRateNames[] = {"60 fps", "30 fps (the game's own)"};
    if (Combo("Frame rate", &fr, kRateNames, 2)) {
        s.frame_rate = fr ? 30 : 60;
        Changed();
    }
    ImGui::TextDisabled("Takes effect the next time the game starts.");

    Row("Resolution");
    {
        auto choices = InternalChoices(ScreenMax().second, s.internal_height);
        const InternalChoice* cur = &choices.front();
        for (const auto& c : choices)
            if (c.lines == s.internal_height) cur = &c;
        if (ImGui::BeginCombo("Internal resolution", cur->name.c_str())) {
            for (const auto& c : choices)
                if (ImGui::Selectable(c.name.c_str(), &c == cur)) {
                    s.internal_height = c.lines;
                    Changed();
                }
            ImGui::EndCombo();
        }
    }
    if (!s.internal_height) {
        int pct = int(s.render_percent);
        if (ImGui::SliderInt("Render scale %", &pct, 25, 200)) s.render_percent = uint32_t(pct);
        if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
        int cap = int(s.render_max_height);
        if (ImGui::SliderInt("Max render height", &cap, 0, 4320, cap ? "%d lines" : "No limit")) s.render_max_height = uint32_t(cap);
        if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    }

    Row("Anti-aliasing and filtering");
    int ms = IndexOf(kMsaaValues, s.msaa);
    if (Combo("MSAA", &ms, kMsaaNames, int(std::size(kMsaaNames)))) {
        s.msaa = kMsaaValues[ms];
        Changed();
    }
    if (ImGui::Checkbox("SMAA", &s.smaa)) Changed();
    int an = IndexOf(kAnisoValues, s.aniso);
    if (Combo("Anisotropic filtering", &an, kAnisoNames, int(std::size(kAnisoNames)))) {
        s.aniso = kAnisoValues[an];
        Changed();
    }

    Row("Picture");
    int br = int(s.brightness), co = int(s.contrast);
    if (ImGui::SliderInt("Brightness", &br, 0, 100)) s.brightness = uint32_t(br);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Contrast", &co, 0, 100)) s.contrast = uint32_t(co);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::Checkbox("FPS counter", &s.fps_counter)) Changed();

    // xboxrecomp: post-processing, off or neutral by default
    Row("Post-processing");
    // the game's own bloom (d3d12.cpp, GameBloomComposite), its strength
    int bs = int(s.game_bloom);
    if (ImGui::SliderInt("Bloom", &bs, 0, 200, bs == 0 ? "off" : bs == 100 ? "as the game" : "%d %%"))
        s.game_bloom = uint32_t(bs);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    int sh = int(s.sharpen), sa = int(s.saturation), vi = int(s.vibrance), wa = int(s.warmth);
    if (ImGui::SliderInt("Sharpening", &sh, 0, 100, sh ? "%d" : "off")) s.sharpen = uint32_t(sh);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Saturation", &sa, 0, 200, "%d %%")) s.saturation = uint32_t(sa);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Vibrance", &vi, 0, 100, vi ? "%d" : "off")) s.vibrance = uint32_t(vi);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Warmth", &wa, 0, 100, wa == 50 ? "as the game" : wa < 50 ? "cooler %d" : "warmer %d"))
        s.warmth = uint32_t(wa);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    int vg = int(s.vignette), gr = int(s.grain), fv = int(std::clamp(s.fov, 80u, 130u));
    if (ImGui::SliderInt("Vignette", &vg, 0, 100, vg ? "%d" : "off")) s.vignette = uint32_t(vg);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Film grain", &gr, 0, 100, gr ? "%d" : "off")) s.grain = uint32_t(gr);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
    if (ImGui::SliderInt("Field of view", &fv, 80, 130, fv == 100 ? "as the game" : "%d %%")) s.fov = uint32_t(fv);
    if (ImGui::IsItemDeactivatedAfterEdit()) Changed();

        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Audio")) {
        page = "Audio";
        Row("Output");
        // xboxrecomp: the console mixes to 5.1 (Dolby Digital out of the console); stereo folds it down
        int sp = s.speakers == 6 ? 1 : 0;
        const char* const kSpeakerNames[] = {"Stereo", "5.1 surround"};
        if (Combo("Speakers", &sp, kSpeakerNames, 2)) {
            s.speakers = sp ? 6 : 2;
            Changed();
        }
        ImGui::TextDisabled("Takes effect the next time the game starts.");
        ImGui::TextDisabled("5.1 needs Windows' speaker setup set to 5.1 (or a 5.1 HDMI receiver).");
        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Controls")) {
        page = "Controls";
        DrawControls(s);
        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Accessibility")) {
        page = "Accessibility";
        Row("Colour blindness");
        int cb = int(std::min(s.colorblind, 3u));
        const char* const kCbNames[] = {"Off", "Protanopia (red-weak)", "Deuteranopia (green-weak)",
                                        "Tritanopia (blue-weak)"};
        if (Combo("Correction filter", &cb, kCbNames, 4)) {
            s.colorblind = uint32_t(cb);
            Changed();
        }
        ImGui::BeginDisabled(!s.colorblind);
        int st = int(s.colorblind_strength);
        if (ImGui::SliderInt("Strength %", &st, 0, 100)) s.colorblind_strength = uint32_t(st);
        if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
        ImGui::EndDisabled();
        ImGui::TextDisabled("Shifts the colours a colour-blind player confuses toward ones they can tell apart.");
        ImGui::EndTabItem();
    }
    if (tabs && ImGui::BeginTabItem("Enhancements")) {
        page = "Enhancements";
        Row("Hints and chatter");
        {
            static const char* const kPromptNames[] = {"Automatic", "Controller", "Keyboard and mouse"};
            int bp = int(std::min(s.button_prompts, 2u));
            if (Combo("Button prompts", &bp, kPromptNames, 3)) {
                s.button_prompts = uint32_t(bp);
                Changed();
            }
            ImGui::TextDisabled("The buttons shown: the controller's, or your key bindings. Automatic: the HUD's buttons,");
            ImGui::TextDisabled("the tutorials and the quit prompt switch as soon as you use the other device; the menus'");
            ImGui::TextDisabled("own text is set when the game starts, from the device you used last.");
        }
        if (ImGui::Checkbox("Show each tutorial once", &s.tutorials_once)) Changed();
        ImGui::TextDisabled("A tutorial popup (camera, trolls and Dungbombs, END DAY...) shows the first time only;");
        ImGui::TextDisabled("a minigame's own tutorials (owl racing...) always show, and so do room names and house");
        ImGui::TextDisabled("points. Delete tutorials_seen.txt to see them all again.");
        if (ImGui::Checkbox("Leave out \"time to turn in\" and \"we need to go\" lines", &s.quiet_turn_in)) Changed();
        ImGui::TextDisabled("Harry, Ron and Hermione stop saying they're tired, have nothing to do, \"We need to go\",");
        ImGui::TextDisabled("\"Can we go now?\", and Neville stops asking to go back to bed. Their hints stay: the way");
        ImGui::TextDisabled("to go, the wrong floor, Girding Potion and Fred and George.");
        if (ImGui::Checkbox("Reparo's own sound", &s.reparo_sound)) Changed();
        ImGui::TextDisabled("The Xbox game casts Reparo with Glacius's sound; this plays Reparo's own, which is on the");
        ImGui::TextDisabled("disc unused. Takes a restart. Off: as on the console.");
        Row("Controls");
        {
            // one switch for the three (2026-11-10): on, turning twice as quick, the instant first step and full
            // speed through turns; off, all three as the console
            bool on = s.turn_response > 100 || s.instant_step || s.turn_full_speed;
            if (ImGui::Checkbox("Responsive movement", &on)) {
                s.turn_response = on ? 200u : 100u;
                s.instant_step = s.turn_full_speed = on;
                Changed();
            }
            ImGui::TextDisabled("Harry turns to face the stick twice as quickly (the game eases every turn to a crawl),");
            ImGui::TextDisabled("sets off the moment the stick moves from standing without the shuffle and turn-on-the-");
            ImGui::TextDisabled("spot wait, and keeps his running speed through turns. Off: as on the console.");
        }
        Row("Graphics");
        if (ImGui::Checkbox("Smooth stretched textures", &s.smooth_textures)) Changed();
        ImGui::TextDisabled("Skies and the landscapes behind the levels drawn with a smoother filter where they are");
        ImGui::TextDisabled("stretched: soft gradients instead of blocks. Off: as on the console.");
        if (ImGui::Checkbox("Cutscenes without black bars", &s.cutscene_bars_hidden)) Changed();
        ImGui::TextDisabled("In 16:9 cutscenes fill the screen: the bars at the top and bottom are left out.");
        ImGui::TextDisabled("Off: the bars as the game draws them.");
        if (ImGui::Checkbox("Hide the saving / loading compass", &s.hide_save_icon)) Changed();
        ImGui::TextDisabled("The turning compass in the top left corner while the game saves or loads is not drawn.");
        Row("Startup");
        if (ImGui::Checkbox("Skip the EA and Warner Bros. logos", &s.skip_intro)) Changed();
        ImGui::TextDisabled("The logo movies before the title screen are not played. Off: played, as on the console.");
        ImGui::EndTabItem();
    }
    if (tabs && !start && ImGui::BeginTabItem("Debug")) {
        page = "Debug";
        DrawDebug(s);
        ImGui::EndTabItem();
    }
    if (tabs) ImGui::EndTabBar();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Defaults (this page)")) {
        ResetPage(s, page);
        Changed(true);
    }
    ImGui::SameLine();
    if (start) {
        if (ImGui::Checkbox("Show this page at startup", &s.start_menu)) Changed();
        ImGui::Spacing();
        if (ImGui::Button("Start game", ImVec2(160 * io.FontGlobalScale, 0))) {
            SetBootAspect(s.aspect);
            Changed();
            g_start = false;
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to hppoa.toml; F10 in game changes them too");
    } else {
        if (ImGui::Button("Close")) open = false;
        ImGui::SameLine();
        if (ImGui::Checkbox("Show the launcher at startup", &s.show_launcher)) Changed();
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to hppoa.toml");
    }
    ImGui::PopItemWidth();
    ImGui::End();
    if (!open) g_open = false;
}

// xboxrecomp: a small note while recorded pipelines are made ahead (startup), as HP:CoS's
void DrawPrebuild() {
    uint32_t total = gpu::g_prebuild_total, done = gpu::g_prebuild_done;
    if (!total || done >= total) return;
    char text[64];
    std::snprintf(text, sizeof(text), "Preparing shaders %u / %u", done, total);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float sz = ImGui::GetFontSize();
    ImVec2 p(12, ImGui::GetIO().DisplaySize.y - sz - 10);
    dl->AddText(ImGui::GetFont(), sz, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), sz, p, IM_COL32(230, 230, 230, 255), text);
}

void DrawFps() {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", g_fps.load());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float sz = ImGui::GetFontSize() * 1.25f;
    ImVec2 p(12, 8);
    dl->AddText(ImGui::GetFont(), sz, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), sz, p, IM_COL32(255, 255, 80, 255), text);
}

// xboxrecomp (hppoa): Quit To Desktop (hle_quit.c decides when). The prompt sits at the bottom right of the picture,
// beside the game's own "A to select"; the box in the middle, in the game's UI font from its disc
ImFont* g_game_font = nullptr;

void GameRect(float& x, float& y, float& w, float& h) {
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    float a = Aspect();
    w = std::min(d.x, d.y * a), h = w / a;
    x = (d.x - w) * 0.5f, y = (d.y - h) * 0.5f;
}

// an Xbox face button: a coloured disc with its letter, or the Back button's grey pill
void PadButton(ImDrawList* dl, ImFont* f, float sz, ImVec2 c, const char* label, ImU32 col, bool pill) {
    ImVec2 t = f->CalcTextSizeA(sz * 0.62f, FLT_MAX, 0, label);
    if (pill) {
        float hw = t.x * 0.5f + sz * 0.3f, hh = sz * 0.42f;
        dl->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), col, hh);
    } else {
        dl->AddCircleFilled(c, sz * 0.5f, col, 32);
    }
    dl->AddText(f, sz * 0.62f, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), IM_COL32(255, 255, 255, 255), label);
}

void ShadowText(ImDrawList* dl, ImFont* f, float sz, ImVec2 p, ImU32 col, const char* text) {
    dl->AddText(f, sz, ImVec2(p.x + sz * 0.06f, p.y + sz * 0.06f), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(f, sz, p, col, text);
}

// xboxrecomp (hppoa): Button prompts for keyboard and mouse: the key bound to a pad input as a key cap (a light face,
// the button's colour as its rim), as the HUD's buttons (d3d12.cpp's PromptPicture). False with the controller's
bool KeyCap(ImDrawList* dl, ImFont* f, float sz, ImVec2 c, int pad_input, ImU32 rim) {
    if (!PromptsKbmNow()) return false;
    const uint32_t vk = Get().keys[pad_input];
    if (!vk) return false;
    std::string label = PromptKeyLabel(vk);
    if (label == "Escape") label = "Esc";
    const float ts = sz * 0.56f;
    ImVec2 t = f->CalcTextSizeA(ts, FLT_MAX, 0, label.c_str());
    float hw = std::max(t.x * 0.5f + sz * 0.28f, sz * 0.46f), hh = sz * 0.46f, r = sz * 0.16f;
    dl->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), IM_COL32(32, 28, 24, 255), r);
    dl->AddRectFilled(ImVec2(c.x - hw + 1, c.y - hh + 1), ImVec2(c.x + hw - 1, c.y + hh - 1), rim, r);
    float in = sz * 0.09f;
    dl->AddRectFilled(ImVec2(c.x - hw + in, c.y - hh + in), ImVec2(c.x + hw - in, c.y + hh - in * 1.3f),
                      IM_COL32(242, 236, 218, 255), r * 0.7f);
    dl->AddText(f, ts, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f - in * 0.15f), IM_COL32(34, 30, 24, 255), label.c_str());
    return true;
}

// a key cap's width, for laying out what follows it (the pad button's otherwise)
float KeyCapWidth(ImFont* f, float sz, int pad_input, float pad_w) {
    if (!PromptsKbmNow() || !Get().keys[pad_input]) return pad_w;
    std::string label = PromptKeyLabel(Get().keys[pad_input]);
    if (label == "Escape") label = "Esc";
    ImVec2 t = f->CalcTextSizeA(sz * 0.56f, FLT_MAX, 0, label.c_str());
    return 2.0f * std::max(t.x * 0.5f + sz * 0.28f, sz * 0.46f);
}

void DrawQuit(int state) {
    ImFont* f = g_game_font ? g_game_font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float gx, gy, gw, gh;
    GameRect(gx, gy, gw, gh);
    const ImU32 parchment = IM_COL32(236, 222, 186, 255);
    if (state == 1) {
        float sz = gh * 0.034f;
        const char* text = "Quit To Desktop";
        ImVec2 t = f->CalcTextSizeA(sz, FLT_MAX, 0, text);
        float right = gx + gw * 0.955f, cy = gy + gh * 0.935f;
        ImVec2 tp(right - t.x, cy - t.y * 0.5f);
        const float kw = KeyCapWidth(f, sz, 13, 0.0f);
        float left = tp.x - sz * 2.2f;  // the Back pill's left edge, near enough
        if (KeyCap(dl, f, sz, ImVec2(tp.x - sz * 0.35f - kw * 0.5f, cy), 13, IM_COL32(90, 90, 90, 255)))
            left = tp.x - sz * 0.35f - kw;
        else
            PadButton(dl, f, sz, ImVec2(tp.x - sz * 1.15f, cy), "BACK", IM_COL32(70, 70, 70, 235), true);
        ShadowText(dl, f, sz, tp, parchment, text);
        // PC Settings (hle_quit.c: Y here opens it), to the left of Quit To Desktop
        const char* pc = "PC Settings";
        ImVec2 pt = f->CalcTextSizeA(sz, FLT_MAX, 0, pc);
        ImVec2 pp(left - sz * 1.1f - pt.x, cy - pt.y * 0.5f);
        const ImU32 yellow = IM_COL32(220, 180, 30, 255);
        const float yw = KeyCapWidth(f, sz, 3, sz);
        if (!KeyCap(dl, f, sz, ImVec2(pp.x - sz * 0.35f - yw * 0.5f, cy), 3, yellow))
            PadButton(dl, f, sz, ImVec2(pp.x - sz * 0.35f - yw * 0.5f, cy), "Y", yellow, false);
        ShadowText(dl, f, sz, pp, parchment, pc);
        return;
    }
    // the box: a dark panel with a gold rule, the question, A Yes / B No
    float sz = gh * 0.05f, w = gw * 0.46f, h = gh * 0.26f;
    ImVec2 c(gx + gw * 0.5f, gy + gh * 0.5f), p0(c.x - w * 0.5f, c.y - h * 0.5f), p1(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), IM_COL32(0, 0, 0, 110));
    dl->AddRectFilled(p0, p1, IM_COL32(28, 20, 12, 235), sz * 0.3f);
    dl->AddRect(p0, p1, IM_COL32(196, 158, 82, 255), sz * 0.3f, 0, std::max(1.5f, sz * 0.06f));
    const char* q = "Quit to desktop?";
    ImVec2 qt = f->CalcTextSizeA(sz, FLT_MAX, 0, q);
    ShadowText(dl, f, sz, ImVec2(c.x - qt.x * 0.5f, p0.y + h * 0.18f), parchment, q);
    const char* note = "Progress since your last save will be lost.";
    float nsz = sz * 0.55f;
    ImVec2 nt = f->CalcTextSizeA(nsz, FLT_MAX, 0, note);
    ShadowText(dl, f, nsz, ImVec2(c.x - nt.x * 0.5f, p0.y + h * 0.45f), IM_COL32(200, 186, 150, 255), note);
    float by = p0.y + h * 0.76f, bsz = sz * 0.8f;
    // the answers: each button (or its key) then its word, a key cap as wide as its name. The two together centred in
    // the box, measured as drawn: a wide key (Space) pushed Yes to the right of fixed places (2026-11-10)
    const struct { int input; const char* pad; ImU32 col; const char* word; } kAns[] = {
        {0, "A", IM_COL32(60, 160, 60, 255), "Yes"},
        {1, "B", IM_COL32(190, 50, 40, 255), "No"}};
    const float gap = bsz * 0.25f, between = bsz * 1.6f;
    float kws[2], tws[2], total = between;
    for (int i = 0; i < 2; i++) {
        kws[i] = KeyCapWidth(f, bsz, kAns[i].input, bsz);
        tws[i] = f->CalcTextSizeA(bsz, FLT_MAX, 0, kAns[i].word).x;
        total += kws[i] + gap + tws[i];
    }
    float x = c.x - total * 0.5f;
    for (int i = 0; i < 2; i++) {
        const auto& a = kAns[i];
        const ImVec2 kc(x + kws[i] * 0.5f, by);
        if (!KeyCap(dl, f, bsz, kc, a.input, a.col)) PadButton(dl, f, bsz, kc, a.pad, a.col, false);
        ShadowText(dl, f, bsz, ImVec2(x + kws[i] + gap, by - bsz * 0.55f), parchment, a.word);
        x += kws[i] + gap + tws[i] + between;
    }
}

// ── xboxrecomp (hppoa): PC Settings, the F10 settings from the menus (hle_quit.c: Y beside Quit To Desktop) ───────
//
// HP:CoS's Options > PC Settings (its menu.cpp, app_GameOpt*) for this title's settings: pages of rows, each a label
// and a value as words; left and right step the value (A steps it on), with the same limits and the same saving
// (Changed) as the panel's own controls. Key bindings, the button remapping and the Debug tab stay in F10. A label
// ending in '*' takes effect at the next start.

// the internal resolution as players know them: "1080p", "4K (2160p)", the Xbox's own 480 lines as Original
std::string LinesName(uint32_t lines) {
    if (!lines) return "Match window";
    if (lines == 480) return "Original (480p)";
    if (lines == 2160) return "4K (2160p)";
    if (lines == 2880) return "5K (2880p)";
    if (lines == 4320) return "8K (4320p)";
    return std::to_string(lines) + "p";
}

struct GameRow {
    const char* label;
    std::string (*value)(const Settings&);
    void (*step)(Settings&, int dir);  // dir -1 or +1; changes the settings only
    bool display = false;  // applies to the window (ApplyDisplay)
    bool held = false;     // the window's size and shape: chosen here, applied as the page is left (app_GameOptLeave)
};

std::string OnOff(bool b) { return b ? "On" : "Off"; }

uint32_t Stepped(uint32_t v, int dir, uint32_t lo, uint32_t hi, uint32_t by) {
    int64_t n = int64_t(v) + int64_t(dir) * by;
    return uint32_t(std::clamp<int64_t>(n, lo, hi));
}

int Cycled(int i, int dir, int n) { return ((i + dir) % n + n) % n; }

std::string Num(uint32_t v, const char* off = nullptr) { return v == 0 && off ? off : std::to_string(v); }

// the next of a list of values from v (a value off the list goes to its neighbour that way)
uint32_t NextOf(const uint32_t* list, size_t n, uint32_t v, int dir) {
    if (dir > 0) {
        for (size_t i = 0; i < n; i++)
            if (list[i] > v) return list[i];
        return list[n - 1];
    }
    for (size_t i = n; i-- > 0;)
        if (list[i] < v) return list[i];
    return list[0];
}

const GameRow kDisplayRows[] = {
    {"Aspect ratio *", [](const Settings& s) { return std::string(kAspectNames[std::min(uint32_t(s.aspect), 2u)]); },
     [](Settings& s, int d) {
         s.aspect = AspectMode(Cycled(int(s.aspect), d, 3));
         if (s.width && s.height && !Shaped(s.width, s.height, s.aspect)) {
             auto m = ModesFor(s.aspect);
             if (!m.empty()) s.width = m.front().first, s.height = m.front().second;
         }
     },
     true, true},
    {"Display mode", [](const Settings& s) { return std::string(kDisplayNames[std::min(uint32_t(s.display_mode), 2u)]); },
     [](Settings& s, int d) { s.display_mode = DisplayMode(Cycled(int(s.display_mode), d, 3)); }, true},
    {"Resolution",
     [](const Settings& s) {
         return s.width && s.height ? std::to_string(s.width) + " x " + std::to_string(s.height) : std::string("Desktop");
     },
     [](Settings& s, int d) {
         // Desktop, then the shape's sizes biggest first
         auto m = ModesFor(s.aspect);
         int cur = 0;
         for (size_t i = 0; i < m.size(); i++)
             if (m[i].first == s.width && m[i].second == s.height) cur = int(i) + 1;
         int n = Cycled(cur, d, int(m.size()) + 1);
         if (n == 0) s.width = s.height = 0;
         else s.width = m[n - 1].first, s.height = m[n - 1].second;
     },
     true, true},
    {"Vsync", [](const Settings& s) { return OnOff(s.vsync); }, [](Settings& s, int) { s.vsync = !s.vsync; }},
    {"Frame rate *", [](const Settings& s) { return std::string(s.frame_rate >= 60 ? "60 fps" : "30 fps (the game's)"); },
     [](Settings& s, int) { s.frame_rate = s.frame_rate >= 60 ? 30 : 60; }},
};

const GameRow kGraphicsRows[] = {
    // the lens first, where it is looked for (it sat last on Post-processing, unseen: 2026-11-15)
    {"Field of view",
     [](const Settings& s) {
         uint32_t f = std::clamp(s.fov, 80u, 130u);
         return f == 100 ? std::string("Game") : std::to_string(f) + "%";
     },
     [](Settings& s, int d) { s.fov = Stepped(std::clamp(s.fov, 80u, 130u), d, 80, 130, 5); }},
    {"Internal resolution", [](const Settings& s) { return LinesName(s.internal_height); },
     [](Settings& s, int d) {
         auto c = InternalChoices(ScreenMax().second, s.internal_height);
         int cur = 0;
         for (size_t i = 0; i < c.size(); i++)
             if (c[i].lines == s.internal_height) cur = int(i);
         s.internal_height = c[size_t(std::clamp(cur + d, 0, int(c.size()) - 1))].lines;
     }},
    {"Render scale", [](const Settings& s) { return std::to_string(s.render_percent) + "%"; },
     [](Settings& s, int d) { s.render_percent = Stepped(s.render_percent, d, 25, 200, 25); }},
    {"Max render height",
     [](const Settings& s) { return s.render_max_height ? std::to_string(s.render_max_height) + "p" : std::string("No limit"); },
     [](Settings& s, int d) {
         // 0 (no limit) last: up from 4320 lifts the limit, down from it comes back to 4320
         static const uint32_t k[] = {480, 720, 960, 1080, 1440, 2160, 2880, 4320, 0xFFFFFFFFu};
         uint32_t v = s.render_max_height ? s.render_max_height : 0xFFFFFFFFu;
         v = NextOf(k, std::size(k), v, d);
         s.render_max_height = v == 0xFFFFFFFFu ? 0 : v;
     }},
    {"MSAA", [](const Settings& s) { return std::string(kMsaaNames[IndexOf(kMsaaValues, s.msaa)]); },
     [](Settings& s, int d) {
         s.msaa = kMsaaValues[std::clamp(IndexOf(kMsaaValues, s.msaa) + d, 0, int(std::size(kMsaaValues)) - 1)];
     }},
    {"SMAA", [](const Settings& s) { return OnOff(s.smaa); }, [](Settings& s, int) { s.smaa = !s.smaa; }},
    {"Anisotropic filter", [](const Settings& s) { return std::string(kAnisoNames[IndexOf(kAnisoValues, s.aniso)]); },
     [](Settings& s, int d) {
         s.aniso = kAnisoValues[std::clamp(IndexOf(kAnisoValues, s.aniso) + d, 0, int(std::size(kAnisoValues)) - 1)];
     }},
    {"Brightness", [](const Settings& s) { return std::to_string(s.brightness); },
     [](Settings& s, int d) { s.brightness = Stepped(s.brightness, d, 0, 100, 5); }},
    {"Contrast", [](const Settings& s) { return std::to_string(s.contrast); },
     [](Settings& s, int d) { s.contrast = Stepped(s.contrast, d, 0, 100, 5); }},
    {"FPS counter", [](const Settings& s) { return OnOff(s.fps_counter); },
     [](Settings& s, int) { s.fps_counter = !s.fps_counter; }},
};

const GameRow kPostRows[] = {
    {"Bloom",
     [](const Settings& s) {
         return s.game_bloom == 0 ? std::string("Off") : s.game_bloom == 100 ? std::string("Game")
                                                                             : std::to_string(s.game_bloom) + "%";
     },
     [](Settings& s, int d) { s.game_bloom = Stepped(s.game_bloom, d, 0, 200, 10); }},
    {"Sharpening", [](const Settings& s) { return Num(s.sharpen, "Off"); },
     [](Settings& s, int d) { s.sharpen = Stepped(s.sharpen, d, 0, 100, 5); }},
    {"Saturation", [](const Settings& s) { return std::to_string(s.saturation) + "%"; },
     [](Settings& s, int d) { s.saturation = Stepped(s.saturation, d, 0, 200, 10); }},
    {"Vibrance", [](const Settings& s) { return Num(s.vibrance, "Off"); },
     [](Settings& s, int d) { s.vibrance = Stepped(s.vibrance, d, 0, 100, 5); }},
    {"Warmth",
     [](const Settings& s) {
         return s.warmth == 50 ? std::string("Game") : (s.warmth < 50 ? "Cool " : "Warm ") + std::to_string(s.warmth);
     },
     [](Settings& s, int d) { s.warmth = Stepped(s.warmth, d, 0, 100, 5); }},
    {"Vignette", [](const Settings& s) { return Num(s.vignette, "Off"); },
     [](Settings& s, int d) { s.vignette = Stepped(s.vignette, d, 0, 100, 5); }},
    {"Film grain", [](const Settings& s) { return Num(s.grain, "Off"); },
     [](Settings& s, int d) { s.grain = Stepped(s.grain, d, 0, 100, 5); }},
};

const GameRow kAudioRows[] = {
    {"Speakers *", [](const Settings& s) { return std::string(s.speakers == 6 ? "5.1 surround" : "Stereo"); },
     [](Settings& s, int) { s.speakers = s.speakers == 6 ? 2 : 6; }},
};

const GameRow kControlsRows[] = {
    {"Keyboard and mouse", [](const Settings& s) { return OnOff(s.kbm); }, [](Settings& s, int) { s.kbm = !s.kbm; }},
    {"Mouse sensitivity", [](const Settings& s) { return std::to_string(s.mouse_sensitivity); },
     [](Settings& s, int d) { s.mouse_sensitivity = Stepped(s.mouse_sensitivity, d, 1, 100, 1); }},
    {"Invert mouse Y", [](const Settings& s) { return OnOff(s.mouse_invert_y); },
     [](Settings& s, int) { s.mouse_invert_y = !s.mouse_invert_y; }},
    {"Camera keys in menus", [](const Settings& s) { return OnOff(s.kbm_arrows_menu); },
     [](Settings& s, int) { s.kbm_arrows_menu = !s.kbm_arrows_menu; }},
    {"Use controller", [](const Settings& s) { return OnOff(s.controller); },
     [](Settings& s, int) { s.controller = !s.controller; }},
    {"Left deadzone", [](const Settings& s) { return std::to_string(s.deadzone_left) + "%"; },
     [](Settings& s, int d) { s.deadzone_left = Stepped(s.deadzone_left, d, 0, 50, 1); }},
    {"Left sensitivity", [](const Settings& s) { return std::to_string(s.sensitivity_left) + "%"; },
     [](Settings& s, int d) { s.sensitivity_left = Stepped(s.sensitivity_left, d, 25, 200, 5); }},
    {"Right deadzone", [](const Settings& s) { return std::to_string(s.deadzone_right) + "%"; },
     [](Settings& s, int d) { s.deadzone_right = Stepped(s.deadzone_right, d, 0, 50, 1); }},
    {"Right sensitivity", [](const Settings& s) { return std::to_string(s.sensitivity_right) + "%"; },
     [](Settings& s, int d) { s.sensitivity_right = Stepped(s.sensitivity_right, d, 25, 200, 5); }},
    {"Invert camera X", [](const Settings& s) { return OnOff(s.invert_x); },
     [](Settings& s, int) { s.invert_x = !s.invert_x; }},
    {"Invert camera Y", [](const Settings& s) { return OnOff(s.invert_y); },
     [](Settings& s, int) { s.invert_y = !s.invert_y; }},
    {"Vibration", [](const Settings& s) { return OnOff(s.vibration); },
     [](Settings& s, int) { s.vibration = !s.vibration; }},
    {"Vibration strength", [](const Settings& s) { return std::to_string(s.vibration_strength) + "%"; },
     [](Settings& s, int d) { s.vibration_strength = Stepped(s.vibration_strength, d, 0, 100, 5); }},
};

const GameRow kAccessRows[] = {
    {"Colour filter",
     [](const Settings& s) {
         static const char* const k[] = {"Off", "Protanopia", "Deuteranopia", "Tritanopia"};
         return std::string(k[std::min(s.colorblind, 3u)]);
     },
     [](Settings& s, int d) { s.colorblind = uint32_t(Cycled(int(std::min(s.colorblind, 3u)), d, 4)); }},
    {"Filter strength", [](const Settings& s) { return std::to_string(s.colorblind_strength) + "%"; },
     [](Settings& s, int d) { s.colorblind_strength = Stepped(s.colorblind_strength, d, 0, 100, 5); }},
};

const GameRow kEnhanceRows[] = {
    {"Button prompts",
     [](const Settings& s) {
         static const char* const k[] = {"Automatic", "Controller", "Keyboard"};
         return std::string(k[std::min(s.button_prompts, 2u)]);
     },
     [](Settings& s, int d) { s.button_prompts = uint32_t(Cycled(int(std::min(s.button_prompts, 2u)), d, 3)); }},
    {"Each tutorial once", [](const Settings& s) { return OnOff(s.tutorials_once); },
     [](Settings& s, int) { s.tutorials_once = !s.tutorials_once; }},
    {"\"Time to turn in\" lines", [](const Settings& s) { return std::string(s.quiet_turn_in ? "Left out" : "Kept"); },
     [](Settings& s, int) { s.quiet_turn_in = !s.quiet_turn_in; }},
    {"Reparo's own sound", [](const Settings& s) { return OnOff(s.reparo_sound); },
     [](Settings& s, int) { s.reparo_sound = !s.reparo_sound; }},
    // one switch for the three, as F10's (on: turning twice as quick, the instant first step, full speed in turns)
    {"Responsive movement",
     [](const Settings& s) { return OnOff(s.turn_response > 100 || s.instant_step || s.turn_full_speed); },
     [](Settings& s, int) {
         bool on = !(s.turn_response > 100 || s.instant_step || s.turn_full_speed);
         s.turn_response = on ? 200u : 100u;
         s.instant_step = s.turn_full_speed = on;
     }},
    {"Smooth stretched textures", [](const Settings& s) { return OnOff(s.smooth_textures); },
     [](Settings& s, int) { s.smooth_textures = !s.smooth_textures; }},
    {"Cutscene bars", [](const Settings& s) { return std::string(s.cutscene_bars_hidden ? "Hidden" : "Shown"); },
     [](Settings& s, int) { s.cutscene_bars_hidden = !s.cutscene_bars_hidden; }},
    {"Saving compass", [](const Settings& s) { return std::string(s.hide_save_icon ? "Hidden" : "Shown"); },
     [](Settings& s, int) { s.hide_save_icon = !s.hide_save_icon; }},
    {"Skip logo movies", [](const Settings& s) { return OnOff(s.skip_intro); },
     [](Settings& s, int) { s.skip_intro = !s.skip_intro; }},
};

struct GamePage {
    const char* name;
    const GameRow* rows;
    int n;
};

#define GAME_PAGE(name, rows) {name, rows, int(std::size(rows))}
const GamePage kGamePages[] = {
    GAME_PAGE("Display", kDisplayRows),        GAME_PAGE("Graphics", kGraphicsRows),
    GAME_PAGE("Post-processing", kPostRows),   GAME_PAGE("Audio", kAudioRows),
    GAME_PAGE("Controls", kControlsRows),      GAME_PAGE("Accessibility", kAccessRows),
    GAME_PAGE("Enhancements", kEnhanceRows),
};
#undef GAME_PAGE

const GamePage* PageAt(int p) { return p >= 0 && p < int(std::size(kGamePages)) ? &kGamePages[p] : nullptr; }

// the box: as the quit box (a dark panel with a gold rule, the game's font), the page's name on top, its rows, and
// the buttons at the bottom. Rows past kShown scroll (hle_quit.c keeps sel within top .. top + kShown - 1)
void DrawPcSettings(int page, int sel, int top) {
    constexpr int kShown = 9;  // hle_quit.c's kPcShown
    ImFont* f = g_game_font ? g_game_font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float gx, gy, gw, gh;
    GameRect(gx, gy, gw, gh);
    const ImU32 parchment = IM_COL32(236, 222, 186, 255), dim = IM_COL32(200, 186, 150, 255);
    const ImU32 gold = IM_COL32(196, 158, 82, 255), lit = IM_COL32(255, 236, 160, 255);
    const float sz = gh * 0.04f, hsz = gh * 0.058f, step = gh * 0.057f;
    const float w = std::min(gw * 0.8f, gh * 1.0f), h = gh * 0.84f;
    const ImVec2 c(gx + gw * 0.5f, gy + gh * 0.5f), p0(c.x - w * 0.5f, c.y - h * 0.5f), p1(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), IM_COL32(0, 0, 0, 140));
    dl->AddRectFilled(p0, p1, IM_COL32(28, 20, 12, 240), sz * 0.3f);
    dl->AddRect(p0, p1, gold, sz * 0.3f, 0, std::max(1.5f, sz * 0.06f));

    const char* title = page >= 0 ? app_GameOptLabel(-1, page) : "PC Settings";
    ImVec2 tt = f->CalcTextSizeA(hsz, FLT_MAX, 0, title);
    ShadowText(dl, f, hsz, ImVec2(c.x - tt.x * 0.5f, p0.y + h * 0.04f), parchment, title);
    float rule = p0.y + h * 0.04f + tt.y + sz * 0.35f;
    dl->AddLine(ImVec2(p0.x + w * 0.06f, rule), ImVec2(p1.x - w * 0.06f, rule), gold, std::max(1.0f, sz * 0.04f));

    const int n = app_GameOptRows(page);
    const float lx = p0.x + w * 0.08f, vr = p1.x - w * 0.08f, y0 = rule + sz * 0.6f;
    bool restart = false;
    for (int i = 0; i < n; i++)
        if (const char* l = app_GameOptLabel(page, i); page >= 0 && *l && l[std::strlen(l) - 1] == '*') restart = true;
    for (int i = 0; i < kShown && top + i < n; i++) {
        const int r = top + i;
        const float y = y0 + step * float(i);
        const bool on = r == sel;
        if (on) {
            dl->AddRectFilled(ImVec2(p0.x + w * 0.04f, y - step * 0.08f), ImVec2(p1.x - w * 0.04f, y + step * 0.92f),
                              IM_COL32(196, 158, 82, 70), sz * 0.2f);
        }
        const ImU32 col = on ? lit : dim;
        ShadowText(dl, f, sz, ImVec2(lx, y), col, app_GameOptLabel(page, r));
        if (page < 0) continue;
        std::string v = app_GameOptValue(page, r);
        if (on) v = "<  " + v + "  >";  // left and right change it
        ImVec2 vt = f->CalcTextSizeA(sz, FLT_MAX, 0, v.c_str());
        ShadowText(dl, f, sz, ImVec2(vr - vt.x, y), col, v.c_str());
    }
    // more above or below: a small triangle beside the first or last row shown
    const float ax = p0.x + w * 0.05f, as = sz * 0.22f;
    if (top > 0) {
        const float y = y0 + sz * 0.5f;
        dl->AddTriangleFilled(ImVec2(ax, y - as), ImVec2(ax - as, y + as), ImVec2(ax + as, y + as), gold);
    }
    if (top + kShown < n) {
        const float y = y0 + step * float(kShown - 1) + sz * 0.5f;
        dl->AddTriangleFilled(ImVec2(ax - as, y - as), ImVec2(ax + as, y - as), ImVec2(ax, y + as), gold);
    }
    if (restart)
        ShadowText(dl, f, sz * 0.7f, ImVec2(lx, y0 + step * float(kShown) + sz * 0.1f), dim,
                   "* Applies the next time the game starts");

    // the buttons: A Select (the list) or Change (a page), B Back (Close on the list); each the key bound with
    // keyboard prompts, as the quit box's
    const float bsz = sz * 0.9f, by = p1.y - h * 0.065f, gap = bsz * 0.25f, between = bsz * 1.4f;
    const struct { int input; const char* pad; ImU32 col; const char* word; } kBtn[] = {
        {0, "A", IM_COL32(60, 160, 60, 255), page < 0 ? "Select" : "Change"},
        {1, "B", IM_COL32(190, 50, 40, 255), page < 0 ? "Close" : "Back"}};
    float kws[2], tws[2], total = between;
    for (int i = 0; i < 2; i++) {
        kws[i] = KeyCapWidth(f, bsz, kBtn[i].input, bsz);
        tws[i] = f->CalcTextSizeA(bsz, FLT_MAX, 0, kBtn[i].word).x;
        total += kws[i] + gap + tws[i];
    }
    float x = c.x - total * 0.5f;
    for (int i = 0; i < 2; i++) {
        const auto& b = kBtn[i];
        const ImVec2 kc(x + kws[i] * 0.5f, by);
        if (!KeyCap(dl, f, bsz, kc, b.input, b.col)) PadButton(dl, f, bsz, kc, b.pad, b.col, false);
        ShadowText(dl, f, bsz, ImVec2(x + kws[i] + gap, by - bsz * 0.55f), parchment, b.word);
        x += kws[i] + gap + tws[i] + between;
    }
}

// the window thread's messages: the pointer hidden, F10 and Alt+Enter, then ImGui; while the panel is open it keeps the
// keyboard and mouse
int MsgHook(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    InputMessage(hwnd, msg, wp, lp);  // xboxrecomp: keyboard and mouse as controller 1 (input.cpp)
    // xboxrecomp: no mouse pointer over the game; the panel shows it again
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && !g_open && !g_start && !TrainerOpen()) {
        SetCursor(nullptr);
        return 1;
    }
    bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (down && wp == VK_F10) {
        if (!(lp & (1 << 30)) && !g_start) ToggleMenu();  // not on auto-repeat; the start menu is already up
        return 1;
    }
#ifndef HPPOA_NO_TRAINER
    if (down && wp == VK_F6) {  // xboxrecomp (hppoa): the trainer (only built with HPPOA_TRAINER)
        if (!(lp & (1 << 30)) && !g_start) ToggleTrainer();
        return 1;
    }
#endif
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) {
        if (!(lp & (1 << 30))) {
            Settings& s = Get();
            s.display_mode = s.display_mode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
            Changed(true);
        }
        return 1;
    }
    if (!g_ui) return 0;
    std::lock_guard lock(UiLock());
    ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), msg, WPARAM(wp), LPARAM(lp));
    if (!g_open && !g_start && !TrainerOpen()) return 0;
    switch (msg) {
        case WM_KEYDOWN: case WM_CHAR: case WM_MOUSEWHEEL: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
            return 1;
        case WM_SYSKEYDOWN:
            return wp != VK_F4;  // Alt+F4 still closes
        default:
            return 0;
    }
}

}  // namespace

bool MenuOpen() { return g_open || g_start || TrainerOpen(); }

void ToggleMenu() { g_open = !g_open; }

void OpenStartMenu() { g_start = true; }

bool StartMenuOpen() { return g_start; }

void InitUi() {
    std::lock_guard lock(UiLock());
    if (g_ui) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4;
    // xboxrecomp (hppoa): the panel keeps ImGui's font (first, the default); the quit prompt uses the game's own
    io.Fonts->AddFontDefault();
    std::filesystem::path ttf = std::filesystem::current_path() / "game" / "data" / "gui" / "quorumstd-medium.ttf";  // game\ is the working folder's (the exe may be in bin\)
    std::error_code ec;
    if (std::filesystem::exists(ttf, ec)) g_game_font = io.Fonts->AddFontFromFileTTF(ttf.string().c_str(), 64.0f);
    ImGui_ImplWin32_Init(Window());
    g_ui = true;
    xbox_FramebufferWindowSetMsgHook(MsgHook);
    LOGI("options: F10 opens the panel, F6 the trainer, Alt+Enter flips windowed / borderless");
}

// xboxrecomp (hppoa): L3 + R3 on controller 1 opens or closes the panel (hle_input.c), once per press
extern "C" int hppoa_PadComboHeld(void);

void DrawUi() {
    InputFrame();
    {
        static bool held;
        bool now = hppoa_PadComboHeld() != 0;
        if (now && !held && !g_start) ToggleMenu();
        held = now;
    }
    ImGuiIO& io = ImGui::GetIO();
    // ImGui's 13 px font at 720 lines, bigger with the window
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    io.MouseDrawCursor = false;
    if (g_start) DrawPanel(true);
    else if (g_open) DrawPanel(false);
    if (!g_start && TrainerOpen()) DrawTrainer();
    if (Get().fps_counter) DrawFps();
    DrawPrebuild();
    if (int q = hppoa_QuitState()) DrawQuit(q);
    if (int page, sel, top; hppoa_PcSettings(&page, &sel, &top)) DrawPcSettings(page, sel, top);
}

}  // namespace app

// hle_input.c: the panel has the controller while it is open
extern "C" int hppoa_MenuOpen(void) { return app::MenuOpen(); }

// hle_quit.c / DrawPcSettings: PC Settings's pages. Page -1 is the list of pages; its rows are the pages' names, no
// values. The strings stay good until the next call on this thread
extern "C" int app_GameOptRows(int page) {
    if (page < 0) return int(std::size(app::kGamePages));
    const app::GamePage* p = app::PageAt(page);
    return p ? p->n : 0;
}

extern "C" const char* app_GameOptLabel(int page, int row) {
    if (page < 0) return app::PageAt(row) ? app::kGamePages[row].name : "";
    const app::GamePage* p = app::PageAt(page);
    return p && row >= 0 && row < p->n ? p->rows[row].label : "";
}

// the size rows' choice (aspect, width, height) waits here until the page is left: a resolution change switches the
// screen's mode or resizes the window, which each step would otherwise do
namespace {
app::Settings g_pend;
int g_pend_page = -1;
}  // namespace

extern "C" const char* app_GameOptValue(int page, int row) {
    static thread_local std::string v;
    const app::GamePage* p = app::PageAt(page);
    if (!p || row < 0 || row >= p->n) return "";
    std::lock_guard lock(app::UiLock());
    const app::GameRow& r = p->rows[row];
    v = r.value(r.held && g_pend_page == page ? g_pend : app::Get());
    return v.c_str();
}

extern "C" void app_GameOptStep(int page, int row, int dir) {
    const app::GamePage* p = app::PageAt(page);
    if (!p || row < 0 || row >= p->n) return;
    std::lock_guard lock(app::UiLock());
    const app::GameRow& r = p->rows[row];
    if (r.held) {
        if (g_pend_page != page) g_pend = app::Get(), g_pend_page = page;
        r.step(g_pend, dir < 0 ? -1 : 1);
        return;
    }
    r.step(app::Get(), dir < 0 ? -1 : 1);
    app::Changed(r.display);
}

// a page left (B, Start, the box closed): its size choice applied
extern "C" void app_GameOptLeave(int page) {
    std::lock_guard lock(app::UiLock());
    if (g_pend_page != page) return;
    g_pend_page = -1;
    app::Settings& s = app::Get();
    if (s.aspect == g_pend.aspect && s.width == g_pend.width && s.height == g_pend.height) return;
    s.aspect = g_pend.aspect;
    s.width = g_pend.width;
    s.height = g_pend.height;
    app::Changed(true);
}

// hle_quit.c: Quit To Desktop chosen. The pipeline library is written first (an exit skips the one at quit), then
// the title leaves as it does for its own exit (HalReturnToFirmware: the process ends with code 0)
namespace gpu { void SavePipelineCache(); }
extern "C" void __stdcall xbox_HalReturnToFirmware(unsigned long routine);
extern "C" void hppoa_QuitNow(void) {
    gpu::SavePipelineCache();
    xbox_HalReturnToFirmware(2);
}
