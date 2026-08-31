
#define _CRT_SECURE_NO_WARNINGS

#include "teleport_predict.h"
#include "prediction.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "Detours/src/detours.h"

namespace tp {
namespace {

const int kMoveDataAbsOrigin = 156;
const int kMoveDataVelocity  = 68;

const int kMoveDataFirstRun = 0;

const int kSnapWatchCommands = 200;

const float kAngleEpsilon = 0.05f;

const int kVT_ProcessMovement = 1;

const int kVT_GetViewAngles    = 19;
const int kVT_SetViewAngles    = 20;
const int kVT_IsInGame         = 26;
const int kVT_GetGameDirectory = 35;
const int kVT_GetLevelName     = 51;

struct Vector3 { float x, y, z; };

typedef void* (__cdecl* CreateInterfaceFn_t)(const char* name, int* ret);
typedef void  (__fastcall* ProcessMovementFn)(void* thisptr, void* pPlayer, void* pMove);
typedef void  (__fastcall* ViewAnglesFn)(void*, Vector3*);

ProcessMovementFn oProcessMovement = nullptr;
ViewAnglesFn      oGetViewAngles   = nullptr;
ViewAnglesFn      oSetViewAngles   = nullptr;

void* g_pGameMovement = nullptr;
void* g_pEngine       = nullptr;

bool g_enabled = true;

PredRuntime g_rt;
std::string g_currentLevel;
bool g_levelFailed = false;

float g_tickInterval  = 0.015f;
bool  g_intervalKnown = false;
int   g_intervalTick0 = 0;
unsigned long long g_intervalMs0 = 0;

bool    g_snapWatch = false;
bool    g_snapLeft  = false;
Vector3 g_snapAngle = { 0, 0, 0 };
Vector3 g_ourAngle  = { 0, 0, 0 };
int     g_snapTicks = 0;
bool    g_inOurSetCall = false;

template <typename Fn>
Fn VFunc(void* inst, int index)
{
    void** vt = *reinterpret_cast<void***>(inst);
    return reinterpret_cast<Fn>(vt[index]);
}

const char* EngineGetLevelName()
{
    if (!g_pEngine) return nullptr;
    typedef const char* (__fastcall* Fn)(void*);
    return VFunc<Fn>(g_pEngine, kVT_GetLevelName)(g_pEngine);
}

const char* EngineGetGameDirectory()
{
    if (!g_pEngine) return nullptr;
    typedef const char* (__fastcall* Fn)(void*);
    return VFunc<Fn>(g_pEngine, kVT_GetGameDirectory)(g_pEngine);
}

bool EngineIsInGame()
{
    if (!g_pEngine) return false;
    typedef bool (__fastcall* Fn)(void*);
    return VFunc<Fn>(g_pEngine, kVT_IsInGame)(g_pEngine);
}

void EngineSetViewAngles(const Vector3& ang)
{
    if (!g_pEngine || !oSetViewAngles) return;
    Vector3 a = ang;
    g_inOurSetCall = true;
    oSetViewAngles(g_pEngine, &a);
    g_inOurSetCall = false;
}

float Len(const Vector3& v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

float AngleDiff(float a, float b)
{
    float d = a - b;
    while (d > 180.0f)  d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d < 0.0f ? -d : d;
}

bool AnglesClose(const Vector3& a, const Vector3& b)
{
    return AngleDiff(a.x, b.x) < kAngleEpsilon
        && AngleDiff(a.y, b.y) < kAngleEpsilon
        && AngleDiff(a.z, b.z) < kAngleEpsilon;
}

bool IsServerSnap(const Vector3& v)
{
    return g_snapWatch && g_snapLeft && AnglesClose(v, g_snapAngle);
}

#ifdef _MSC_VER
#define TP_TRY     __try
#define TP_EXCEPT  __except (EXCEPTION_EXECUTE_HANDLER)
#else
#define TP_TRY     if (true)
#define TP_EXCEPT  else
#endif

struct RecvTableX;
struct RecvPropX {
    const char* m_pVarName;
    int         m_RecvType;
    int         m_Flags;
    int         m_StringBufferSize;
    bool        m_bInsideArray;
    const void* m_pExtraData;
    void*       m_pArrayProp;
    void*       m_ArrayLengthProxy;
    void*       m_ProxyFn;
    void*       m_DataTableProxyFn;
    RecvTableX* m_pDataTable;
    int         m_Offset;
    int         m_ElementStride;
    int         m_nElements;
    const char* m_pParentArrayPropName;
};

const int kDPT_Int    = 0;
const int kDPT_Float  = 1;
const int kDPT_Vector = 2;

int g_baseVelOffset = -1;
int g_velOffset     = -1;
int g_flagsOffset   = -1;
int g_gravityOffset = -1;
int g_tickBaseOffset= -1;
bool g_offsetsChecked = false;
bool g_offsetsGood    = false;

bool ModuleRange(const char* name, uintptr_t& base, size_t& size)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h) return false;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(h);
    if (p[0] != 'M' || p[1] != 'Z') return false;

    const int lfanew = *reinterpret_cast<const int*>(p + 0x3C);
    if (lfanew < 0 || lfanew > 0x1000) return false;

    const unsigned char* nt = p + lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) return false;

    const unsigned int sizeOfImage = *reinterpret_cast<const unsigned int*>(nt + 24 + 56);
    if (sizeOfImage < 0x1000 || sizeOfImage > 0x10000000) return false;

    base = reinterpret_cast<uintptr_t>(h);
    size = sizeOfImage;
    return true;
}

const char* FindStringIn(uintptr_t base, size_t size, const char* str)
{
    const size_t n = std::strlen(str) + 1;
    if (size < n) return nullptr;

    const char first = str[0];
    const char* start = reinterpret_cast<const char*>(base);

    for (size_t i = 0; i + n <= size; i++) {
        if (start[i] != first) continue;
        if (std::memcmp(start + i, str, n) == 0) return start + i;
    }
    return nullptr;
}

int FindNetvarOffset(uintptr_t base, size_t size, const char* name, int wantType)
{
    const char* str = FindStringIn(base, size, name);
    if (!str) return -1;

    const uintptr_t want = reinterpret_cast<uintptr_t>(str);

    for (size_t i = 0; i + sizeof(RecvPropX) <= size; i += 8) {
        const RecvPropX* p = reinterpret_cast<const RecvPropX*>(base + i);
        if (reinterpret_cast<uintptr_t>(p->m_pVarName) != want) continue;

        if (p->m_pDataTable)           continue;
        if (p->m_nElements != 1)       continue;
        if (p->m_RecvType != wantType) continue;
        if (p->m_Offset <= 0 || p->m_Offset > 0x8000) continue;

        return p->m_Offset;
    }
    return -1;
}

bool ResolveNetvars()
{
    TP_TRY {
        uintptr_t base = 0; size_t size = 0;
        if (!ModuleRange("client.dll", base, size)) return false;

        g_baseVelOffset  = FindNetvarOffset(base, size, "m_vecBaseVelocity", kDPT_Vector);
        g_velOffset      = FindNetvarOffset(base, size, "m_vecVelocity[0]",  kDPT_Float);
        g_flagsOffset    = FindNetvarOffset(base, size, "m_fFlags",          kDPT_Int);
        g_gravityOffset  = FindNetvarOffset(base, size, "m_flGravity",       kDPT_Float);
        g_tickBaseOffset = FindNetvarOffset(base, size, "m_nTickBase",       kDPT_Int);

        return g_baseVelOffset > 0 && g_velOffset > 0 && g_flagsOffset > 0
            && g_gravityOffset > 0 && g_tickBaseOffset > 0;
    }
    TP_EXCEPT {
        g_baseVelOffset = g_velOffset = g_flagsOffset = -1;
        g_gravityOffset = g_tickBaseOffset = -1;
        return false;
    }
}

void ResetPerTeleportState()
{
    g_snapWatch = false;
    g_snapLeft  = false;
}

void LoadLevel(const char* levelName)
{
    PredReset(g_rt);
    g_levelFailed = false;
    ResetPerTeleportState();

    const char* gameDir = EngineGetGameDirectory();
    if (!gameDir || !levelName) { g_levelFailed = true; return; }

    const char* prefixes[2] = { "", "download/" };

    for (int i = 0; i < 2; i++) {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s/%s%s", gameDir, prefixes[i], levelName);

        PredMap* map = new PredMap();
        if (!PredLoadBsp(path, *map)) { delete map; continue; }

        PredBuildTriggers(*map);
        PredAdopt(g_rt, map);
        return;
    }

    g_levelFailed = true;
}

void MaybeReloadLevel()
{

    if (!EngineIsInGame()) return;

    const char* lvl = EngineGetLevelName();
    if (!lvl || !*lvl) return;

    if (g_currentLevel == lvl) return;

    g_currentLevel = lvl;
    LoadLevel(lvl);
}

void __fastcall Hooked_SetViewAngles(void* thisptr, Vector3* va)
{
    if (g_inOurSetCall || !va) {
        oSetViewAngles(thisptr, va);
        return;
    }

    if (IsServerSnap(*va)) {
        g_snapWatch = false;
        return;
    }

    g_ourAngle = *va;
    if (g_snapWatch && !AnglesClose(*va, g_snapAngle))
        g_snapLeft = true;

    oSetViewAngles(thisptr, va);
}

void __fastcall Hooked_GetViewAngles(void* thisptr, Vector3* va)
{
    oGetViewAngles(thisptr, va);
    if (!va) return;

    if (IsServerSnap(*va)) {
        *va = g_ourAngle;
        EngineSetViewAngles(g_ourAngle);
        g_snapWatch = false;
        return;
    }

    g_ourAngle = *va;

    if (g_snapWatch && !AnglesClose(*va, g_snapAngle))
        g_snapLeft = true;
}

void UpdateTickInterval(int tick)
{
    if (g_intervalKnown) return;

    const unsigned long long now = GetTickCount64();
    if (g_intervalMs0 == 0) { g_intervalMs0 = now; g_intervalTick0 = tick; return; }

    const int ticks = tick - g_intervalTick0;
    const unsigned long long ms = now - g_intervalMs0;
    if (ticks < 100 || ms < 1500) {
        if (ticks < 0) { g_intervalMs0 = now; g_intervalTick0 = tick; }
        return;
    }

    const float measured = (float)ms / 1000.0f / (float)ticks;

    const float candidates[] = { 1.0f / 128.0f, 1.0f / 100.0f, 1.0f / 66.6667f,
                                 1.0f / 64.0f,  1.0f / 60.0f,  1.0f / 50.0f };
    float best = candidates[0];
    for (size_t i = 1; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        if (fabsf(candidates[i] - measured) < fabsf(best - measured)) best = candidates[i];

    g_tickInterval  = best;
    g_intervalKnown = true;
}

void __fastcall Hooked_ProcessMovement(void* thisptr, void* pPlayer, void* pMove)
{
    if (!pMove) { oProcessMovement(thisptr, pPlayer, pMove); return; }

    char* md = static_cast<char*>(pMove);
    Vector3* origin   = reinterpret_cast<Vector3*>(md + kMoveDataAbsOrigin);
    Vector3* velocity = reinterpret_cast<Vector3*>(md + kMoveDataVelocity);

    const bool firstRun =
        (*reinterpret_cast<unsigned char*>(md + kMoveDataFirstRun) & 1) != 0;

    if (!g_offsetsChecked && pPlayer && g_baseVelOffset > 0 && g_velOffset > 0
        && g_flagsOffset > 0 && g_gravityOffset > 0 && g_tickBaseOffset > 0) {
        const Vector3 mvVel = *velocity;
        if (Len(mvVel) > 50.0f) {
            const Vector3 entVel =
                *reinterpret_cast<const Vector3*>((const char*)pPlayer + g_velOffset);
            const float err = fabsf(entVel.x - mvVel.x)
                            + fabsf(entVel.y - mvVel.y)
                            + fabsf(entVel.z - mvVel.z);

            const int flags = *reinterpret_cast<const int*>((const char*)pPlayer + g_flagsOffset);
            const bool layoutOk = (flags & FL_CLIENT_BIT) != 0 && (flags & (FL_CLIENT_BIT >> 1)) == 0;

            g_offsetsChecked = true;
            g_offsetsGood    = (err < 1.0f) && layoutOk;
        }
    }

    oProcessMovement(thisptr, pPlayer, pMove);

    if (!g_enabled) return;

    MaybeReloadLevel();
    if (g_levelFailed || !g_rt.map || !g_offsetsGood || !pPlayer) return;

    const int tick = *reinterpret_cast<const int*>((const char*)pPlayer + g_tickBaseOffset);
    if (firstRun) UpdateTickInterval(tick);

    PredPlayerIO io;
    io.origin       = &origin->x;
    io.velocity     = &velocity->x;
    io.baseVelocity = reinterpret_cast<float*>((char*)pPlayer + g_baseVelOffset);
    io.gravity      = reinterpret_cast<float*>((char*)pPlayer + g_gravityOffset);
    io.flags        = reinterpret_cast<int*>((char*)pPlayer + g_flagsOffset);
    io.curtime      = (float)tick * g_tickInterval;
    io.interval     = g_tickInterval;
    io.mins[0] = -16.0f; io.mins[1] = -16.0f; io.mins[2] =  0.0f;
    io.maxs[0] =  16.0f; io.maxs[1] =  16.0f; io.maxs[2] = 72.0f;

    PredCommandStart(g_rt, tick);
    PredRunTouch(g_rt, io);
    PredCommandEnd(g_rt, tick);

    if (g_rt.viewSnapPending && firstRun) {
        const Vector3 ang = { g_rt.viewSnapAngles[0], g_rt.viewSnapAngles[1], g_rt.viewSnapAngles[2] };
        EngineSetViewAngles(ang);

        g_snapWatch = true;
        g_snapLeft  = false;
        g_snapAngle = ang;
        g_ourAngle  = ang;
        g_snapTicks = 0;
    }

    if (firstRun && g_snapWatch && ++g_snapTicks > kSnapWatchCommands)
        g_snapWatch = false;
}

}

bool Init()
{
    HMODULE client = GetModuleHandleA("client.dll");
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!client || !engine) return false;

    CreateInterfaceFn_t clientFactory =
        reinterpret_cast<CreateInterfaceFn_t>(GetProcAddress(client, "CreateInterface"));
    CreateInterfaceFn_t engineFactory =
        reinterpret_cast<CreateInterfaceFn_t>(GetProcAddress(engine, "CreateInterface"));
    if (!clientFactory || !engineFactory) return false;

    g_pGameMovement = clientFactory("GameMovement001", nullptr);
    g_pEngine       = engineFactory("VEngineClient014", nullptr);
    if (!g_pGameMovement || !g_pEngine) return false;

    void** vt = *reinterpret_cast<void***>(g_pGameMovement);
    oProcessMovement = reinterpret_cast<ProcessMovementFn>(vt[kVT_ProcessMovement]);
    if (!oProcessMovement) return false;

    void** evt = *reinterpret_cast<void***>(g_pEngine);
    oGetViewAngles = reinterpret_cast<ViewAnglesFn>(evt[kVT_GetViewAngles]);
    oSetViewAngles = reinterpret_cast<ViewAnglesFn>(evt[kVT_SetViewAngles]);
    if (!oGetViewAngles || !oSetViewAngles) return false;

    return ResolveNetvars();
}

void Attach()
{
    if (oProcessMovement)
        DetourAttach(&(PVOID&)oProcessMovement, Hooked_ProcessMovement);
    if (oGetViewAngles)
        DetourAttach(&(PVOID&)oGetViewAngles, Hooked_GetViewAngles);
    if (oSetViewAngles)
        DetourAttach(&(PVOID&)oSetViewAngles, Hooked_SetViewAngles);
}

void Detach()
{
    if (oProcessMovement)
        DetourDetach(&(PVOID&)oProcessMovement, Hooked_ProcessMovement);
    if (oGetViewAngles)
        DetourDetach(&(PVOID&)oGetViewAngles, Hooked_GetViewAngles);
    if (oSetViewAngles)
        DetourDetach(&(PVOID&)oSetViewAngles, Hooked_SetViewAngles);
}

bool Toggle()
{
    g_enabled = !g_enabled;
    ResetPerTeleportState();
    return g_enabled;
}

int LoadedCount() { return g_rt.map ? g_rt.map->stats.active : 0; }

bool Ready() { return g_offsetsGood; }

}
