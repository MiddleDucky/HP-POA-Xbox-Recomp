// The part of SCDA-XBERecomp's app layer (src/app/app.h) its renderer reads:
// settings, the window, the output shape, the F10 options panel. Settings live
// in hppoa.toml beside the exe as SCDA's scda.toml does (app.cpp); a RECOMP_*
// environment variable set at launch wins over its line. The window is
// xboxrecomp's framebuffer window.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <filesystem>
#include <mutex>

namespace app {

enum class DisplayMode : uint32_t { Windowed, Borderless, Fullscreen };  // as xboxrecomp's XBOX_DISPLAY_*
enum class AspectMode : uint32_t { Standard, Wide, Stretch };            // 4:3, 16:9, stretch
constexpr uint32_t kFpsUnlocked = 1000;  // fps_limit 'unlocked' (experimental): Swap and vblank waits do not wait
enum ColorBlind : uint32_t { kCbOff, kCbProtanopia, kCbDeuteranopia, kCbTritanopia };

// xboxrecomp: the pad's inputs in one order, the analog buttons as the Xbox's bAnalogButtons (A B X Y Black White LT
// RT) then the digital ones as wButtons' bits (Up Down Left Right Start Back LThumb RThumb)
constexpr uint32_t kPadInputs = 16;
inline const char* const kPadInputNames[kPadInputs] = {"A",    "B",     "X",     "Y",     "Black", "White",
                                                       "LT",   "RT",    "Up",    "Down",  "Left",  "Right",
                                                       "Start", "Back", "LThumb", "RThumb"};
// key bindings: the 16 pad inputs, then the left stick (forward back left right) and the camera (up down left right)
constexpr uint32_t kKeyBinds = kPadInputs + 8;
inline const char* const kKeyBindNames[kKeyBinds] = {
    "A", "B", "X", "Y", "Black", "White", "LT", "RT", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start",
    "Back", "Left stick click", "Right stick click", "Move forward", "Move back", "Move left", "Move right",
    "Camera up", "Camera down", "Camera left", "Camera right"};
// Space, E, left mouse, right mouse, R, Q, Shift, Ctrl, 1-4, Escape, Tab, C, middle mouse; W S A D; arrows
#define kDefaultKeys                                                                                          \
    0x20, 'E', 0x01, 0x02, 'R', 'Q', 0x10, 0x11, '1', '2', '3', '4', 0x1B, 0x09, 'C', 0x04, 'W', 'S', 'A', 'D', \
        0x26, 0x28, 0x25, 0x27

struct Settings {
    DisplayMode display_mode = DisplayMode::Windowed;  // RECOMP_DISPLAY_MODE
    uint32_t width = 1920, height = 1080;  // RECOMP_WINDOW_SIZE: the window, or the fullscreen mode; 0 the desktop's
    AspectMode aspect = AspectMode::Standard;  // RECOMP_ASPECT; read at boot, the title picks its camera then
    uint32_t internal_height = 0;      // RECOMP_INTERNAL_HEIGHT: 3D render lines, 0 follows the window
    uint32_t render_percent = 100;     // RECOMP_RENDER_PERCENT, of the window height
    uint32_t render_max_height = 1080; // RECOMP_RENDER_MAX_HEIGHT, 0 no cap
    uint32_t brightness = 50;          // 0 to 100, 50 each leaves the picture alone
    uint32_t contrast = 50;
    bool fps_counter = false;
    // xboxrecomp (hppoa): F10 Enhancements
    uint32_t button_prompts = 0;       // 0 automatic, 1 controller, 2 keyboard and mouse: the game's text at start (input.cpp)
    bool tutorials_once = true;        // each tutorial popup shown once (hle_quit.c, tutorials_seen.txt)
    bool turn_full_speed = true;       // running keeps its speed through turns (hle_trainer.c)
    bool instant_step = true;          // the first step from standing without the game's wait (hle_trainer.c)
    uint32_t turn_response = 200;      // % of the game's turn speeds, 100 as the console (hle_trainer.c)
    bool hide_save_icon = true;  // the compass shown top left while saving or loading left out (d3d12.cpp)
    bool cutscene_bars_hidden = false; // 16:9 cutscenes without their letterbox bars (hle_widescreen.c, d3d12.cpp)
    bool smooth_textures = true;       // stretched textures (skies, landscapes) filtered bicubic (d3d12.cpp, kTexSmooth)
    bool quiet_turn_in = false;        // the trio's / Neville's "time to turn in" lines left out (hle_debug.c)
    // xboxrecomp: post-processing (d3d12.cpp): the game's own bloom's strength, sharpening and grading over the
    // finished frame. Neutral by default: the console's picture
    uint32_t game_bloom = 100;         // the game's bloom, % of the console's: 0 off to 200
    uint32_t sharpen = 0;              // 0 (off) to 100
    uint32_t saturation = 100;         // 0 to 200 %, 100 as the game
    uint32_t vibrance = 0;             // 0 to 100: saturation for the dull colours more than the vivid
    uint32_t warmth = 50;              // 0 (cool) to 100 (warm), 50 as the game
    uint32_t vignette = 0;             // 0 (off) to 100: the corners darkened
    uint32_t grain = 0;                // 0 (off) to 100: moving film grain
    uint32_t fov = 100;                // xboxrecomp: field of view, % of the game's (hle_widescreen.c), 80 to 130
    bool vsync = true;                 // RECOMP_VSYNC
    uint32_t aniso = 0;                // RECOMP_ANISO, 0 as the game asks
    uint32_t msaa = 0;                 // RECOMP_MSAA samples, 0 off
    bool smaa = false;                 // RECOMP_SMAA, over the finished frame; combines with msaa
    uint32_t shadow_resolution = 512;  // SCDA's shadow-map path; this title has none
    // SCDA's shader model 3 effects, kept off for this title (no setting)
    uint32_t shader_model = 1;
    bool hdr_rendering = false;
    bool tone_mapping = false;
    bool controller = true;            // xboxrecomp: false: the PC's controller is read as at rest (keyboard and mouse stay)
    bool async_shaders = true;         // RECOMP_ASYNC_SHADERS=0: wait for every pipeline
    bool audio_trace = false;          // xboxrecomp: F10 Debug: [DSOUND] lines for each sound (not saved)
    bool debug_keys = false;           // xboxrecomp: F7/F8/F9 renderer debug keys (also RECOMP_DEBUG_KEYS)
    bool show_launcher = true;         // xboxrecomp: hppoa_launcher.exe shows its page; false starts the game straight away (Shift shows it)
    bool start_menu = false;           // xboxrecomp: the settings page before the game starts (RECOMP_START_MENU); hppoa_launcher.exe is the usual way
    // xboxrecomp: frame rate, accessibility and controls (input.cpp, the final pass)
    uint32_t fps_limit = 0;            // 0 the game's own rate, 30: each frame two vblanks at least, kFpsUnlocked none
    uint32_t frame_rate = 60;          // xboxrecomp: 60, or 30 (the game's own, D3DPRESENT_INTERVAL_TWO)
    uint32_t colorblind = 0;           // ColorBlind: correction filter over the finished frame
    uint32_t colorblind_strength = 100;  // 0 to 100 %
    uint32_t speakers = 2;             // xboxrecomp: 2 stereo, 6 5.1 surround (hle_dsound.c, at start)
    bool kbm = true;                   // keyboard and mouse drive pad 1 alongside a controller
    uint32_t mouse_sensitivity = 50;   // 1 to 100
    bool mouse_invert_y = false;
    uint32_t deadzone_left = 10, deadzone_right = 10;          // % of full tilt read as centred
    uint32_t sensitivity_left = 100, sensitivity_right = 100;  // % response past the deadzone
    bool invert_x = false, invert_y = false;                   // the camera (right stick and mouse)
    bool vibration = true;
    uint32_t vibration_strength = 100;  // %
    uint32_t pad_map[kPadInputs] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};  // game input <- pad input
    uint32_t keys[kKeyBinds] = {kDefaultKeys};  // VK codes (1 2 4 5 6 the mouse buttons), 0 unbound
};

Settings& Get();
void Save();
// re-reads hppoa.toml when it changed on disk; the renderer calls it once a frame
void Poll();
// the window to the display settings, any thread
void ApplyDisplay();
void* Window();  // HWND of the framebuffer window
float RenderScale(uint32_t guest_height);
// the output's shape: 4:3, or the window's own for 16:9 / stretch
float Aspect();
AspectMode BootAspect();  // the aspect setting this run started with
void SetBootAspect(AspectMode a);  // xboxrecomp: the start menu's choice, before the title asks
inline float Narrow() { return 4.0f / 3.0f / Aspect(); }
inline bool Widescreen() { return Aspect() > 1.34f; }
std::filesystem::path ExeDir();

// options panel (menu.cpp), drawn by the renderer each frame with the lock held
std::recursive_mutex& UiLock();
bool MenuOpen();
void ToggleMenu();
// xboxrecomp: the start menu, shown before the title's first frame: open while it waits for Start
void OpenStartMenu();
bool StartMenuOpen();
void InitUi();  // ImGui's context and Win32 side, once the window is up
void DrawUi();
// xboxrecomp (hppoa): the trainer on F6 (trainer.cpp)
bool TrainerOpen();
void ToggleTrainer();
void DrawTrainer();
// xboxrecomp: controls (input.cpp): key names as hppoa.toml spells them, the window's keys and mouse, the pointer held
// in the window, and the menu's "press a key" for a binding (Escape clears it)
const char* KeyName(uint32_t vk);
// xboxrecomp (hppoa): Button prompts: keyboard and mouse ones this run (decided at start), and a binding's short name
bool PromptsKbm();
// the same for what is drawn each frame (the HUD's buttons, the quit prompt): with Automatic the device in use now
bool PromptsKbmNow();
std::string PromptKeyLabel(uint32_t vk);
int KeyFromName(const char* name);  // -1 not a key name
void InputMessage(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
void InputFrame();
void StartKeyCapture(int binding);
int KeyCaptureBinding();
bool TakeCapturedKey(uint32_t* vk);
void CancelKeyCapture();
// the window's display has Windows HDR on, from the renderer
inline std::atomic<bool> g_hdr_display{false};
inline std::atomic<float> g_fps{0};

}  // namespace app
