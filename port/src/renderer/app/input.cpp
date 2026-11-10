// xboxrecomp: the controls settings (app.h) applied to every pad read, and keyboard and mouse as controller 1.
//
// hle_input.c's XInputGetState calls app_InputPad on the pad state the PC's controller gave: the game's buttons are
// remapped from the pad's (pad_map), each stick gets its deadzone and response, the camera stick its inversion, and
// on port 0 the keyboard and mouse are merged on top when keyboard_mouse is on: bound keys press their pad input,
// WASD tilt the left stick fully, the mouse's movement tilts the right stick by its speed. app_InputRumble scales the
// game's rumble. The keys and the mouse come from the framebuffer window's messages (menu.cpp's hook, InputMessage):
// a key table, and raw mouse input for movement, which keeps counting at the screen's edge. While the game window has
// the focus and no panel is open the pointer is held inside it, so a click cannot land on another window.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>
#include <xinput.h>

#include "app/app.h"
#include "input/xinput_xbox.h"

extern "C" {
extern int (*g_xbox_host_path_override)(const char* xbox_path, wchar_t* host_path, unsigned long size);
BOOL xbox_translate_path(const char* xbox_path, wchar_t* host_path_buf, DWORD buf_size);
}

namespace app {
bool PromptsKbm();
void NoteDevice(bool kbm);

namespace {

struct KeyName_ {
    uint32_t vk;
    const char* name;
};
const KeyName_ kKeyNames[] = {
    {0, "none"},          {0x01, "Mouse1"},      {0x02, "Mouse2"},     {0x04, "Mouse3"},     {0x05, "Mouse4"},
    {0x06, "Mouse5"},     {0x08, "Backspace"},   {0x09, "Tab"},        {0x0D, "Enter"},      {0x10, "Shift"},
    {0x11, "Ctrl"},       {0x12, "Alt"},         {0x13, "Pause"},      {0x14, "CapsLock"},   {0x1B, "Escape"},
    {0x20, "Space"},      {0x21, "PageUp"},      {0x22, "PageDown"},   {0x23, "End"},        {0x24, "Home"},
    {0x25, "Left"},       {0x26, "Up"},          {0x27, "Right"},      {0x28, "Down"},       {0x2D, "Insert"},
    {0x2E, "Delete"},     {0x60, "Num0"},        {0x61, "Num1"},       {0x62, "Num2"},       {0x63, "Num3"},
    {0x64, "Num4"},       {0x65, "Num5"},        {0x66, "Num6"},       {0x67, "Num7"},       {0x68, "Num8"},
    {0x69, "Num9"},       {0x6A, "NumMultiply"}, {0x6B, "NumPlus"},    {0x6D, "NumMinus"},   {0x6E, "NumDecimal"},
    {0x6F, "NumDivide"},  {0xBA, "Semicolon"},   {0xBB, "Equals"},     {0xBC, "Comma"},      {0xBD, "Minus"},
    {0xBE, "Period"},     {0xBF, "Slash"},       {0xC0, "Backquote"},  {0xDB, "LeftBracket"}, {0xDC, "Backslash"},
    {0xDD, "RightBracket"}, {0xDE, "Quote"},
};

std::atomic<uint8_t> g_down[256];          // keys and mouse buttons held, by VK
std::atomic<long> g_mouse_dx{0}, g_mouse_dy{0};  // raw movement not yet read
std::atomic<bool> g_focus{false};
std::atomic<int> g_capture{-1};            // the menu waits for a key for this binding, -1 none
std::atomic<uint32_t> g_captured{~0u};     // the key it got
bool g_raw = false;                        // raw mouse input registered
bool g_clipped = false;

bool Held(uint32_t vk) { return vk && vk < 256 && g_down[vk].load(std::memory_order_relaxed); }

// the mouse button a message names, as VK
uint32_t MouseVk(unsigned msg, uintptr_t wp) {
    switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: return VK_LBUTTON;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: return VK_RBUTTON;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: return VK_MBUTTON;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: return HIWORD(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        default: return 0;
    }
}

int16_t Clamp16(float v) { return int16_t(std::lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

// a stick past its deadzone, rescaled to start from 0 there, times its response, direction kept
void Shape(int16_t* x, int16_t* y, uint32_t deadzone, uint32_t response) {
    float fx = *x / 32767.0f, fy = *y / 32767.0f, m = std::sqrt(fx * fx + fy * fy);
    float dz = std::min(deadzone, 90u) / 100.0f;
    if (m <= dz || m <= 0.0f) {
        *x = *y = 0;
        return;
    }
    float k = (m - dz) / (1.0f - dz) * (response / 100.0f) / m;
    float nx = fx * k, ny = fy * k, n = std::sqrt(nx * nx + ny * ny);
    if (n > 1.0f) nx /= n, ny /= n;
    *x = Clamp16(nx);
    *y = Clamp16(ny);
}

// the pad's inputs as 0..255 in kPadInputNames' order
void Read(const XBOX_GAMEPAD& g, uint8_t v[kPadInputs]) {
    for (int i = 0; i < 8; i++) v[i] = g.bAnalogButtons[i];
    for (int i = 0; i < 8; i++) v[8 + i] = (g.wButtons >> i & 1) ? 255 : 0;
}

void Write(XBOX_GAMEPAD& g, const uint8_t v[kPadInputs]) {
    for (int i = 0; i < 8; i++) g.bAnalogButtons[i] = v[i];
    g.wButtons &= ~0xFFu;
    for (int i = 0; i < 8; i++)
        if (v[8 + i] > XBOX_ANALOG_BUTTON_THRESHOLD) g.wButtons |= uint16_t(1u << i);
}

// the mouse's movement since the last read as a right stick tilt; read at most every 8 ms, so a title polling twice a
// frame sees the same tilt both times rather than the movement and then nothing
void MouseStick(const Settings& s, float* rx, float* ry) {
    static LARGE_INTEGER last{};
    static float sx, sy;
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    double dt = last.QuadPart ? double(now.QuadPart - last.QuadPart) / double(f.QuadPart) : 0.016;
    if (dt >= 0.008) {
        long dx = g_mouse_dx.exchange(0), dy = g_mouse_dy.exchange(0);
        // counts per second to a tilt: 2000 counts/s is full tilt at sensitivity 50
        float k = float(s.mouse_sensitivity) / 50.0f / 2000.0f;
        float nx = float(dx / std::max(dt, 0.004)) * k, ny = float(-dy / std::max(dt, 0.004)) * k;
        // a little smoothing: the poll and the mouse's reports do not line up
        sx = sx * 0.35f + nx * 0.65f;
        sy = sy * 0.35f + ny * 0.65f;
        if (std::fabs(sx) < 0.01f) sx = 0;
        if (std::fabs(sy) < 0.01f) sy = 0;
        last = now;
    }
    *rx = sx;
    *ry = s.mouse_invert_y ? -sy : sy;
}

}  // namespace

const char* KeyName(uint32_t vk) {
    for (const auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    static thread_local char buf[16];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = char(vk);
        buf[1] = 0;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(buf, sizeof buf, "F%u", vk - VK_F1 + 1);
    } else {
        std::snprintf(buf, sizeof buf, "0x%02X", vk);
    }
    return buf;
}

int KeyFromName(const char* name) {
    if (!name || !*name) return 0;
    for (const auto& k : kKeyNames)
        if (!_stricmp(name, k.name)) return int(k.vk);
    if (!name[1]) {
        char c = char(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return c;
    }
    if ((name[0] == 'F' || name[0] == 'f') && std::isdigit(static_cast<unsigned char>(name[1]))) {
        int n = std::atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        long v = std::strtol(name, nullptr, 16);
        if (v > 0 && v < 256) return int(v);
    }
    return -1;
}

// xboxrecomp (hppoa): the movement keys' speed, stepped with the mouse wheel: slow walk, walk, slow run, run (as the
// game's gaits: the left stick's reach for each, hle_trainer.c's gait bands). Run at start
const float kKbmSpeedReach[] = {0.50f, 0.68f, 0.86f, 1.0f};
const char* const kKbmSpeedName[] = {"slow walk", "walk", "slow run", "run"};
std::atomic<int> g_kbm_speed{3};
std::atomic<bool> g_kbm_stick{false};  // the last pad read's left stick came from the movement keys

void InputMessage(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    if (!g_raw && hwnd) {
        RAWINPUTDEVICE rid{0x01, 0x02, 0, static_cast<HWND>(hwnd)};  // generic desktop, mouse
        g_raw = RegisterRawInputDevices(&rid, 1, sizeof rid) != FALSE;
    }
    switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (wp < 256) {
                if (g_capture >= 0 && wp != VK_F10) g_captured = uint32_t(wp);
                g_down[wp] = 1;
            }
            break;
        case WM_KEYUP: case WM_SYSKEYUP:
            if (wp < 256) g_down[wp] = 0;
            break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            if (g_capture >= 0) g_captured = MouseVk(msg, wp);
            g_down[MouseVk(msg, wp)] = 1;
            break;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            g_down[MouseVk(msg, wp)] = 0;
            break;
        case WM_MOUSEWHEEL:
            if (Get().kbm && !MenuOpen()) {
                int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
                int v = std::clamp(g_kbm_speed.load() + d, 0, 3);
                if (v != g_kbm_speed.exchange(v)) std::fprintf(stderr, "[INPUT] movement keys: %s\n", kKbmSpeedName[v]);
            }
            break;
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof ri;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                    UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_mouse_dx += ri.data.mouse.lLastX;
                g_mouse_dy += ri.data.mouse.lLastY;
            }
            break;
        }
        case WM_SETFOCUS:
            g_focus = true;
            break;
        case WM_KILLFOCUS:
            g_focus = false;
            for (auto& d : g_down) d = 0;
            g_mouse_dx = g_mouse_dy = 0;
            break;
        case WM_ACTIVATE:
            g_focus = LOWORD(wp) != WA_INACTIVE;
            if (!g_focus)
                for (auto& d : g_down) d = 0;
            break;
        default:
            break;
    }
}

void InputFrame() {
    // the pointer held in the game window while keyboard and mouse play, the window has the focus and no panel is up
    HWND w = static_cast<HWND>(Window());
    bool want = Get().kbm && w && GetForegroundWindow() == w && !MenuOpen();
    if (want) {
        RECT r;
        if (GetClientRect(w, &r)) {
            POINT a{r.left, r.top}, b{r.right, r.bottom};
            ClientToScreen(w, &a);
            ClientToScreen(w, &b);
            RECT c{a.x, a.y, b.x, b.y};
            ClipCursor(&c);
            g_clipped = true;
        }
    } else if (g_clipped) {
        ClipCursor(nullptr);
        g_clipped = false;
    }
}

void StartKeyCapture(int binding) {
    g_captured = ~0u;
    g_capture = binding;
}

int KeyCaptureBinding() { return g_capture; }

bool TakeCapturedKey(uint32_t* vk) {
    uint32_t v = g_captured.exchange(~0u);
    if (v == ~0u) return false;
    g_capture = -1;
    *vk = v == VK_ESCAPE ? 0 : v;  // Escape clears the binding
    return true;
}

void CancelKeyCapture() { g_capture = -1; }

// xboxrecomp (hppoa): Button prompts (F10 Enhancements). The game's text (data\<language>\variables\apt_text.txt, read
// once at start: key=value pairs joined by &) names a button with a glyph of its fonts, U+E001 to U+E015 (drawn as
// the PlayStation's in oldclaudelpstd.ttf, the Xbox's on the console: E001 X, E002 Y, E003 B, E004 A, E006/E007 RT,
// E008/E009 LT, E010 Back, E011 Start, E013 left stick, E014 right stick, E015 d-pad), most after the button's own
// words ("tap right trigger\xa0<E006>"). For keyboard and mouse a copy with each glyph (and the words before it) as
// the key bound to that button ("tap [Ctrl]") is written beside the exe and opened in the disc file's place.
// Automatic: the device played with last (input_device.txt beside the exe, written as it changes), or with no record
// keyboard and mouse only when no controller answers. Under Wine XInput may answer with nothing plugged in

namespace {

std::wstring ExeDirFile(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, len);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + name;
}

std::atomic<int> g_device{-1};  // 1 keyboard and mouse, 0 controller, as last written

int LastDevice() {
    FILE* f = _wfopen(ExeDirFile(L"input_device.txt").c_str(), L"r");
    if (!f) return -1;
    char b[32] = {};
    fgets(b, sizeof b, f);
    fclose(f);
    return !std::strncmp(b, "keyboard", 8) ? 1 : !std::strncmp(b, "controller", 10) ? 0 : -1;
}

std::string KeyLabel(uint32_t vk) {
    switch (vk) {
        case 0x01: return "Left click";
        case 0x02: return "Right click";
        case 0x04: return "Middle click";
        case 0x05: return "Mouse 4";
        case 0x06: return "Mouse 5";
        default: return KeyName(vk);
    }
}

std::string Keys(const Settings& s, std::initializer_list<int> binds) {
    std::string out = "[";
    for (int b : binds) {
        if (!s.keys[b]) return std::string();
        if (out.size() > 1) out += "/";
        out += KeyLabel(s.keys[b]);
    }
    return out + "]";
}

// what a glyph becomes, empty to leave it
std::string GlyphText(const Settings& s, int n) {
    switch (n) {
        case 0x01: return Keys(s, {2});
        case 0x02: return Keys(s, {3});
        case 0x03: return Keys(s, {1});
        case 0x04: return Keys(s, {0});
        case 0x06: case 0x07: return Keys(s, {7});
        case 0x08: case 0x09: return Keys(s, {6});
        case 0x10: return Keys(s, {13});
        case 0x11: return Keys(s, {12});
        case 0x13: return Keys(s, {16, 18, 17, 19});
        case 0x14: return "[mouse]";
        case 0x15: return Keys(s, {8, 9, 10, 11});
        default: return std::string();
    }
}

// a glyph whose picture the renderer draws as the bound key (d3d12.cpp's kPromptButtons: the text's own A B X Y and
// triggers, by their captured hashes, 2026-11-10): the text keeps the glyph, so tutorials, the inventory and the pause
// menu show the key as an icon in the button's colour. E008 (left trigger, "to call Ron") has not been captured yet,
// and BACK, START, the sticks and the d-pad have no icon: those are named in words
bool GlyphIcon(int n) {
    switch (n) {
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07: case 0x08: case 0x09: return true;
        // 2026-11-10, from poa_base.big's text icons: BACK, START, the left stick (W A S D) and the d-pad (its four
        // keys). The right stick (E014) shares the left's picture, so it keeps its words ([mouse])
        case 0x10: case 0x11: case 0x13: case 0x15: return true;
        default: return false;
    }
}

// the end of out with each no-break space (C2 A0) as a space, lower case; at[k] the byte in out where tail[k] starts
void Tail(const std::string& out, size_t n, std::string* tail, std::vector<size_t>* at) {
    tail->clear(), at->clear();
    size_t i = out.size();
    while (i > 0 && tail->size() < n) {
        if (i >= 2 && static_cast<unsigned char>(out[i - 2]) == 0xC2 && static_cast<unsigned char>(out[i - 1]) == 0xA0) {
            i -= 2;
            tail->insert(tail->begin(), ' ');
        } else {
            i -= 1;
            tail->insert(tail->begin(), char(std::tolower(static_cast<unsigned char>(out[i]))));
        }
        at->insert(at->begin(), i);
    }
}

bool EndsWith(const std::string& t, const char* w) {
    size_t l = std::strlen(w);
    return t.size() >= l && !t.compare(t.size() - l, l, w);
}

// "Pull left trigger <E009> to call your friends" -> "Press [Shift] to call your friends"; "while pulling the right
// trigger <E006>" -> "while holding [Ctrl]"; "tap right trigger <E006>" -> "tap [Ctrl]"; "the left thumbstick <E013>"
// -> "the [W/A/S/D]"; a glyph alone ("press <E004> to bow") -> "press [Space] to bow"
std::string KbmText(const Settings& s, const std::string& in, int* changed) {
    static const char* const kWords[] = {"left trigger ", "right trigger ", "left thumbstick ", "right thumbstick "};
    std::string out, tail;
    std::vector<size_t> at;
    out.reserve(in.size() + in.size() / 8);
    for (size_t i = 0; i < in.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0xEE && i + 2 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x80) {
            const int n = static_cast<unsigned char>(in[i + 2]) - 0x80;
            std::string rep = n >= 0 && n < 0x40 ? GlyphText(s, n) : std::string();
            if (!rep.empty()) {
                Tail(out, 40, &tail, &at);
                bool trigger = false;
                for (const char* w : kWords) {
                    const size_t k = tail.size() - std::strlen(w);
                    if (!EndsWith(tail, w) || k == 0 || (tail[k - 1] != ' ' && tail[k - 1] != '=')) continue;
                    trigger = std::strstr(w, "trigger") != nullptr;
                    out.resize(at[k]);  // the space or "=" before the words stays
                    break;
                }
                // "Wiggle the left thumbstick <E013> repeatedly": the game counts swings from side to side, so "Press
                // [A] and [D] alternately" (the move left and move right bindings; by request, 2026-11-09)
                // with the movement keys' icon the game's own words stay: "Wiggle the <W A S D> repeatedly" (by request,
                // 2026-11-10)
                if (n == 0x13 && !GlyphIcon(0x13)) {
                    Tail(out, 40, &tail, &at);
                    if (EndsWith(tail, "=wiggle the ") && !Keys(s, {18}).empty() && !Keys(s, {19}).empty()) {
                        out.resize(at[tail.size() - std::strlen("=wiggle the ")]);
                        rep = "=Press " + Keys(s, {18}) + " and " + Keys(s, {19}) + " alternately";
                        size_t j = i + 3;  // " repeatedly" after it: "alternately" says it
                        while (j < in.size() && (in[j] == ' ' || (static_cast<unsigned char>(in[j]) == 0xC2 &&
                                                                  j + 1 < in.size() && static_cast<unsigned char>(in[j + 1]) == 0xA0)))
                            j += in[j] == ' ' ? 1 : 2;
                        if (!in.compare(j, 10, "repeatedly")) {
                            out += rep;
                            i = j + 9;  // the loop's ++ steps past its last letter
                            ++*changed;
                            continue;
                        }
                    }
                }
                if (trigger) {  // a key is pressed, not pulled
                    Tail(out, 40, &tail, &at);
                    static const struct { const char* from; const char* to; } kVerbs[] = {
                        {" pulling the ", " holding "}, {" pull and hold ", " press and hold "}, {" pull ", " press "},
                        {"=pull and hold ", "=Press and hold "}, {"=pull ", "=Press "}};
                    for (const auto& v : kVerbs) {
                        if (!EndsWith(tail, v.from)) continue;
                        size_t from = at[tail.size() - std::strlen(v.from)];
                        bool cap = std::isupper(static_cast<unsigned char>(out[from + 1])) != 0;
                        std::string to = v.to;
                        if (cap && to[0] == ' ') to[1] = char(std::toupper(static_cast<unsigned char>(to[1])));
                        out.resize(from);
                        out += to;
                        break;
                    }
                }
                // an icon's glyph stays (the words and verbs before it changed as for a key's name)
                out += GlyphIcon(n) ? in.substr(i, 3) : rep;
                i += 2;
                ++*changed;
                continue;
            }
        }
        out += char(c);
    }
    return out;
}

bool g_inside;

// the keys whose text names a button: each also given as KBM_... (the keys) and PAD_... (the controller's, as the
// game has it) after the rest, for the HUD's variables to switch between as the device does (app_PromptVariant)
std::unordered_set<std::string> g_variant_keys;
std::atomic<bool> g_variants{false};

int PromptsOverride(const char* xbox_path, wchar_t* host, unsigned long size) {
    static const char kTail[] = "\\variables\\apt_text.txt";
    const size_t pl = xbox_path ? std::strlen(xbox_path) : 0, tl = sizeof kTail - 1;
    // forced to the controller's: the disc's own file
    if (g_inside || pl < tl || _stricmp(xbox_path + pl - tl, kTail) || Get().button_prompts == 1) return 0;
    const bool kbm_start = PromptsKbm();
    wchar_t orig[MAX_PATH];
    g_inside = true;
    BOOL ok = xbox_translate_path(xbox_path, orig, MAX_PATH);
    g_inside = false;
    if (!ok) return 0;
    FILE* f = _wfopen(orig, L"rb");
    if (!f) return 0;
    std::string text;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    // entry by entry ("key=value", joined by &): the start's device's text, the variants after
    std::string body, extra;
    int changed = 0;
    g_variant_keys.clear();
    for (size_t pos = 0; pos <= text.size();) {
        size_t amp = text.find('&', pos);
        if (amp == std::string::npos) amp = text.size();
        std::string entry = text.substr(pos, amp - pos);
        size_t eq = entry.find('=');
        std::string kbm = entry;
        int n = 0;
        // the value with its "=": the rewrites for a sentence's start ("=pull ", "=wiggle the ") look for it; given
        // without, "Wiggle the left thumbstick" kept [W/A/S/D] and "Pull and hold" its verb (2026-11-10)
        if (eq != std::string::npos) kbm = entry.substr(0, eq) + KbmText(Get(), entry.substr(eq), &n);
        if (!body.empty() || pos) body += '&';
        body += kbm_start ? kbm : entry;
        if (n && !entry.compare(0, 4, "GUI_")) {
            changed += n;
            g_variant_keys.insert(entry.substr(0, eq));
            extra += "&KBM_" + kbm.substr(4) + "&PAD_" + entry.substr(4);
        }
        pos = amp + 1;
    }
    body += extra;
    // one copy per language: the folder above "variables"
    std::string lang = xbox_path;
    lang.resize(pl - tl);
    lang = lang.substr(lang.find_last_of('\\') + 1);
    std::wstring out = ExeDirFile((L"prompts_" + std::wstring(lang.begin(), lang.end()) + L".txt").c_str());
    FILE* o = _wfopen(out.c_str(), L"wb");
    if (!o) return 0;
    bool wrote = fwrite(body.data(), 1, body.size(), o) == body.size();
    fclose(o);
    if (!wrote || out.size() + 1 > size) return 0;
    g_variants = true;
    std::fprintf(stderr, "[PROMPTS] %s: %s text, %zu entries (%d button glyphs) also as KBM_/PAD_ for the HUD\n",
                 lang.c_str(), kbm_start ? "keyboard" : "controller", g_variant_keys.size(), changed);
    wcscpy(host, out.c_str());
    return 1;
}

struct PromptsInit {
    PromptsInit() { g_xbox_host_path_override = PromptsOverride; }
} g_prompts_init;

}  // namespace

std::string PromptKeyLabel(uint32_t vk) { return KeyLabel(vk); }

// Automatic follows the device last pressed or moved (NoteDevice); the game's text keeps the start's choice
bool PromptsKbmNow() {
    const uint32_t mode = Get().button_prompts;
    if (mode == 1) return false;
    if (mode == 2) return true;
    const bool start = PromptsKbm();  // also the device's first value
    const int now = g_device.load();
    return now < 0 ? start : now == 1;
}

bool PromptsKbm() {
    static int kbm = -1;
    if (kbm >= 0) return kbm;
    const Settings& s = Get();
    const char* why;
    if (s.button_prompts == 1) kbm = 0, why = "controller (F10)";
    else if (s.button_prompts == 2) kbm = 1, why = "keyboard and mouse (F10)";
    else if (!s.kbm) kbm = 0, why = "controller (keyboard play is off)";
    else if (!s.controller) kbm = 1, why = "keyboard and mouse (the controller is off)";
    else if ((kbm = LastDevice()) >= 0) why = kbm ? "keyboard and mouse, played with last" : "controller, played with last";
    else {
        XINPUT_CAPABILITIES caps;
        kbm = 1;
        for (DWORD i = 0; i < 4; i++)
            if (XInputGetCapabilities(i, 0, &caps) == ERROR_SUCCESS) kbm = 0;
        why = kbm ? "keyboard and mouse (no controller)" : "controller (one connected)";
    }
    g_device = kbm;
    std::fprintf(stderr, "[PROMPTS] button prompts: %s\n", why);
    return kbm;
}

}  // namespace app

// hle_quit.c: the prefix ("KBM_" or "PAD_") for a text key the HUD is given (Help_Text_Display's tutorials...) with
// the device in use now, when the text file has that key's variants; 0 to leave it
// Off unless RECOMP_PROMPT_VARIANTS=1 (2026-11-10): handed a KBM_ name the help line showed nothing, its tutorials
// went missing (the UI seems to take its text by the GUI_ names only); the tutorials keep the start's device's words,
// and their buttons, drawn as icons (d3d12.cpp's kPromptButtons), still follow the device in use
extern "C" const char* app_PromptVariant(const char* key) {
    static const bool on = [] {
        const char* e = std::getenv("RECOMP_PROMPT_VARIANTS");
        return e && e[0] == '1';
    }();
    if (!on || !app::g_variants || std::strncmp(key, "GUI_", 4) || !app::g_variant_keys.count(key)) return nullptr;
    return app::PromptsKbmNow() ? "KBM_" : "PAD_";
}

// d3d12.cpp: the HUD's button pictures as keys this run, and the key bound to a pad input
extern "C" int app_PromptsKbm(void) { return app::PromptsKbmNow(); }
extern "C" uint32_t app_KeyBinding(int binding) {
    return binding >= 0 && binding < int(app::kKeyBinds) ? app::Get().keys[binding] : 0;
}
extern "C" uint32_t app_PadInputKey(int pad_input) {
    return pad_input >= 0 && pad_input < int(app::kPadInputs) ? app::Get().keys[pad_input] : 0;
}

namespace app {

void NoteDevice(bool kbm) {
    if (g_device.exchange(kbm ? 1 : 0) == (kbm ? 1 : 0)) return;
    if (FILE* f = _wfopen(ExeDirFile(L"input_device.txt").c_str(), L"w")) {
        std::fputs(kbm ? "keyboard\n" : "controller\n", f);
        fclose(f);
    }
}

}  // namespace app

// hle_input.c: the left stick of the last read came from the movement keys (exact: no range compensation)
extern "C" int app_KbmStick(void) { return app::g_kbm_stick.load(); }

// hle_input.c: a pad read for the game, after the PC's controller filled it in
extern "C" void app_InputPad(int port, XBOX_GAMEPAD* g) {
    const app::Settings& s = app::Get();
    if (!s.controller) std::memset(g, 0, sizeof(*g));  // xboxrecomp: "Use controller" off: the pad at rest
    uint8_t in[app::kPadInputs], out[app::kPadInputs];
    app::Read(*g, in);
    for (uint32_t i = 0; i < app::kPadInputs; i++) out[i] = in[std::min(s.pad_map[i], app::kPadInputs - 1)];
    app::Shape(&g->sThumbLX, &g->sThumbLY, s.deadzone_left, s.sensitivity_left);
    app::Shape(&g->sThumbRX, &g->sThumbRY, s.deadzone_right, s.sensitivity_right);
    if (port == 0) app::g_kbm_stick = false;
    bool pad_used = false, kbm_used = false;
    if (port == 0) {
        for (uint32_t i = 0; i < app::kPadInputs; i++) pad_used |= in[i] > 64;
        pad_used |= std::abs(int(g->sThumbLX)) > 16000 || std::abs(int(g->sThumbLY)) > 16000 ||
                    std::abs(int(g->sThumbRX)) > 16000 || std::abs(int(g->sThumbRY)) > 16000;
    }
    if (port == 0 && s.kbm && GetForegroundWindow() == static_cast<HWND>(app::Window()) && !app::MenuOpen()) {
        for (uint32_t i = 0; i < app::kKeyBinds; i++) kbm_used |= app::Held(s.keys[i]);
        for (uint32_t i = 0; i < app::kPadInputs; i++)
            if (app::Held(s.keys[i])) out[i] = 255;
        float lx = float(app::Held(s.keys[19])) - float(app::Held(s.keys[18]));
        float ly = float(app::Held(s.keys[16])) - float(app::Held(s.keys[17]));
        app::g_kbm_stick = lx || ly;
        if (lx || ly) {
            // the mouse wheel's speed: the stick's reach for that gait
            float m = std::sqrt(lx * lx + ly * ly) / app::kKbmSpeedReach[std::clamp(app::g_kbm_speed.load(), 0, 3)];
            g->sThumbLX = app::Clamp16(lx / m);
            g->sThumbLY = app::Clamp16(ly / m);
        }
        float rx, ry;
        app::MouseStick(s, &rx, &ry);
        rx += float(app::Held(s.keys[23])) - float(app::Held(s.keys[22]));
        ry += float(app::Held(s.keys[20])) - float(app::Held(s.keys[21]));
        kbm_used |= std::fabs(rx) > 0.2f || std::fabs(ry) > 0.2f;
        if (rx || ry) {
            float m = std::sqrt(rx * rx + ry * ry);
            if (m > 1.0f) rx /= m, ry /= m;
            g->sThumbRX = app::Clamp16(rx);
            g->sThumbRY = app::Clamp16(ry);
        }
    }
    if (pad_used != kbm_used) app::NoteDevice(kbm_used);
    if (s.invert_x) g->sThumbRX = int16_t(std::max(-32767, -int(g->sThumbRX)));
    if (s.invert_y) g->sThumbRY = int16_t(std::max(-32767, -int(g->sThumbRY)));
    app::Write(*g, out);
}

// hle_input.c: keyboard and mouse play as controller 1 (port 0 is there with no controller plugged in)
extern "C" int app_KbmOn(void) { return app::Get().kbm; }

// hle_input.c: the game's rumble before it goes to the PC's controller
extern "C" void app_InputRumble(uint16_t* left, uint16_t* right) {
    const app::Settings& s = app::Get();
    uint32_t k = s.vibration && s.controller ? std::min(s.vibration_strength, 100u) : 0;
    *left = uint16_t(*left * k / 100);
    *right = uint16_t(*right * k / 100);
}
