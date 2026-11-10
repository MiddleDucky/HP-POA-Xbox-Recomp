// xboxrecomp (hppoa): the trainer, on F6. Everything it changes is the title's own: its tuning variables, its debug
// menu, the characters' health and items, the game clock, its start switches. The game side is src/hle_trainer.c.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/log.h"
#include "../../trainer.h"

namespace app {

namespace {

std::atomic<bool> g_trainer{false};

// the title's group masks, named after the variables in them
struct Group {
    uint32_t mask;
    const char* name;
};
const Group kGroups[] = {
    {0x1, "Characters & AI"},  {0x2, "Player & debug"},    {0x4, "Camera & world"},     {0x8, "Movement & carrying"},
    {0x10, "Items & pickups"}, {0x20, "Jumping"},          {0x40, "Spells"},            {0x80, "Targets & hair"},
    {0x100, "Time & meshes"},  {0x200, "Sliding objects"}, {0x400, "Crawlspaces"},      {0x800, "Doors & slopes"},
    {0x1000, "Containers"},    {0x2000, "Minigames"},      {0x4000, "Targeting"},       {0x8000, "Calling & panels"},
    {0x10000, "Talking"},      {0x20000, "Fall damage"},   {0x40000, "Carrying together"}, {0x80000, "Rumble"},
    {0x100000, "Calling to the player"},
};

const char* GroupName(uint32_t mask) {
    for (const Group& g : kGroups)
        if (g.mask == mask) return g.name;
    return "Other";
}

const char* const kSlotNames[HPPOA_CHARACTER_SLOTS] = {"Harry", "Ron", "Hermione", "Ron (2)", "Hermione (2)"};

// names for item ids the title's item list leaves empty, from the HUD and collection code that reads them
const char* KnownShared(int id) {
    switch (id) {
        case 0x01: return "Folio Bruti pages";
        case 0x0F: return "Bertie Bott's beans";
        case 0x4A: return "House points";
        case 0x34: return "Advanced Duelling Techniques";
        case 0x35: return "Advanced Defensive Techniques";
        case 0x36: return "More Efficient Spell Casting";
        default: return nullptr;
    }
}
const char* KnownCarried(int id) {
    switch (id) {
        case 0x01: return "Dungbombs";
        case 0x02: return "Owl treats";
        case 0x04: return "Stink pellets";
        case 0x0A: return "Wiggenweld potions";
        case 0x0B: return "Antidotes";
        case 0x0C: return "Girding potions";
        case 0x2B: return "Broom kit";
        default: return nullptr;
    }
}

void Row(const char* label) {
    ImGui::Spacing();
    ImGui::SeparatorText(label);
}

// ── guest values ────────────────────────────────────────────────────────────

template <class T>
bool Read(uint32_t va, T* out) {
    if (!hppoa_GuestOk(va, sizeof(T))) return false;
    std::memcpy(out, hppoa_GuestPtr(va), sizeof(T));
    return true;
}
template <class T>
void Write(uint32_t va, T v) {
    if (hppoa_GuestOk(va, sizeof(T))) std::memcpy(hppoa_GuestPtr(va), &v, sizeof(T));
}

int FindFreeze(uint32_t va) {
    HppoaTrainer& t = g_hppoa_trainer;
    for (int i = 0; i < t.freeze_count; i++)
        if (t.freezes[i].va == va) return i;
    return -1;
}
void SetFreeze(uint32_t va, uint32_t size, uint32_t value) {
    HppoaTrainer& t = g_hppoa_trainer;
    int i = FindFreeze(va);
    if (i < 0) {
        if (t.freeze_count >= HPPOA_MAX_FREEZES) return;
        i = t.freeze_count;
        t.freezes[i].va = 0;  // not applied until whole
        t.freeze_count = i + 1;
    }
    t.freezes[i].size = size, t.freezes[i].value = value;
    t.freezes[i].va = va;
}
void ClearFreeze(uint32_t va) {
    HppoaTrainer& t = g_hppoa_trainer;
    int i = FindFreeze(va);
    if (i < 0) return;
    t.freezes[i].va = 0;
    t.freezes[i] = t.freezes[t.freeze_count - 1];
    t.freeze_count = t.freeze_count - 1;
}
// a frozen value follows an edit
void Refreeze(uint32_t va, uint32_t size) {
    int i = FindFreeze(va);
    if (i < 0) return;
    uint32_t v = 0;
    if (size == 1) { uint8_t b; if (Read(va, &b)) v = b; }
    else if (size == 2) { uint16_t h; if (Read(va, &h)) v = h; }
    else Read(va, &v);
    g_hppoa_trainer.freezes[i].value = v;
}
bool LockBox(const char* id, uint32_t va, uint32_t size) {
    bool on = FindFreeze(va) >= 0;
    if (ImGui::Checkbox(id, &on)) {
        if (on) {
            SetFreeze(va, size, 0);
            Refreeze(va, size);
        } else {
            ClearFreeze(va);
        }
        return true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock: written back every frame");
    return false;
}

// ── tuning variables ────────────────────────────────────────────────────────

uint32_t VarSize(const HppoaTrainerVar& v) { return v.kind == HPPOA_TV_BOOL ? 1 : 4; }

// the value as shown (floats divided by their scale)
double VarValue(const HppoaTrainerVar& v, bool* ok) {
    *ok = false;
    switch (v.kind) {
        case HPPOA_TV_BOOL: { uint8_t b; if ((*ok = Read(v.va, &b))) return b ? 1 : 0; break; }
        case HPPOA_TV_FLOAT: { float f; if ((*ok = Read(v.va, &f))) return f / (v.scale ? v.scale : 1.0f); break; }
        case HPPOA_TV_INT: { int32_t i; if ((*ok = Read(v.va, &i))) return i; break; }
        default: { uint32_t u; if ((*ok = Read(v.va, &u))) return u; break; }
    }
    return 0;
}
void SetVarValue(const HppoaTrainerVar& v, double x) {
    switch (v.kind) {
        case HPPOA_TV_BOOL: Write<uint8_t>(v.va, x != 0 ? 1 : 0); break;
        case HPPOA_TV_FLOAT: Write<float>(v.va, float(x) * (v.scale ? v.scale : 1.0f)); break;
        case HPPOA_TV_INT: Write<int32_t>(v.va, int32_t(x)); break;
        default: Write<uint32_t>(v.va, uint32_t(x)); break;
    }
    Refreeze(v.va, VarSize(v));
}
bool VarChanged(const HppoaTrainerVar& v) {
    bool ok;
    double x = VarValue(v, &ok);
    if (!ok) return false;
    if (v.kind == HPPOA_TV_FLOAT) return std::fabs(x - v.def) > 1e-4 * std::max(1.0, std::fabs(double(v.def)));
    return x != double(v.def);
}

const HppoaTrainerVar* VarByName(const char* name) {
    for (int i = 0, n = hppoa_TrainerVarCount(); i < n; i++)
        if (!std::strcmp(hppoa_TrainerVar(i)->name, name)) return hppoa_TrainerVar(i);
    return nullptr;
}

// the value's editor; true when it was changed
bool VarEditor(const HppoaTrainerVar& v) {
    bool ok;
    double x = VarValue(v, &ok);
    if (!ok) {
        ImGui::TextDisabled("(not readable)");
        return false;
    }
    bool changed = false;
    switch (v.kind) {
        case HPPOA_TV_BOOL: {
            bool b = x != 0;
            if (ImGui::Checkbox("##v", &b)) SetVarValue(v, b), changed = true;
            break;
        }
        case HPPOA_TV_FLOAT: {
            float f = float(x);
            float speed = std::max(0.001f, std::fabs(f) * 0.01f);
            bool bounded = v.min > -1e30f && v.max < 1e30f && v.max > v.min;
            if (ImGui::DragFloat("##v", &f, speed, bounded ? v.min : 0.0f, bounded ? v.max : 0.0f, "%.4g"))
                SetVarValue(v, f), changed = true;
            break;
        }
        case HPPOA_TV_INT: {
            int i = int(x);
            if (ImGui::InputInt("##v", &i)) {
                if (v.max > v.min) i = int(std::clamp(double(i), double(v.min), double(v.max)));
                SetVarValue(v, i), changed = true;
            }
            break;
        }
        default: {
            uint32_t u = uint32_t(x), step = 1;
            if (ImGui::InputScalar("##v", ImGuiDataType_U32, &u, &step)) SetVarValue(v, u), changed = true;
            break;
        }
    }
    return changed;
}

std::filesystem::path PresetPath() { return ExeDir() / "trainer_vars.txt"; }

// the changed variables (and which are locked) to trainer_vars.txt, name=value [lock]
int SavePreset() {
    std::ofstream f(PresetPath());
    if (!f) return -1;
    f << "# hppoa trainer (F6): tuning variables changed from the game's values. name=value, \"lock\" to keep it\n";
    int n = 0;
    for (int i = 0, c = hppoa_TrainerVarCount(); i < c; i++) {
        const HppoaTrainerVar& v = *hppoa_TrainerVar(i);
        bool locked = FindFreeze(v.va) >= 0;
        if (!VarChanged(v) && !locked) continue;
        bool ok;
        double x = VarValue(v, &ok);
        if (!ok) continue;
        char line[160];
        std::snprintf(line, sizeof line, "%s=%.9g%s\n", v.name, x, locked ? " lock" : "");
        f << line;
        n++;
    }
    return n;
}
int LoadPreset() {
    std::ifstream f(PresetPath());
    if (!f) return -1;
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const HppoaTrainerVar* v = VarByName(line.substr(0, eq).c_str());
        if (!v) continue;
        SetVarValue(*v, std::atof(line.c_str() + eq + 1));
        if (line.find(" lock") != std::string::npos) {
            SetFreeze(v->va, VarSize(*v), 0);
            Refreeze(v->va, VarSize(*v));
        }
        n++;
    }
    return n;
}

// ── start switches (game\runtime.cfg, the title's command line) ───────────────

struct Launch {
    bool loaded = false;
    std::vector<std::string> other;              // the lines left as they are
    std::map<std::string, std::string> sw;       // lower-case switch -> value ("" for a bare switch)
    std::vector<std::string> chapters;
    std::map<std::string, std::vector<std::string>> rooms;
    char chapter[64] = "", room[64] = "", point[64] = "";
    bool nomovie = false, nomusic = false, nosound = false;
    std::string msg;
};
Launch g_launch;

const char* const kManaged[] = {"startchapter", "startroom", "startdebugpoint", "nomovie", "nomusic", "nosound"};

std::filesystem::path CfgPath() { return ExeDir() / "game" / "runtime.cfg"; }

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void LoadLaunch() {
    Launch& l = g_launch;
    l = Launch{};
    l.loaded = true;
    std::error_code ec;
    std::set<std::string> chapters;
    for (auto& e : std::filesystem::directory_iterator(ExeDir() / "game" / "data" / "packages" / "plfs", ec)) {
        std::string n = Lower(e.path().filename().string());
        if (n.size() < 5 || n.substr(n.size() - 4) != ".pkg") continue;
        n.resize(n.size() - 4);
        size_t dash = n.find('-');
        if (dash == std::string::npos) {
            if (n != "gplfs") chapters.insert(n);
        } else {
            chapters.insert(n.substr(0, dash));
            l.rooms[n.substr(0, dash)].push_back(n.substr(dash + 1));
        }
    }
    l.chapters.assign(chapters.begin(), chapters.end());
    for (auto& [c, r] : l.rooms) std::sort(r.begin(), r.end());
    std::ifstream f(CfgPath());
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::string key = line, val;
        if (!key.empty() && key[0] == '-') key.erase(0, 1);
        size_t eq = key.find('=');
        if (eq != std::string::npos) val = key.substr(eq + 1), key.resize(eq);
        key = Lower(key);
        if (std::find(std::begin(kManaged), std::end(kManaged), key) != std::end(kManaged)) l.sw[key] = val;
        else if (!line.empty()) l.other.push_back(line);
    }
    auto copy = [](char* dst, const std::string& s) { std::snprintf(dst, 64, "%s", s.c_str()); };
    copy(l.chapter, l.sw.count("startchapter") ? l.sw["startchapter"] : "");
    copy(l.room, l.sw.count("startroom") ? l.sw["startroom"] : "");
    copy(l.point, l.sw.count("startdebugpoint") ? l.sw["startdebugpoint"] : "");
    l.nomovie = l.sw.count("nomovie"), l.nomusic = l.sw.count("nomusic"), l.nosound = l.sw.count("nosound");
}

void SaveLaunch() {
    Launch& l = g_launch;
    std::filesystem::path cfg = CfgPath(), bak = cfg;
    bak += ".trainer-bak";
    std::error_code ec;
    if (!std::filesystem::exists(bak, ec)) std::filesystem::copy_file(cfg, bak, ec);
    std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
    if (!f) {
        l.msg = "Could not write " + cfg.string();
        return;
    }
    for (const std::string& s : l.other) f << s << "\r\n";
    if (l.chapter[0]) f << "-startChapter=" << l.chapter << "\r\n";
    if (l.chapter[0] && l.room[0]) f << "-startRoom=" << l.room << "\r\n";
    if (l.chapter[0] && l.point[0]) f << "-startDebugPoint=" << l.point << "\r\n";
    if (l.nomovie) f << "-nomovie\r\n";
    if (l.nomusic) f << "-nomusic\r\n";
    if (l.nosound) f << "-nosound\r\n";
    l.msg = "Saved to game\\runtime.cfg: restart the game for it to take effect (the old file is runtime.cfg.trainer-bak)";
    LOGI("trainer: wrote %s", cfg.string().c_str());
}

// ── pages ───────────────────────────────────────────────────────────────────

void DrawCheats() {
    HppoaTrainer& t = g_hppoa_trainer;
    bool in_game = hppoa_TrainerInGame();
    if (!in_game) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Not in a level: character values show once one is loaded.");

    Row("Health");
    bool god = t.god_mode;
    if (ImGui::Checkbox("God mode (health kept full)", &god)) t.god_mode = god;
    uint32_t player = hppoa_TrainerPlayer();
    if (ImGui::BeginTable("##health", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Character");
        ImGui::TableSetupColumn("Health");
        ImGui::TableSetupColumn("Most");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int s = 0; s < HPPOA_CHARACTER_SLOTS; s++) {
            uint32_t chr = hppoa_TrainerCharacter(s), h = hppoa_TrainerHealth(chr);
            if (!h) continue;
            ImGui::PushID(s);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", kSlotNames[s], chr == player ? "  (playing)" : "");
            float cur = 0, most = 0;
            Read(h + 0x34, &cur), Read(h + 0x3C, &most);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##cur", &cur, 0.1f, 0.0f, std::max(most, 0.0f), "%.1f")) Write(h + 0x34, cur);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##max", &most, 0.1f, 0.1f, 1000.0f, "%.1f")) Write(h + 0x3C, most);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Refill")) Write(h + 0x34, most);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    Row("Game speed");
    float speed = t.speed;
    if (ImGui::SliderFloat("Speed", &speed, 0.1f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) t.speed = speed;
    ImGui::SameLine();
    if (ImGui::Button("1x")) t.speed = 1.0f;
    ImGui::TextDisabled("Scales the game clock's step each frame (gameplay, cutscenes and timers alike).");

    Row("Debug switches (the title's own)");
    static const char* const kSwitches[][2] = {
        {"NoClip", "Walk through walls"},
        {"DebugMove", "Debug movement"},
        {"DebugRender", "Debug drawing"},
        {"AllTargetsHighlight", "Highlight every target"},
        {"DebugTargetting", "Targeting debug"},
        {"DebugTargettingCamera", "Targeting camera debug"},
        {"DebugSpellCasting", "Spell casting debug"},
        {"DebugAnimation", "Animation debug"},
        {"DebugBehaviours", "Behaviour debug"},
        {"DebugTransitionEngine", "Transition engine debug"},
        {"SpellLogging", "Log spells"},
        {"CameraLogging", "Log the camera"},
        {"BehaviourLogging", "Log behaviours"},
        {"KineticsLogging", "Log kinetics"},
    };
    if (ImGui::BeginTable("##switches", 2)) {
        for (auto& sw : kSwitches) {
            const HppoaTrainerVar* v = VarByName(sw[0]);
            if (!v) continue;
            ImGui::TableNextColumn();
            ImGui::PushID(sw[0]);
            bool ok;
            bool b = VarValue(*v, &ok) != 0;
            if (ImGui::Checkbox(sw[1], &b)) SetVarValue(*v, b);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sw[0]);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Logs go to this run's log. A switch the retail build compiled out does nothing.");

    Row("Original debug menu");
    bool dm = t.debug_menu;
    if (ImGui::Checkbox("Unlock the developers' debug menu", &dm)) t.debug_menu = dm;
    ImGui::BeginDisabled(!dm);
    ImGui::SameLine();
    if (ImGui::Button("Open it now")) t.press_debug_key = 1;
    ImGui::EndDisabled();
    ImGui::TextWrapped(
        "On the title screen or main menu: chapter select, then room select (X) or start the chapter (0), then the "
        "debug start points. \"Open it now\" sends the menus' debug key ('G') to the front end.");
}

void DrawItems() {
    if (!hppoa_TrainerInGame()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Not in a level: the items show once one is loaded.");
        return;
    }
    static bool named_only = true, nonzero_only = false;
    ImGui::Checkbox("Named items only", &named_only);
    ImGui::SameLine();
    ImGui::Checkbox("Owned only", &nonzero_only);

    uint32_t shared = hppoa_TrainerSharedItems();
    Row("Collected (shared by the trio)");
    auto quick = [&](const char* label, int id, int add) {
        ImGui::PushID(label);
        if (ImGui::Button(label)) {
            int16_t v = 0;
            Read(shared + uint32_t(id) * 2, &v);
            Write<int16_t>(shared + uint32_t(id) * 2, int16_t(std::clamp(int(v) + add, 0, 32767)));
            Refreeze(shared + uint32_t(id) * 2, 2);
        }
        ImGui::PopID();
    };
    quick("+1000 beans", 0x0F, 1000);
    ImGui::SameLine();
    quick("+100 house points", 0x4A, 100);
    if (ImGui::BeginTable("##shared", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                             ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Count");
        ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int id = 0; id < HPPOA_ITEM_IDS; id++) {
            char name[40];
            hppoa_TrainerItemName(id, name, sizeof name);
            if (const char* k = KnownShared(id)) std::snprintf(name, sizeof name, "%s", k);
            uint32_t va = shared + uint32_t(id) * 2;
            int16_t v = 0;
            if (!Read(va, &v)) continue;
            if ((named_only && !name[0]) || (nonzero_only && !v)) continue;
            ImGui::PushID(id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%02X", id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name[0] ? name : "-");
            ImGui::TableNextColumn();
            int x = v;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##c", &x)) {
                Write<int16_t>(va, int16_t(std::clamp(x, -32768, 32767)));
                Refreeze(va, 2);
            }
            ImGui::TableNextColumn();
            LockBox("##l", va, 2);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // the characters' carried items; the trio may share one inventory
    std::vector<std::pair<uint32_t, std::string>> invs;
    for (int s = 0; s < HPPOA_CHARACTER_SLOTS; s++) {
        uint32_t inv = hppoa_TrainerInventory(hppoa_TrainerCharacter(s));
        if (!inv) continue;
        auto it = std::find_if(invs.begin(), invs.end(), [&](auto& p) { return p.first == inv; });
        if (it == invs.end()) invs.push_back({inv, kSlotNames[s]});
        else it->second += std::string(", ") + kSlotNames[s];
    }
    for (auto& [inv, who] : invs) {
        Row(("Carried: " + who).c_str());
        ImGui::PushID(int(inv));
        if (ImGui::BeginTable("##inv", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                              ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Item");
            ImGui::TableSetupColumn("Count");
            ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (int id = 0; id < HPPOA_ITEM_IDS; id++) {
                char name[40];
                hppoa_TrainerItemName(id, name, sizeof name);
                if (const char* k = KnownCarried(id)) std::snprintf(name, sizeof name, "%s", k);
                uint32_t va = inv + uint32_t(id);
                int8_t v = 0;
                if (!Read(va, &v)) continue;
                if ((named_only && !name[0]) || (nonzero_only && !v)) continue;
                ImGui::PushID(id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%02X", id);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name[0] ? name : "-");
                ImGui::TableNextColumn();
                int x = v;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##c", &x)) {
                    Write<int8_t>(va, int8_t(std::clamp(x, -128, 127)));
                    Refreeze(va, 1);
                }
                ImGui::TableNextColumn();
                LockBox("##l", va, 1);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("Counts the HUD and menus show are refreshed when the game next updates them.");
}

void DrawVariables() {
    static char filter[64];
    static int group = -1;  // index into kGroups, -1 all
    static bool changed_only = false;
    ImGui::SetNextItemWidth(200 * ImGui::GetIO().FontGlobalScale);
    ImGui::InputTextWithHint("##filter", "Search", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180 * ImGui::GetIO().FontGlobalScale);
    if (ImGui::BeginCombo("##group", group < 0 ? "All groups" : kGroups[group].name)) {
        if (ImGui::Selectable("All groups", group < 0)) group = -1;
        for (int i = 0; i < int(std::size(kGroups)); i++)
            if (ImGui::Selectable(kGroups[i].name, group == i)) group = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Changed only", &changed_only);

    static std::string msg;
    if (ImGui::Button("Save changed to trainer_vars.txt")) {
        int n = SavePreset();
        msg = n < 0 ? "Could not write trainer_vars.txt" : std::to_string(n) + " saved";
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        int n = LoadPreset();
        msg = n < 0 ? "No trainer_vars.txt beside hppoa.exe" : std::to_string(n) + " loaded";
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all")) {
        for (int i = 0, c = hppoa_TrainerVarCount(); i < c; i++) {
            const HppoaTrainerVar& v = *hppoa_TrainerVar(i);
            ClearFreeze(v.va);
            if (VarChanged(v)) SetVarValue(v, v.def);
        }
        msg = "Every variable back to the game's value";
    }
    if (!msg.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", msg.c_str());
    }

    std::string f = Lower(filter);
    std::vector<int> rows;
    for (int i = 0, c = hppoa_TrainerVarCount(); i < c; i++) {
        const HppoaTrainerVar& v = *hppoa_TrainerVar(i);
        if (group >= 0 && v.group != kGroups[group].mask) continue;
        if (!f.empty() && Lower(v.name).find(f) == std::string::npos) continue;
        if (changed_only && !VarChanged(v) && FindFreeze(v.va) < 0) continue;
        rows.push_back(i);
    }
    ImGui::Text("%zu of %d", rows.size(), hppoa_TrainerVarCount());
    if (!ImGui::BeginTable("##vars", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                            ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 22)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Variable", 0, 3.0f);
    ImGui::TableSetupColumn("Group", 0, 1.6f);
    ImGui::TableSetupColumn("Value", 0, 2.0f);
    ImGui::TableSetupColumn("Game's", 0, 1.0f);
    ImGui::TableSetupColumn("Lock / reset", 0, 1.3f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin(int(rows.size()));
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const HppoaTrainerVar& v = *hppoa_TrainerVar(rows[r]);
            ImGui::PushID(rows[r]);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool changed = VarChanged(v);
            if (changed) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "%s", v.name);
            else ImGui::TextUnformatted(v.name);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nat %08X\nrange %g .. %g", v.name, v.va, double(v.min), double(v.max));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", GroupName(v.group));
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            VarEditor(v);
            ImGui::TableNextColumn();
            if (v.kind == HPPOA_TV_BOOL) ImGui::TextDisabled("%s", v.def != 0 ? "on" : "off");
            else ImGui::TextDisabled("%.4g", double(v.def));
            ImGui::TableNextColumn();
            LockBox("##lock", v.va, VarSize(v));
            ImGui::SameLine();
            ImGui::BeginDisabled(!changed);
            if (ImGui::SmallButton("Reset")) SetVarValue(v, v.def);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Some are read only when a level or character is set up: change them, then load a save.");
}

void DrawLaunch() {
    Launch& l = g_launch;
    if (!l.loaded) LoadLaunch();
    ImGui::TextWrapped("The title's start switches, in game\\runtime.cfg. They apply the next time the game starts.");

    Row("Start in a chapter (skips the front end)");
    ImGui::SetNextItemWidth(200 * ImGui::GetIO().FontGlobalScale);
    if (ImGui::BeginCombo("Chapter", l.chapter[0] ? l.chapter : "(the front end, as normal)")) {
        if (ImGui::Selectable("(the front end, as normal)", !l.chapter[0])) l.chapter[0] = 0, l.room[0] = 0;
        for (const std::string& c : l.chapters)
            if (ImGui::Selectable(c.c_str(), c == l.chapter)) std::snprintf(l.chapter, 64, "%s", c.c_str()), l.room[0] = 0;
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(!l.chapter[0]);
    ImGui::SetNextItemWidth(200 * ImGui::GetIO().FontGlobalScale);
    if (ImGui::BeginCombo("Room", l.room[0] ? l.room : "(the chapter's own start)")) {
        if (ImGui::Selectable("(the chapter's own start)", !l.room[0])) l.room[0] = 0;
        auto it = l.rooms.find(l.chapter);
        if (it != l.rooms.end())
            for (const std::string& r : it->second)
                if (ImGui::Selectable(r.c_str(), r == l.room)) std::snprintf(l.room, 64, "%s", r.c_str());
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(200 * ImGui::GetIO().FontGlobalScale);
    ImGui::InputTextWithHint("Debug start point", "(none)", l.point, sizeof l.point);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Chapters and rooms are the ones the disc has packages for; the debug menu lists start points.");

    Row("Other switches");
    ImGui::Checkbox("No movies (-nomovie)", &l.nomovie);
    ImGui::Checkbox("No music (-nomusic)", &l.nomusic);
    ImGui::Checkbox("No sound (-nosound)", &l.nosound);

    ImGui::Spacing();
    if (ImGui::Button("Save to runtime.cfg")) SaveLaunch();
    ImGui::SameLine();
    if (ImGui::Button("Clear and save")) {
        l.chapter[0] = l.room[0] = l.point[0] = 0;
        l.nomovie = l.nomusic = l.nosound = false;
        SaveLaunch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) LoadLaunch();
    if (!l.msg.empty()) ImGui::TextWrapped("%s", l.msg.c_str());
}

}  // namespace

bool TrainerOpen() { return g_trainer; }

void ToggleTrainer() { g_trainer = !g_trainer; }

void DrawTrainer() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760 * io.FontGlobalScale, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin("Trainer (F6)", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!open) g_trainer = false;
        return;
    }
    ImGui::PushItemWidth(240 * io.FontGlobalScale);
    if (ImGui::BeginTabBar("##trainer")) {
        if (ImGui::BeginTabItem("Cheats")) {
            DrawCheats();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Items")) {
            DrawItems();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game variables")) {
            DrawVariables();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Start switches")) {
            DrawLaunch();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::End();
    if (!open) g_trainer = false;
}

}  // namespace app
