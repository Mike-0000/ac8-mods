// Behaviour script bias. Included by acepilots_hook.cpp inside its namespace.
//
// Every enemy type is assigned a script set: one Lua behaviour script per slot (missile evasion near/far,
// gun evasion, dogfight at three ranges, escape, break away, pursuit ...). The game keeps the 40 sets as an
// array of rows in a manager object, next to an array of their names, and each unit copies its row when it
// is set up. This rewrites the rows in that array: with a chance set by ScriptBias, a slot gets a script with
// a higher difficulty score, taken from the same slot of another set. Enemies that spawn afterwards fly the
// new mix; the stock rows are kept and put back when the bias returns to 0.
//
// Scores live in acepilots_scripts.ini next to the DLL and are re-read when the file changes.
// Everything read from the engine is checked before it is trusted and nothing is written until all checks pass.

const uintptr_t kObjectArrayRva = 0xdc60cd0;   // GUObjectArray
const uintptr_t kNameToStringRva = 0x186f950;  // void FName::ToString(FString& out) const
const unsigned char kNameToStringBytes[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55, 0x48,
                                            0x8b, 0xec, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9, 0x48, 0x8b, 0xda};
// FUObjectArray: chunk table at +0x10, object count at +0x24; 65536 items of 24 bytes per chunk, object pointer first.
const size_t kArrayChunks = 0x10, kArrayCount = 0x24, kItemSize = 24;
const size_t kObjectIndex = 0x0c, kObjectName = 0x18;
// The manager: TArray of rows (0xf8 bytes: 8 bytes, then 15 slots of 16) and a parallel TArray of row names.
const size_t kManagerRows = 0x2e78, kManagerRowNames = 0x2e88;
const size_t kRowSize = 0xf8, kRowSlots = 0x08, kSlotSize = 0x10;
enum { kSlotCount = 15, kMaxSets = 64, kMaxScripts = 256 };
const int kSwapSlots[] = {2, 3, 4, 5, 9, 10, 11, 12, 13, 14};
const wchar_t* const kSlotNames[kSlotCount] = {L"root", L"evade", L"missile evasion near", L"missile evasion far", L"gun evasion",
    L"special-weapon evasion", L"blackout", L"-", L"dogfight", L"dogfight near", L"dogfight mid", L"dogfight far", L"escape",
    L"break away", L"pursuit"};
// Sets that are not ordinary fighters (large aircraft, one-off bosses, test rows) are never rewritten.
const char* const kSkipSets[] = {"NLID_1000", "NLID_009", "NLID_9"};
// A unit's copy of its set (kind 0 units), and the name of the set it was given.
const size_t kUnitScriptRow = 0x16a8, kUnitScriptSet = 0x13d0;

struct RawArray { char* data; int num; int max; };
struct EngineString { wchar_t* data; int num; int max; };
typedef void (*NameToStringFn)(const void* name, EngineString* out);

struct SeedScore { const char* name; int score; int unused; };
const SeedScore kSeedScores[] = {
#include "script_scores.inc"
};

struct Script {
    void* object;
    char name[64];
    int score, unused;
    unsigned char slot[kSlotCount][kSlotSize];  // the bytes of a slot that holds this script
    bool inSlot[kSlotCount];
};

char* g_objectArray = nullptr;
NameToStringFn g_nameToString = nullptr;
char g_scoreFile[MAX_PATH];
FILETIME g_scoreFileTime = {};
struct FileScore { char name[64]; int score, unused; };
FileScore g_fileScores[400];
int g_fileScoreCount = 0;

char* g_manager = nullptr;
int g_managerIndex = -1;
int g_setCount = 0, g_slotPointerAt = 0;
unsigned char g_stockRows[kMaxSets][kRowSize], g_writtenRows[kMaxSets][kRowSize];
char g_setNames[kMaxSets][32];
unsigned char g_setNameRaw[kMaxSets][8];
bool g_setSkipped[kMaxSets];
short g_stockScript[kMaxSets][kSlotCount];
Script g_scripts[kMaxScripts];
int g_scriptCount = 0;
bool g_rowsWritten = false;

int g_appliedBias = -1, g_appliedUnused = -1;
int g_slotsSwapped = 0, g_slotsEligible = 0;
unsigned long long g_sampledSwapped = 0, g_sampledStock = 0, g_sampleCounter = 0;
ULONGLONG g_scriptsNext = 0, g_findNext = 0, g_rerollNext = 0;
unsigned int g_random = 0x2545f491;
const char* g_scriptStatus = "off";

unsigned int NextRandom() {
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return g_random;
}
float RandomUnit() { return (NextRandom() & 0xffffff) / 16777216.0f; }

// ---- reading engine memory that may not be what it is assumed to be

bool Readable(const void* p, size_t size) {
    if (!p || ((uintptr_t)p & 3)) return false;
    __try {
        volatile unsigned char sink = 0;
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < size; i += 0x400) sink ^= b[i];
        sink ^= b[size - 1];
        (void)sink;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// An engine name as text. The engine allocates the string; the few dozen short ones made here are not freed.
bool NameText(const void* name, char* out, size_t size) {
    out[0] = 0;
    if (!g_nameToString) return false;
    __try {
        EngineString text = {nullptr, 0, 0};
        g_nameToString(name, &text);
        if (!text.data || text.num <= 1 || text.num > 200) return false;
        size_t n = 0;
        for (; n + 1 < size && n < (size_t)text.num - 1; ++n) {
            wchar_t c = text.data[n];
            if (c < 0x20 || c > 0x7e) return false;
            out[n] = (char)c;
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

bool ObjectName(const void* object, char* out, size_t size) {
    return Readable(object, 0x30) && NameText((const char*)object + kObjectName, out, size);
}

// The manager's two arrays, if they look like 20 to 64 rows with as many names.
bool ManagerArrays(const char* object, RawArray* rows, RawArray* names) {
    __try {
        *rows = *(const RawArray*)(object + kManagerRows);
        *names = *(const RawArray*)(object + kManagerRowNames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (rows->num < 20 || rows->num > kMaxSets || rows->max < rows->num || rows->max > 4096) return false;
    if (names->num != rows->num || names->max < names->num || names->max > 4096) return false;
    if (!rows->data || ((uintptr_t)rows->data & 7) || !names->data || ((uintptr_t)names->data & 3)) return false;
    return true;
}

// ---- scores

void WriteDefaultScores() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_scoreFile, "w") != 0 || !f) return;
    fprintf(f, "; Difficulty score for each enemy behaviour script, 0 to 100. Higher counts as harder.\n");
    fprintf(f, "; Script bias only ever swaps a script for one with a HIGHER score from the same slot.\n");
    fprintf(f, "; 0 means never use this script as a replacement. \"unused\" marks scripts no campaign plane has;\n");
    fprintf(f, "; they are only used when \"Include unused scripts\" is on. Remove the word to treat one as normal.\n");
    fprintf(f, "; These scores are a first guess from counting manoeuvres in each script. Edit them freely:\n");
    fprintf(f, "; the file is re-read within a second and the mix is re-rolled. Delete the file to get it back.\n");
    for (const SeedScore& s : kSeedScores) fprintf(f, "%s = %d%s\n", s.name, s.score, s.unused ? " unused" : "");
    fclose(f);
}

bool LoadScoresIfChanged() {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(g_scoreFile, GetFileExInfoStandard, &info)) {
        WriteDefaultScores();
        if (!GetFileAttributesExA(g_scoreFile, GetFileExInfoStandard, &info)) return false;
    }
    if (CompareFileTime(&info.ftLastWriteTime, &g_scoreFileTime) == 0) return false;
    g_scoreFileTime = info.ftLastWriteTime;
    FILE* f = nullptr;
    if (fopen_s(&f, g_scoreFile, "r") != 0 || !f) return false;
    g_fileScoreCount = 0;
    char line[256];
    while (fgets(line, sizeof line, f) && g_fileScoreCount < (int)_countof(g_fileScores)) {
        FileScore& s = g_fileScores[g_fileScoreCount];
        float score;
        if (line[0] == ';' || sscanf_s(line, " %63[A-Za-z0-9_] = %f", s.name, (unsigned)sizeof s.name, &score) != 2) continue;
        s.score = (int)Clamp(score, 0, 100);
        s.unused = strstr(line, "unused") != nullptr;
        ++g_fileScoreCount;
    }
    fclose(f);
    for (int i = 0; i < g_scriptCount; ++i) {
        g_scripts[i].score = 50, g_scripts[i].unused = 1;  // a script the file does not list
        for (int k = 0; k < g_fileScoreCount; ++k)
            if (!strcmp(g_fileScores[k].name, g_scripts[i].name)) g_scripts[i].score = g_fileScores[k].score, g_scripts[i].unused = g_fileScores[k].unused;
    }
    Log("scripts: %d scores read from acepilots_scripts.ini", g_fileScoreCount);
    return true;
}

// ---- the manager

void ForgetManager(const char* why) {
    if (g_manager) Log("scripts: lost the script table (%s); will look for it again", why);
    g_manager = nullptr;
    g_setCount = g_scriptCount = 0;
    g_rowsWritten = false;
    g_appliedBias = -1;
    g_slotsSwapped = g_slotsEligible = 0;
}

void* SlotObject(const unsigned char* slot) { return *(void* const*)(slot + g_slotPointerAt); }

int ScriptFor(void* object, const unsigned char* slotBytes, int slot) {
    for (int i = 0; i < g_scriptCount; ++i)
        if (g_scripts[i].object == object) {
            if (!g_scripts[i].inSlot[slot]) memcpy(g_scripts[i].slot[slot], slotBytes, kSlotSize), g_scripts[i].inSlot[slot] = true;
            return i;
        }
    if (g_scriptCount >= kMaxScripts) return -1;
    Script& s = g_scripts[g_scriptCount];
    memset(&s, 0, sizeof s);
    if (!ObjectName(object, s.name, sizeof s.name) || strncmp(s.name, "LA_", 3) != 0) return -1;
    s.object = object;
    s.score = 50, s.unused = 1;
    memcpy(s.slot[slot], slotBytes, kSlotSize);
    s.inSlot[slot] = true;
    return g_scriptCount++;
}

// Checks that `object` is the manager and takes a copy of its stock rows. Nothing is kept unless every row,
// name and script reads back as expected.
bool AdoptManager(char* object, int index) {
    RawArray rows, names;
    if (!ManagerArrays(object, &rows, &names)) return false;
    if (!Readable(rows.data, rows.num * kRowSize) || !Readable(names.data, names.num * 8)) return false;
    char text[64];
    if (!NameText(names.data, text, sizeof text) || strncmp(text, "NLID_", 5) != 0) return false;

    // which half of a slot holds the script pointer: the root slot of the first row is always filled
    const unsigned char* root = (const unsigned char*)rows.data + kRowSlots;
    int pointerAt = -1;
    for (int at = 0; at <= 8 && pointerAt < 0; at += 8) {
        void* candidate = *(void* const*)(root + at);
        if (ObjectName(candidate, text, sizeof text) && !strncmp(text, "LA_", 3)) pointerAt = at;
    }
    if (pointerAt < 0) return false;

    g_slotPointerAt = pointerAt;
    g_setCount = rows.num;
    g_scriptCount = 0;
    for (int i = 0; i < g_setCount; ++i) {
        memcpy(g_stockRows[i], rows.data + i * kRowSize, kRowSize);
        memcpy(g_setNameRaw[i], names.data + i * 8, 8);
        if (!NameText(names.data + i * 8, g_setNames[i], sizeof g_setNames[i]) || strncmp(g_setNames[i], "NLID_", 5) != 0) {
            g_setCount = g_scriptCount = 0;
            return false;
        }
        const unsigned char* slots = g_stockRows[i] + kRowSlots;
        g_setSkipped[i] = false;
        for (const char* skip : kSkipSets)
            if (!strncmp(g_setNames[i], skip, strlen(skip))) g_setSkipped[i] = true;
        for (int s = 0; s < kSlotCount; ++s) {
            void* scriptObject = SlotObject(slots + s * kSlotSize);
            g_stockScript[i][s] = -1;
            if (!scriptObject) continue;
            int script = ScriptFor(scriptObject, slots + s * kSlotSize, s);
            if (script < 0) {  // a filled slot that is not a script: the layout is not what was assumed
                g_setCount = g_scriptCount = 0;
                return false;
            }
            g_stockScript[i][s] = (short)script;
        }
        if (g_stockScript[i][14] < 0 || g_stockScript[i][9] < 0) g_setSkipped[i] = true;  // no pursuit or dogfight: not a fighter
    }
    g_manager = object;
    g_managerIndex = index;
    g_rowsWritten = false;
    g_appliedBias = -1;
    g_scoreFileTime = {};  // scores are matched to the scripts just found
    int fighters = 0;
    for (int i = 0; i < g_setCount; ++i) fighters += !g_setSkipped[i];
    Log("scripts: found the script table: %d sets (%d fighter sets), %d scripts, first set %s", g_setCount, fighters,
        g_scriptCount, g_setNames[0]);
    return true;
}

char* ObjectAt(int index, int* count) {
    __try {
        *count = *(const int*)(g_objectArray + kArrayCount);
        if (index < 0 || index >= *count || *count > 20000000) return nullptr;
        char** chunks = *(char** const*)(g_objectArray + kArrayChunks);
        return *(char* const*)(chunks[index >> 16] + (size_t)(index & 0xffff) * kItemSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *count = 0;
        return nullptr;
    }
}

bool FindManager() {
    if (!g_objectArray) return false;
    int count = 0;
    ObjectAt(0, &count);
    RawArray rows, names;
    for (int i = 0; i < count; ++i) {
        int unused;
        char* object = ObjectAt(i, &unused);
        if (!object || ((uintptr_t)object & 7)) continue;
        if (ManagerArrays(object, &rows, &names) && AdoptManager(object, i)) return true;
    }
    return false;
}

// The manager is still the object it was and its arrays still have the rows that were copied.
bool ManagerStillThere(RawArray* rows) {
    int count;
    RawArray names;
    if (g_managerIndex >= 0 && ObjectAt(g_managerIndex, &count) != g_manager) return false;
    if (!ManagerArrays(g_manager, rows, &names) || rows->num != g_setCount) return false;
    if (!Readable(rows->data, rows->num * kRowSize) || !Readable(names.data, names.num * 8)) return false;
    return memcmp(names.data, g_setNameRaw, (size_t)g_setCount * 8) == 0;
}

// ---- the mix

// Rolls a new mix for every fighter set and writes it into the manager's rows.
void ApplyMix(RawArray rows, int biasPercent, bool allowUnused) {
    float bias = biasPercent / 100.0f;
    int examples = 0;
    g_slotsSwapped = g_slotsEligible = 0;
    for (int i = 0; i < g_setCount; ++i) {
        unsigned char row[kRowSize];
        memcpy(row, g_stockRows[i], kRowSize);
        for (int s : kSwapSlots) {
            int stock = g_stockScript[i][s];
            if (g_setSkipped[i] || stock < 0) continue;
            ++g_slotsEligible;
            if (RandomUnit() >= bias) continue;
            // harder scripts seen in this slot, best first
            int pool[kMaxScripts], poolSize = 0;
            for (int c = 0; c < g_scriptCount; ++c) {
                const Script& script = g_scripts[c];
                if (!script.inSlot[s] || script.score <= g_scripts[stock].score || (script.unused && !allowUnused)) continue;
                int at = poolSize++;
                for (; at > 0 && g_scripts[pool[at - 1]].score < script.score; --at) pool[at] = pool[at - 1];
                pool[at] = c;
            }
            if (!poolSize) continue;
            if (biasPercent >= 100 && poolSize > 3) poolSize = 3;  // at full bias only the hardest few, never just one
            // the higher the bias, the more the pick leans to the top of the pool
            float weights[kMaxScripts], total = 0;
            for (int k = 0; k < poolSize; ++k) total += weights[k] = expf((1 + 6 * bias) * g_scripts[pool[k]].score / 100.0f);
            float pick = RandomUnit() * total;
            int chosen = pool[poolSize - 1];
            for (int k = 0; k < poolSize; ++k)
                if ((pick -= weights[k]) <= 0) { chosen = pool[k]; break; }
            memcpy(row + kRowSlots + s * kSlotSize, g_scripts[chosen].slot[s], kSlotSize);
            ++g_slotsSwapped;
            if (examples < 6) {
                ++examples;
                Log("scripts:   %s %ls: %s (%d) -> %s (%d)", g_setNames[i], kSlotNames[s], g_scripts[stock].name,
                    g_scripts[stock].score, g_scripts[chosen].name, g_scripts[chosen].score);
            }
        }
        // only the slots are written; the first 8 bytes of a row are the engine's
        memcpy(rows.data + i * kRowSize + kRowSlots, row + kRowSlots, kRowSize - kRowSlots);
        memcpy(g_writtenRows[i], rows.data + i * kRowSize, kRowSize);
    }
    g_rowsWritten = true;
    g_appliedBias = biasPercent;
    g_appliedUnused = allowUnused;
    Log("scripts: bias %d%%%s: %d of %d slots swapped across the fighter sets; applies to enemies that spawn from now on",
        biasPercent, allowUnused ? " with unused scripts" : "", g_slotsSwapped, g_slotsEligible);
}

// Whether a plane in the air is flying its set's stock scripts or a mix; one plane in 32 calls is looked at.
void SampleUnit(const char* unit) {
    if (!g_manager || !g_rowsWritten || (++g_sampleCounter & 31) || *(const int*)(unit + kUnitKind) != 0) return;
    for (int i = 0; i < g_setCount; ++i) {
        if (memcmp(unit + kUnitScriptSet, g_setNameRaw[i], 8) != 0) continue;
        bool stock = memcmp(unit + kUnitScriptRow + kRowSlots, g_stockRows[i] + kRowSlots, kRowSize - kRowSlots) == 0;
        ++(stock ? g_sampledStock : g_sampledSwapped);
        return;
    }
}

// Called from the flight hook, so on the game's own thread and only while planes exist.
void ScriptsTick(const char* unit, ULONGLONG now) {
    SampleUnit(unit);
    if (now < g_scriptsNext) return;
    g_scriptsNext = now + 1000;
    int bias = g_set.scriptBias;
    bool allowUnused = g_set.scriptUnused != 0;
    if (!g_manager) {
        g_scriptStatus = bias ? "looking for the script table" : "off";
        if (!bias || now < g_findNext) return;  // the engine is not searched until the feature is used
        // searching walks every engine object, so back off when it keeps failing
        static int tries = 0;
        ++tries;
        g_findNext = now + (tries < 3 ? 5000 : tries < 8 ? 15000 : 60000);
        if (!FindManager()) {
            if (tries == 1 || tries % 5 == 0) Log("scripts: script table not found (attempt %d)", tries);
            return;
        }
        tries = 0;
    }
    RawArray rows;
    if (!ManagerStillThere(&rows)) {
        ForgetManager("the object changed");
        return;
    }
    // If the game rebuilt its rows they no longer match what was last put there; take them as the new stock.
    const unsigned char* expected = g_rowsWritten ? g_writtenRows[0] : g_stockRows[0];
    if (memcmp(rows.data, expected, (size_t)g_setCount * kRowSize) != 0) {
        char* manager = g_manager;
        int index = g_managerIndex;
        ForgetManager("the game rebuilt its rows");
        if (!AdoptManager(manager, index)) return;
    }
    bool scoresChanged = LoadScoresIfChanged();
    if (!bias) {
        if (g_rowsWritten) {
            for (int i = 0; i < g_setCount; ++i) memcpy(rows.data + i * kRowSize, g_stockRows[i], kRowSize);
            g_rowsWritten = false;
            g_slotsSwapped = 0;
            Log("scripts: bias 0%%: stock scripts restored");
        }
        g_appliedBias = 0;
        g_scriptStatus = "stock";
        return;
    }
    if (bias != g_appliedBias || (int)allowUnused != g_appliedUnused || scoresChanged || now >= g_rerollNext) {
        g_random ^= (unsigned int)now * 2654435761u;
        ApplyMix(rows, bias, allowUnused);
        g_rerollNext = now + 60000;  // a fresh mix every minute, so later waves differ from earlier ones
    }
    g_scriptStatus = "active";
}
