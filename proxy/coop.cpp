// dinput8.dll proxy for Fable Anniversary: the co-op mod's entry point.
// - forwards DirectInput8Create to the system DLL
// - per-profile "My Documents" (saves never touch the player's real profile or Steam Cloud)
// - lets several copies run on one PC (single-instance mutex renamed per process)
// - coopbridge test server (loopback only) for reading game memory from scripts
// - UDP link to a second game: shows the other player as a puppet (FABLE_COOP_LISTEN / FABLE_COOP_PEER)
#define WIN32_LEAN_AND_MEAN  // coopbridge pulls in winsock2.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <share.h>
#include <math.h>
#include <stdio.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <coopbridge/server.h>

using coopbridge::json::Value;

static FILE *g_log;
static wchar_t g_profileDir[MAX_PATH];  // <game root>\CoopProfiles\<profile>
static BYTE *g_exeBase;
static const ULONG_PTR kGhidraBase = 0x00400000;  // addresses in our notes use the exe's preferred base

void coop_log(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

// Replaces one import of the main exe; returns the original pointer, or NULL if not imported.
static void *patch_iat(const char *dll, const char *fn, void *hook)
{
    BYTE *base = g_exeBase;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn)) continue;
            void *orig = (void *)iat->u1.Function;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return orig;
        }
    }
    return NULL;
}

// The game quits when "UnrealEngine3_8" already exists; a per-process name lets two copies run on one PC.
static decltype(&CreateMutexW) real_CreateMutexW;
static HANDLE WINAPI hk_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR name)
{
    wchar_t unique[64];
    if (name && !wcsncmp(name, L"UnrealEngine3_", 14)) {
        swprintf_s(unique, L"%ls_%lu", name, GetCurrentProcessId());
        coop_log("CreateMutexW(\"%ls\") renamed to \"%ls\"", name, unique);
        name = unique;
    }
    return real_CreateMutexW(sa, own, name);
}

// Saves live in <My Documents>\My Games\FableHD; point My Documents at the profile folder.
static decltype(&SHGetFolderPathW) real_SHGetFolderPathW;
static HRESULT WINAPI hk_SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path)
{
    if ((csidl & 0xff) == CSIDL_PERSONAL) {
        wcscpy_s(path, MAX_PATH, g_profileDir);
        return S_OK;
    }
    return real_SHGetFolderPathW(hwnd, csidl, token, flags, path);
}

static void init_profile()
{
    wchar_t name[64] = L"p1";
    GetEnvironmentVariableW(L"FABLE_COOP_PROFILE", name, 64);
    GetModuleFileNameW(NULL, g_profileDir, MAX_PATH);
    for (int up = 0; up < 3; up++) *wcsrchr(g_profileDir, L'\\') = 0;  // ...\Binaries\Win32\exe -> game root
    wcscat_s(g_profileDir, L"\\CoopProfiles");
    CreateDirectoryW(g_profileDir, NULL);
    wcscat_s(g_profileDir, L"\\");
    wcscat_s(g_profileDir, name);
    CreateDirectoryW(g_profileDir, NULL);
}

// ---- game thread: jobs queued by the bridge run after CMainGameComponent's per-frame update ----

struct Job {
    std::function<Value()> fn;
    Value result;
    std::string error;
    bool done = false;
};
static std::mutex g_jobsLock;
static std::condition_variable g_jobsDone;
static std::deque<std::shared_ptr<Job>> g_jobs;
static volatile LONG g_frames;
static std::map<ULONG_PTR, std::vector<BYTE>> g_pins;  // bytes rewritten every frame (guarded by g_jobsLock)

static bool safe_copy(void *dst, const void *src, size_t n)
{
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Player 2 puppet: the bridge pushes position samples as they arrive; every frame the target is
// where the samples were `delay` seconds ago, interpolated between the two around that time
// (snapshot interpolation). The puppet walks or runs there through the game's own scripted move-to
// order (so its legs match its speed); only when it is far off is it teleported through its
// CTCPhysicsNavigator's teleport slot, which applies on the next update without paths or collisions.
// CTCPhysicsBase layout: position x,y,z at +0x0c, facing x,y at +0x5c.
struct PuppetSample {
    double t;      // local receive time, seconds
    float v[5];    // x, y, z, facing x, facing y
};
struct Puppet {
    float *phys = nullptr;  // CTCPhysicsNavigator of the puppet, null when off
    DWORD actor = 0;        // its CScriptGameResourceObjectScriptedThing (fable.spawn scripted)
    double delay = 0.1;
    std::deque<PuppetSample> samples;
};
static Puppet g_puppetCtl;
// CTCPhysicsNavigator vftable slot 0x90: queue a teleport; snap=false applies it as is on the next update
typedef void(__thiscall *NavTeleportFn)(void *nav, const float *pos, bool snap);

static double now_s()
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / f.QuadPart;
}

// ponytail: holds the last sample when samples run out; extrapolate with velocity if late packets show
static void sample_at(const std::deque<PuppetSample> &s, double t, float out[5])
{
    size_t i = 1;
    while (i < s.size() && s[i].t < t) i++;
    if (i >= s.size() || t <= s[0].t) { memcpy(out, (t <= s[0].t ? s.front() : s.back()).v, sizeof(float) * 5); return; }
    const PuppetSample &a = s[i - 1], &b = s[i];
    float k = (float)((t - a.t) / (b.t - a.t));
    for (int j = 0; j < 5; j++) out[j] = a.v[j] + (b.v[j] - a.v[j]) * k;
}

static bool safe_call(void *fn, DWORD self, const DWORD *args, int n, DWORD *ret);
static void *game_va(ULONG_PTR ghidraVa);

static bool place_puppet(float *phys, const float v[5])
{
    __try {
        (*(NavTeleportFn **)phys)[0x90 / 4](phys, v, false);
        float len = sqrtf(v[3] * v[3] + v[4] * v[4]);
        if (len > 1e-4f) { phys[0x5c / 4] = v[3] / len; phys[0x5c / 4 + 1] = v[4] / len; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void net_tick(Puppet &pp);
static char g_peerAnimName[48];  // the remote hero's main anim, "" unknown

static void(__fastcall *real_MainUpdate)(void *self, void *edx);
static void __fastcall hk_MainUpdate(void *self, void *edx)
{
    real_MainUpdate(self, edx);
    InterlockedIncrement(&g_frames);
    std::unique_lock<std::mutex> lk(g_jobsLock);
    Puppet &pp = g_puppetCtl;
    net_tick(pp);
    // The puppet is placed on the remote position every frame (navigator teleport slot) and plays the remote
    // hero's anims, so its own AI must not walk it: its action queue (scripted actor slot 0x28) is emptied.
    // Walk orders (until 2026-11-02) lagged on sharp turns: the puppet finished its old path, stopped, then
    // waited for the next order (owner). A strike moves the hero 2.4 m in 0.8 s; orders made it glide.
    if (pp.phys && pp.actor && !pp.samples.empty()) {
        double t = now_s() - pp.delay;
        while (pp.samples.size() > 2 && pp.samples[1].t < t) pp.samples.pop_front();
        float v[5];
        sample_at(pp.samples, t, v);
        DWORD ret;
        if (!safe_call(game_va(0x1d0fee0), pp.actor, NULL, 0, &ret) || !place_puppet(pp.phys, v)) {
            coop_log("puppet %p / actor %08lx faulted, puppet off", pp.phys, (unsigned long)pp.actor);
            pp.phys = nullptr;
        }
    }
    for (auto it = g_pins.begin(); it != g_pins.end();)
        if (safe_copy((void *)it->first, it->second.data(), it->second.size())) ++it;
        else { coop_log("pin at %08lx faulted, dropped", (unsigned long)it->first); it = g_pins.erase(it); }
    while (!g_jobs.empty()) {
        std::shared_ptr<Job> j = g_jobs.front();
        g_jobs.pop_front();
        lk.unlock();
        try { j->result = j->fn(); }
        catch (const std::exception &e) { j->error = e.what(); }
        lk.lock();
        j->done = true;
        g_jobsDone.notify_all();
    }
}

// Runs fn on the game thread and waits for it (bridge thread only).
static Value on_game_thread(std::function<Value()> fn)
{
    auto j = std::make_shared<Job>();
    j->fn = std::move(fn);
    std::unique_lock<std::mutex> lk(g_jobsLock);
    g_jobs.push_back(j);
    if (!g_jobsDone.wait_for(lk, std::chrono::seconds(5), [&] { return j->done; }))
        throw coopbridge::Error(-32012, "game thread did not run the job (paused or loading?)");
    if (!j->error.empty()) throw coopbridge::Error(-32013, j->error);
    return j->result;
}

static void *game_va(ULONG_PTR ghidraVa) { return g_exeBase + (ghidraVa - kGhidraBase); }

static void hook_vftable_slot(ULONG_PTR vftable, int slot, void *hook, void **orig)
{
    void **p = (void **)game_va(vftable) + slot;
    DWORD old;
    VirtualProtect(p, sizeof(void *), PAGE_READWRITE, &old);
    *orig = *p;
    *p = hook;
    VirtualProtect(p, sizeof(void *), old, &old);
}

// Overwrites game code if it still holds `expect` (another build of the exe: leave it alone).
static bool patch_code(ULONG_PTR va, const char *expect, const char *with, size_t n)
{
    BYTE *p = (BYTE *)game_va(va);
    DWORD old;
    if (memcmp(p, expect, n) || !VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, with, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

// ---- UE3 names: GNames = TArray<FNameEntry *>, ANSI text at entry+0x10 ----

static const ULONG_PTR kGNames = 0x35071e8;
static const char *ue_name(int i)
{
    BYTE **data = *(BYTE ***)game_va(kGNames);
    int num = ((int *)game_va(kGNames))[1];
    return i >= 0 && i < num && data[i] ? (const char *)data[i] + 0x10 : "";
}
static int ue_find(const char *s)
{
    int num = ((int *)game_va(kGNames))[1];
    for (int i = 0; i < num; i++)
        if (!strcmp(ue_name(i), s)) return i;
    return -1;
}

// The puppet (CREATURE_YOUNG_HERO, an NPC) gets villager-man anims; they also pose the face bones,
// which on the hero's child mesh gives wide eyes and an open mouth (owner, 2026-11-01). The puppet's
// anim nodes play the remote hero's main anim instead; until one arrives, villager anims become the
// hero's idle/walk/run (owner: face normal, 2026-11-02).
// UE3: SkeletalMeshComponent +0x1f4 = anim node list (TArray); AnimNode +0x5c SkelComponent, +0x78 NodeTotalWeight;
// AnimNodeSequence +0xf0 AnimSeqName (FName index, number). SetAnim(FName) is virtual only.
static const ULONG_PTR kAnimNodeSequenceVft = 0x2b43090, kSetAnimSlot = 0x19c / 4, kSetAnim = 0x804ea0;
typedef void(__fastcall *SetAnimFn)(BYTE *node, void *edx, int name, int number);
static SetAnimFn real_SetAnim;
static BYTE *g_puppetSkel;  // the puppet's SkeletalMeshComponent, null when no puppet
static int g_peerAnim = -1; // GNames index of the remote hero's main anim, -1 unknown
static int g_peerAnimNum;   // its FName number: variants share a name (ANIM_HERO_IDLE 0, 3..10; combat idle is 2)
static void __fastcall hk_SetAnim(BYTE *node, void *edx, int name, int number)
{
    static int idle = -1, walk = -1, run = -1;
    static DWORD lookedAt;
    if (g_puppetSkel && *(BYTE **)(node + 0x5c) == g_puppetSkel) {
        if (g_peerAnim >= 0) name = g_peerAnim, number = g_peerAnimNum;
        else {
            if ((idle < 0 || walk < 0 || run < 0) && GetTickCount() - lookedAt > 1000) {  // names load with their packages
                lookedAt = GetTickCount();
                idle = ue_find("ANIM_HERO_IDLE"), walk = ue_find("ANIM_HERO_WALK"), run = ue_find("ANIM_HERO_RUN");
            }
            const char *s = ue_name(name);
            if (idle >= 0 && walk >= 0 && run >= 0 && (!strncmp(s, "ANIM_VILLAGER", 13) || !strncmp(s, "ANIM_BIPED", 10)))
                name = strstr(s, "RUN") ? run : strstr(s, "WALK") ? walk : idle, number = 0;
        }
    }
    real_SetAnim(node, edx, name, number);
}

// The sequence nodes of a SkeletalMeshComponent, at most `max`.
static int anim_sequences(BYTE *skel, BYTE **out, int max)
{
    BYTE **list;
    int num, n = 0;
    if (!skel || !safe_copy(&list, skel + 0x1f4, 4) || !safe_copy(&num, skel + 0x1f8, 4) || !list) return 0;
    for (int i = 0; i < num && n < max; i++) {
        BYTE *node;
        void *vft;
        if (safe_copy(&node, list + i, 4) && node && safe_copy(&vft, node, 4) && vft == game_va(kAnimNodeSequenceVft)) out[n++] = node;
    }
    return n;
}

// Name and FName number of the sequence weighing most in the final pose ("" if none).
// ponytail: one anim; an upper-body anim over legs walking sends the walk. Send per-branch anims if attacks show it.
static void main_anim(BYTE *skel, char *out, size_t size, int *number)
{
    BYTE *seq[64];
    int n = anim_sequences(skel, seq, 64), best = -1, name[2];
    float bestW = 0;
    *number = 0;
    for (int i = 0; i < n; i++) {
        float w;
        if (safe_copy(&w, seq[i] + 0x78, 4) && safe_copy(name, seq[i] + 0xf0, 8) && name[0] > 0 && w > bestW)
            bestW = w, best = name[0], *number = name[1];
    }
    strncpy_s(out, size, best > 0 ? ue_name(best) : "", _TRUNCATE);
}

// ---- Fable engine calls (addresses at Ghidra base, see NOTES.md) ----

typedef void(__thiscall *CharStringCtor)(void *self, const char *s, int len);
typedef void(__thiscall *CharStringDtor)(void *self);
// CGameScriptInterface vftable slot 0x184: CreateCreature(out CScriptThing, def, position, script name, flag)
typedef void *(__thiscall *CreateCreatureFn)(void *gsi, void *out, void *def, const float *pos, void *script, bool flag);

static std::string hex32(ULONG_PTR v);

static const ULONG_PTR kGsiGlobal = 0x35bbc34;  // CGameScriptInterface *, used by ~2500 code sites
static BYTE g_puppet[12];       // CScriptThing returned for the last spawned creature, kept so it stays referenced

// Any game function: pushes args right to left, ecx = self, restores esp afterwards so
// thiscall, stdcall and cdecl all work. Returns eax.
static DWORD raw_call(void *fn, DWORD self, const DWORD *args, int n)
{
    DWORD result, saved;
    __asm {
        mov saved, esp
        mov ecx, n
        mov esi, args
    next_arg:
        test ecx, ecx
        jz call_it
        dec ecx
        push dword ptr [esi + ecx * 4]
        jmp next_arg
    call_it:
        mov ecx, self
        call fn
        mov result, eax
        mov esp, saved
    }
    return result;
}

static bool safe_call(void *fn, DWORD self, const DWORD *args, int n, DWORD *ret)
{
    __try { *ret = raw_call(fn, self, args, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH can't share a function with C++ unwinding; keep the raw call alone.
static bool call_create_creature(void *gsi, void *out, void *def, const float *pos, void *script, bool flag)
{
    __try {
        CreateCreatureFn fn = (*(CreateCreatureFn **)gsi)[0x184 / 4];
        fn(gsi, out, def, pos, script, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// GSI slot 0x30: take scripted control of a creature, priority 4 as the trader escort quest does; returns
// its CScriptGameResourceObjectScriptedThing (stored at wrapper +8), or 0 if another script holds it.
static DWORD begin_scripting(void *gsi, void *scriptThing)
{
    DWORD *wrapper = new DWORD[8]();  // ponytail: one leaked 32-byte wrapper per scripted spawn
    DWORD args[3] = {(DWORD)scriptThing, (DWORD)wrapper, 4}, ret;
    if (!safe_call(game_va(0x1c9ec00), (DWORD)gsi, args, 3, &ret)) throw std::runtime_error("begin scripting crashed");
    return (ret & 0xff) ? wrapper[2] : 0;
}

static Value spawn_creature(const std::string &def, const float pos[3], bool scripted)
{
    void *gsi = *(void **)game_va(kGsiGlobal);
    if (!gsi) throw std::runtime_error("no script interface yet (not in game?)");
    auto ctor = (CharStringCtor)game_va(0x16b7800);
    auto dtor = (CharStringDtor)game_va(0x16b75d0);
    void *defStr = 0, *scriptStr = 0;  // CCharString is one pointer
    ctor(&defStr, def.c_str(), -1);
    ctor(&scriptStr, "", -1);
    BYTE out[12] = {};
    bool ok = call_create_creature(gsi,out, &defStr, pos, &scriptStr, false);
    dtor(&defStr);
    dtor(&scriptStr);
    if (!ok) throw std::runtime_error("CreateCreature crashed");
    memcpy(g_puppet, out, sizeof out);
    Value r = Value::object();
    r.set("thing_vftable", hex32(((ULONG *)out)[0]));
    r.set("thing_1", hex32(((ULONG *)out)[1]));
    r.set("thing_2", hex32(((ULONG *)out)[2]));
    if (scripted) r.set("actor", hex32(begin_scripting(gsi, g_puppet)));
    return r;
}

// ---- network: local UDP between two game instances (Steam relay later) ----
// FABLE_COOP_LISTEN=<port> and FABLE_COOP_PEER=<host:port> switch it on. Every 50 ms the local hero's
// position goes to the peer; received positions drive the puppet, which is spawned (or respawned after
// the game removed it, e.g. on a region load) when positions arrive and the local hero exists.
// Free co-op: each packet carries the sender's region (current map index, CWorldMap +0xbc; the 73-map
// table is the same in every copy, one region def per map). The puppet exists only while both are in
// the same region and the peer is talking; otherwise it is removed (GSI slot 0x1c8, as the cutscene
// command RemoveAllThings does). Parking it under the map instead left it asleep: its teleports never applied.

struct NetPos {
    DWORD magic;  // kNetMagic
    DWORD seq;
    float v[5];   // x, y, z, facing x, facing y
    int region;   // sender's map index, -1 unknown
    float day;    // sender's time of day, 0..1 (hours / 24), -1 unknown
    char anim[48];  // sender's hero main anim (UE3 name, names differ in index between copies), "" unknown
    int animNum;    // its FName number
};
static const DWORD kNetMagic = 0x354f4346;  // "FCO5"
// Time of day: CGameTimeManager at [[[0x362b3cc] + 4] + 0x1c], +8 = time of day as a fraction (+0xc copy);
// its SetTimeOfDay console command (0x1cb8470) writes both. The host's time wins (FABLE_COOP_HOST=1).
// ponytail: time of day only; day count (CGameTimeManager "DayCount") once quests depend on days.
static bool g_host;
static const float kDaySlack = 0.001f;  // ~1.4 game minutes apart before the guest is reset to the host

static float *time_of_day()
{
    DWORD p = 0;
    if (!safe_copy(&p, game_va(0x362b3cc), 4) || !p || !safe_copy(&p, (void *)(p + 4), 4) || !p ||
        !safe_copy(&p, (void *)(p + 0x1c), 4)) return nullptr;
    return p ? (float *)(p + 8) : nullptr;
}
static const ULONG_PTR kWorldMapGlobal = 0x3630374;  // CWorldMap *
static int g_peerRegion = -1;
static double g_lastRecv;
static BYTE g_puppetScript[12];  // the puppet's CScriptThing, kept for removing it
static const double kPeerSilent = 5.0;  // s without packets before the puppet is removed

static int region_now()
{
    DWORD wm = 0;
    int idx = -1;
    safe_copy(&wm, game_va(kWorldMapGlobal), 4);
    if (wm) safe_copy(&idx, (void *)(wm + 0xbc), 4);
    return idx;
}
// Looks like the child hero (owner, 2026-11-01). A second CREATURE_HERO_CHILD crashed the game.
// ponytail: child look only; pick an adult look-alike (CREATURE_HERO_TRAINING? untested) once heroes grow up
static const char *kPuppetDef = "CREATURE_YOUNG_HERO";
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_peer;
static double g_lastSend;
static DWORD g_seq;

// The CThing behind a handle object (CScriptThing's CGameScriptThing +0xc, scripted actor +0x14), or 0.
static DWORD thing_of(DWORD handle)
{
    DWORD thing;
    return safe_call(game_va(0x16e6ea0), handle, NULL, 0, &thing) ? thing : 0;
}

// Local hero: GSI slot 0x130 (fastcall) -> CScriptThing, +4 -> CGameScriptThing, +0xc handle -> CThing. 0 outside the game.
static DWORD hero_thing()
{
    DWORD gsi = *(DWORD *)game_va(kGsiGlobal), st, sgt;
    if (!gsi || !safe_call(game_va(0x1c90550), gsi, NULL, 0, &st) || !st || !safe_copy(&sgt, (void *)(st + 4), 4)) return 0;
    return sgt ? thing_of(sgt + 0xc) : 0;
}

// A creature's SkeletalMeshComponent: CThing +0xa4 IActor, +8 UE3 pawn, +0x384 mesh component. Null if missing.
static BYTE *skel_of(DWORD thing)
{
    DWORD actor, pawn;
    BYTE *skel = NULL;
    if (thing && safe_copy(&actor, (void *)(thing + 0xa4), 4) && actor && safe_copy(&pawn, (void *)(actor + 8), 4) && pawn)
        safe_copy(&skel, (void *)(pawn + 0x384), 4);
    return skel;
}

static void net_init()
{
    char listen[16] = "", peer[64] = "";
    if (!GetEnvironmentVariableA("FABLE_COOP_LISTEN", listen, sizeof listen) ||
        !GetEnvironmentVariableA("FABLE_COOP_PEER", peer, sizeof peer)) return;
    char *colon = strrchr(peer, ':');
    WSADATA wsa;
    if (!colon || WSAStartup(MAKEWORD(2, 2), &wsa)) { coop_log("net: bad FABLE_COOP_PEER %s", peer); return; }
    *colon = 0;
    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons((u_short)atoi(colon + 1));
    sockaddr_in me = {AF_INET, htons((u_short)atoi(listen))};
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nonblocking = 1;
    if (inet_pton(AF_INET, peer, &g_peer.sin_addr) != 1 || s == INVALID_SOCKET ||
        bind(s, (sockaddr *)&me, sizeof me) || ioctlsocket(s, FIONBIO, &nonblocking)) {
        coop_log("net: setup failed (peer %s, listen %s, wsa %d)", peer, listen, WSAGetLastError());
        if (s != INVALID_SOCKET) closesocket(s);
        return;
    }
    g_sock = s;
    char host[4] = "";
    g_host = GetEnvironmentVariableA("FABLE_COOP_HOST", host, sizeof host) && host[0] == '1';
    coop_log("net: UDP listening on %s, peer %s:%s, %s", listen, peer, colon + 1, g_host ? "host" : "guest");
}

static void net_tick(Puppet &pp)
{
    if (g_sock == INVALID_SOCKET) return;
    double now = now_s();
    DWORD heroThing = hero_thing();
    float *hero = NULL;
    if (heroThing) safe_copy(&hero, (void *)(heroThing + 0x6c), 4);  // CTCPhysicsControlled
    int here = hero ? region_now() : -1;
    if (hero && now - g_lastSend >= 0.05) {
        NetPos m = {kNetMagic, ++g_seq};
        m.region = here;
        float *day = time_of_day();
        if (!day || !safe_copy(&m.day, day, 4)) m.day = -1;
        main_anim(skel_of(heroThing), m.anim, sizeof m.anim, &m.animNum);
        if (safe_copy(m.v, hero + 0x0c / 4, 12) && safe_copy(m.v + 3, hero + 0x5c / 4, 8))
            sendto(g_sock, (const char *)&m, sizeof m, 0, (sockaddr *)&g_peer, sizeof g_peer);
        g_lastSend = now;
    }
    NetPos m;
    bool got = false;
    while (recv(g_sock, (char *)&m, sizeof m, 0) == sizeof m)
        if (m.magic == kNetMagic) {
            if (m.region != g_peerRegion) coop_log("net: peer region %d (here %d)", m.region, here);
            g_peerRegion = m.region;
            g_lastRecv = now;
            float *day = hero && !g_host && m.day >= 0 ? time_of_day() : nullptr, mine;
            if (day && safe_copy(&mine, day, 4)) {
                float d = fabsf(mine - m.day);
                if (fminf(d, 1 - d) > kDaySlack) {  // a day wraps from 1 to 0
                    float set[2] = {m.day, m.day};
                    if (d > 0.01f) coop_log("net: time of day %.3f -> host's %.3f", mine, m.day);
                    safe_copy(day, set, sizeof set);
                }
            }
            m.anim[sizeof m.anim - 1] = 0;
            if (m.anim[0] && strcmp(m.anim, g_peerAnimName)) {  // look the name up once per change
                strcpy_s(g_peerAnimName, m.anim);
                g_peerAnim = ue_find(g_peerAnimName);
            }
            g_peerAnimNum = m.animNum;
            if (m.region != here) continue;
            PuppetSample smp = {now};
            memcpy(smp.v, m.v, sizeof smp.v);
            pp.samples.push_back(smp);
            if (pp.samples.size() > 200) pp.samples.pop_front();  // not driven (no puppet): keep it bounded
            got = true;
        }
    DWORD thing = pp.actor ? thing_of(pp.actor + 0x14) : 0;
    g_puppetSkel = skel_of(thing);
    if (g_puppetSkel && g_peerAnim >= 0) {  // the game sets anims only on change: put the remote one on every node
        BYTE *seq[64];
        for (int i = 0, n = anim_sequences(g_puppetSkel, seq, 64); i < n; i++) {
            DWORD name[2], ret, args[2] = {(DWORD)g_peerAnim, (DWORD)g_peerAnimNum};
            if (safe_copy(name, seq[i] + 0xf0, 8) && (name[0] != args[0] || name[1] != args[1]))
                safe_call((void *)real_SetAnim, (DWORD)seq[i], args, 2, &ret);
        }
    }
    if (pp.actor && !thing) {  // the game removed it (region load): stop touching its old body
        coop_log("net: puppet gone");
        pp.actor = 0;
        pp.phys = nullptr;
    }
    if (thing && (here < 0 || g_peerRegion != here || now - g_lastRecv > kPeerSilent)) {  // peer elsewhere or gone
        DWORD gsi = *(DWORD *)game_va(kGsiGlobal), ret, args[3] = {(DWORD)g_puppetScript, 0, 1};
        bool ok = safe_call(game_va(0x1c8ec40), gsi, args, 3, &ret);
        coop_log("net: peer in region %d, here %d, silent %.1f s: puppet removed%s",
                 g_peerRegion, here, now - g_lastRecv, ok ? "" : " (call faulted)");
        pp.actor = 0;
        pp.phys = nullptr;
        pp.samples.clear();
        return;
    }
    if (thing || !got || !hero) return;
    try {
        void *gsi = *(void **)game_va(kGsiGlobal);
        spawn_creature(kPuppetDef, pp.samples.back().v, false);
        memcpy(g_puppetScript, g_puppet, sizeof g_puppetScript);
        pp.actor = begin_scripting(gsi, g_puppet);
        thing = pp.actor ? thing_of(pp.actor + 0x14) : 0;
        if (!thing || !safe_copy(&pp.phys, (void *)(thing + 0x6c), 4)) throw std::runtime_error("no puppet body");
        pp.delay = 0.1;  // ponytail: fixed jitter buffer, measure real jitter over Steam
        coop_log("net: puppet spawned, actor %08lx physics %p", (unsigned long)pp.actor, pp.phys);
    } catch (const std::exception &e) {
        coop_log("net: puppet spawn failed: %s", e.what());
        pp.actor = 0;
        pp.phys = nullptr;
    }
}

// ---- bridge: memory methods answered on the transport thread, game calls queued to the game thread ----

static ULONG_PTR num_param(const Value &p, const char *key)
{
    const Value *v = p.find(key);
    if (!v) throw coopbridge::Error(-32010, std::string("missing ") + key);
    return v->isString() ? strtoul(v->str().c_str(), NULL, 0) : (ULONG_PTR)v->asNumber();
}

static float float_param(const Value &p, const char *key)
{
    const Value *v = p.find(key);
    if (!v || !v->isNumber()) throw coopbridge::Error(-32010, std::string("missing number ") + key);
    return (float)v->asNumber();
}

static std::string hex32(ULONG_PTR v)
{
    char b[16];
    sprintf_s(b, "0x%08lx", (unsigned long)v);
    return b;
}

static std::vector<BYTE> from_hex(const Value &p)
{
    const Value *h = p.find("hex");
    if (!h || !h->isString() || h->str().size() % 2) throw coopbridge::Error(-32010, "hex must be an even-length string");
    std::vector<BYTE> b(h->str().size() / 2);
    for (size_t i = 0; i < b.size(); i++) b[i] = (BYTE)strtoul(h->str().substr(i * 2, 2).c_str(), NULL, 16);
    return b;
}

static std::vector<BYTE> read_mem(ULONG_PTR addr, size_t size)
{
    std::vector<BYTE> buf(size);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)addr, buf.data(), size, &got) || got != size)
        throw coopbridge::Error(-32011, "unreadable memory at " + hex32(addr));
    return buf;
}

// Every 4-aligned address in committed private read-write memory holding `value`.
static Value scan_u32(ULONG_PTR value, size_t limit)
{
    Value hits = Value::array();
    size_t n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    const SIZE_T kChunk = 1 << 20;  // a 32-bit process can't copy its big heap regions in one piece
    std::vector<BYTE> buf(kChunk);
    ULONG_PTR stackLo, stackHi;  // our own stack holds `value` and leftovers of earlier calls
    GetCurrentThreadStackLimits(&stackLo, &stackHi);
    for (BYTE *p = 0; VirtualQuery(p, &mbi, sizeof mbi) && n < limit; p = (BYTE *)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) continue;
        if ((ULONG_PTR)mbi.BaseAddress >= stackLo && (ULONG_PTR)mbi.BaseAddress < stackHi) continue;
        for (SIZE_T off = 0; off < mbi.RegionSize && n < limit; off += kChunk) {
            BYTE *at = (BYTE *)mbi.BaseAddress + off;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), at, buf.data(), min(kChunk, mbi.RegionSize - off), &got)) continue;
            for (SIZE_T i = 0; i + 4 <= got && n < limit; i += 4)
                if (*(DWORD *)&buf[i] == value) { hits.push(hex32((ULONG_PTR)at + i)); n++; }
        }
    }
    return hits;
}

static void register_methods(coopbridge::Server &s)
{
    s.Register("fable.base", [](const Value &) {
        Value r = Value::object();
        r.set("exe_base", hex32((ULONG_PTR)g_exeBase));
        return r;
    }, false);
    // Objects of a class: params {vftable: Ghidra VA at base 0x400000}.
    s.Register("fable.instances", [](const Value &p) {
        ULONG_PTR vft = num_param(p, "vftable") - kGhidraBase + (ULONG_PTR)g_exeBase;
        return scan_u32(vft, (size_t)num_param(p, "limit"));
    }, false);
    s.Register("mem.read", [](const Value &p) {
        std::vector<BYTE> b = read_mem(num_param(p, "addr"), num_param(p, "size"));
        std::string out;
        char h[3];
        for (BYTE c : b) { sprintf_s(h, "%02x", c); out += h; }
        return Value(out);
    }, false);
    s.Register("mem.floats", [](const Value &p) {
        size_t count = num_param(p, "count");
        std::vector<BYTE> b = read_mem(num_param(p, "addr"), count * 4);
        Value arr = Value::array();
        for (size_t i = 0; i < count; i++) arr.push(((float *)b.data())[i]);
        return arr;
    }, false);
    s.Register("fable.frames", [](const Value &) { return Value((double)g_frames); }, false);
    // {addr, hex}: written on the game thread between frames
    s.Register("mem.write", [](const Value &p) {
        ULONG_PTR addr = num_param(p, "addr");
        std::vector<BYTE> b = from_hex(p);
        return on_game_thread([addr, b] {
            if (!safe_copy((void *)addr, b.data(), b.size())) throw std::runtime_error("write faulted at " + hex32(addr));
            return Value(true);
        });
    }, false);
    // {addr, hex} rewrites those bytes after every frame; {addr} alone stops it
    s.Register("mem.pin", [](const Value &p) {
        ULONG_PTR addr = num_param(p, "addr");
        std::lock_guard<std::mutex> lk(g_jobsLock);
        if (p.find("hex")) g_pins[addr] = from_hex(p);
        else g_pins.erase(addr);
        return Value((double)g_pins.size());
    }, false);
    // {phys, actor, x, y, z, fx, fy, delay?}: one position sample for the puppet; {phys: 0} stops driving it
    s.Register("fable.puppet", [](const Value &p) {
        float *phys = (float *)num_param(p, "phys");
        PuppetSample smp = {now_s()};
        DWORD actor = 0;
        if (phys) {
            actor = (DWORD)num_param(p, "actor");
            const char *keys[5] = {"x", "y", "z", "fx", "fy"};
            for (int i = 0; i < 5; i++) smp.v[i] = float_param(p, keys[i]);
        }
        std::lock_guard<std::mutex> lk(g_jobsLock);
        if (phys != g_puppetCtl.phys || actor != g_puppetCtl.actor) { g_puppetCtl.samples.clear(); }
        g_puppetCtl.phys = phys;
        g_puppetCtl.actor = actor;
        if (!phys) return Value(true);
        if (p.find("delay")) g_puppetCtl.delay = float_param(p, "delay");
        g_puppetCtl.samples.push_back(smp);
        return Value(true);
    }, false);
    // {size}: zeroed scratch memory for building game objects from scripts (never freed)
    s.Register("mem.alloc", [](const Value &p) {
        void *m = VirtualAlloc(NULL, num_param(p, "size"), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!m) throw coopbridge::Error(-32011, "VirtualAlloc failed");
        return Value(hex32((ULONG_PTR)m));
    }, false);
    // {fn: Ghidra VA, self: ecx (optional), args: [u32...]} -> eax, run on the game thread
    s.Register("fable.call", [](const Value &p) {
        void *fn = game_va(num_param(p, "fn"));
        DWORD self = p.find("self") ? (DWORD)num_param(p, "self") : 0;
        std::vector<DWORD> args;
        if (const Value *a = p.find("args"))
            for (const Value &v : a->items())
                args.push_back(v.isString() ? strtoul(v.str().c_str(), NULL, 0) : (DWORD)v.asNumber());
        return on_game_thread([fn, self, args] {
            DWORD ret;
            if (!safe_call(fn, self, args.data(), (int)args.size(), &ret)) throw std::runtime_error("call crashed");
            return Value(hex32(ret));
        });
    }, false);
    // params {def: "CREATURE_...", x, y, z, scripted?}; z is height; scripted also returns "actor" for fable.puppet
    s.Register("fable.spawn", [](const Value &p) {
        const Value *d = p.find("def");
        if (!d || !d->isString()) throw coopbridge::Error(-32010, "missing def");
        std::string def = d->str();
        float pos[3] = {float_param(p, "x"), float_param(p, "y"), float_param(p, "z")};
        bool scripted = p.find("scripted") && num_param(p, "scripted");
        return on_game_thread([def, pos, scripted] { return spawn_creature(def, pos, scripted); });
    }, false);
}

static coopbridge::Adapter g_adapter;  // no standard capabilities yet
static coopbridge::Server g_server;

static void start_bridge()
{
    coopbridge::Options opt;
    opt.game = "fable-anniversary";
    opt.build = "proxy-dev";
    opt.tcpPort = 0;
    char ep[MAX_PATH];
    sprintf_s(ep, "%ls\\bridge-endpoint.json", g_profileDir);
    opt.endpointFile = ep;
    register_methods(g_server);
    if (g_server.Start(opt, g_adapter)) coop_log("bridge listening, endpoint file %s", ep);
    else coop_log("bridge failed: %s", g_server.lastError().c_str());
}

typedef HRESULT(WINAPI *DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

// Exported undecorated via /EXPORT in build.bat.
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID *out, LPUNKNOWN outer)
{
    static DI8Create real;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\dinput8.dll");
        real = (DI8Create)GetProcAddress(LoadLibraryA(path), "DirectInput8Create");
        coop_log("real DirectInput8Create = %p (%s)", (void *)real, path);
        start_bridge();  // first call comes after loader lock, safe to start threads and wait on them
        net_init();
    }
    return real ? real(inst, ver, iid, out, outer) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        g_exeBase = (BYTE *)GetModuleHandleA(NULL);
        init_profile();
        char logname[MAX_PATH];
        sprintf_s(logname, "%ls\\fable_coop.log", g_profileDir);
        g_log = _fsopen(logname, "w", _SH_DENYWR);
        coop_log("fable-coop proxy loaded, exe base %p, pid %lu, profile %ls", g_exeBase, GetCurrentProcessId(), g_profileDir);
        *(void **)&real_CreateMutexW = patch_iat("KERNEL32.dll", "CreateMutexW", hk_CreateMutexW);
        *(void **)&real_SHGetFolderPathW = patch_iat("SHELL32.dll", "SHGetFolderPathW", hk_SHGetFolderPathW);
        coop_log("IAT hooks: CreateMutexW %p SHGetFolderPathW %p", real_CreateMutexW, real_SHGetFolderPathW);
        hook_vftable_slot(0x2ee6e84, 5, (void *)hk_MainUpdate, (void **)&real_MainUpdate);  // CMainGameComponent per-frame update
        coop_log("CMainGameComponent update hook, original %p", real_MainUpdate);
        // 0x1f9f0a0 = "world paused?": when the window is minimised (IsIconic, jnz at +0x11) or not in
        // front (GetForegroundWindow, jz at +0x1f) it pauses. Take the in-front branch always, so a
        // game in the background keeps running (two games on one PC; co-op never pauses for one player).
        bool bg = patch_code(0x1f9f0b1, "\x75\x0e", "\x90\x90", 2) && patch_code(0x1f9f0bf, "\x74\x15", "\xeb\x15", 2);
        coop_log("run in background patch: %s", bg ? "on" : "FAILED (unknown exe build)");
        bool hookAnim = ((void **)game_va(kAnimNodeSequenceVft))[kSetAnimSlot] == game_va(kSetAnim);
        if (hookAnim) hook_vftable_slot(kAnimNodeSequenceVft, kSetAnimSlot, (void *)hk_SetAnim, (void **)&real_SetAnim);
        coop_log("puppet hero-anim hook: %s", hookAnim ? "on" : "FAILED (unknown exe build)");
    }
    return TRUE;
}
