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
    Row("Trainer");
#ifdef HPPOA_RELEASE
    if (ImGui::Button("Open the trainer")) ToggleTrainer();  // no F6 in a release build
#else
    if (ImGui::Button("Open the trainer (F6)")) ToggleTrainer();
#endif
    ImGui::SameLine();
    ImGui::TextDisabled("god mode, items, game speed, the title's debug menu and variables");

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
        std::memcpy(&s.smooth_textures, &d.smooth_textures, sizeof(s.smooth_textures));
        std::memcpy(&s.hide_save_icon, &d.hide_save_icon, sizeof(s.hide_save_icon));
        std::memcpy(&s.cutscene_bars_hidden, &d.cutscene_bars_hidden, sizeof(s.cutscene_bars_hidden));
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
        if (!KeyCap(dl, f, sz, ImVec2(tp.x - sz * 0.35f - kw * 0.5f, cy), 13, IM_COL32(90, 90, 90, 255)))
            PadButton(dl, f, sz, ImVec2(tp.x - sz * 1.15f, cy), "BACK", IM_COL32(70, 70, 70, 235), true);
        ShadowText(dl, f, sz, tp, parchment, text);
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
#ifndef HPPOA_RELEASE
    if (down && wp == VK_F6) {  // xboxrecomp (hppoa): the trainer (not in a release build)
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
    std::filesystem::path ttf = ExeDir() / "game" / "data" / "gui" / "quorumstd-medium.ttf";
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
}

}  // namespace app

// hle_input.c: the panel has the controller while it is open
extern "C" int hppoa_MenuOpen(void) { return app::MenuOpen(); }

// hle_quit.c: Quit To Desktop chosen. The pipeline library is written first (an exit skips the one at quit), then
// the title leaves as it does for its own exit (HalReturnToFirmware: the process ends with code 0)
namespace gpu { void SavePipelineCache(); }
extern "C" void __stdcall xbox_HalReturnToFirmware(unsigned long routine);
extern "C" void hppoa_QuitNow(void) {
    gpu::SavePipelineCache();
    xbox_HalReturnToFirmware(2);
}
