// hppoa_launcher.exe: picks 4:3 or 16:9 and the graphics settings, writes them to hppoa.toml beside it and starts
// hppoa.exe with the environment run_win.sh gives it. Behind the page the title as text over a coloured backdrop (no
// game art). Dear ImGui over Direct3D 11.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "gamefiles.h"
#include "font/title_font.inc"  // Magic School One (FontMesa), the title's font

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ── settings: hppoa.toml, the keys the game reads (renderer/app/app.cpp) ─────────────────────────────────────────────

struct Settings {
    bool wide = false;            // aspect '16:9', otherwise '4:3'
    int display = 0;              // 'windowed', 'borderless', 'fullscreen'
    uint32_t width = 1920, height = 1080;
    uint32_t internal_height = 0;
    uint32_t msaa = 0, aniso = 0;
    bool smaa = false, vsync = true;
    bool show = true;             // show_launcher: false starts the game without this page
    int fps30 = 0;                // frame_rate: 0 60 fps, 1 30 (the game's own)
    int colorblind = 0;           // 'off', 'protanopia', 'deuteranopia', 'tritanopia'
    uint32_t cb_strength = 100;
    bool kbm = true;              // keyboard_mouse
    uint32_t mouse_sens = 50;
    bool vibration = true, invert_x = false, invert_y = false;
    bool controller = true;  // the in-game F10 option of the same name
    int speakers = 0;        // speakers: 0 stereo (2), 1 5.1 surround (6), as F10 > Audio > Speakers
};

const char* const kColorBlindKeys[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
const char* const kColorBlindNames[] = {"Off", "Protanopia (red-weak)", "Deuteranopia (green-weak)", "Tritanopia (blue-weak)"};

const char* const kSpeakerNames[] = {"Stereo", "5.1 surround"};

const char* const kDisplayKeys[] = {"windowed", "borderless", "fullscreen"};
const char* const kDisplayNames[] = {"Windowed", "Borderless", "Fullscreen"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"Off", "2x", "4x", "8x"};
constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"Game's", "2x", "4x", "8x", "16x"};

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1) - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// hppoa.toml as its lines, so a save keeps the comments and every key this page does not touch
std::vector<std::string> g_lines;

std::string Value(const std::string& key) {
    for (const auto& l : g_lines) {
        std::string t = Trim(l);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos || Trim(t.substr(0, eq)) != key) continue;
        std::string v = Trim(t.substr(eq + 1));
        if (v.size() >= 2 && (v[0] == '\'' || v[0] == '"')) v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

void SetValue(const std::string& key, const std::string& v) {
    for (auto& l : g_lines) {
        std::string t = Trim(l);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq != std::string::npos && Trim(t.substr(0, eq)) == key) {
            l = key + " = " + v;
            return;
        }
    }
    g_lines.push_back(key + " = " + v);
}

std::string TomlPath() { return Narrow(ExeDir()) + "hppoa.toml"; }

Settings Load() {
    Settings s;
    g_lines.clear();
    std::ifstream f(TomlPath());
    for (std::string l; std::getline(f, l);) g_lines.push_back(l);
    auto u = [](const std::string& v, uint32_t d) { return v.empty() ? d : uint32_t(std::strtoul(v.c_str(), nullptr, 10)); };
    s.wide = Value("aspect") == "16:9";
    std::string dm = Value("display_mode");
    for (int i = 0; i < 3; i++)
        if (dm == kDisplayKeys[i]) s.display = i;
    s.width = u(Value("width"), s.width);
    s.height = u(Value("height"), s.height);
    s.internal_height = u(Value("internal_height"), 0);
    s.msaa = u(Value("msaa"), 0);
    s.aniso = u(Value("anisotropic_filtering"), 0);
    s.smaa = Value("smaa") == "true";
    s.vsync = Value("vsync") != "false";
    s.show = Value("show_launcher") != "false";
    s.fps30 = Value("frame_rate") == "30" ? 1 : 0;
    std::string cb = Value("colorblind");
    for (int i = 0; i < 4; i++)
        if (cb == kColorBlindKeys[i]) s.colorblind = i;
    s.cb_strength = std::min(u(Value("colorblind_strength"), 100), 100u);
    s.kbm = Value("keyboard_mouse") != "false";
    s.mouse_sens = std::clamp(u(Value("mouse_sensitivity"), 50), 1u, 100u);
    s.vibration = Value("vibration") != "false";
    s.controller = Value("controller") != "false";
    s.invert_x = Value("invert_camera_x") == "true";
    s.invert_y = Value("invert_camera_y") == "true";
    s.speakers = u(Value("speakers"), 2) == 6 ? 1 : 0;
    return s;
}

void Save(const Settings& s) {
    auto q = [](const char* v) { return std::string("'") + v + "'"; };
    SetValue("display_mode", q(kDisplayKeys[s.display]));
    SetValue("width", std::to_string(s.width));
    SetValue("height", std::to_string(s.height));
    SetValue("aspect", q(s.wide ? "16:9" : "4:3"));
    SetValue("internal_height", std::to_string(s.internal_height));
    SetValue("msaa", std::to_string(s.msaa));
    SetValue("smaa", s.smaa ? "true" : "false");
    SetValue("anisotropic_filtering", std::to_string(s.aniso));
    SetValue("vsync", s.vsync ? "true" : "false");
    SetValue("show_launcher", s.show ? "true" : "false");
    SetValue("start_menu", "false");  // this page instead of the in-game one
    SetValue("frame_rate", s.fps30 ? "30" : "60");
    SetValue("colorblind", q(kColorBlindKeys[s.colorblind]));
    SetValue("colorblind_strength", std::to_string(s.cb_strength));
    SetValue("keyboard_mouse", s.kbm ? "true" : "false");
    SetValue("mouse_sensitivity", std::to_string(s.mouse_sens));
    SetValue("vibration", s.vibration ? "true" : "false");
    SetValue("controller", s.controller ? "true" : "false");
    SetValue("invert_camera_x", s.invert_x ? "true" : "false");
    SetValue("invert_camera_y", s.invert_y ? "true" : "false");
    SetValue("speakers", s.speakers ? "6" : "2");
    std::ofstream f(TomlPath(), std::ios::trunc);
    for (const auto& l : g_lines) f << l << "\n";
}

// sizes of the chosen shape, biggest first: the monitor's own and the common ones that fit the desktop
bool Shaped(uint32_t w, uint32_t h, bool wide) {
    float r = float(w) / float(h), want = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    return r > want - 0.02f && r < want + 0.02f;
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

// the biggest size of a shape inside w x h, even on both sides (a 3440 x 1440 screen holds 2560 x 1440 at 16:9)
std::pair<uint32_t, uint32_t> FitShape(uint32_t w, uint32_t h, bool wide) {
    uint32_t rw = wide ? 16 : 4, rh = wide ? 9 : 3, k = std::min(w / rw, h / rh) & ~1u;
    return {k * rw, k * rh};
}

// up to the screen's biggest mode: its own modes of the shape, the biggest of the shape it holds, the common ones it
// holds; the screen's own size on top when it has another shape (16:10, ultrawide), the picture keeping its shape in it
std::vector<std::pair<uint32_t, uint32_t>> Sizes(bool wide) {
    static const std::pair<uint32_t, uint32_t> kCommon[] = {
        {640, 480},   {800, 600},   {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440},
        {2048, 1536}, {2880, 2160}, {1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
        {3840, 2160}, {5120, 2880}, {7680, 4320}, {3840, 2880}, {5760, 4320}};
    auto [sw, sh] = ScreenMax();
    std::vector<std::pair<uint32_t, uint32_t>> m;
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++)
        if (dm.dmPelsWidth >= 640 && Shaped(dm.dmPelsWidth, dm.dmPelsHeight, wide))
            m.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
    auto fit = FitShape(sw, sh, wide);
    if (fit.first >= 640) m.push_back(fit);
    for (auto [w, h] : kCommon)
        if (Shaped(w, h, wide) && w <= sw && h <= sh) m.emplace_back(w, h);
    std::sort(m.begin(), m.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
    m.erase(std::unique(m.begin(), m.end()), m.end());
    if (!Shaped(sw, sh, wide)) m.insert(m.begin(), {sw, sh});
    return m;
}

// the size kept when it has the shape or is the screen's own, otherwise the biggest that has
void FitSize(Settings& s) {
    if (s.width && s.height && (Shaped(s.width, s.height, s.wide) || std::pair{s.width, s.height} == ScreenMax())) return;
    auto m = Sizes(s.wide);
    if (m.empty()) return;
    s.width = m.front().first;
    s.height = m.front().second;
}

// hppoa.exe beside this one, with run_win.sh's environment
std::string FilesLogLine();

bool StartGame() {
    const std::pair<const wchar_t*, const wchar_t*> kEnv[] = {
        {L"RECOMP_TITLE_KEVENTS", L"1"}, {L"RECOMP_VBLANK", L"1"},       {L"RECOMP_AC97_READY", L"1"},
        {L"RECOMP_DSP_ACK", L"0x834D0810"}, {L"RECOMP_PB_EXEC", L"1"}, {L"RECOMP_FB_WINDOW", L"1"},
        {L"RECOMP_USB", L"1"},           {L"RECOMP_START_MENU", L"0"},  {L"RECOMP_ACK_SLEEP_US", L"250"},
        {L"RECOMP_HEAP_RECLAIM", L"1"},
    };
    for (auto [k, v] : kEnv)
        if (!GetEnvironmentVariableW(k, nullptr, 0)) SetEnvironmentVariableW(k, v);
    std::wstring dir = ExeDir(), exe = dir + L"hppoa.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    // the game's console output (its [CRASH] reports among it) to hppoa_run.log beside it: a windowed launcher has no
    // console to hand down
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW((dir + L"hppoa_run.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (log != INVALID_HANDLE_VALUE) {
        // the game folder's state first, so a log sent in says whether the files were whole
        std::string files = FilesLogLine();
        DWORD put = 0;
        WriteFile(log, files.data(), DWORD(files.size()), &put, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = si.hStdError = log;
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, dir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ── Game files ──────────────────────────────────────────────────────────────────────────────────────────────────────
// game\ checked against the disc's files (gamefiles.h). Every byte is read once, on a fresh start (no game_check.txt
// made under this list), and again only from the page's Verify button; every other
// start looks at the sizes alone (a few ms) and never reads them all on its own. The full check's result, the damaged
// files by name, is kept in game_check.txt beside the launcher and reported on later starts until a Verify clears it; a damaged or missing file is named on the page and in the run log (2026-10-29: a player's Burrow had no
// music, and nothing said whether their files were whole; from the CoS port).

struct Check {
    GameCheck c{};
    HANDLE thread = nullptr;
    BOOL contents = FALSE;
    BOOL finished = FALSE;  // ran to the end (not cancelled, nothing failed)
    bool ran = false;       // a check has finished this session
    std::string date;       // when the contents were last checked, "" never
};
Check g_check;

std::wstring GameDir() { return ExeDir() + L"game"; }
std::wstring StampPath() { return ExeDir() + L"game_check.txt"; }

std::string Today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char d[16];
    std::snprintf(d, sizeof d, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return d;
}

// the last full check, when it was made under this list
struct Stamp {
    bool valid = false;
    std::string date;
    std::vector<std::string> damaged;  // its files with wrong contents (missing and wrong-size ones a size check finds)
};

Stamp ReadStamp() {
    Stamp st;
    std::ifstream f(Narrow(StampPath()));
    char want[32];
    std::snprintf(want, sizeof want, "list %08lX", (unsigned long)gamefiles_ListId());
    std::string l;
    if (!std::getline(f, l) || Trim(l) != want) return st;
    while (std::getline(f, l)) {
        l = Trim(l);
        if (l.rfind("date ", 0) == 0) st.date = l.substr(5);
        else if (l.rfind("damaged ", 0) == 0) st.damaged.push_back(l.substr(8));
    }
    st.valid = !st.date.empty();
    return st;
}

void WriteStamp() {
    std::ofstream f(Narrow(StampPath()), std::ios::trunc);
    char list[32];
    std::snprintf(list, sizeof list, "list %08lX", (unsigned long)gamefiles_ListId());
    f << list << "\n"
      << "date " << g_check.date << "\n";
    for (int k = 0; k < g_check.c.listed; k++)
        if (g_check.c.kind[k] == GAMEFILE_CONTENT) f << "damaged " << gamefiles_Name(g_check.c.index[k]) << "\n";
    f << "# hppoa_launcher.exe's last full read of the game folder. Later starts check the sizes only; the launcher's\n"
         "# Verify game files button reads everything again (or delete this file to do it at the next start).\n";
}

DWORD WINAPI CheckThread(void*) {
    g_check.finished = gamefiles_Check(GameDir().c_str(), g_check.contents, &g_check.c);
    return 0;
}

bool StartCheck(bool contents) {
    g_check.c = GameCheck{};
    g_check.contents = contents;
    g_check.finished = FALSE;
    g_check.thread = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    return g_check.thread != nullptr;
}

bool FilesWhole() {
    return g_check.c.missing + g_check.c.wrong_size + g_check.c.wrong_content == 0;
}

// after a check: a full one recorded; a size check gets the damage the last full one found added, the stamp kept
void FinishCheck(const Stamp* st = nullptr) {
    g_check.ran = true;
    if (!g_check.finished) return;
    if (g_check.contents) {
        g_check.date = Today();
        WriteStamp();
    } else if (st) {
        for (const std::string& name : st->damaged)
            gamefiles_AddResult(&g_check.c, gamefiles_Find(name.c_str()), GAMEFILE_CONTENT);
    }
}

const char* KindName(int kind) {
    return kind == GAMEFILE_MISSING ? "missing" : kind == GAMEFILE_SIZE ? "wrong size" : "damaged";
}

// the run log's first line
std::string FilesLogLine() {
    char head[200];
    if (!g_check.ran) return "[FILES] game folder not checked\r\n";
    if (!g_check.finished) {
        return "[FILES] game folder check stopped: " + Narrow(g_check.c.error) + "\r\n";
    }
    std::snprintf(head, sizeof head, "[FILES] game folder: %d files listed, contents checked %s%s: ", gamefiles_Count(),
                  g_check.date.empty() ? "never" : g_check.date.c_str(), g_check.contents ? "" : ", sizes now");
    std::string line = head;
    if (FilesWhole()) return line + "all match the disc\r\n";
    std::snprintf(head, sizeof head, "%d missing, %d wrong size, %d damaged:", g_check.c.missing, g_check.c.wrong_size,
                  g_check.c.wrong_content);
    line += head;
    for (int k = 0; k < g_check.c.listed; k++)
        line += std::string(" ") + gamefiles_Name(g_check.c.index[k]) + " (" + KindName(g_check.c.kind[k]) + ")";
    return line + "\r\n";
}

void DrawCheck() {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##check", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Checking the game files");
    ImGui::TextWrapped("Every file in the game folder is compared with the game disc's, so a damaged or missing one "
                       "is found before you play. This happens once.");
    ImGui::Spacing();
    double done = double(g_check.c.done), total = double(g_check.c.total);
    char text[64];
    std::snprintf(text, sizeof text, "%.2f of %.2f GB", done / 1e9, total / 1e9);
    ImGui::ProgressBar(total > 0 ? float(done / total) : 0.0f, ImVec2(-FLT_MIN, 28 * sc), text);
    ImGui::TextDisabled("%s", gamefiles_Name(g_check.c.current));
    ImGui::Spacing();
    bool cancelling = g_check.c.cancel != 0;
    if (cancelling) ImGui::BeginDisabled();
    if (ImGui::Button("Skip", ImVec2(160 * sc, 36 * sc))) InterlockedExchange(&g_check.c.cancel, 1);
    if (cancelling) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("It runs again at the next start.");
    ImGui::End();
}

enum class FilesAction { None, Verify };

// the page's game files part: a line when all is well, the files named and what to do when not
FilesAction DrawFiles(float sc) {
    FilesAction act = FilesAction::None;
    if (g_check.ran && g_check.finished && !FilesWhole()) {
        int bad = g_check.c.missing + g_check.c.wrong_size + g_check.c.wrong_content;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.4f, 1));
        if (g_check.c.missing == gamefiles_Count())
            ImGui::TextWrapped("No game files found. Put the game's files in the 'game' folder beside the launcher.");
        else
            ImGui::TextWrapped("%d game file%s %s damaged or missing. The game may crash, or lose sound or "
                               "pictures, where it uses them:", bad, bad == 1 ? "" : "s", bad == 1 ? "is" : "are");
        ImGui::PopStyleColor();
        if (g_check.c.missing != gamefiles_Count()) {
            int shown = std::min(g_check.c.listed, 6);
            for (int k = 0; k < shown; k++)
                ImGui::BulletText("%s (%s)", gamefiles_Name(g_check.c.index[k]), KindName(g_check.c.kind[k]));
            if (bad > shown) ImGui::TextDisabled("...and %d more (all of them in the run log)", bad - shown);
            ImGui::TextWrapped("Copy them again from your disc or game dump, then press Verify game files.");
        }
    }
    if (ImGui::Button("Verify game files", ImVec2(200 * sc, 0))) act = FilesAction::Verify;
    ImGui::SameLine();
    if (!g_check.ran) ImGui::TextDisabled("Game files not checked.");
    else if (!g_check.finished) ImGui::TextDisabled("Skipped: runs again next start.");
    else if (FilesWhole())
        ImGui::TextDisabled("All match the disc (checked %s).", g_check.date.c_str());
    else if (g_check.contents) ImGui::TextDisabled("Read through just now.");
    else ImGui::TextDisabled("Sizes checked; contents %s.", g_check.date.c_str());
    return act;
}

// ── Direct3D 11 ─────────────────────────────────────────────────────────────────────────────────────────────────────

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void MakeRtv() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    // xboxrecomp: d3d11 and what it loads (dxgi) from System32 only, not the exe's folder: a dxgi.dll left beside the
    // exes (DXVK's, put there for the game) was taken by Wine's own d3d11, which then called DXGID3D10CreateDevice, a
    // function DXVK's dxgi lacks: the launcher crashed before its window opened (2026-10-29, the CoS port). Not linked,
    // so nothing loads it before this.
    HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
                              reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")))
                        : nullptr;
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                 &g_swap, &g_dev, &fl, &g_ctx)))
        return false;
    MakeRtv();
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_swap && wp != SIZE_MINIMIZED) {
                if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
                g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                MakeRtv();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── the page ────────────────────────────────────────────────────────────────────────────────────────────────────────

template <size_t N>
int IndexOf(const uint32_t (&values)[N], uint32_t v) {
    for (size_t i = 0; i < N; i++)
        if (values[i] == v) return int(i);
    return 0;
}

ImFont* g_title_font = nullptr;  // Magic School One, for the title (WinMain)

// xboxrecomp: a coloured backdrop with the title as plain text in Magic School One, so the page says which game it starts
// without carrying any of the game's artwork (2026-10-29: the picture made from the title's textures is gone, in every
// build, for copyright)
void DrawBackground() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float ww = io.DisplaySize.x, wh = io.DisplaySize.y;
    // purple, lighter at the top
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ww, wh), IM_COL32(84, 44, 128, 255), IM_COL32(84, 44, 128, 255), IM_COL32(30, 14, 52, 255), IM_COL32(30, 14, 52, 255));
    ImFont* f = g_title_font ? g_title_font : ImGui::GetFont();
    // "Harry Potter" on its own line, bigger; the rest of the title under it on one line, made smaller to fit if need be.
    // Magic School One draws small for its size, so both are bigger than a book face would be
    struct Line {
        const char* text;
        float size;
    };
    float big = std::min(wh * 0.15f, ww * 0.13f), rest = big * 0.62f;
    float rest_w = f->CalcTextSizeA(rest, FLT_MAX, 0, "and the Prisoner of Azkaban").x;
    if (rest_w > ww * 0.92f) rest *= ww * 0.92f / rest_w;
    const Line lines[2] = {{"Harry Potter", big}, {"and the Prisoner of Azkaban", rest}};
    float y = wh * 0.05f;
    for (const Line& l : lines) {
        ImVec2 t = f->CalcTextSizeA(l.size, FLT_MAX, 0, l.text);
        ImVec2 p((ww - t.x) * 0.5f, y);
        dl->AddText(f, l.size, ImVec2(p.x + 2, p.y + 2), IM_COL32(0, 0, 0, 200), l.text);
        dl->AddText(f, l.size, p, IM_COL32(222, 186, 92, 255), l.text);
        y += l.size * 0.9f;
    }
}

// returns true when Play is pressed (settings saved)
bool DrawPage(Settings& s, std::string& error, FilesAction& files) {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##page", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    // the shape, two big choices
    float bw = (pw - ImGui::GetStyle().WindowPadding.x * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    for (int i = 0; i < 2; i++) {
        bool on = s.wide == (i == 1);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.55f, 0.15f, 1.0f));
        if (ImGui::Button(i ? "16:9  widescreen" : "4:3  original", ImVec2(bw, 44 * sc))) {
            s.wide = i == 1;
            FitSize(s);
        }
        if (on) ImGui::PopStyleColor();
        if (!i) ImGui::SameLine();
    }
    ImGui::Spacing();
    ImGui::PushItemWidth(pw * 0.5f);

    ImGui::Combo("Display mode", &s.display, kDisplayNames, 3);
    {
        auto sizes = Sizes(s.wide);
        std::string cur = std::to_string(s.width) + " x " + std::to_string(s.height);
        if (ImGui::BeginCombo(s.display == 0 ? "Window size" : "Resolution", cur.c_str())) {
            for (auto [w, h] : sizes) {
                std::string name = std::to_string(w) + " x " + std::to_string(h);
                if (std::pair{w, h} == ScreenMax()) name += "  (screen)";
                if (ImGui::Selectable(name.c_str(), w == s.width && h == s.height)) {
                    s.width = w;
                    s.height = h;
                }
            }
            ImGui::EndCombo();
        }
    }
    {
        auto choices = InternalChoices(ScreenMax().second, s.internal_height);
        const InternalChoice* cur = &choices.front();
        for (const auto& c : choices)
            if (c.lines == s.internal_height) cur = &c;
        if (ImGui::BeginCombo("Internal resolution", cur->name.c_str())) {
            for (const auto& c : choices)
                if (ImGui::Selectable(c.name.c_str(), &c == cur)) s.internal_height = c.lines;
            ImGui::EndCombo();
        }
    }
    int ms = IndexOf(kMsaaValues, s.msaa);
    if (ImGui::Combo("MSAA", &ms, kMsaaNames, int(std::size(kMsaaNames)))) s.msaa = kMsaaValues[ms];
    int an = IndexOf(kAnisoValues, s.aniso);
    if (ImGui::Combo("Anisotropic filtering", &an, kAnisoNames, int(std::size(kAnisoNames)))) s.aniso = kAnisoValues[an];
    ImGui::Checkbox("SMAA", &s.smaa);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Vsync", &s.vsync);
    const char* const kRate[] = {"60 fps", "30 fps (the game's own)"};
    ImGui::Combo("Frame rate", &s.fps30, kRate, 2);
    ImGui::Combo("Speakers", &s.speakers, kSpeakerNames, 2);
    if (s.speakers) ImGui::TextDisabled("5.1 needs Windows' speaker setup set to 5.1 (or a 5.1 HDMI receiver).");
    ImGui::Combo("Colour-blind filter", &s.colorblind, kColorBlindNames, 4);
    if (s.colorblind) {
        int st = int(s.cb_strength);
        if (ImGui::SliderInt("Filter strength %", &st, 0, 100)) s.cb_strength = uint32_t(st);
    }
    ImGui::Checkbox("Keyboard and mouse", &s.kbm);
    if (s.kbm) {
        ImGui::SameLine(0, 30 * sc);
        ImGui::SetNextItemWidth(pw * 0.3f);
        int ms = int(s.mouse_sens);
        if (ImGui::SliderInt("Mouse sensitivity", &ms, 1, 100)) s.mouse_sens = uint32_t(ms);
    }
    ImGui::Checkbox("Use controller", &s.controller);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Vibration", &s.vibration);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Invert camera X", &s.invert_x);
    ImGui::SameLine(0, 30 * sc);
    ImGui::Checkbox("Invert camera Y", &s.invert_y);
    ImGui::TextDisabled("Key bindings, button remapping and stick settings: F10 > Controls in the game.");
    ImGui::PopItemWidth();

    ImGui::Spacing();
    files = DrawFiles(sc);
    ImGui::Spacing();
    bool skip = !s.show;
    if (ImGui::Checkbox("Don't show this at the next startup", &skip)) s.show = !skip;
    if (skip) ImGui::TextDisabled("Play starts the game directly. Hold Shift while starting, or use F10 in the game, to bring this back.");
    ImGui::Spacing();
    bool play = false;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Play", ImVec2(bw, 40 * sc))) play = true;
    ImGui::PopStyleColor(2);
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(bw, 40 * sc))) PostQuitMessage(0);
    if (!error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error.c_str());
    ImGui::TextDisabled("Saved to hppoa.toml. F10 in the game changes the picture settings too.");
    ImGui::End();

    if (play) Save(s);
    return play;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool unpacking = false;  // no disc image unpacking in this launcher
    // xboxrecomp: then the game files: read through on a fresh start (a page with its progress), otherwise their sizes
    // only, here (a few ms)
    bool checking = false;
    if (!unpacking) {
        Stamp st = ReadStamp();
        if (!st.valid) {
            checking = StartCheck(true);  // a fresh start: every byte, once
        } else {
            g_check.date = st.date;
            g_check.contents = FALSE;
            g_check.finished = gamefiles_Check(GameDir().c_str(), FALSE, &g_check.c);
            FinishCheck(&st);
        }
    }
    // the page was turned off: straight to the game with the saved settings, unless Shift is held or a file is wrong
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!checking && FilesWhole() && !Load().show && !shift && StartGame()) {
        CoUninitialize();
        return 0;
    }
    SetProcessDPIAware();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
                   nullptr, nullptr, L"hppoa_launcher", nullptr};
    RegisterClassExW(&wc);
    // 4:3 like the art, a good part of the screen
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ch = std::clamp(sh * 2 / 3, 600, 1200), cw = ch * 4 / 3;
    RECT rc{0, 0, cw, ch};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Harry Potter and the Prisoner of Azkaban", style, CW_USEDEFAULT,
                              CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!InitD3D(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 could not start.", L"Launcher", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.FontGlobalScale = std::max(1.0f, float(ch) / 600.0f);
    // xboxrecomp: the page's own font first (the default), then the title's, Magic School One, built into the exe
    // (font/title_font.inc); the default font stands in should it fail
    io.Fonts->AddFontDefault();
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;  // the exe's own bytes, not the atlas's to free
        g_title_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kTitleFont), int(sizeof kTitleFont),
                                                      64.0f, &cfg);
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6;
    ImGui::GetStyle().FrameRounding = 4;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    Settings s = Load();
    FitSize(s);
    std::string error;
    bool started = false;
    for (MSG msg{}; !started;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) goto done;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawBackground();
        if (checking) {
            DrawCheck();
            if (WaitForSingleObject(g_check.thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(g_check.thread);
                g_check.thread = nullptr;
                checking = false;
                FinishCheck();
                // the page was turned off: on to the game as before, when every file is whole
                if (g_check.finished && FilesWhole() && !s.show && !shift && StartGame()) started = true;
            }
        } else if (FilesAction files = FilesAction::None; DrawPage(s, error, files)) {
            if (StartGame()) started = true;
            else error = "hppoa.exe could not be started (it must sit beside the launcher).";
        } else if (files == FilesAction::Verify) {
            error.clear();
            checking = StartCheck(true);
        }
        ImGui::Render();
        const float black[4] = {0, 0, 0, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, black);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
done:
    // closed while checking: stopped, no stamp written, so the next start checks again
    if (g_check.thread) {
        InterlockedExchange(&g_check.c.cancel, 1);
        WaitForSingleObject(g_check.thread, INFINITE);
        CloseHandle(g_check.thread);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}
