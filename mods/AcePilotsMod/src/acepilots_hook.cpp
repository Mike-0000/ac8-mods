// AcePilotsMod native part for ACE COMBAT 8 (Steam build 25201480, offline single-player only).
//
// Hooks two game functions by replacing the vtable entries that point at them:
//
//   the rank weight lookup     Enemy behaviour trees pick a manoeuvre by weighted random choice. Each option
//                              holds five weights, one per pilot rank. Options that get likelier with rank
//                              are "skilled", options that get rarer are "weak"; the settings scale each.
//   the plane flight values    ALiveAIPlane::CorrectNpcParameter fills a plane's turn rates and acceleration
//                              every frame; the settings scale what it returns and the plane's own copy of
//                              its difficulty row (missiles in the air, time before a blackout).
//
// It also owns the settings: acepilots.ini, the presets, the F6 menu, F8 preset cycling and the banner.
// Scripts\main.lua loads this DLL with package.loadlib, reads the same ini, and does what needs the
// engine's reflection: pilot rank and the behaviour flags. Build: hook\build.cmd.
#include <windows.h>
#include <windowsx.h>
#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---------------------------------------------------------------- game layout
// Addresses relative to the exe's load address.
const uintptr_t kLookupRva = 0x7bd8cc0;        // float Lookup(Node* option, Unit* unit)
const uintptr_t kLookupSlotRvas[] = {0xc5b19e0};
const unsigned char kLookupBytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xfa,
                                      0x48, 0x8b, 0xd9, 0x48, 0x85, 0xd2, 0x74, 0x23, 0xe8, 0x46, 0x19, 0x13, 0xff};
const uintptr_t kFlightRva = 0x7cd42c0;        // void CorrectNpcParameter(PlaneInterface* this, FlightValues* out)
const uintptr_t kFlightSlotRvas[] = {0xc4bfd60, 0xc4c2260, 0xc4c6168, 0xc4e75a8, 0xc508390, 0xc51d2e8, 0xc5465c0, 0xc54a720,
                                     0xc54b7a8, 0xc54e128, 0xc5513c8, 0xc5624d8, 0xc5675a0, 0xc568bd8, 0xcbf48e8};
const unsigned char kFlightBytes[] = {0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x57, 0x49, 0x8d, 0xab, 0xa8,
                                      0xf8, 0xff, 0xff, 0x48, 0x81, 0xec, 0x40, 0x08, 0x00, 0x00};

const size_t kNodeWeights = 0x50;       // float[5], ranks 1..5
const size_t kUnitFaction = 0x3b2;      // byte: 1 the player's side, 2 the enemy
const size_t kUnitRank = 0x252c;        // int PilotRank
const size_t kUnitKind = 0x254c;        // int: 0 unit, 1 child part, 2 grandchild part
const size_t kPlaneInterface = 0x25f0;  // the flight function's `this` is the unit plus this
const int kFactionPlayer = 1;
// Per kind: the unit's copy of its LiveNpcDifficultyDataTable row. Field offsets inside the row follow.
const size_t kUnitDifficultyRow[3] = {0x1868, 0x1e98, 0x24d0};
const size_t kRowDifficulty = 0x08;     // byte, 1 Casual .. 5 Ace
const size_t kRowRankBonus = 0x0c;      // int
const size_t kRowBlackoutStartMax = 0x1c, kRowBlackoutStartMin = 0x20;  // float seconds added before a blackout
const size_t kRowMissilesPlane = 0x48, kRowMissilesOther = 0x4c;        // int, missiles in the air at once
// What the game's table holds per difficulty, so a changed row can be put back.
const int kRankBonus[6] = {0, -1, -1, 0, 1, 1};
const float kBlackoutStartMax[6] = {0, 0, 1, 2, 3, 4}, kBlackoutStartMin[6] = {0, -3, -2, -1, 0, 1};
const int kStockMissiles = 2;
// FlightValues: nine floats from +8 in table order.
enum { kPitch = 0x08, kRoll = 0x0c, kYaw = 0x10, kAcceleration = 0x24, kDeceleration = 0x28 };

typedef float (*LookupFn)(void* node, void* unit);
typedef void (*FlightFn)(void* plane, void* out);

LookupFn g_lookup = nullptr;
FlightFn g_flight = nullptr;
bool g_workerStarted = false;
char g_ini[MAX_PATH];
char g_log[MAX_PATH];

// ---------------------------------------------------------------- settings

struct Settings {
    int forceRank;        // 0 leaves ranks to the game, 1..5 sets every enemy pilot (applied by main.lua)
    float skilled;        // skilled options are multiplied by at least this
    float topAce;         // ...and by up to this, the more an option is reserved for high ranks
    float weakDivide;     // weak options are divided by this
    int rank5Odds;        // every pilot uses the rank 5 weights
    float agility;        // turn rates and acceleration
    int missiles;         // missiles in the air per enemy
    float blackout;       // extra seconds of hard turning before a blackout
    int attackQueue, flares, evade, taunts;  // behaviour flags (applied by main.lua)
    int airOnly, includeAllies, fromDifficulty;  // scope; not part of a preset
    int scriptBias, scriptUnused;                // behaviour script mix; not part of a preset either
};

struct Preset { const wchar_t* name; const char* key; Settings s; };
const Preset kPresets[] = {
    {L"Off", "Off", {0, 1, 1, 1, 0, 1, 2, 0, 0, 0, 0, 0}},
    {L"All rank 5", "AllRank5", {5, 1, 1, 1, 0, 1, 2, 0, 0, 0, 0, 0}},
    {L"Boosted 2", "Boosted2", {0, 2, 2, 2, 0, 1, 2, 0, 0, 0, 0, 0}},
    {L"Boosted 3", "Boosted3", {0, 3, 3, 3, 0, 1, 2, 0, 0, 0, 0, 0}},
    {L"Boosted 4", "Boosted4", {0, 4, 4, 4, 0, 1, 2, 0, 0, 0, 0, 0}},
    {L"MAX", "Max", {5, 2, 20, 1000, 1, 1.5f, 4, 5, 1, 1, 1, 1}},
};
const int kPresetCount = _countof(kPresets);

Settings g_set = {5, 1, 1, 1, 0, 1, 2, 0, 0, 0, 0, 0, 1, 0, 4, 0, 0};  // All rank 5 until the ini is read
FILETIME g_iniTime = {};
bool g_dirty = false;
ULONGLONG g_dirtySince = 0;

enum Trend { kWeak, kFlat, kSkilled };
struct RankStats {
    unsigned long long calls;
    double stock[3];
    double now[3];
};
RankStats g_stats[6];
unsigned long long g_calls = 0, g_reported = 0, g_skipped = 0;
unsigned long long g_flightCalls = 0, g_flightRaised = 0, g_rowsChanged = 0;
int g_difficultySeen = 0;  // bit 1: a unit with a readable difficulty was seen, bit 2: one without
int g_unreadableLogged = 0, g_flightLogged = 0, g_rowLogged = 0;
ULONGLONG g_nextReport = 0;

void Log(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_log, "a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d [hook] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void Sanitize(Settings& s) {
    s.forceRank = (int)Clamp((float)s.forceRank, 0, 5);
    s.skilled = Clamp(s.skilled, 1, 10);
    s.topAce = Clamp(s.topAce, 1, 20);
    s.weakDivide = Clamp(s.weakDivide, 1, 1000);
    s.agility = Clamp(s.agility, 1, 3);
    s.missiles = (int)Clamp((float)s.missiles, 2, 8);
    s.blackout = Clamp(s.blackout, 0, 10);
    s.fromDifficulty = (int)Clamp((float)s.fromDifficulty, 1, 5);
    s.scriptBias = (int)Clamp((float)s.scriptBias, 0, 100);
    for (int* flag : {&s.rank5Odds, &s.attackQueue, &s.flares, &s.evade, &s.taunts, &s.airOnly, &s.includeAllies, &s.scriptUnused})
        *flag = *flag ? 1 : 0;
}

// The preset the settings match, ignoring the scope fields; -1 for a custom mix.
int PresetOf(const Settings& s) {
    for (int i = 0; i < kPresetCount; ++i)
        if (memcmp(&kPresets[i].s, &s, offsetof(Settings, airOnly)) == 0) return i;
    return -1;
}

void ApplyPreset(int index) {
    memcpy(&g_set, &kPresets[index].s, offsetof(Settings, airOnly));
}

bool OddsChanged(const Settings& s) { return s.skilled > 1 || s.topAce > 1 || s.weakDivide > 1 || s.rank5Odds; }

void LogSettings(const char* why) {
    const Settings& s = g_set;
    int preset = PresetOf(s);
    Log("settings (%s): preset %s | rank %d, skilled x%g, top-ace x%g, weak /%g, rank-5 odds %d | agility x%g, missiles %d, "
        "blackout +%gs | queue %d flares %d evade %d taunts %d | air only %d, allies %d, from difficulty %d | script bias %d%%%s",
        why, preset < 0 ? "Custom" : kPresets[preset].key, s.forceRank, s.skilled, s.topAce, s.weakDivide, s.rank5Odds,
        s.agility, s.missiles, s.blackout, s.attackQueue, s.flares, s.evade, s.taunts, s.airOnly, s.includeAllies,
        s.fromDifficulty, s.scriptBias, s.scriptUnused ? " with unused scripts" : "");
}

struct IniField { const char* key; int* i; float* f; const char* comment; };
const IniField* IniFields(int* count) {
    static const IniField fields[] = {
        {"ForceRank", &g_set.forceRank, nullptr, "0 leaves pilot ranks to the game, 1 to 5 sets every enemy pilot to that rank."},
        {"SkilledBoost", nullptr, &g_set.skilled, "1 to 10. Manoeuvre options that better pilots pick more often are multiplied by this."},
        {"TopAceBoost", nullptr, &g_set.topAce, "1 to 20. Options reserved for the highest ranks are multiplied by up to this."},
        {"WeakDivide", nullptr, &g_set.weakDivide, "1 to 1000. Options that better pilots pick less often are divided by this. 1000 is never."},
        {"Rank5Odds", &g_set.rank5Odds, nullptr, "1 makes every pilot choose manoeuvres with the rank 5 odds."},
        {"Agility", nullptr, &g_set.agility, "1 to 3. Enemy turn rates and acceleration are multiplied by this."},
        {"Missiles", &g_set.missiles, nullptr, "2 to 8. Missiles each enemy can have in the air at once. The game uses 2."},
        {"BlackoutBonus", nullptr, &g_set.blackout, "0 to 10. Extra seconds an enemy can turn hard before blacking out."},
        {"IgnoreAttackQueue", &g_set.attackQueue, nullptr, "1 lets every enemy attack without waiting for a turn."},
        {"ForceFlares", &g_set.flares, nullptr, "1 always allows enemies to use flares."},
        {"ForceEvade", &g_set.evade, nullptr, "1 always allows enemies to evade."},
        {"ForceTaunts", &g_set.taunts, nullptr, "1 always allows enemy taunt manoeuvres."},
        {"AirOnly", &g_set.airOnly, nullptr, "1 changes aircraft only, 0 also ground and sea units (rank and flags)."},
        {"IncludeAllies", &g_set.includeAllies, nullptr, "1 also changes allied units."},
        {"FromDifficulty", &g_set.fromDifficulty, nullptr, "Manoeuvre odds change on this difficulty and above: 1 any, 4 the second-highest tier, 5 the top tier."},
        {"ScriptBias", &g_set.scriptBias, nullptr, "0 to 100. Chance that each behaviour slot of each fighter type gets a harder script. Scores: acepilots_scripts.ini."},
        {"ScriptUnused", &g_set.scriptUnused, nullptr, "1 lets the script bias also pick scripts that no campaign plane uses."},
    };
    *count = _countof(fields);
    return fields;
}

void StampIni() {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExA(g_ini, GetFileExInfoStandard, &info)) g_iniTime = info.ftLastWriteTime;
}

void SaveIni() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_ini, "w") != 0 || !f) return;
    int preset = PresetOf(g_set), count;
    const IniField* fields = IniFields(&count);
    fprintf(f, "; AcePilotsMod settings. In game: F6 opens the menu, F8 steps through the presets.\n");
    fprintf(f, "; This file is rewritten when a setting changes; edits made here are picked up within a second.\n");
    fprintf(f, "; Preset is only a label for the values below (Off, AllRank5, Boosted2, Boosted3, Boosted4, Max, Custom).\n");
    fprintf(f, "Preset=%s\n", preset < 0 ? "Custom" : kPresets[preset].key);
    for (int i = 0; i < count; ++i) {
        fprintf(f, "; %s\n", fields[i].comment);
        if (fields[i].i) fprintf(f, "%s=%d\n", fields[i].key, *fields[i].i);
        else fprintf(f, "%s=%g\n", fields[i].key, *fields[i].f);
    }
    fclose(f);
    StampIni();
}

void LoadIni() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_ini, "r") != 0 || !f) return;
    int count, mode = -1;
    const IniField* fields = IniFields(&count);
    float strength = 2, rank = 5;
    bool current = false;
    // A key missing from the file means the stock value, not whatever was set before.
    ApplyPreset(0);
    g_set.airOnly = 1, g_set.includeAllies = 0, g_set.fromDifficulty = 4, g_set.scriptBias = 0, g_set.scriptUnused = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        float value;
        if (line[0] == ';' || sscanf_s(line, " %63[A-Za-z0-9] = %f", key, (unsigned)sizeof key, &value) != 2) continue;
        for (int i = 0; i < count; ++i) {
            if (_stricmp(key, fields[i].key) != 0) continue;
            if (fields[i].i) *fields[i].i = (int)value;
            else *fields[i].f = value;
            if (!_stricmp(key, "ForceRank")) current = true;
        }
        if (!_stricmp(key, "Mode")) mode = (int)value;  // the file layout before the menu existed
        else if (!_stricmp(key, "Strength")) strength = value;
        else if (!_stricmp(key, "Rank")) rank = value;
    }
    fclose(f);
    if (!current && mode >= 0) {
        ApplyPreset(mode == 1 ? 1 : mode == 2 ? (strength >= 4 ? 4 : strength >= 3 ? 3 : 2) : mode == 3 ? 5 : 0);
        if (mode == 1) g_set.forceRank = (int)rank;
        Sanitize(g_set);
        SaveIni();
        LogSettings("converted from the old file layout");
    } else {
        Sanitize(g_set);
        LogSettings("read from file");
    }
    memset(g_stats, 0, sizeof g_stats);
}

// A setting changed in the menu or by F8: takes effect at once, the file follows shortly.
void Changed() {
    Sanitize(g_set);
    memset(g_stats, 0, sizeof g_stats);
    g_dirty = true;
    g_dirtySince = GetTickCount64();
}

// ---------------------------------------------------------------- units

bool IsPlayerSide(const char* unit) { return *(const unsigned char*)(unit + kUnitFaction) == kFactionPlayer; }

// The unit's own copy of its difficulty row, or null if what is there does not look like one.
char* DifficultyRow(char* unit) {
    int kind = *(const int*)(unit + kUnitKind);
    if (kind < 0 || kind > 2) return nullptr;
    char* row = unit + kUnitDifficultyRow[kind];
    int difficulty = *(const unsigned char*)(row + kRowDifficulty);
    int bonus = *(const int*)(row + kRowRankBonus);
    if (difficulty >= 1 && difficulty <= 5 && bonus == kRankBonus[difficulty]) return row;
    if (g_unreadableLogged < 6) {  // raw values, to correct the offsets from a real run
        ++g_unreadableLogged;
        const unsigned char* p = (const unsigned char*)row;
        Log("unit kind %d rank %d: difficulty row not recognised: %02x %02x %02x %02x | %d", kind,
            *(const int*)(unit + kUnitRank), p[8], p[9], p[10], p[11], bonus);
    }
    return nullptr;
}

// Sets the missile and blackout fields of a unit's difficulty row to what the settings ask for. The stock
// values are known per difficulty, so turning a setting back down restores them.
void SyncRow(char* unit, char* row, bool enemy) {
    int difficulty = *(const unsigned char*)(row + kRowDifficulty);
    int missiles = enemy ? g_set.missiles : kStockMissiles;
    float bonus = enemy ? g_set.blackout : 0;
    float startMax = kBlackoutStartMax[difficulty] + bonus, startMin = kBlackoutStartMin[difficulty] + bonus;
    int* plane = (int*)(row + kRowMissilesPlane);
    int* other = (int*)(row + kRowMissilesOther);
    float* rowMax = (float*)(row + kRowBlackoutStartMax);
    float* rowMin = (float*)(row + kRowBlackoutStartMin);
    if (*plane == missiles && *other == missiles && *rowMax == startMax && *rowMin == startMin) return;
    if (*plane < 0 || *plane > 8 || *rowMax < -20 || *rowMax > 40) return;  // not what was expected; leave it
    if (g_rowLogged < 6) {
        ++g_rowLogged;
        Log("unit %p rank %d: missiles in the air %d -> %d, blackout delay +%.1f/+%.1f s -> +%.1f/+%.1f s", unit,
            *(const int*)(unit + kUnitRank), *plane, missiles, *rowMin, *rowMax, startMin, startMax);
    }
    *plane = *other = missiles;
    *rowMax = startMax;
    *rowMin = startMin;
    ++g_rowsChanged;
}

#include "acepilots_scripts.inl"

void Report(ULONGLONG now) {
    if (now < g_nextReport || g_calls == g_reported) return;
    g_nextReport = now + 15000;
    g_reported = g_calls;
    double share[6];
    int last = 0;
    bool distinct = true;
    char pairs[64] = "";
    for (int r = 1; r <= 5; ++r) {
        const RankStats& s = g_stats[r];
        share[r] = -1;
        if (s.calls < 50) continue;
        double stock = s.stock[kWeak] + s.stock[kFlat] + s.stock[kSkilled];
        double current = s.now[kWeak] + s.now[kFlat] + s.now[kSkilled];
        if (stock <= 0 || current <= 0) continue;
        share[r] = 100.0 * s.now[kWeak] / current;
        Log("rank %d: %llu weights read, weak-option share stock %.1f%% -> now %.1f%%", r, s.calls,
            100.0 * s.stock[kWeak] / stock, share[r]);
        if (last && share[r] >= share[last]) {
            distinct = false;
            sprintf_s(pairs + strlen(pairs), sizeof pairs - strlen(pairs), " %d>=%d", r, last);
        }
        last = r;
    }
    Log("%llu weights read in total; flight values scaled %llu times in %llu calls; difficulty rows changed %llu times",
        g_calls, g_flightRaised, g_flightCalls, g_rowsChanged);
    if (!g_set.rank5Odds && last) Log("ranks distinct (weak share falls as rank rises): %s%s", distinct ? "yes" : "NO, rank", pairs);
    if (g_skipped) Log("%llu weights left alone: unit set up below FromDifficulty", g_skipped);
    if (g_manager && (g_sampledSwapped || g_sampledStock)) {
        Log("scripts: %d of %d slots swapped; planes looked at since the last report: %llu flying a mix, %llu flying stock scripts",
            g_slotsSwapped, g_slotsEligible, g_sampledSwapped, g_sampledStock);
        g_sampledSwapped = g_sampledStock = 0;
    }
}

float LookupHook(void* node, void* unit) {
    // The original validates the unit and returns the weight for its clamped rank; anything it accepts is safe to read.
    float weight = g_lookup(node, unit);
    const Settings s = g_set;
    if (!OddsChanged(s) || !node || !unit) return weight;
    char* u = (char*)unit;
    if (!s.includeAllies && IsPlayerSide(u)) return weight;

    int rank = *(const int*)(u + kUnitRank);
    rank = rank < 1 ? 1 : rank > 5 ? 5 : rank;
    const float* w = (const float*)((const char*)node + kNodeWeights);
    Trend trend = w[4] > w[0] ? kSkilled : w[4] < w[0] ? kWeak : kFlat;

    char* row = DifficultyRow(u);
    int seen = row ? 1 : 2;  // each case is reported once
    if (!(g_difficultySeen & seen)) {
        g_difficultySeen |= seen;
        Log(row ? "difficulty readable from a unit: odds change on units set up on FromDifficulty or above"
                : "difficulty not readable from a unit: odds change on such units regardless of difficulty");
    }
    float result = weight;
    if (row && *(const unsigned char*)(row + kRowDifficulty) < s.fromDifficulty) {
        ++g_skipped;
    } else {
        float base = s.rank5Odds ? w[4] : weight;
        if (trend == kWeak) {
            result = base / s.weakDivide;
        } else if (trend == kSkilled) {
            // An option rank 1 never picks is a top-ace move and gets the full top-ace factor; one that merely
            // gets likelier with rank gets its own rank 5 to rank 1 ratio, kept between the two settings.
            float high = s.topAce > s.skilled ? s.topAce : s.skilled;
            result = base * (w[0] > 0 ? Clamp(w[4] / w[0], s.skilled, high) : high);
        } else {
            result = base;
        }
    }
    RankStats& st = g_stats[rank];
    ++st.calls;
    ++g_calls;
    st.stock[trend] += weight;
    st.now[trend] += result;
    Report(GetTickCount64());
    return result;
}

// Callers may reuse one FlightValues for many calls, and the original does not always rewrite it. Remember what
// was written where, so values that are still this hook's own are not multiplied a second time.
struct Written { void* out; float pitch; };
Written g_written[256];

void FlightHook(void* plane, void* out) {
    float before = out ? *(float*)((char*)out + kPitch) : 0;
    g_flight(plane, out);
    if (!plane || !out) return;
    const Settings s = g_set;
    char* unit = (char*)plane - kPlaneInterface;
    bool enemy = s.includeAllies || !IsPlayerSide(unit);
    ScriptsTick(unit, GetTickCount64());
    // The row is synced even with every setting at stock, so a value that was raised earlier comes back down.
    if (char* row = DifficultyRow(unit)) SyncRow(unit, row, enemy);
    if (s.agility <= 1.0f || !enemy) return;
    ++g_flightCalls;
    char* v = (char*)out;
    float pitch = *(float*)(v + kPitch);
    Written& slot = g_written[((uintptr_t)out >> 4) & 255];
    if (slot.out == out && slot.pitch == pitch && pitch == before) return;  // untouched since this hook wrote it
    if (!(pitch > 0.001f && pitch < 2000.0f)) return;  // not a turn rate
    const size_t fields[] = {kPitch, kRoll, kYaw, kAcceleration, kDeceleration};
    for (size_t field : fields) *(float*)(v + field) *= s.agility;
    slot.out = out;
    slot.pitch = *(float*)(v + kPitch);
    ++g_flightRaised;
    if (g_flightLogged < 4) {
        ++g_flightLogged;
        Log("plane %p: pitch rate %.2f -> %.2f, roll rate -> %.2f (x%.2f)", plane, pitch, slot.pitch,
            *(float*)(v + kRoll), s.agility);
    }
}

// ---------------------------------------------------------------- drawing helpers

const COLORREF kBack = RGB(13, 17, 24), kPanel = RGB(22, 28, 38), kLine = RGB(44, 54, 70), kText = RGB(236, 240, 246);
const COLORREF kDim = RGB(140, 154, 176), kAccent = RGB(72, 168, 255), kAccentDim = RGB(34, 74, 116), kHot = RGB(255, 96, 72);

float g_scale = 1.0f;
int S(float v) { return (int)(v * g_scale + 0.5f); }

void Fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void Round(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF edge) {
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, edge);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

HFONT Font(float size, int weight) {
    return CreateFontW(-S(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

void Text(HDC dc, const wchar_t* text, RECT r, HFONT font, COLORREF color, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &r, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[32] = L"";
    GetClassNameW(hwnd, cls, 32);
    RECT r;
    if (pid != GetCurrentProcessId() || !wcsncmp(cls, L"AcePilots", 9) || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r)) return TRUE;
    struct Best { HWND hwnd; RECT rect; }* best = (Best*)out;
    if ((r.right - r.left) * (r.bottom - r.top) > (best->rect.right - best->rect.left) * (best->rect.bottom - best->rect.top)) {
        best->hwnd = hwnd;
        best->rect = r;
    }
    return TRUE;
}

// The game's main window and its rectangle; the work area when it cannot be found (the test host).
HWND GameWindow(RECT* rect) {
    struct { HWND hwnd; RECT rect; } best = {nullptr, {0, 0, 0, 0}};
    EnumWindows(FindGameWindow, (LPARAM)&best);
    if (!best.hwnd) SystemParametersInfoW(SPI_GETWORKAREA, 0, &best.rect, 0);
    *rect = best.rect;
    return best.hwnd;
}

// ---------------------------------------------------------------- banner
// A small always-on-top window shown for a few seconds when F8 changes the preset. Like the menu it is plain
// Win32 owned by this DLL and never touches the game's renderer or UI.
const wchar_t kBannerClass[] = L"AcePilotsBanner";
const int kBannerMs = 4000;
HWND g_banner = nullptr;
wchar_t g_bannerText[96] = L"";
ULONGLONG g_bannerHideAt = 0;

LRESULT CALLBACK BannerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    Fill(dc, r, kBack);
    HFONT caption = Font(15, FW_SEMIBOLD), mode = Font(30, FW_BOLD);
    RECT top = {0, S(12), r.right, S(36)}, main = {0, S(38), r.right, r.bottom - S(8)};
    Text(dc, L"ENEMY PILOTS   \x00B7   F8 next preset   \x00B7   F6 menu", top, caption, kDim, DT_CENTER | DT_SINGLELINE);
    Text(dc, g_bannerText, main, mode, kText, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    DeleteObject(caption);
    DeleteObject(mode);
    EndPaint(hwnd, &ps);
    return 0;
}

void ShowBanner() {
    int preset = PresetOf(g_set);
    swprintf_s(g_bannerText, L"%s", preset < 0 ? L"Custom" : kPresets[preset].name);
    RECT game;
    GameWindow(&game);
    g_scale = Clamp((game.bottom - game.top) / 1080.0f, 0.8f, 2.0f);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!g_banner) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = BannerProc;
        wc.hInstance = inst;
        wc.lpszClassName = kBannerClass;
        RegisterClassW(&wc);
        g_banner = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                   kBannerClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
        if (!g_banner) return;
        SetLayeredWindowAttributes(g_banner, 0, 255, LWA_ALPHA);
    }
    int w = S(560), h = S(104);
    SetWindowPos(g_banner, HWND_TOPMOST, game.left + ((game.right - game.left) - w) / 2, game.top + (game.bottom - game.top) / 8,
                 w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_banner, nullptr, TRUE);
    g_bannerHideAt = GetTickCount64() + kBannerMs;
}

// ---------------------------------------------------------------- menu
// A real window that takes focus while it is open, so the mouse and keyboard work normally in it and nothing
// typed there reaches the game. Closing it hands focus back to the game window.

enum Kind { kSlider, kSteps, kToggle };
struct Control {
    const wchar_t* label;
    const wchar_t* help;
    Kind kind;
    int column, section;  // section titles are drawn when the section changes
    float* f;
    int* i;
    float lo, hi, step;
    const wchar_t* format;       // for the value text
    const float* steps;          // kSteps: the allowed values
    int stepCount;
    const wchar_t* const* names; // optional name per integer value, from lo
};

const float kWeakSteps[] = {1, 1.5f, 2, 3, 4, 6, 10, 25, 100, 1000};
const wchar_t* const kRankNames[] = {L"Game decides", L"1", L"2", L"3", L"4", L"5"};
const wchar_t* const kFromNames[] = {L"Any difficulty", L"Easy and up", L"Normal and up", L"Elite and up", L"Ace only"};
const wchar_t* const kSections[] = {L"PILOT SKILL", L"AIRCRAFT", L"AGGRESSION", L"APPLIES TO", L"BEHAVIOUR SCRIPTS"};

const Control kControls[] = {
    {L"Pilot rank", L"Sets every enemy pilot to one rank. Rank drives manoeuvre choice and gun handling. 5 is a named ace.",
     kSlider, 0, 0, nullptr, &g_set.forceRank, 0, 5, 1, nullptr, nullptr, 0, kRankNames},
    {L"Smart moves", L"Multiplies the odds of manoeuvres that better pilots choose more often. 1 is the stock game.",
     kSlider, 0, 0, &g_set.skilled, nullptr, 1, 10, 0.5f, L"\x00D7%g"},
    {L"Ace-only moves", L"Extra weight, up to this much, for manoeuvres that low ranks never use: the hardest breaks and flares.",
     kSlider, 0, 0, &g_set.topAce, nullptr, 1, 20, 1, L"\x00D7%g"},
    {L"Weak moves", L"Divides the odds of lazy manoeuvres. Never removes them wherever a better option exists.",
     kSteps, 0, 0, &g_set.weakDivide, nullptr, 0, 0, 0, L"\x00F7%g", kWeakSteps, _countof(kWeakSteps)},
    {L"Everyone flies with ace odds", L"Every pilot chooses manoeuvres with the rank 5 odds, whatever their rank.",
     kToggle, 0, 0, nullptr, &g_set.rank5Odds},
    {L"Agility", L"Multiplies enemy turn rates and acceleration. Applies at once to planes in the air. Above 2 they may overshoot.",
     kSlider, 0, 1, &g_set.agility, nullptr, 1, 3, 0.1f, L"\x00D7%.1f"},
    {L"Missiles in the air", L"How many missiles each enemy may have flying at once. The game uses 2.",
     kSlider, 0, 1, nullptr, &g_set.missiles, 2, 8, 1, L"%d"},
    {L"G tolerance", L"Extra seconds an enemy can hold a hard turn before blacking out and easing off.",
     kSlider, 0, 1, &g_set.blackout, nullptr, 0, 10, 0.5f, L"+%g s"},
    {L"No attack queue", L"Enemies attack whenever they can instead of waiting for a turn. More of them on you at once.",
     kToggle, 1, 2, nullptr, &g_set.attackQueue},
    {L"Flares always allowed", L"Enemies may use flares even where the mission had switched them off.",
     kToggle, 1, 2, nullptr, &g_set.flares},
    {L"Evasion always allowed", L"Enemies may evade even where the mission had switched it off.",
     kToggle, 1, 2, nullptr, &g_set.evade},
    {L"Taunt manoeuvres", L"Enemies may use their showy provoke manoeuvres.",
     kToggle, 1, 2, nullptr, &g_set.taunts},
    {L"Manoeuvre odds change on", L"The lowest difficulty on which the pilot skill odds are changed. Everything else applies on any difficulty.",
     kSlider, 1, 3, nullptr, &g_set.fromDifficulty, 1, 5, 1, nullptr, nullptr, 0, kFromNames},
    {L"Aircraft only", L"Off also raises rank and flags on ground and sea units, whose guns and SAMs use rank too.",
     kToggle, 1, 3, nullptr, &g_set.airOnly},
    {L"Include allies", L"Applies everything to your own side as well.",
     kToggle, 1, 3, nullptr, &g_set.includeAllies},
    {L"Script bias", L"Chance that each behaviour slot of each fighter type gets a harder script. Re-rolled every minute. Applies to enemies that spawn afterwards.",
     kSlider, 1, 4, nullptr, &g_set.scriptBias, 0, 100, 5, L"%d%%"},
    {L"Include unused scripts", L"Also draw from scripts that are in the game files but that no campaign plane uses. Untested by the developers on these enemies.",
     kToggle, 1, 4, nullptr, &g_set.scriptUnused},
};
const int kControlCount = _countof(kControls);

const wchar_t kMenuClass[] = L"AcePilotsMenu";
HWND g_menu = nullptr, g_gameWindow = nullptr;
bool g_menuOpen = false;
volatile LONG g_openRequest = 0;  // set by the test entry; the window must be made on the worker thread
int g_selected = 0;      // control index, or -1 - preset index while a preset chip has the keyboard
int g_dragging = -1;
RECT g_controlRect[kControlCount], g_trackRect[kControlCount], g_chipRect[kPresetCount];
RECT g_closeRect;

float Value(const Control& c) { return c.f ? *c.f : (float)*c.i; }

// Where a control's value sits along its track, 0..1.
float Fraction(const Control& c) {
    float v = Value(c);
    if (c.kind == kSteps) {
        int at = 0;
        for (int k = 0; k < c.stepCount; ++k)
            if (fabsf(c.steps[k] - v) < fabsf(c.steps[at] - v)) at = k;
        return (float)at / (c.stepCount - 1);
    }
    return (v - c.lo) / (c.hi - c.lo);
}

void SetFraction(const Control& c, float t) {
    t = Clamp(t, 0, 1);
    float v;
    if (c.kind == kSteps) v = c.steps[(int)(t * (c.stepCount - 1) + 0.5f)];
    else v = c.lo + floorf(t * (c.hi - c.lo) / c.step + 0.5f) * c.step;
    if (v == Value(c)) return;
    if (c.f) *c.f = v;
    else *c.i = (int)(v + 0.5f);
    Changed();
}

void Nudge(const Control& c, int direction) {
    if (c.kind == kToggle) {
        *c.i = !*c.i;
        Changed();
        return;
    }
    int count = c.kind == kSteps ? c.stepCount - 1 : (int)((c.hi - c.lo) / c.step + 0.5f);
    SetFraction(c, Fraction(c) + (float)direction / count);
}

void ValueText(const Control& c, wchar_t* out, size_t size) {
    float v = Value(c);
    if (c.names) swprintf_s(out, size, L"%s", c.names[(int)(v - c.lo + 0.5f)]);
    else if (c.kind == kSteps && v >= 1000) swprintf_s(out, size, L"Never");
    else if (c.f) swprintf_s(out, size, c.format, (double)v);
    else swprintf_s(out, size, c.format, (int)v);
}

void PaintMenu(HDC dc, const RECT& client) {
    Fill(dc, client, kBack);
    HFONT title = Font(26, FW_BOLD), section = Font(13, FW_BOLD), label = Font(16, FW_NORMAL), value = Font(16, FW_SEMIBOLD);
    HFONT tiny = Font(13, FW_NORMAL), chip = Font(15, FW_SEMIBOLD);
    int pad = S(28), width = client.right;

    RECT r = {pad, S(18), width - pad, S(54)};
    Text(dc, L"ENEMY PILOTS", r, title, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    g_closeRect = {width - pad - S(120), S(22), width - pad, S(50)};
    Round(dc, g_closeRect, S(8), kPanel, kLine);
    Text(dc, L"Close   F6", g_closeRect, tiny, kDim, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // presets
    int preset = PresetOf(g_set), chipW = (width - 2 * pad - S(8) * kPresetCount) / (kPresetCount + 1), x = pad, top = S(70);
    for (int p = 0; p <= kPresetCount; ++p) {
        RECT c = {x, top, x + chipW, top + S(40)};
        bool custom = p == kPresetCount, on = custom ? preset < 0 : preset == p, focus = !custom && g_selected == -1 - p;
        if (!custom) g_chipRect[p] = c;
        COLORREF edge = focus ? kText : on ? (p == kPresetCount - 1 ? kHot : kAccent) : kLine;
        Round(dc, c, S(10), on ? (p == kPresetCount - 1 ? RGB(92, 30, 24) : kAccentDim) : kPanel, edge);
        wchar_t text[40];
        if (custom) swprintf_s(text, L"Custom");
        else swprintf_s(text, L"%d   %s", p + 1, kPresets[p].name);
        Text(dc, text, c, chip, on || focus ? kText : custom ? kLine : kDim, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        x += chipW + S(8);
    }

    // controls, two columns
    int colW = (width - 2 * pad - S(32)) / 2, rowH = S(44), y[2] = {S(128), S(128)}, lastSection = -1;
    for (int k = 0; k < kControlCount; ++k) {
        const Control& c = kControls[k];
        int left = pad + c.column * (colW + S(32));
        if (c.section != lastSection) {
            lastSection = c.section;
            if (y[c.column] > S(128)) y[c.column] += S(10);
            RECT head = {left, y[c.column], left + colW, y[c.column] + S(26)};
            Text(dc, kSections[c.section], head, section, kAccent, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT rule = {left, head.bottom, left + colW, head.bottom + 1};
            Fill(dc, rule, kLine);
            y[c.column] += S(32);
        }
        RECT row = {left - S(8), y[c.column], left + colW + S(8), y[c.column] + rowH - S(4)};
        g_controlRect[k] = row;
        if (k == g_selected) Round(dc, row, S(8), kPanel, kAccent);
        RECT name = {left, row.top, left + colW, row.bottom};
        if (c.kind == kToggle) {
            Text(dc, c.label, name, label, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT sw = {left + colW - S(52), row.top + S(9), left + colW, row.bottom - S(9)};
            g_trackRect[k] = sw;
            bool on = *c.i != 0;
            Round(dc, sw, sw.bottom - sw.top, on ? kAccent : kPanel, on ? kAccent : kLine);
            int d = sw.bottom - sw.top - S(6), kx = on ? sw.right - d - S(3) : sw.left + S(3);
            RECT knob = {kx, sw.top + S(3), kx + d, sw.top + S(3) + d};
            Round(dc, knob, d, on ? kText : kDim, on ? kText : kDim);
        } else {
            RECT half = {left, row.top + S(2), left + colW, row.top + S(22)};
            Text(dc, c.label, half, label, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            wchar_t text[48];
            ValueText(c, text, _countof(text));
            bool stock = Fraction(c) <= 0.0001f && k != 12;
            Text(dc, text, half, value, stock ? kDim : kAccent, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            RECT track = {left, row.bottom - S(12), left + colW, row.bottom - S(7)};
            g_trackRect[k] = {track.left, row.top + S(20), track.right, row.bottom};
            Round(dc, track, S(5), kLine, kLine);
            int knobX = track.left + (int)((track.right - track.left) * Fraction(c));
            RECT fill = {track.left, track.top, knobX, track.bottom};
            if (fill.right > fill.left + 2) Round(dc, fill, S(5), kAccent, kAccent);
            RECT knob = {knobX - S(7), track.top - S(5), knobX + S(7), track.bottom + S(5)};
            Round(dc, knob, S(14), kText, kText);
        }
        y[c.column] += rowH;
    }

    // footer: what the selected control does, live numbers, keys
    int bottom = client.bottom;
    RECT rule = {pad, bottom - S(92), width - pad, bottom - S(92) + 1};
    Fill(dc, rule, kLine);
    RECT help = {pad, bottom - S(84), width - pad, bottom - S(60)};
    Text(dc, g_selected >= 0 ? kControls[g_selected].help : L"Presets fill in every slider at once. Change any slider afterwards to make your own mix.",
         help, label, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    double stock = 0, now = 0, weakStock = 0, weakNow = 0;
    for (int rnk = 1; rnk <= 5; ++rnk)
        for (int t = 0; t < 3; ++t) {
            stock += g_stats[rnk].stock[t];
            now += g_stats[rnk].now[t];
            if (t == kWeak) weakStock += g_stats[rnk].stock[t], weakNow += g_stats[rnk].now[t];
        }
    wchar_t live[200];
    if (stock > 0 && now > 0)
        swprintf_s(live, L"Live:  weak moves %.0f%% \x2192 %.0f%% of %llu manoeuvre choices   \x00B7   agility applied %llu times   \x00B7   scripts %hs, %d/%d slots swapped",
                   100 * weakStock / stock, 100 * weakNow / now, g_calls, g_flightRaised, g_scriptStatus, g_slotsSwapped, g_slotsEligible);
    else
        swprintf_s(live, L"Live:  no manoeuvre choices seen since the last change   \x00B7   agility applied %llu times   \x00B7   scripts %hs, %d/%d slots swapped",
                   g_flightRaised, g_scriptStatus, g_slotsSwapped, g_slotsEligible);
    RECT liveRect = {pad, bottom - S(58), width - pad, bottom - S(36)};
    Text(dc, live, liveRect, tiny, kDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT keys = {pad, bottom - S(34), width - pad, bottom - S(12)};
    Text(dc, L"Mouse, or  \x2191\x2193 select   \x2190\x2192 change   1\x2013" L"6 preset   F8 next preset   Esc close      Gamepad: D-pad, A toggle, LB/RB preset, B close",
         keys, tiny, kDim, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (HFONT f : {title, section, label, value, tiny, chip}) DeleteObject(f);
}

void CloseMenu(bool returnFocus) {
    if (!g_menuOpen) return;
    g_menuOpen = false;
    g_dragging = -1;
    ReleaseCapture();
    ShowWindow(g_menu, SW_HIDE);
    if (returnFocus && g_gameWindow && IsWindow(g_gameWindow)) SetForegroundWindow(g_gameWindow);
}

void SelectPreset(int index) {
    ApplyPreset(index);
    Changed();
}

void NextPreset(int direction) {
    int preset = PresetOf(g_set);
    SelectPreset(preset < 0 ? (direction > 0 ? 0 : kPresetCount - 1) : (preset + direction + kPresetCount) % kPresetCount);
}

void MoveSelection(int direction) {
    if (g_selected < 0) g_selected = direction > 0 ? 0 : g_selected;  // from the presets down into the controls
    else if (g_selected == 0 && direction < 0) g_selected = -1 - (PresetOf(g_set) < 0 ? 0 : PresetOf(g_set));
    else g_selected = (int)Clamp((float)(g_selected + direction), 0, (float)kControlCount - 1);
}

void MenuKey(WPARAM key) {
    if (key == VK_ESCAPE || key == VK_F6) { CloseMenu(true); return; }
    if (key == VK_F8) NextPreset(1);
    else if (key >= '1' && key < '1' + kPresetCount) SelectPreset((int)(key - '1'));
    else if (key >= VK_NUMPAD1 && key < VK_NUMPAD1 + kPresetCount) SelectPreset((int)(key - VK_NUMPAD1));
    else if (key == VK_UP) MoveSelection(-1);
    else if (key == VK_DOWN || key == VK_TAB) MoveSelection(1);
    else if (key == VK_LEFT || key == VK_RIGHT) {
        int direction = key == VK_LEFT ? -1 : 1;
        if (g_selected >= 0) Nudge(kControls[g_selected], direction);
        else {
            int p = (int)Clamp((float)(-1 - g_selected + direction), 0, (float)kPresetCount - 1);
            g_selected = -1 - p;
            SelectPreset(p);
        }
    } else if (key == VK_RETURN || key == VK_SPACE) {
        if (g_selected >= 0 && kControls[g_selected].kind == kToggle) Nudge(kControls[g_selected], 1);
        else if (g_selected < 0) SelectPreset(-1 - g_selected);
    } else if (key == VK_HOME && g_selected >= 0) SetFraction(kControls[g_selected], 0);
    else if (key == VK_END && g_selected >= 0) SetFraction(kControls[g_selected], 1);
    InvalidateRect(g_menu, nullptr, FALSE);
}

void DragTo(int k, int x) {
    const RECT& t = g_trackRect[k];
    SetFraction(kControls[k], (float)(x - t.left) / (float)(t.right - t.left));
}

LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintMenu(mem, client);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        MenuKey(wp);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&g_closeRect, p)) { CloseMenu(true); return 0; }
        for (int i = 0; i < kPresetCount; ++i)
            if (PtInRect(&g_chipRect[i], p)) { g_selected = -1 - i; SelectPreset(i); }
        for (int k = 0; k < kControlCount; ++k) {
            if (!PtInRect(&g_controlRect[k], p)) continue;
            g_selected = k;
            if (kControls[k].kind == kToggle) Nudge(kControls[k], 1);
            else if (PtInRect(&g_trackRect[k], p)) { g_dragging = k; SetCapture(hwnd); DragTo(k, p.x); }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging >= 0) { DragTo(g_dragging, GET_X_LPARAM(lp)); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        g_dragging = -1;
        ReleaseCapture();
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        for (int k = 0; k < kControlCount; ++k)
            if (PtInRect(&g_controlRect[k], p) && kControls[k].kind != kToggle) {
                g_selected = k;
                Nudge(kControls[k], GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
            }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) CloseMenu(false);  // clicked back into the game or another window
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
        return TRUE;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);  // keeps the live line current
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenMenu() {
    RECT game;
    HWND gameWindow = GameWindow(&game);
    if (gameWindow) g_gameWindow = gameWindow;
    g_scale = Clamp((game.bottom - game.top) / 1080.0f, 0.75f, 2.0f);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!g_menu) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = inst;
        wc.lpszClassName = kMenuClass;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        g_menu = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kMenuClass, L"Enemy pilots", WS_POPUP, 0, 0, 10, 10,
                                 nullptr, nullptr, inst, nullptr);
        if (!g_menu) return;
        SetTimer(g_menu, 1, 500, nullptr);
    }
    int w = S(1040), h = S(752);
    SetWindowPos(g_menu, HWND_TOPMOST, game.left + ((game.right - game.left) - w) / 2, game.top + ((game.bottom - game.top) - h) / 2,
                 w, h, SWP_SHOWWINDOW);
    g_menuOpen = true;
    g_selected = -1 - (PresetOf(g_set) < 0 ? 0 : PresetOf(g_set));
    if (g_banner) ShowWindow(g_banner, SW_HIDE);
    SetForegroundWindow(g_menu);
    SetFocus(g_menu);
    InvalidateRect(g_menu, nullptr, FALSE);
}

// ---------------------------------------------------------------- gamepad (menu only)

struct PadState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
typedef DWORD (WINAPI* PadGetState)(DWORD, PadState*);
PadGetState g_padGetState = nullptr;
WORD g_padButtons = 0;
ULONGLONG g_padRepeatAt = 0;

void PollPad() {
    if (!g_padGetState) {
        static bool tried = false;
        if (tried) return;
        tried = true;
        HMODULE lib = LoadLibraryW(L"xinput1_4.dll");
        if (!lib) lib = LoadLibraryW(L"xinput9_1_0.dll");
        if (lib) g_padGetState = (PadGetState)GetProcAddress(lib, "XInputGetState");
        if (!g_padGetState) return;
    }
    WORD buttons = 0;
    for (DWORD pad = 0; pad < 4; ++pad) {
        PadState state = {};
        if (g_padGetState(pad, &state) == 0) buttons |= state.buttons;
    }
    ULONGLONG now = GetTickCount64();
    WORD pressed = buttons & ~g_padButtons;
    const WORD kDirections = 0x000F;  // d-pad
    if (pressed & kDirections) g_padRepeatAt = now + 350;
    else if ((buttons & kDirections) && now > g_padRepeatAt) {  // hold a direction to repeat
        pressed |= buttons & kDirections;
        g_padRepeatAt = now + 90;
    }
    g_padButtons = buttons;
    if (!pressed) return;
    if (pressed & 0x0001) MenuKey(VK_UP);
    if (pressed & 0x0002) MenuKey(VK_DOWN);
    if (pressed & 0x0004) MenuKey(VK_LEFT);
    if (pressed & 0x0008) MenuKey(VK_RIGHT);
    if (pressed & 0x1000) MenuKey(VK_RETURN);          // A
    if (pressed & 0x0100) { NextPreset(-1); InvalidateRect(g_menu, nullptr, FALSE); }  // LB
    if (pressed & 0x0200) { NextPreset(1); InvalidateRect(g_menu, nullptr, FALSE); }   // RB
    if (pressed & 0x2000) CloseMenu(true);             // B
}

// ---------------------------------------------------------------- worker thread
// Owns both windows, the hotkeys and the ini. The hooks only read g_set.

bool OursInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI Worker(LPVOID) {
    bool f6Down = false, f8Down = false, first = true;
    ULONGLONG nextIniCheck = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ULONGLONG now = GetTickCount64();
        if (now >= nextIniCheck) {
            nextIniCheck = now + 300;
            WIN32_FILE_ATTRIBUTE_DATA info;
            bool exists = GetFileAttributesExA(g_ini, GetFileExInfoStandard, &info) != 0;
            if (exists && CompareFileTime(&info.ftLastWriteTime, &g_iniTime) != 0 && !g_dirty) {
                g_iniTime = info.ftLastWriteTime;
                Sleep(30);  // let the writer finish
                LoadIni();
                if (first) ShowBanner();
                if (g_menuOpen) InvalidateRect(g_menu, nullptr, FALSE);
            } else if (!exists && first) {
                SaveIni();
                LogSettings("new file");
                ShowBanner();
            }
            first = false;
        }
        if (g_dirty && now - g_dirtySince > 250 && g_dragging < 0) {
            g_dirty = false;
            SaveIni();
            LogSettings("changed in game");
        }
        // With the menu open its window has the keyboard; these catch the keys while the game has it.
        bool front = OursInFront();
        bool f6 = front && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        bool f8 = front && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (((f6 && !f6Down) || InterlockedExchange(&g_openRequest, 0)) && !g_menuOpen) OpenMenu();
        if (f8 && !f8Down && !g_menuOpen) {
            NextPreset(1);
            ShowBanner();
        }
        f6Down = f6;
        f8Down = f8;
        if (g_menuOpen) PollPad();
        if (g_banner && g_bannerHideAt && now > g_bannerHideAt) {
            ShowWindow(g_banner, SW_HIDE);
            g_bannerHideAt = 0;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, g_menuOpen ? 15 : 30, QS_ALLINPUT);
    }
}

// ---------------------------------------------------------------- install

// Points each vtable entry at `hook`, after checking it still points at `expected` and that the code there matches.
int Install(void** const* slots, size_t count, const void* expected, const unsigned char* bytes, size_t byteCount,
            void* hook, const char* what) {
    if (bytes && memcmp(expected, bytes, byteCount) != 0) {
        Log("%s not hooked: its code differs from build 25201480 (game updated?)", what);
        return 0;
    }
    int done = 0;
    for (size_t i = 0; i < count; ++i) {
        void** slot = slots[i];
        DWORD old = 0;
        if (*slot != expected || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) continue;
        *slot = hook;
        VirtualProtect(slot, sizeof *slot, old, &old);
        ++done;
    }
    if (done != (int)count) Log("%s: only %d of %zu vtable entries matched", what, done, count);
    return done;
}

void StartWorker() {
    if (g_workerStarted) return;
    g_workerStarted = true;
    CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
}

void Start(HMODULE self) {
    char dir[MAX_PATH];
    GetModuleFileNameA(self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    sprintf_s(g_ini, "%s\\acepilots.ini", dir);
    sprintf_s(g_log, "%s\\acepilots.log", dir);
    sprintf_s(g_scoreFile, "%s\\acepilots_scripts.ini", dir);
    // The patched vtable entries point into this DLL for the rest of the session, so it must never unload.
    HMODULE pinned;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&LookupHook, &pinned);
}

}  // namespace

// Test entries: hook caller-supplied slots instead of the game's, and drive the menu.
extern "C" __declspec(dllexport) int acepilots_install_at(void** slot) {
    g_lookup = (LookupFn)*slot;
    int done = Install(&slot, 1, *slot, nullptr, 0, (void*)&LookupHook, "test lookup");
    StartWorker();
    return done;
}
extern "C" __declspec(dllexport) int acepilots_install_flight_at(void** slot) {
    g_flight = (FlightFn)*slot;
    return Install(&slot, 1, *slot, nullptr, 0, (void*)&FlightHook, "test flight values");
}
extern "C" __declspec(dllexport) void* acepilots_menu_window() { return g_menuOpen ? g_menu : nullptr; }
extern "C" __declspec(dllexport) void acepilots_open_menu() { InterlockedExchange(&g_openRequest, 1); }
extern "C" __declspec(dllexport) void acepilots_test_engine(void* objectArray, void* nameToString) {
    g_objectArray = (char*)objectArray;
    g_nameToString = (NameToStringFn)nameToString;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    Start(self);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* name = strrchr(exe, '\\');
    if (_stricmp(name ? name + 1 : exe, "AceCombat8.exe") != 0) return TRUE;  // test host installs by hand
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);

    void** lookupSlots[_countof(kLookupSlotRvas)];
    for (size_t i = 0; i < _countof(kLookupSlotRvas); ++i) lookupSlots[i] = (void**)(base + kLookupSlotRvas[i]);
    g_lookup = (LookupFn)(base + kLookupRva);
    if (Install(lookupSlots, _countof(lookupSlots), (const void*)g_lookup, kLookupBytes, sizeof kLookupBytes,
                (void*)&LookupHook, "rank weight lookup"))
        Log("installed: rank weight lookup hooked");

    void** flightSlots[_countof(kFlightSlotRvas)];
    for (size_t i = 0; i < _countof(kFlightSlotRvas); ++i) flightSlots[i] = (void**)(base + kFlightSlotRvas[i]);
    g_flight = (FlightFn)(base + kFlightRva);
    int planes = Install(flightSlots, _countof(flightSlots), (const void*)g_flight, kFlightBytes, sizeof kFlightBytes,
                         (void*)&FlightHook, "plane flight values");
    if (planes) Log("installed: plane flight values hooked in %d plane classes", planes);
    if (memcmp((const void*)(base + kNameToStringRva), kNameToStringBytes, sizeof kNameToStringBytes) == 0) {
        g_objectArray = (char*)(base + kObjectArrayRva);
        g_nameToString = (NameToStringFn)(base + kNameToStringRva);
    } else {
        Log("script bias unavailable: the engine's name function differs from build 25201480 (game updated?)");
    }
    StartWorker();  // the menu and presets work even if a hook could not be installed
    return TRUE;
}
