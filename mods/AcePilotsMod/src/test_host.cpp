// Stand-in for the game: fake versions of the two hooked functions behind fake vtable entries, with option nodes,
// units and flight values laid out like the real ones. Loads a private copy of the hook DLL, installs it on the
// fake entries and checks the results.
//     test_host.exe <path to acepilots_hook.dll>            (build.cmd test)
//     test_host.exe <path to acepilots_hook.dll> menu       open the menu and leave it up for a look
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

typedef float (*LookupFn)(void* node, void* unit);
typedef void (*FlightFn)(void* plane, void* out);

struct Node { char pad[0x50]; float w[5]; };
struct Unit { char bytes[0x2700]; };
struct FlightValues { char pad[8]; float v[9]; float extra; };  // pitch, roll, yaw, 4 speeds, acceleration, deceleration

static float Lookup(void* node, void* unit) {
    int rank = *(int*)((char*)unit + 0x252c);
    rank = rank < 1 ? 1 : rank > 5 ? 5 : rank;
    return ((Node*)node)->w[rank - 1];
}

static bool g_flightWrites = true;
static void Flight(void*, void* out) {
    if (!g_flightWrites) return;  // the real function has paths that leave the values alone
    float row[9] = {60, 160, 7, 750, 2400, 600, 700, 300, 300};
    memcpy(((FlightValues*)out)->v, row, sizeof row);
}

static void* g_lookupSlot = (void*)&Lookup;
static void* g_flightSlot = (void*)&Flight;
static int g_failures = 0;
static std::string g_iniPath;

static Unit MakeUnit(int rank, int difficulty, int bonus, int faction = 2) {
    static const float startMax[6] = {0, 0, 1, 2, 3, 4}, startMin[6] = {0, -3, -2, -1, 0, 1};
    Unit u;
    memset(&u, 0, sizeof u);
    u.bytes[0x3b2] = (char)faction;
    *(int*)(u.bytes + 0x252c) = rank;
    char* row = u.bytes + 0x1868;
    row[0x08] = (char)difficulty;
    *(int*)(row + 0x0c) = bonus;
    *(float*)(row + 0x1c) = startMax[difficulty];
    *(float*)(row + 0x20) = startMin[difficulty];
    *(int*)(row + 0x48) = 2;  // missiles in the air, planes
    *(int*)(row + 0x4c) = 2;
    return u;
}
static void* Plane(Unit& u) { return u.bytes + 0x25f0; }
static int Missiles(Unit& u) { return *(int*)(u.bytes + 0x18b0); }
static float BlackoutMax(Unit& u) { return *(float*)(u.bytes + 0x1884); }

static void Check(const char* what, float got, float want) {
    bool ok = fabsf(got - want) < 1e-3f * (1 + fabsf(want));
    if (!ok) ++g_failures;
    printf("%s %-60s got %9.3f want %9.3f\n", ok ? "ok  " : "FAIL", what, got, want);
}

static void WriteIni(const char* text) {
    FILE* f = nullptr;
    fopen_s(&f, g_iniPath.c_str(), "w");
    fputs(text, f);
    fclose(f);
    Sleep(700);  // the hook's worker re-reads the file a few times a second
}

static float IniValue(const char* key) {
    FILE* f = nullptr;
    char line[512], name[64];
    float value = -999, v;
    if (fopen_s(&f, g_iniPath.c_str(), "r") != 0 || !f) return value;
    while (fgets(line, sizeof line, f))
        if (line[0] != ';' && sscanf_s(line, " %63[A-Za-z0-9] = %f", name, (unsigned)sizeof name, &v) == 2 && !strcmp(name, key)) value = v;
    fclose(f);
    return value;
}

static bool IniHas(const char* text) {
    FILE* f = nullptr;
    char line[512];
    bool found = false;
    if (fopen_s(&f, g_iniPath.c_str(), "r") != 0 || !f) return false;
    while (fgets(line, sizeof line, f)) found = found || strstr(line, text);
    fclose(f);
    return found;
}

// ---------------------------------------------------------------- a fake engine for the script bias
// An object array, names, script objects and a manager with 24 script sets, laid out like the real ones.
struct FakeString { wchar_t* data; int num; int max; };
static std::string g_names[512];
static int g_nameCount = 0;
static int Name(const std::string& s) { g_names[g_nameCount] = s; return g_nameCount++; }
static void FakeNameToString(const void* name, FakeString* out) {
    static wchar_t buffers[64][80];
    static int next = 0;
    int index = *(const int*)name;
    wchar_t* buffer = buffers[next++ & 63];
    const std::string& s = g_names[index < 0 || index >= g_nameCount ? 0 : index];
    for (size_t i = 0; i <= s.size(); ++i) buffer[i] = (wchar_t)s[i];
    out->data = buffer;
    out->num = (int)s.size() + 1;
}
struct FakeObject { char pad[0x18]; int name, number; char rest[0x20]; };
static FakeObject g_scriptObjects[64];
static int g_scriptObjectCount = 0;
static FakeObject* ScriptObject(const std::string& name) {
    FakeObject* o = &g_scriptObjects[g_scriptObjectCount++];
    memset(o, 0, sizeof *o);
    o->name = Name(name);
    return o;
}
enum { kSets = 24, kRow = 0xf8 };
static const int kSwap[] = {2, 3, 4, 5, 9, 10, 11, 12, 13, 14};
static unsigned char g_rows[kSets * kRow], g_rowsStock[kSets * kRow];
static int g_setNameValues[kSets][2];
static FakeObject* g_a[15], * g_b[15], * g_c[15], * g_d[15], * g_root;
static void** Slot(unsigned char* rows, int set, int slot) { return (void**)(rows + set * kRow + 8 + slot * 16); }
static int CountSlots(int from, int to, FakeObject** want) {
    int n = 0;
    for (int i = from; i < to; ++i)
        for (int s : kSwap) n += *Slot(g_rows, i, s) == want[s];
    return n;
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    std::string dir = std::string(temp) + "acepilots_test";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string dll = dir + "\\acepilots_hook.dll", log = dir + "\\acepilots.log";
    g_iniPath = dir + "\\acepilots.ini";
    DeleteFileA(log.c_str());
    DeleteFileA(g_iniPath.c_str());
    if (!CopyFileA(argv[1], dll.c_str(), FALSE)) { printf("copy failed\n"); return 2; }
    HMODULE mod = LoadLibraryA(dll.c_str());
    if (!mod) { printf("load failed %lu\n", GetLastError()); return 2; }
    auto install = (int (*)(void**))GetProcAddress(mod, "acepilots_install_at");
    auto installFlight = (int (*)(void**))GetProcAddress(mod, "acepilots_install_flight_at");
    auto openMenu = (void (*)())GetProcAddress(mod, "acepilots_open_menu");
    auto menuWindow = (void* (*)())GetProcAddress(mod, "acepilots_menu_window");
    if (!install || !installFlight || !openMenu || !menuWindow) { printf("exports missing\n"); return 2; }
    bool look = argc > 2 && !strcmp(argv[2], "menu");
    if (look) WriteIni(argc > 3 ? argv[3] : "ForceRank=5\n");
    if (!install(&g_lookupSlot) || !installFlight(&g_flightSlot)) { printf("install failed\n"); return 2; }
    LookupFn lookup = (LookupFn)g_lookupSlot;
    FlightFn flight = (FlightFn)g_flightSlot;

    Node weak = {}, skilled = {}, flat = {}, top = {};
    float ww[5] = {50, 40, 30, 20, 10}, sw[5] = {50, 60, 70, 80, 90}, fw[5] = {7, 7, 7, 7, 7}, tw[5] = {0, 0, 10, 20, 30};
    memcpy(weak.w, ww, sizeof ww); memcpy(skilled.w, sw, sizeof sw); memcpy(flat.w, fw, sizeof fw); memcpy(top.w, tw, sizeof tw);
    Unit ace4 = MakeUnit(4, 5, 1), ace6 = MakeUnit(6, 5, 1), ace2 = MakeUnit(2, 5, 1), hard4 = MakeUnit(4, 4, 1);
    Unit normal3 = MakeUnit(3, 3, 0), unreadable4 = MakeUnit(4, 5, 0), ally4 = MakeUnit(4, 5, 1, 1);
    FlightValues fv = {};

    if (look) {  // a few manoeuvre choices so the live line has numbers, then show the menu
        Sleep(700);
        for (int i = 0; i < 60; ++i) { lookup(&weak, &ace4); lookup(&top, &ace4); flight(Plane(ace4), &fv); }
        openMenu();
        Sleep(look && argc > 4 ? atoi(argv[4]) : 6000);
        return 0;
    }

    Sleep(700);
    Check("no file: one is written with the default preset", IniValue("ForceRank"), 5);
    Check("no file: Preset label written", (float)IniHas("Preset=AllRank5"), 1);

    WriteIni("Mode=2\nStrength=3\nRank=5\n");
    Check("old layout Mode=2 Strength=3 becomes Boosted 3", IniValue("SkilledBoost"), 3);
    Check("old layout: weak divide follows", IniValue("WeakDivide"), 3);
    Check("old layout: rank left to the game", IniValue("ForceRank"), 0);
    Check("old layout: file relabelled", (float)IniHas("Preset=Boosted3"), 1);

    WriteIni("ForceRank=0\n");
    Check("stock: weak, rank 4 untouched", lookup(&weak, &ace4), 20);
    flight(Plane(ace4), &fv);
    Check("stock: pitch rate untouched", fv.v[0], 60);
    Check("stock: missiles in the air untouched", (float)Missiles(ace4), 2);

    WriteIni("ForceRank=0\nSkilledBoost=2\nTopAceBoost=2\nWeakDivide=2\n");
    Check("boosted 2: weak, rank 4 halved", lookup(&weak, &ace4), 10);
    Check("boosted 2: skilled, rank 4 doubled", lookup(&skilled, &ace4), 160);
    Check("boosted 2: top-ace option doubled, no more", lookup(&top, &ace6), 60);
    Check("boosted 2: flat option untouched", lookup(&flat, &ace4), 7);
    Check("boosted 2: rank 2 weak halved", lookup(&weak, &ace2), 20);
    Check("boosted 2: unit set up on Normal left alone", lookup(&weak, &normal3), 30);
    Check("boosted 2: unit set up on Hard boosted", lookup(&weak, &hard4), 10);
    Check("boosted 2: unreadable difficulty, boosted anyway", lookup(&weak, &unreadable4), 10);
    Check("boosted 2: ally left alone", lookup(&weak, &ally4), 20);
    Check("label is Boosted2", (float)IniHas("Preset=Boosted2") + (float)(IniValue("SkilledBoost") == 2), 1);  // file not rewritten on read
    WriteIni("SkilledBoost=2\nTopAceBoost=2\nWeakDivide=2\nFromDifficulty=5\nForceRank=0\n");
    Check("FromDifficulty=5: Hard left alone", lookup(&weak, &hard4), 20);
    Check("FromDifficulty=5: Ace still boosted", lookup(&weak, &ace4), 10);
    WriteIni("SkilledBoost=2\nTopAceBoost=2\nWeakDivide=2\nIncludeAllies=1\nForceRank=0\n");
    Check("IncludeAllies=1: ally boosted", lookup(&weak, &ally4), 10);

    WriteIni("ForceRank=5\nSkilledBoost=2\nTopAceBoost=20\nWeakDivide=1000\nRank5Odds=1\nAgility=1.5\nMissiles=4\nBlackoutBonus=5\n"
             "IgnoreAttackQueue=1\nForceFlares=1\nForceEvade=1\nForceTaunts=1\nFromDifficulty=1\n");
    Check("max: weak option all but removed (rank 5 odds)", lookup(&weak, &ace2), 0.01f);
    Check("max: skilled option, rank 5 weight x2 minimum", lookup(&skilled, &ace2), 180);
    Check("max: top-ace option x20", lookup(&top, &ace2), 600);
    Check("max: applies on Normal with FromDifficulty=1", lookup(&weak, &normal3), 0.01f);
    flight(Plane(ace2), &fv);
    Check("max: pitch rate x1.5", fv.v[0], 90);
    Check("max: roll rate x1.5", fv.v[1], 240);
    Check("max: top speed untouched", fv.v[4], 2400);
    Check("max: acceleration x1.5", fv.v[7], 450);
    Check("max: missiles in the air raised", (float)Missiles(ace2), 4);
    Check("max: blackout delay on Ace 4 + 5", BlackoutMax(ace2), 9);
    flight(Plane(hard4), &fv);
    Check("max: blackout delay on Hard 3 + 5", BlackoutMax(hard4), 8);
    flight(Plane(ace2), &fv);
    Check("max: rewritten values scaled again, not compounded", fv.v[0], 90);
    g_flightWrites = false;
    flight(Plane(ace2), &fv);
    flight(Plane(ace2), &fv);
    Check("max: values the game left alone are not scaled twice", fv.v[0], 90);
    g_flightWrites = true;
    flight(Plane(ally4), &fv);
    Check("max: ally's pitch rate untouched", fv.v[0], 60);
    Check("max: ally's missiles untouched", (float)Missiles(ally4), 2);
    flight(Plane(unreadable4), &fv);
    Check("max: unreadable unit's memory not written", (float)Missiles(unreadable4), 2);
    Check("max: unreadable unit still gets agility", fv.v[0], 90);

    WriteIni("ForceRank=0\nAgility=2.5\nMissiles=6\nBlackoutBonus=0\n");
    flight(Plane(ace2), &fv);
    Check("custom: agility 2.5", fv.v[0], 150);
    Check("custom: missiles 6", (float)Missiles(ace2), 6);
    Check("custom: blackout back to the game's 4", BlackoutMax(ace2), 4);
    Check("custom: odds untouched", lookup(&weak, &ace4), 20);
    WriteIni("ForceRank=0\n");
    flight(Plane(ace2), &fv);
    Check("back to stock: missiles 2 again", (float)Missiles(ace2), 2);
    Check("back to stock: pitch rate 60", fv.v[0], 60);
    WriteIni("ForceRank=9\nAgility=50\nWeakDivide=0\nMissiles=1\n");
    flight(Plane(ace2), &fv);
    Check("out-of-range values are clamped: agility 3", fv.v[0], 180);
    Check("out-of-range values are clamped: missiles 2", (float)Missiles(ace2), 2);

    // ---- script bias, against the fake engine
    auto testEngine = (void (*)(void*, void*))GetProcAddress(mod, "acepilots_test_engine");
    Name("None");
    g_root = ScriptObject("LA_AIPlane");
    for (int s : kSwap) {
        g_a[s] = ScriptObject("LA_Test_" + std::to_string(s) + "_A");
        g_b[s] = ScriptObject("LA_Test_" + std::to_string(s) + "_B");
        g_c[s] = ScriptObject("LA_Test_" + std::to_string(s) + "_C");
        g_d[s] = ScriptObject("LA_Test_" + std::to_string(s) + "_D");
    }
    // sets 0-10 use the A scripts, 11-21 the B scripts, 22 the C scripts (nobody else has them), 23 is a bomber set
    for (int i = 0; i < kSets; ++i) {
        g_setNameValues[i][0] = Name(i == 23 ? "NLID_1000" : "NLID_00" + std::to_string(10 + i) + "_CP");
        g_setNameValues[i][1] = 0;
        *Slot(g_rows, i, 0) = g_root;
        for (int s : kSwap) *Slot(g_rows, i, s) = i < 11 ? g_a[s] : i < 22 ? g_b[s] : i == 22 ? g_c[s] : g_d[s];
    }
    memcpy(g_rowsStock, g_rows, sizeof g_rows);
    static char manager[0x2f00];
    struct Raw { void* data; int num, max; };
    *(Raw*)(manager + 0x2e78) = {g_rows, kSets, kSets};
    *(Raw*)(manager + 0x2e88) = {g_setNameValues, kSets, kSets};
    // the object array: a null entry, an object at the very end of a page (reading past it faults), a plain
    // object, then the manager
    char* page = (char*)VirtualAlloc(nullptr, 0x2000, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    static char plain[0x4000];
    static char items[4 * 24];
    *(void**)(items + 1 * 24) = page + 0x1000 - 0x40;
    *(void**)(items + 2 * 24) = plain;
    *(void**)(items + 3 * 24) = manager;
    static void* chunks[1] = {items};
    static char objectArray[0x40];
    *(void***)(objectArray + 0x10) = chunks;
    *(int*)(objectArray + 0x24) = 4;
    testEngine(objectArray, (void*)&FakeNameToString);
    {
        std::string scores = dir + "\\acepilots_scripts.ini";
        FILE* sf = nullptr;
        fopen_s(&sf, scores.c_str(), "w");
        for (int s : kSwap) fprintf(sf, "LA_Test_%d_A = 30\nLA_Test_%d_B = 60\nLA_Test_%d_C = 90 unused\nLA_Test_%d_D = 0\n", s, s, s, s);
        fclose(sf);
    }
    Unit flyer = MakeUnit(4, 5, 1);
    memcpy(flyer.bytes + 0x13d0, g_setNameValues[0], 8);
    auto tickScripts = [&]() { Sleep(1100); flight(Plane(flyer), &fv); };

    WriteIni("ForceRank=0\n");
    tickScripts();
    Check("bias 0: rows untouched", (float)memcmp(g_rows, g_rowsStock, sizeof g_rows), 0);
    WriteIni("ForceRank=0\nScriptBias=100\n");
    tickScripts();
    Check("bias 100: every slot of the A sets now has the B script", (float)CountSlots(0, 11, g_b), 110);
    Check("bias 100: B sets unchanged without unused scripts", (float)CountSlots(11, 22, g_b), 110);
    Check("bias 100: bomber set untouched", (float)CountSlots(23, 24, g_d), 10);
    Check("bias 100: root slot untouched", (float)(*Slot(g_rows, 0, 0) == g_root), 1);
    Check("bias 100: first 8 bytes of a row untouched", (float)memcmp(g_rows, g_rowsStock, 8), 0);
    WriteIni("ForceRank=0\nScriptBias=100\nScriptUnused=1\n");
    tickScripts();
    Check("unused on: B sets move up to the C script", (float)CountSlots(11, 22, g_c), 110);
    Check("unused on: A sets hold only B or C", (float)(CountSlots(0, 11, g_b) + CountSlots(0, 11, g_c)), 110);
    Check("unused on: A sets get a mix, not one script", (float)(CountSlots(0, 11, g_b) > 0 && CountSlots(0, 11, g_c) > 0), 1);
    WriteIni("ForceRank=0\nScriptBias=50\n");
    tickScripts();
    int half = CountSlots(0, 11, g_b);
    Check("bias 50: roughly half the A slots swapped", (float)(half > 30 && half < 80), 1);
    Check("bias 50: the rest still stock", (float)(half + CountSlots(0, 11, g_a)), 110);
    // a plane that spawned now carries a mixed row; one that spawned earlier carries the stock row
    memcpy(flyer.bytes + 0x16a8, g_rows, kRow);
    for (int i = 0; i < 64; ++i) flight(Plane(flyer), &fv);
    memcpy(flyer.bytes + 0x16a8, g_rowsStock, kRow);
    for (int i = 0; i < 64; ++i) flight(Plane(flyer), &fv);
    {
        std::string scores = dir + "\\acepilots_scripts.ini";
        FILE* sf = nullptr;
        fopen_s(&sf, scores.c_str(), "w");
        for (int s : kSwap) fprintf(sf, "LA_Test_%d_A = 30\nLA_Test_%d_B = 10\nLA_Test_%d_C = 90 unused\nLA_Test_%d_D = 0\n", s, s, s, s);
        fclose(sf);
    }
    WriteIni("ForceRank=0\nScriptBias=100\n");
    tickScripts();
    Check("edited scores: B now ranks below A, so A sets stay stock", (float)CountSlots(0, 11, g_a), 110);
    Check("edited scores: B sets move to A", (float)CountSlots(11, 22, g_a), 110);
    // the game rebuilds its rows from its table: the mod's mix is gone, the stock is back, and it is mixed again
    memcpy(g_rows, g_rowsStock, sizeof g_rows);
    tickScripts();
    tickScripts();
    Check("rebuilt rows: adopted again and mixed", (float)CountSlots(0, 22, g_a), 220);
    WriteIni("ForceRank=0\n");
    tickScripts();
    Check("bias back to 0: stock rows restored exactly", (float)memcmp(g_rows, g_rowsStock, sizeof g_rows), 0);    *(int*)(manager + 0x2e78 + 8) = 5;  // the array shrinks: no longer what was adopted
    WriteIni("ForceRank=0\nScriptBias=100\n");
    tickScripts();
    Check("a manager that changed shape is dropped, nothing written", (float)memcmp(g_rows, g_rowsStock, sizeof g_rows), 0);
    *(int*)(manager + 0x2e78 + 8) = kSets;
    WriteIni("ForceRank=0\n");
    tickScripts();
    // The menu, driven by key messages. The host is not the foreground process, so the window does not take focus.
    WriteIni("ForceRank=0\n");
    openMenu();
    Sleep(400);
    HWND menu = (HWND)menuWindow();
    Check("menu opens", menu != nullptr, 1);
    auto key = [&](WPARAM k, int times = 1) { for (int i = 0; i < times; ++i) SendMessageW(menu, WM_KEYDOWN, k, 0); };
    key('6');
    Sleep(700);
    Check("menu key 6 selects MAX: label", (float)IniHas("Preset=Max"), 1);
    Check("menu key 6 selects MAX: agility", IniValue("Agility"), 1.5f);
    Check("MAX takes effect at once", lookup(&weak, &ace4), 0.01f);
    key(VK_DOWN, 5);   // presets -> pilot rank .. -> agility (6th control)
    key(VK_DOWN);
    key(VK_RIGHT, 5);  // agility 1.5 -> 2.0
    Sleep(700);
    Check("arrow keys move a slider: agility 2.0", IniValue("Agility"), 2.0f);
    Check("a changed slider makes the preset Custom", (float)IniHas("Preset=Custom"), 1);
    flight(Plane(ace4), &fv);
    Check("slider change applies to planes at once", fv.v[0], 120);
    key(VK_DOWN, 3);   // -> no attack queue
    key(VK_RETURN);
    Sleep(700);
    Check("enter flips a toggle", IniValue("IgnoreAttackQueue"), 0);
    key(VK_F8);
    Sleep(700);
    Check("F8 from Custom goes to the first preset", (float)IniHas("Preset=Off"), 1);
    key('3');
    Sleep(700);
    Check("key 3 selects Boosted 2", IniValue("WeakDivide"), 2);
    RECT client;
    GetClientRect(menu, &client);
    SendMessageW(menu, WM_LBUTTONDOWN, 0, MAKELPARAM(client.right * 22 / 100, 90 * client.bottom / 640));  // second chip
    SendMessageW(menu, WM_LBUTTONUP, 0, 0);
    Sleep(700);
    Check("clicking the second chip selects All rank 5", IniValue("ForceRank"), 5);
    key(VK_ESCAPE);
    Sleep(100);
    Check("escape closes the menu", menuWindow() == nullptr, 1);

    // Enough reads for the 15-second report: ranks 2..5 across weak and top-ace options.
    WriteIni("ForceRank=0\nSkilledBoost=2\nTopAceBoost=2\nWeakDivide=2\n");
    Unit ace3 = MakeUnit(3, 5, 1), ace5 = MakeUnit(5, 5, 1);
    Unit* ranks[4] = {&ace2, &ace3, &ace4, &ace5};
    for (int i = 0; i < 400; ++i)
        for (Unit* u : ranks) { lookup(&weak, u); lookup(&top, u); }
    Sleep(15500);
    lookup(&weak, &ace4);

    printf("---- log ----\n");
    FILE* f = nullptr;
    char line[1024];
    bool distinct = false;
    if (fopen_s(&f, log.c_str(), "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            if (!strstr(line, "settings (")) fputs(line, stdout);
            if (strstr(line, "ranks distinct") && strstr(line, "yes")) distinct = true;
        }
        fclose(f);
    }
    if (!distinct) { ++g_failures; printf("FAIL no 'ranks distinct: yes' line in the log\n"); }
    printf("%s\n", g_failures ? "FAILED" : "ALL OK");
    return g_failures ? 1 : 0;
}
