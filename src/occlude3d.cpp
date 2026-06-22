/**
 * occlude3d - Shared depth-correct world-space geometry renderer for Ashita addons.
 *
 * Addons submit world geometry via RaiseEvent('occlude3d', byteTable); the plugin trampoline-hooks
 * CXiActorNameDraw::OnMove (where the world depth buffer is bound) and draws the submitted geometry
 * there, so it occludes correctly.
 */
#include "occlude3d.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <string>
#include <vector>

#define OCCLUDE3D_VERSION  1.0

#define OCCLUDE3D_SENTINEL "occlude3d"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Trampoline hook for CXiActorNameDraw::OnMove.
//
// The function prologue is a single 5-byte instruction (A1 imm32 = mov eax,[imm32]), so we overwrite
// exactly those 5 bytes with a JMP to our detour. The detour saves all registers, calls a worker,
// restores, then JMPs to a trampoline (original 5 bytes + JMP back to addr+5) so OnMove runs intact.
////////////////////////////////////////////////////////////////////////////////////////////////////
namespace
{
    volatile uint32_t g_hookCalls   = 0;
    void*             g_trampoline  = nullptr;
    uintptr_t         g_hookAddr    = 0;
    uint8_t           g_origBytes[5] = { 0 };
    bool              g_installed   = false;
    occlude3d*        g_instance    = nullptr;
}

static void do_hook_work(void)
{
    g_hookCalls++;
    if (g_instance != nullptr)
        g_instance->RenderHookDraw();
}

namespace
{

    __declspec(naked) void hook_OnMove(void)
    {
        __asm
        {
            pushad
            pushfd
            call do_hook_work
            popfd
            popad
            jmp  dword ptr [g_trampoline]
        }
    }
}

occlude3d::occlude3d(void)
    : m_AshitaCore(nullptr)
    , m_LogManager(nullptr)
    , m_Device(nullptr)
    , m_NameDrawAddr(0)
    , m_HookMatches(0)
    , m_Hooked(false)
    , m_TriedAutoHook(false)
    , m_WarnedUnhooked(false)
    , m_Stats(false)
    , m_QpcFreq(0)
    , m_StatFrames(0)
    , m_HookMs(0.0)
    , m_EventMs(0.0)
    , m_ItemsAcc(0.0)
    , m_OwnersAcc(0.0)
    , m_BytesAcc(0.0)
    , m_StateAcc(0.0)
    , m_FrameItems(0)
    , m_FrameOwners(0)
    , m_FrameState(0)
    , m_DropOversize(0)
    , m_DropNoSlot(0)
    , m_DropBadItem(0)
    , m_DropBadTex(0)
    , m_LastDropOwner(0)
    , m_UiOpen(false)
    , m_Announced(false)
    , m_DispItems(0.0)
    , m_DispOwners(0.0)
    , m_DispState(0.0)
    , m_DispDrawMs(0.0)
    , m_DispRecvMs(0.0)
    , m_DispBytes(0.0)
    , m_HistPos(0)
{
    memset(this->m_DropWarnTick, 0, sizeof(this->m_DropWarnTick));
    memset(this->m_Owners, 0, sizeof(this->m_Owners));
    memset(this->m_Fonts,  0, sizeof(this->m_Fonts));
    memset(this->m_HistDrawMs, 0, sizeof(this->m_HistDrawMs));
    memset(this->m_HistItems,  0, sizeof(this->m_HistItems));
    LARGE_INTEGER f; if (QueryPerformanceFrequency(&f)) this->m_QpcFreq = f.QuadPart;
}

occlude3d::~occlude3d(void)
{}

const char* occlude3d::GetName(void) const
{
    return "occlude3d";
}

const char* occlude3d::GetAuthor(void) const
{
    return "SQLCommit";
}

const char* occlude3d::GetDescription(void) const
{
    return "Shared depth-correct world-space geometry renderer for Ashita addons.";
}

const char* occlude3d::GetLink(void) const
{
    return "https://github.com/SQLCommit";
}

double occlude3d::GetVersion(void) const
{
    return OCCLUDE3D_VERSION;
}

double occlude3d::GetInterfaceVersion(void) const
{
    return ASHITA_INTERFACE_VERSION;
}

int32_t occlude3d::GetPriority(void) const
{
    return 0;
}

uint32_t occlude3d::GetFlags(void) const
{
    return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands) |
           static_cast<uint32_t>(Ashita::PluginFlags::UseDirect3D) |
           static_cast<uint32_t>(Ashita::PluginFlags::UsePluginEvents);
}

bool occlude3d::Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id)
{
    this->m_AshitaCore = core;
    this->m_LogManager = logger;
    UNREFERENCED_PARAMETER(id);

    if (core != nullptr && core->GetPointerManager() != nullptr)
    {
        core->GetPointerManager()->Add(
            OCCLUDE3D_SENTINEL,
            static_cast<uintptr_t>(OCCLUDE3D_VERSION * 100.0 + 0.5));
    }


    return true;
}

void occlude3d::Release(void)
{
    this->RemoveHook();

    for (int i = 0; i < kMaxOwners; ++i)
    {
        if (this->m_Owners[i].used)
        {
            this->RefBufferTextures(this->m_Owners[i].buf, this->m_Owners[i].size, false);
            this->m_Owners[i].used = false;
        }
    }
    for (int i = 0; i < kMaxFonts; ++i)
    {
        if (this->m_Fonts[i].used)
        {
            this->RefTexture(this->m_Fonts[i].tex, false);
            this->m_Fonts[i].used = false;
        }
    }

    if (this->m_AshitaCore != nullptr && this->m_AshitaCore->GetPointerManager() != nullptr)
    {
        this->m_AshitaCore->GetPointerManager()->Delete(OCCLUDE3D_SENTINEL);
    }

    if (this->m_LogManager != nullptr)
    {
        this->m_LogManager->Log(
            static_cast<uint32_t>(Ashita::LogLevel::Info),
            "occlude3d",
            "Unloaded.");
    }
}

bool occlude3d::HandleCommand(int32_t mode, const char* command, bool injected)
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(injected);

    if (command == nullptr)
        return false;

    std::vector<std::string> args;
    if (Ashita::Commands::GetCommandArgs(command, &args) < 1)
        return false;

    if (_stricmp(args[0].c_str(), "/occlude3d") != 0 && _stricmp(args[0].c_str(), "/o3d") != 0)
        return false;

    const char* sub = (args.size() > 1) ? args[1].c_str() : "";

    if (_stricmp(sub, "hook") == 0)
    {
        if (!this->m_Hooked)
        {
            const uintptr_t a = this->ResolveNameDraw();
            this->InstallHook(a);
        }
        else
        {
            this->RemoveHook();
        }
    }
    else if (_stricmp(sub, "stats") == 0)
    {
        this->m_Stats = !this->m_Stats;
        this->ResetStatAccumulators();
    }
    else if (_stricmp(sub, "ui") == 0 || sub[0] == '\0')
    {
        this->m_UiOpen = !this->m_UiOpen;
    }

    {
        char body[160];
        _snprintf_s(body, sizeof(body), _TRUNCATE, "hook=%s, stats=%s (panel: /o3d).",
            this->m_Hooked ? "ON" : "off", this->m_Stats ? "ON" : "off");
        this->WriteChat(body);
    }

    return true;
}

bool occlude3d::Direct3DInitialize(IDirect3DDevice8* device)
{
    this->m_Device = device;

    if (this->m_LogManager != nullptr)
    {
        this->m_LogManager->Logf(
            static_cast<uint32_t>(Ashita::LogLevel::Info),
            "occlude3d",
            "Direct3DInitialize ok (device=0x%p).", static_cast<void*>(device));
    }

    return true;
}

uintptr_t occlude3d::ResolveNameDraw(void)
{
    if (this->m_LogManager == nullptr)
        return 0;
    const uint32_t LL = static_cast<uint32_t>(Ashita::LogLevel::Info);

    HMODULE h = GetModuleHandleA("FFXiMain.dll");
    if (h == nullptr) h = GetModuleHandleA("ffximain.dll");
    if (h == nullptr)
    {
        this->m_LogManager->Log(LL, "occlude3d", "HOOK: FFXiMain.dll module handle not found.");
        return 0;
    }

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    const IMAGE_NT_HEADERS* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
    const uint8_t* base = reinterpret_cast<const uint8_t*>(h);
    const uint32_t size = nt->OptionalHeader.SizeOfImage;

    this->m_LogManager->Logf(LL, "occlude3d", "HOOK: FFXiMain base=0x%08X SizeOfImage=0x%X", reinterpret_cast<uint32_t>(base), size);

    //   CXiActorNameDraw::OnMove signature (Provided by Atom0s in Discord chat):
    //   A1 ?? ?? ?? ?? 83 38 60 0F ?? ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 56 6A
    const uint8_t pat[]  = { 0xA1,0,0,0,0, 0x83,0x38,0x60, 0x0F,0,0,0,0,0, 0x8B,0x0D,0,0,0,0, 0x56,0x6A };
    const char    mask[] = "x????xxxx?????xx????xx";
    const int     plen   = static_cast<int>(sizeof(pat));

    int       found = 0;
    uintptr_t firstAddrs[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i + plen <= size; ++i)
    {
        bool m = true;
        for (int j = 0; j < plen; ++j)
        {
            if (mask[j] == 'x' && base[i + j] != pat[j]) { m = false; break; }
        }
        if (m)
        {
            if (found < 4) firstAddrs[found] = reinterpret_cast<uintptr_t>(base + i);
            ++found;
        }
    }

    this->m_NameDrawAddr = firstAddrs[0];
    this->m_HookMatches  = found;
    this->m_LogManager->Logf(LL, "occlude3d", "HOOK scan: matches=%d  [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X",
        found, static_cast<uint32_t>(firstAddrs[0]), static_cast<uint32_t>(firstAddrs[1]),
        static_cast<uint32_t>(firstAddrs[2]), static_cast<uint32_t>(firstAddrs[3]));

    if (found != 1)
        this->m_LogManager->Logf(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
            "HOOK: expected exactly 1 signature match but found %d -- using [0]=0x%08X; verify against this client.",
            found, static_cast<uint32_t>(firstAddrs[0]));

    for (int k = 0; k < 4 && firstAddrs[k] != 0; ++k)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(firstAddrs[k]);
        char line[320];
        int  off = 0;
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "HOOK [%d] @0x%08X:", k, static_cast<uint32_t>(firstAddrs[k]));
        for (int i = 0; i < 56 && off < static_cast<int>(sizeof(line)) - 4; ++i)
            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " %02X", p[i]);
        this->m_LogManager->Log(LL, "occlude3d", line);
    }

    return this->m_NameDrawAddr;
}

bool occlude3d::InstallHook(uintptr_t addr)
{
    if (addr == 0 || g_installed)
        return false;

    const uint32_t LL = static_cast<uint32_t>(Ashita::LogLevel::Info);

    if (*reinterpret_cast<const uint8_t*>(addr) != 0xA1)
    {
        if (this->m_LogManager)
            this->m_LogManager->Logf(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
                "HOOK: prologue at 0x%08X is 0x%02X, expected 0xA1 (mov eax,[imm32]) -- refusing to splice an unexpected instruction.",
                static_cast<uint32_t>(addr), *reinterpret_cast<const uint8_t*>(addr));
        return false;
    }

    uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (tramp == nullptr)
    {
        if (this->m_LogManager) this->m_LogManager->Log(LL, "occlude3d", "HOOK: VirtualAlloc failed.");
        return false;
    }

    uint8_t* target = reinterpret_cast<uint8_t*>(addr);
    memcpy(g_origBytes, target, 5);
    memcpy(tramp, g_origBytes, 5);
    tramp[5] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + 6) = static_cast<int32_t>((addr + 5) - (reinterpret_cast<uintptr_t>(tramp) + 10));

    g_trampoline = tramp;
    g_hookAddr   = addr;
    g_hookCalls  = 0;
    g_instance   = this;

    DWORD oldProt = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        if (this->m_LogManager) this->m_LogManager->Log(LL, "occlude3d", "HOOK: VirtualProtect failed.");
        VirtualFree(tramp, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        return false;
    }
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hook_OnMove) - (addr + 5));
    VirtualProtect(target, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    g_installed   = true;
    this->m_Hooked = true;
    this->m_WarnedUnhooked = false;
    if (this->m_LogManager)
        this->m_LogManager->Logf(LL, "occlude3d", "HOOK installed at 0x%08X (trampoline 0x%08X).",
            static_cast<uint32_t>(addr), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tramp)));
    return true;
}

void occlude3d::RemoveHook(void)
{
    if (!g_installed)
        return;

    uint8_t* target = reinterpret_cast<uint8_t*>(g_hookAddr);
    DWORD oldProt = 0;
    if (VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        memcpy(target, g_origBytes, 5);
        VirtualProtect(target, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
    }
    g_instance = nullptr;
    if (g_trampoline != nullptr)
    {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
    }
    g_installed   = false;
    this->m_Hooked = false;
    if (this->m_LogManager)
        this->m_LogManager->Logf(static_cast<uint32_t>(Ashita::LogLevel::Info), "occlude3d",
            "HOOK removed. Total OnMove calls observed: %u", g_hookCalls);
}

void occlude3d::HandleEvent(const char* eventName, const void* eventData, const uint32_t eventSize)
{
    if (eventName == nullptr || eventData == nullptr) return;
    if (strcmp(eventName, "occlude3d") != 0) return;

    const uint8_t* p = static_cast<const uint8_t*>(eventData);
    if (eventSize < 16 || eventSize > static_cast<uint32_t>(kOwnerBuf))
    {
        uint32_t ow = 0; if (eventSize >= 8) memcpy(&ow, p + 4, 4);
        this->m_DropOversize++; this->m_LastDropOwner = ow;
        this->WarnDrop(0, "submission too large for one owner (>16 KB) -- split it across owner ids", ow);
        return;
    }

    uint32_t magic = 0, owner = 0, ttl = 0;
    memcpy(&magic, p + 0, 4); memcpy(&owner, p + 4, 4); memcpy(&ttl, p + 8, 4);

    if (magic == 0x4F334446u)
    {
        if (eventSize < 28) return;
        uint32_t fontId = 0, tex = 0, cols = 0, rows = 0, firstCp = 0; float adv = 0.0f;
        memcpy(&fontId, p+4, 4); memcpy(&tex, p+8, 4); memcpy(&cols, p+12, 4);
        memcpy(&rows, p+16, 4); memcpy(&firstCp, p+20, 4); memcpy(&adv, p+24, 4);
        int fs = -1, ff = -1;
        for (int i = 0; i < kMaxFonts; ++i)
        {
            if (this->m_Fonts[i].used && this->m_Fonts[i].id == fontId) { fs = i; break; }
            if (!this->m_Fonts[i].used && ff < 0) ff = i;
        }
        if (tex == 0)
        {
            if (fs >= 0) { this->RefTexture(this->m_Fonts[fs].tex, false); this->m_Fonts[fs].used = false; }
            return;
        }
        if (fs < 0) fs = ff;
        if (fs < 0) return;
        Font& fo = this->m_Fonts[fs];
        if (fo.used && fo.tex != tex) this->RefTexture(fo.tex, false);
        if (!fo.used || fo.tex != tex) this->RefTexture(tex, true);
        fo.used = true; fo.id = fontId; fo.tex = tex;
        fo.cols = (cols ? cols : 1); fo.rows = (rows ? rows : 1); fo.firstCp = firstCp;
        fo.advance = (adv > 0.0f ? adv : 0.6f);
        return;
    }

    if (magic != 0x4F334432u) return;

    LARGE_INTEGER _es; const bool _st = this->StatsOn();
    if (_st) QueryPerformanceCounter(&_es);

    int slot = -1, freeSlot = -1;
    for (int i = 0; i < kMaxOwners; ++i)
    {
        if (this->m_Owners[i].used && this->m_Owners[i].id == owner) { slot = i; break; }
        if (!this->m_Owners[i].used && freeSlot < 0) freeSlot = i;
    }
    if (slot < 0) slot = freeSlot;
    if (slot < 0)
    {
        this->m_DropNoSlot++; this->m_LastDropOwner = owner;
        this->WarnDrop(1, "all owner slots in use -- a new owner's geometry was dropped", owner);
        return;
    }

    SubOwner& o = this->m_Owners[slot];
    if (o.used)
        this->RefBufferTextures(o.buf, o.size, false);
    o.used   = true;
    o.id     = owner;
    o.expire = GetTickCount() + ttl;
    o.size   = eventSize;
    memcpy(o.buf, p, eventSize);
    this->RefBufferTextures(o.buf, o.size, true);

    if (!this->m_Hooked && !this->m_TriedAutoHook)
    {
        this->m_TriedAutoHook = true;
        const uintptr_t a = this->ResolveNameDraw();
        if (a != 0)
            this->InstallHook(a);
    }

    if (!this->m_Hooked && this->m_TriedAutoHook && !this->m_WarnedUnhooked)
    {
        this->m_WarnedUnhooked = true;
        if (this->m_LogManager != nullptr)
            this->m_LogManager->Log(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
                "Geometry submitted but the occlusion hook is not installed -- nothing will render. Try '/o3d hook' (status in /o3d).");
        this->WriteChat("geometry submitted but the occlusion hook isn't installed -- nothing will render. Try /o3d hook.", 0x44);
    }

    if (_st && this->m_QpcFreq > 0)
    {
        LARGE_INTEGER _ee; QueryPerformanceCounter(&_ee);
        this->m_EventMs   += static_cast<double>(_ee.QuadPart - _es.QuadPart) * 1000.0 / static_cast<double>(this->m_QpcFreq);
        this->m_BytesAcc  += static_cast<double>(eventSize);
    }
}

static inline bool TickAlive(uint32_t now, uint32_t expire)
{
    return static_cast<int32_t>(now - expire) < 0;
}

static inline bool Finite(float x)   { return (x == x) && ((x - x) == 0.0f); }
static inline bool Finite3(const float* v) { return Finite(v[0]) && Finite(v[1]) && Finite(v[2]); }

static const float kLayerStep    = 0.010f;
static const float kBarFillNudge = 0.006f;
static const float kOutlineFrac  = 0.07f;

bool occlude3d::HasGeometry(void) const
{
    const uint32_t now = GetTickCount();
    for (int i = 0; i < kMaxOwners; ++i)
        if (this->m_Owners[i].used && this->m_Owners[i].size >= 20 && TickAlive(now, this->m_Owners[i].expire))
            return true;
    return false;
}

static bool ReadablePtr(uint32_t p)
{
    if (p == 0)
        return false;
    uint32_t tmp = 0; SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(p), &tmp, 4, &got) != 0 && got == 4;
}

void occlude3d::RefTexture(uint32_t texPtr, bool addref)
{
    if (!ReadablePtr(texPtr))
        return;
    uint32_t vtbl = 0; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(texPtr), &vtbl, 4, &got) || got != 4 || !ReadablePtr(vtbl))
        return;
    IDirect3DBaseTexture8* t = reinterpret_cast<IDirect3DBaseTexture8*>(static_cast<uintptr_t>(texPtr));
    __try
    {
        if (addref) t->AddRef(); else t->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

struct ItemLayout { uint16_t header; int8_t texOff; int8_t countOff; uint8_t elem; uint8_t minCount; bool mul3; };
static const ItemLayout kItemLayout[16] = {
    /* 0 LINE      */ { 32, -1, -1,  0, 0, false },
    /* 1 MARKER    */ { 20, -1, -1,  0, 0, false },
    /* 2 RIBBON    */ { 12, -1,  8, 12, 2, false },
    /* 3 TEXRIBBON */ { 16,  8, 12, 12, 2, false },
    /* 4 TRIS      */ {  4, -1,  0, 16, 3, true  },
    /* 5 TEXTRIS   */ {  8,  0,  4, 24, 3, true  },
    /* 6 TEXT      */ { 32, -1, 28,  1, 0, false },
    /* 7 BQUAD     */ { 28,  0, -1,  0, 0, false },
    /* 8 BAR       */ { 36,  0, -1,  0, 0, false },
    /* 9 RRECT     */ { 28, -1, -1,  0, 0, false },
    /*10 GQUAD     */ { 32,  8, -1,  0, 0, false },
    /*11 SPRITE    */ { 48,  0, -1,  0, 0, false },
    /*12 ARC       */ { 32, -1, -1,  0, 0, false },
    /*13 DECAL     */ { 32,  0, -1,  0, 0, false },
    /*14 NINESLICE */ { 36,  0, -1,  0, 0, false },
    /*15 PARTICLE  */ { 44,  0, -1,  0, 0, false },
};

static bool DecodeItem(uint32_t type, const uint8_t* b, uint32_t pos, uint32_t end, uint32_t* bodyLen, uint32_t* texPtr)
{
    *bodyLen = 0; *texPtr = 0;
    if (type > 15) return false;
    const ItemLayout& L = kItemLayout[type];
    if (pos + L.header > end) return false;
    uint32_t len = L.header;
    if (L.countOff >= 0)
    {
        uint32_t cnt = 0; memcpy(&cnt, b + pos + L.countOff, 4);
        const uint32_t avail = end - (pos + L.header);
        if (cnt < L.minCount) return false;
        if (L.mul3 && (cnt % 3u) != 0u) return false;
        if (L.elem != 0 && cnt > avail / L.elem) return false;
        len += cnt * L.elem;
    }
    if (L.texOff >= 0) memcpy(texPtr, b + pos + L.texOff, 4);
    *bodyLen = len;
    return true;
}

void occlude3d::RefBufferTextures(const uint8_t* b, uint32_t size, bool addref)
{
    if (b == nullptr || size < 20)
        return;
    const uint32_t end = size;
    uint32_t pos = 16, items = 0; memcpy(&items, b + 12, 4);
    uint32_t seen[512]; int nseen = 0;
    for (uint32_t it = 0; it < items && pos + 4 <= end; ++it)
    {
        uint32_t tw = 0; memcpy(&tw, b + pos, 4); pos += 4;
        uint32_t bodyLen = 0, tp = 0;
        if (!DecodeItem(tw & 0xFFu, b, pos, end, &bodyLen, &tp)) break;
        pos += bodyLen;
        if (tp != 0)
        {
            bool dup = false;
            for (int s = 0; s < nseen; ++s) { if (seen[s] == tp) { dup = true; break; } }
            if (!dup) { if (nseen < 512) seen[nseen++] = tp; this->RefTexture(tp, addref); }
        }
    }
}

const occlude3d::Font* occlude3d::FindFont(uint32_t id) const
{
    for (int i = 0; i < kMaxFonts; ++i)
        if (this->m_Fonts[i].used && this->m_Fonts[i].id == id)
            return &this->m_Fonts[i];
    return nullptr;
}

void occlude3d::WriteChat(const char* body, uint8_t bodyColor)
{
    if (this->m_AshitaCore == nullptr || this->m_AshitaCore->GetChatManager() == nullptr || body == nullptr)
        return;
    char buffer[256];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
        "\x1E\x51" "[" "\x1E\x06" "occlude3d" "\x1E\x51" "]" "\x1E\x01" " " "\x1E%c" "%s" "\x1E\x01", bodyColor, body);
    this->m_AshitaCore->GetChatManager()->AddChatMessage(1, false, buffer);
}

static void FourCC(uint32_t id, char out[5])
{
    out[0] = static_cast<char>((id >> 24) & 0xFF); out[1] = static_cast<char>((id >> 16) & 0xFF);
    out[2] = static_cast<char>((id >> 8) & 0xFF);  out[3] = static_cast<char>(id & 0xFF); out[4] = 0;
    for (int k = 0; k < 4; ++k) if (out[k] < 32 || out[k] > 126) out[k] = '?';
}

void occlude3d::WarnDrop(int cat, const char* reason, uint32_t owner)
{
    if (cat < 0 || cat > 2) cat = 0;
    const uint32_t now = GetTickCount();
    if (this->m_DropWarnTick[cat] != 0 && (now - this->m_DropWarnTick[cat]) < 5000)
        return;
    this->m_DropWarnTick[cat] = now;
    char fc[5]; FourCC(owner, fc);
    if (this->m_LogManager != nullptr)
        this->m_LogManager->Logf(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
            "DROP from owner '%s' (0x%08X): %s. [totals: oversize=%u noSlot=%u badItem=%u]",
            fc, owner, reason, this->m_DropOversize, this->m_DropNoSlot, this->m_DropBadItem);
    char cm[224];
    _snprintf_s(cm, sizeof(cm), _TRUNCATE, "dropped a submission from '%s' -- %s", fc, reason);
    this->WriteChat(cm, 0x44);
}

void occlude3d::DrawSubmittedGuarded(IDirect3DDevice8* dev)
{
    __try
    {
        this->DrawSubmittedGeometry(dev);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void occlude3d::DrawSubmittedGeometry(IDirect3DDevice8* dev)
{
    struct V { float x, y, z; D3DCOLOR c; };
    const uint32_t now = GetTickCount();
    this->m_FrameItems = 0; this->m_FrameOwners = 0; this->m_FrameState = 0;

    D3DMATRIX view;
    dev->GetTransform(D3DTS_VIEW, &view);
    const float eye[3] = {
        -(view._41 * view._11 + view._42 * view._12 + view._43 * view._13),
        -(view._41 * view._21 + view._42 * view._22 + view._43 * view._23),
        -(view._41 * view._31 + view._42 * view._32 + view._43 * view._33),
    };
    const float rx = view._11, ry = view._21, rz = view._31;
    const float ux = view._12, uy = view._22, uz = view._32;

    D3DMATRIX proj; dev->GetTransform(D3DTS_PROJECTION, &proj);
    D3DVIEWPORT8 vp; dev->GetViewport(&vp);
    const float pxFactor = (static_cast<float>(vp.Height) * 0.5f) * proj._22;

    struct VT { float x, y, z; D3DCOLOR c; float u, v; };
    static const int   kMaxRibPts = 256;
    static V           strip[kMaxRibPts * 2];
    static VT          tstrip[kMaxRibPts * 2];

    static const int kBatchMax = 8192;
    static VT        batch[kBatchMax];
    int      batchN = 0, batchStage = -1;
    uint32_t batchTex = 0, batchFlags = 0;

    auto flush = [&]() {
        if (batchN >= 3 && batchStage >= 0)
        {
            dev->SetRenderState(D3DRS_ZFUNC,     D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRS_DESTBLEND, (batchFlags & 0x01u) ? D3DBLEND_ONE  : D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_ZWRITEENABLE,    (batchFlags & 0x04u) ? TRUE : FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, (batchFlags & 0x04u) ? TRUE : FALSE);
            dev->SetTexture(0, reinterpret_cast<IDirect3DBaseTexture8*>(static_cast<uintptr_t>(batchTex)));
            if (batchStage == 0) {
                dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_BLENDTEXTUREALPHA);
                dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                this->m_FrameState += 15;
            } else {
                dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
                dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                this->m_FrameState += 16;
            }
            dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, batchN / 3, batch, sizeof(VT));
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        }
        batchN = 0; batchStage = -1;
    };

    auto prep = [&](float c[3], float& hw, float& hh, uint32_t fl)
    {
        const int lyr = static_cast<int>((fl >> 4) & 0x0Fu);
        const float toe[3] = { eye[0]-c[0], eye[1]-c[1], eye[2]-c[2] };
        const float d = sqrtf(toe[0]*toe[0]+toe[1]*toe[1]+toe[2]*toe[2]);
        if (d < 1e-4f) return;
        if ((fl & 0x08u) && pxFactor > 1e-3f) { const float s = d / pxFactor; hw *= s; hh *= s; }
        if (lyr > 0) { const float k = (static_cast<float>(lyr) * kLayerStep) / d; c[0]+=toe[0]*k; c[1]+=toe[1]*k; c[2]+=toe[2]*k; }
    };
    auto colState = [&](uint32_t fl)
    {
        dev->SetRenderState(D3DRS_ZFUNC,     D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_DESTBLEND, (fl & 0x01u) ? D3DBLEND_ONE  : D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_ZWRITEENABLE,    (fl & 0x04u) ? TRUE : FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, (fl & 0x04u) ? TRUE : FALSE);
        this->m_FrameState += 2;
    };
    uint32_t goodTex[32]; int nGoodTex = 0;
    auto texOK = [&](uint32_t tp) -> bool {
        if (tp == 0) return false;
        for (int i = 0; i < nGoodTex; ++i) if (goodTex[i] == tp) return true;
        if (ReadablePtr(tp)) { if (nGoodTex < 32) goodTex[nGoodTex++] = tp; return true; }
        this->m_DropBadTex++;
        return false;
    };
    auto quad = [&](const float c[3], float hw, float hh, D3DCOLOR cTL, D3DCOLOR cTR, D3DCOLOR cBR, D3DCOLOR cBL, uint32_t texPtr, uint32_t fl)
    {
        if (!Finite3(c) || !Finite(hw) || !Finite(hh)) return;
        float cr[4][3];
        const float sw[4] = { -hw, hw, hw, -hw }, sh[4] = { hh, hh, -hh, -hh };
        for (int k = 0; k < 4; ++k) { cr[k][0]=c[0]+rx*sw[k]+ux*sh[k]; cr[k][1]=c[1]+ry*sw[k]+uy*sh[k]; cr[k][2]=c[2]+rz*sw[k]+uz*sh[k]; }
        if (texPtr != 0)
        {
            if (!texOK(texPtr)) return;
            if (batchStage != 1 || batchTex != texPtr || batchFlags != fl || batchN + 6 > kBatchMax) flush();
            batchTex = texPtr; batchFlags = fl; batchStage = 1;
            const VT q[6] = {
                { cr[0][0],cr[0][1],cr[0][2], cTL,0,0 }, { cr[1][0],cr[1][1],cr[1][2], cTR,1,0 }, { cr[2][0],cr[2][1],cr[2][2], cBR,1,1 },
                { cr[0][0],cr[0][1],cr[0][2], cTL,0,0 }, { cr[2][0],cr[2][1],cr[2][2], cBR,1,1 }, { cr[3][0],cr[3][1],cr[3][2], cBL,0,1 },
            };
            for (int qi = 0; qi < 6; ++qi) batch[batchN++] = q[qi];
        }
        else
        {
            flush();
            colState(fl);
            const V q[6] = {
                { cr[0][0],cr[0][1],cr[0][2], cTL }, { cr[1][0],cr[1][1],cr[1][2], cTR }, { cr[2][0],cr[2][1],cr[2][2], cBR },
                { cr[0][0],cr[0][1],cr[0][2], cTL }, { cr[2][0],cr[2][1],cr[2][2], cBR }, { cr[3][0],cr[3][1],cr[3][2], cBL },
            };
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(V));
        }
    };

    for (int oi = 0; oi < kMaxOwners; ++oi)
    {
        SubOwner& o = this->m_Owners[oi];
        if (!o.used)
            continue;
        if (!TickAlive(now, o.expire))
        {
            this->RefBufferTextures(o.buf, o.size, false);
            o.used = false;
            continue;
        }
        if (o.size < 20)
            continue;
        this->m_FrameOwners++;

        const uint8_t* b   = o.buf;
        const uint32_t end = o.size;
        uint32_t       pos = 16;
        uint32_t       items = 0; memcpy(&items, b + 12, 4);

        uint32_t it = 0;
        for (; it < items && pos + 4 <= end; ++it)
        {
            uint32_t typeWord = 0; memcpy(&typeWord, b + pos, 4); pos += 4;
            this->m_FrameItems++;
            const uint32_t type  = typeWord & 0xFFu;
            const uint32_t flags = (typeWord >> 8) & 0xFFFFFFu;

            if (type == 0 || type == 1 || type == 2 || type == 4)
            {
                flush();
                colState(flags);
            }

            if (type == 0)
            {
                if (pos + 32 > end) break;
                uint32_t col; float a[3], bp[3], w;
                memcpy(&col, b+pos, 4); memcpy(a, b+pos+4, 12); memcpy(bp, b+pos+16, 12); memcpy(&w, b+pos+28, 4);
                pos += 32;
                if (!Finite3(a) || !Finite3(bp) || !Finite(w)) continue;
                if (w > 0.0f)
                {
                    const float dir[3] = { bp[0]-a[0], bp[1]-a[1], bp[2]-a[2] };
                    const float mid[3] = { (a[0]+bp[0])*0.5f, (a[1]+bp[1])*0.5f, (a[2]+bp[2])*0.5f };
                    const float toe[3] = { eye[0]-mid[0], eye[1]-mid[1], eye[2]-mid[2] };
                    float s[3] = { dir[1]*toe[2]-dir[2]*toe[1], dir[2]*toe[0]-dir[0]*toe[2], dir[0]*toe[1]-dir[1]*toe[0] };
                    const float len = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
                    if (len > 1e-4f)
                    {
                        const float hw = w*0.5f/len; s[0]*=hw; s[1]*=hw; s[2]*=hw;
                        const V q[4] = {
                            { a[0]-s[0], a[1]-s[1], a[2]-s[2], col }, { a[0]+s[0], a[1]+s[1], a[2]+s[2], col },
                            { bp[0]-s[0], bp[1]-s[1], bp[2]-s[2], col }, { bp[0]+s[0], bp[1]+s[1], bp[2]+s[2], col },
                        };
                        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
                        continue;
                    }
                }
                const V v[2] = { { a[0],a[1],a[2], col }, { bp[0],bp[1],bp[2], col } };
                dev->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(V));
            }
            else if (type == 1)
            {
                if (pos + 20 > end) break;
                uint32_t col; float a[3], sz;
                memcpy(&col, b+pos, 4); memcpy(a, b+pos+4, 12); memcpy(&sz, b+pos+16, 4);
                pos += 20;
                if (!Finite3(a) || !Finite(sz)) continue;
                const float h = (sz > 0.0f) ? sz : 1.0f;
                const V v[6] = {
                    { a[0]-h,a[1],a[2], col }, { a[0]+h,a[1],a[2], col },
                    { a[0],a[1]-h,a[2], col }, { a[0],a[1]+h,a[2], col },
                    { a[0],a[1],a[2]-h, col }, { a[0],a[1],a[2]+h, col },
                };
                dev->DrawPrimitiveUP(D3DPT_LINELIST, 3, v, sizeof(V));
            }
            else if (type == 2)
            {
                if (pos + 12 > end) break;
                uint32_t col, n; float w;
                memcpy(&col, b+pos, 4); memcpy(&w, b+pos+4, 4); memcpy(&n, b+pos+8, 4);
                pos += 12;
                if (n < 2 || n > (end - pos) / 12) break;
                const float* pts = reinterpret_cast<const float*>(b + pos);
                pos += n*12;
                const uint32_t np = (n > static_cast<uint32_t>(kMaxRibPts)) ? kMaxRibPts : n;
                { bool ok = Finite(w); for (uint32_t i = 0; ok && i < np*3; ++i) ok = Finite(pts[i]); if (!ok) continue; }
                for (uint32_t i = 0; i < np; ++i)
                {
                    const float* P = pts + i*3;
                    const float* Pp = pts + (i==0 ? 0 : i-1)*3;
                    const float* Pn = pts + (i+1>=np ? np-1 : i+1)*3;
                    const float dir[3] = { Pn[0]-Pp[0], Pn[1]-Pp[1], Pn[2]-Pp[2] };
                    const float toe[3] = { eye[0]-P[0], eye[1]-P[1], eye[2]-P[2] };
                    float s[3] = { dir[1]*toe[2]-dir[2]*toe[1], dir[2]*toe[0]-dir[0]*toe[2], dir[0]*toe[1]-dir[1]*toe[0] };
                    const float len = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
                    const float hw = (len > 1e-4f) ? (w*0.5f/len) : 0.0f;
                    s[0]*=hw; s[1]*=hw; s[2]*=hw;
                    strip[i*2+0] = { P[0]-s[0], P[1]-s[1], P[2]-s[2], col };
                    strip[i*2+1] = { P[0]+s[0], P[1]+s[1], P[2]+s[2], col };
                }
                dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, np*2 - 2, strip, sizeof(V));
            }
            else if (type == 3)
            {
                if (pos + 16 > end) break;
                uint32_t col, texPtr, n; float w;
                memcpy(&col, b+pos, 4); memcpy(&w, b+pos+4, 4); memcpy(&texPtr, b+pos+8, 4); memcpy(&n, b+pos+12, 4);
                pos += 16;
                if (n < 2 || n > (end - pos) / 12) break;
                const float* pts = reinterpret_cast<const float*>(b + pos);
                pos += n*12;
                const uint32_t np = (n > static_cast<uint32_t>(kMaxRibPts)) ? kMaxRibPts : n;
                { bool ok = Finite(w); for (uint32_t i = 0; ok && i < np*3; ++i) ok = Finite(pts[i]); if (!ok) continue; }
                for (uint32_t i = 0; i < np; ++i)
                {
                    const float* P  = pts + i*3;
                    const float* Pp = pts + (i==0 ? 0 : i-1)*3;
                    const float* Pn = pts + (i+1>=np ? np-1 : i+1)*3;
                    const float dir[3] = { Pn[0]-Pp[0], Pn[1]-Pp[1], Pn[2]-Pp[2] };
                    const float toe[3] = { eye[0]-P[0], eye[1]-P[1], eye[2]-P[2] };
                    float s[3] = { dir[1]*toe[2]-dir[2]*toe[1], dir[2]*toe[0]-dir[0]*toe[2], dir[0]*toe[1]-dir[1]*toe[0] };
                    const float len = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
                    const float dpt = sqrtf(toe[0]*toe[0]+toe[1]*toe[1]+toe[2]*toe[2]);
                    const float halfW = (pxFactor > 1e-3f) ? (w * dpt / pxFactor) : (w * 0.02f);
                    const float hw = (len > 1e-4f) ? (halfW / len) : 0.0f;
                    s[0]*=hw; s[1]*=hw; s[2]*=hw;
                    const float u = (np > 1) ? (static_cast<float>(i) / static_cast<float>(np-1)) : 0.0f;
                    tstrip[i*2+0] = { P[0]-s[0], P[1]-s[1], P[2]-s[2], col, u, 0.0f };
                    tstrip[i*2+1] = { P[0]+s[0], P[1]+s[1], P[2]+s[2], col, u, 0.5f };
                }
                if (!texOK(texPtr)) continue;
                const int need = static_cast<int>(np - 1) * 6;
                if (batchStage != 0 || batchTex != texPtr || batchFlags != flags || batchN + need > kBatchMax) flush();
                batchTex = texPtr; batchFlags = flags; batchStage = 0;
                for (uint32_t i = 0; i + 1 < np && batchN + 6 <= kBatchMax; ++i)
                {
                    const VT A = tstrip[i*2+0], B = tstrip[i*2+1], Cc = tstrip[(i+1)*2+0], D = tstrip[(i+1)*2+1];
                    batch[batchN++] = A;  batch[batchN++] = B;  batch[batchN++] = Cc;
                    batch[batchN++] = Cc; batch[batchN++] = B;  batch[batchN++] = D;
                }
            }
            else if (type == 4)
            {
                if (pos + 4 > end) break;
                uint32_t vc = 0; memcpy(&vc, b+pos, 4); pos += 4;
                if (vc < 3 || (vc % 3) != 0 || vc > (end - pos) / 16) break;
                const V* verts = reinterpret_cast<const V*>(b + pos);
                pos += vc * 16;
                { bool ok = true; for (uint32_t k = 0; ok && k < vc; ++k) ok = Finite(verts[k].x) && Finite(verts[k].y) && Finite(verts[k].z); if (!ok) continue; }
                dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, vc / 3, verts, sizeof(V));
            }
            else if (type == 5)
            {
                if (pos + 8 > end) break;
                uint32_t texPtr = 0, vc = 0;
                memcpy(&texPtr, b+pos, 4); memcpy(&vc, b+pos+4, 4); pos += 8;
                if (vc < 3 || (vc % 3) != 0 || vc > (end - pos) / 24) break;
                const VT* sv = reinterpret_cast<const VT*>(b + pos);
                pos += vc * 24;
                { bool ok = true; for (uint32_t k = 0; ok && k < vc; ++k) ok = Finite(sv[k].x) && Finite(sv[k].y) && Finite(sv[k].z); if (!ok) continue; }
                if (!texOK(texPtr)) continue;
                if (batchStage != 1 || batchTex != texPtr || batchFlags != flags || batchN + static_cast<int>(vc) > kBatchMax) flush();
                batchTex = texPtr; batchFlags = flags; batchStage = 1;
                for (uint32_t k = 0; k < vc && batchN < kBatchMax; ++k) batch[batchN++] = sv[k];
            }
            else if (type == 6)
            {
                if (pos + 32 > end) break;
                uint32_t fontId = 0, col = 0, align = 0, gc = 0; float size = 0.0f, anc[3];
                memcpy(&fontId, b+pos, 4); memcpy(&col, b+pos+4, 4); memcpy(&size, b+pos+8, 4);
                memcpy(anc, b+pos+12, 12); memcpy(&align, b+pos+24, 4); memcpy(&gc, b+pos+28, 4);
                pos += 32;
                if (gc > end - pos) break;
                const uint8_t* glyphs = b + pos;
                pos += gc;
                const Font* fnt = this->FindFont(fontId);
                if (fnt == nullptr || !texOK(fnt->tex) || gc == 0) continue;
                if (!Finite3(anc) || !Finite(size)) continue;
                const float adv = size * fnt->advance;
                const float du  = 1.0f / static_cast<float>(fnt->cols);
                const float dv  = 1.0f / static_cast<float>(fnt->rows);
                const uint32_t cells = fnt->cols * fnt->rows;
                const float total = static_cast<float>(gc) * adv;
                float startx;
                if (align == 1)      startx = adv * 0.5f;
                else if (align == 2) startx = -total + adv * 0.5f;
                else                 startx = -total * 0.5f + adv * 0.5f;
                auto mkv = [&](float e, float uo, float uu, float vv, D3DCOLOR c) -> VT {
                    return VT{ anc[0]+rx*e+ux*uo, anc[1]+ry*e+uy*uo, anc[2]+rz*e+uz*uo, c, uu, vv };
                };
                auto emit = [&](float ox, float oy, D3DCOLOR gcol)
                {
                    for (uint32_t gi = 0; gi < gc; ++gi)
                    {
                        if (batchN + 6 > kBatchMax) { flush(); batchTex = fnt->tex; batchFlags = flags; batchStage = 1; }
                        uint32_t cell = (glyphs[gi] >= fnt->firstCp) ? (glyphs[gi] - fnt->firstCp) : 0;
                        if (cell >= cells) cell = (63u >= fnt->firstCp && (63u - fnt->firstCp) < cells) ? (63u - fnt->firstCp) : 0;
                        const float u0 = static_cast<float>(cell % fnt->cols) * du, u1 = u0 + du;
                        const float v0 = static_cast<float>(cell / fnt->cols) * dv, v1 = v0 + dv;
                        const float cx = startx + static_cast<float>(gi) * adv + ox;
                        const float hl = cx - adv * 0.5f, hr = cx + adv * 0.5f;
                        const float yt = size + oy, yb = oy;
                        const VT tl = mkv(hl, yt, u0, v0, gcol), tr = mkv(hr, yt, u1, v0, gcol);
                        const VT br = mkv(hr, yb, u1, v1, gcol), bl = mkv(hl, yb, u0, v1, gcol);
                        batch[batchN++] = tl; batch[batchN++] = tr; batch[batchN++] = br;
                        batch[batchN++] = tl; batch[batchN++] = br; batch[batchN++] = bl;
                    }
                };
                if (batchStage != 1 || batchTex != fnt->tex || batchFlags != flags) flush();
                batchTex = fnt->tex; batchFlags = flags; batchStage = 1;
                if (flags & 0x100u)
                {
                    const D3DCOLOR ocol = col & 0xFF000000u;
                    const uint32_t olvl = (flags >> 9) & 0x0Fu;
                    const float o = size * ((olvl == 0u) ? kOutlineFrac : static_cast<float>(olvl) * 0.01f);
                    for (int s = 0; s < 16; ++s)
                    {
                        const float ang = (6.28318531f * static_cast<float>(s)) / 16.0f;
                        emit(o * cosf(ang), o * sinf(ang), ocol);
                    }
                }
                emit(0.0f, 0.0f, col);
            }
            else if (type == 7)
            {
                if (pos + 28 > end) break;
                uint32_t texPtr = 0, col = 0; float c[3], hw = 0.0f, hh = 0.0f;
                memcpy(&texPtr, b+pos, 4); memcpy(&col, b+pos+4, 4); memcpy(c, b+pos+8, 12);
                memcpy(&hw, b+pos+20, 4); memcpy(&hh, b+pos+24, 4);
                pos += 28;
                prep(c, hw, hh, flags);
                quad(c, hw, hh, col, col, col, col, texPtr, flags);
            }
            else if (type == 8)
            {
                if (pos + 36 > end) break;
                uint32_t texPtr = 0, bg = 0, fc = 0; float c[3], hw = 0.0f, hh = 0.0f, frac = 0.0f;
                memcpy(&texPtr, b+pos, 4); memcpy(&bg, b+pos+4, 4); memcpy(&fc, b+pos+8, 4);
                memcpy(c, b+pos+12, 12); memcpy(&hw, b+pos+24, 4); memcpy(&hh, b+pos+28, 4); memcpy(&frac, b+pos+32, 4);
                pos += 36;
                if (!Finite3(c) || !Finite(hw) || !Finite(hh) || !Finite(frac)) continue;
                if (texPtr != 0 && !texOK(texPtr)) continue;
                if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;
                prep(c, hw, hh, flags);
                quad(c, hw, hh, bg, bg, bg, bg, texPtr, flags);
                if (frac > 0.0001f)
                {
                    const float fw = hw * frac;
                    const float off = -hw + fw;
                    float fcen[3] = { c[0]+rx*off, c[1]+ry*off, c[2]+rz*off };
                    const float toe[3] = { eye[0]-fcen[0], eye[1]-fcen[1], eye[2]-fcen[2] };
                    const float d = sqrtf(toe[0]*toe[0]+toe[1]*toe[1]+toe[2]*toe[2]);
                    if (d > 1e-4f) { const float k = kBarFillNudge / d; fcen[0]+=toe[0]*k; fcen[1]+=toe[1]*k; fcen[2]+=toe[2]*k; } // nudge fill in front of bg
                    quad(fcen, fw, hh, fc, fc, fc, fc, texPtr, flags);
                }
            }
            else if (type == 9)
            {
                if (pos + 28 > end) break;
                uint32_t col = 0; float c[3], hw = 0.0f, hh = 0.0f, rad = 0.0f;
                memcpy(&col, b+pos, 4); memcpy(c, b+pos+4, 12); memcpy(&hw, b+pos+16, 4); memcpy(&hh, b+pos+20, 4); memcpy(&rad, b+pos+24, 4);
                pos += 28;
                if (!Finite3(c) || !Finite(hw) || !Finite(hh) || !Finite(rad)) continue;
                prep(c, hw, hh, flags);
                if (rad < 0.0f) rad = 0.0f; const float mr = (hw < hh ? hw : hh); if (rad > mr) rad = mr;
                flush();
                colState(flags);
                const int SEG = 4;
                const float HALFPI = 1.57079633f, PIc = 3.14159265f;
                const float ix = hw - rad, iy = hh - rad;
                const float ccx[4] = { ix, -ix, -ix, ix }, ccy[4] = { iy, iy, -iy, -iy };
                const float a0[4]  = { 0.0f, HALFPI, PIc, PIc + HALFPI };
                float bx[4 * (SEG + 1)], by[4 * (SEG + 1)]; int nb = 0;
                for (int cc = 0; cc < 4; ++cc)
                    for (int s = 0; s <= SEG; ++s) { const float a = a0[cc] + HALFPI * (static_cast<float>(s)/SEG); bx[nb] = ccx[cc] + rad*cosf(a); by[nb] = ccy[cc] + rad*sinf(a); ++nb; }
                V fan[3 * 4 * (SEG + 1)]; int fv = 0;
                for (int i = 0; i < nb; ++i)
                {
                    const int j = (i + 1) % nb;
                    fan[fv++] = V{ c[0], c[1], c[2], col };
                    fan[fv++] = V{ c[0]+rx*bx[i]+ux*by[i], c[1]+ry*bx[i]+uy*by[i], c[2]+rz*bx[i]+uz*by[i], col };
                    fan[fv++] = V{ c[0]+rx*bx[j]+ux*by[j], c[1]+ry*bx[j]+uy*by[j], c[2]+rz*bx[j]+uz*by[j], col };
                }
                dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, fv / 3, fan, sizeof(V));
            }
            else if (type == 10)
            {
                if (pos + 32 > end) break;
                uint32_t top = 0, bot = 0, texPtr = 0; float c[3], hw = 0.0f, hh = 0.0f;
                memcpy(&top, b+pos, 4); memcpy(&bot, b+pos+4, 4); memcpy(&texPtr, b+pos+8, 4);
                memcpy(c, b+pos+12, 12); memcpy(&hw, b+pos+24, 4); memcpy(&hh, b+pos+28, 4);
                pos += 32;
                prep(c, hw, hh, flags);
                quad(c, hw, hh, top, top, bot, bot, texPtr, flags);
            }
            else if (type == 11)
            {
                if (pos + 48 > end) break;
                uint32_t texPtr = 0, col = 0; float c[3], hw = 0, hh = 0, rot = 0, uv[4];
                memcpy(&texPtr,b+pos,4); memcpy(&col,b+pos+4,4); memcpy(c,b+pos+8,12);
                memcpy(&hw,b+pos+20,4); memcpy(&hh,b+pos+24,4); memcpy(&rot,b+pos+28,4); memcpy(uv,b+pos+32,16);
                pos += 48;
                if (!Finite3(c) || !Finite(hw) || !Finite(hh) || !Finite(rot)) continue; // skip NaN/Inf
                prep(c, hw, hh, flags);
                const float ca = cosf(rot), sa = sinf(rot);
                const float rrx = view._11*ca + view._12*sa, rry = view._21*ca + view._22*sa, rrz = view._31*ca + view._32*sa;
                const float rux = -view._11*sa + view._12*ca, ruy = -view._21*sa + view._22*ca, ruz = -view._31*sa + view._32*ca;
                float cr[4][3];
                const float sw[4] = { -hw, hw, hw, -hw }, sh[4] = { hh, hh, -hh, -hh };
                for (int k = 0; k < 4; ++k) { cr[k][0]=c[0]+rrx*sw[k]+rux*sh[k]; cr[k][1]=c[1]+rry*sw[k]+ruy*sh[k]; cr[k][2]=c[2]+rrz*sw[k]+ruz*sh[k]; }
                const float us[4] = { uv[0], uv[2], uv[2], uv[0] }, vs[4] = { uv[1], uv[1], uv[3], uv[3] };
                if (texPtr != 0)
                {
                    if (!texOK(texPtr)) continue;
                    if (batchStage != 1 || batchTex != texPtr || batchFlags != flags || batchN + 6 > kBatchMax) flush();
                    batchTex = texPtr; batchFlags = flags; batchStage = 1;
                    const VT q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col, us[0],vs[0] }, { cr[1][0],cr[1][1],cr[1][2], col, us[1],vs[1] }, { cr[2][0],cr[2][1],cr[2][2], col, us[2],vs[2] },
                        { cr[0][0],cr[0][1],cr[0][2], col, us[0],vs[0] }, { cr[2][0],cr[2][1],cr[2][2], col, us[2],vs[2] }, { cr[3][0],cr[3][1],cr[3][2], col, us[3],vs[3] },
                    };
                    for (int qi = 0; qi < 6; ++qi) batch[batchN++] = q[qi];
                }
                else
                {
                    flush();
                    colState(flags);
                    const V q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[1][0],cr[1][1],cr[1][2], col }, { cr[2][0],cr[2][1],cr[2][2], col },
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[2][0],cr[2][1],cr[2][2], col }, { cr[3][0],cr[3][1],cr[3][2], col },
                    };
                    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(V));
                }
            }
            else if (type == 12)
            {
                if (pos + 32 > end) break;
                uint32_t col = 0; float c[3], rad = 0, a0 = 0, sweep = 0, inner = 0;
                memcpy(&col,b+pos,4); memcpy(c,b+pos+4,12); memcpy(&rad,b+pos+16,4); memcpy(&a0,b+pos+20,4); memcpy(&sweep,b+pos+24,4); memcpy(&inner,b+pos+28,4);
                pos += 32;
                if (!Finite3(c) || !Finite(rad) || !Finite(a0) || !Finite(sweep) || !Finite(inner)) continue;
                float hw = rad, hh = rad; prep(c, hw, hh, flags); rad = hw;
                flush();
                colState(flags);
                if (inner < 0.0f) inner = 0.0f; if (inner > 0.95f) inner = 0.95f;
                const float ir = rad * inner;
                int SEG = static_cast<int>(fabsf(sweep) / 0.13f) + 1; if (SEG < 2) SEG = 2; if (SEG > 96) SEG = 96;
                static V arcv[96 * 6]; int vc = 0;
                for (int s = 0; s < SEG && vc + 6 <= 96 * 6; ++s)
                {
                    const float t0 = a0 + sweep * (static_cast<float>(s)/SEG), t1 = a0 + sweep * (static_cast<float>(s+1)/SEG);
                    const float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
                    #define O3D_ARCP(cc,ss,r) V{ c[0]+rx*((cc)*(r))+ux*((ss)*(r)), c[1]+ry*((cc)*(r))+uy*((ss)*(r)), c[2]+rz*((cc)*(r))+uz*((ss)*(r)), col }
                    const V o0 = O3D_ARCP(c0,s0,rad), o1 = O3D_ARCP(c1,s1,rad), i0 = O3D_ARCP(c0,s0,ir), i1 = O3D_ARCP(c1,s1,ir);
                    #undef O3D_ARCP
                    arcv[vc++]=i0; arcv[vc++]=o0; arcv[vc++]=o1;
                    arcv[vc++]=i0; arcv[vc++]=o1; arcv[vc++]=i1;
                }
                if (vc >= 3) dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, vc / 3, arcv, sizeof(V));
            }
            else if (type == 13)
            {
                if (pos + 32 > end) break;
                uint32_t texPtr = 0, col = 0; float c[3], hw = 0, hh = 0, rot = 0;
                memcpy(&texPtr,b+pos,4); memcpy(&col,b+pos+4,4); memcpy(c,b+pos+8,12); memcpy(&hw,b+pos+20,4); memcpy(&hh,b+pos+24,4); memcpy(&rot,b+pos+28,4);
                pos += 32;
                if (!Finite3(c) || !Finite(hw) || !Finite(hh) || !Finite(rot)) continue;
                const float ca = cosf(rot), sa = sinf(rot);
                float cr[4][3];
                const float sw[4] = { -hw, hw, hw, -hw }, sh[4] = { hh, hh, -hh, -hh };
                for (int k = 0; k < 4; ++k) { cr[k][0]=c[0]+ca*sw[k]+(-sa)*sh[k]; cr[k][1]=c[1]; cr[k][2]=c[2]+sa*sw[k]+ca*sh[k]; }
                if (texPtr != 0)
                {
                    if (!texOK(texPtr)) continue;
                    if (batchStage != 1 || batchTex != texPtr || batchFlags != flags || batchN + 6 > kBatchMax) flush();
                    batchTex = texPtr; batchFlags = flags; batchStage = 1;
                    const VT q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col,0,0 }, { cr[1][0],cr[1][1],cr[1][2], col,1,0 }, { cr[2][0],cr[2][1],cr[2][2], col,1,1 },
                        { cr[0][0],cr[0][1],cr[0][2], col,0,0 }, { cr[2][0],cr[2][1],cr[2][2], col,1,1 }, { cr[3][0],cr[3][1],cr[3][2], col,0,1 },
                    };
                    for (int qi = 0; qi < 6; ++qi) batch[batchN++] = q[qi];
                }
                else
                {
                    flush();
                    colState(flags);
                    const V q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[1][0],cr[1][1],cr[1][2], col }, { cr[2][0],cr[2][1],cr[2][2], col },
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[2][0],cr[2][1],cr[2][2], col }, { cr[3][0],cr[3][1],cr[3][2], col },
                    };
                    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(V));
                }
            }
            else if (type == 14)
            {
                if (pos + 36 > end) break;
                uint32_t texPtr = 0, col = 0; float c[3], hw = 0, hh = 0, bf = 0, bwld = 0;
                memcpy(&texPtr,b+pos,4); memcpy(&col,b+pos+4,4); memcpy(c,b+pos+8,12); memcpy(&hw,b+pos+20,4); memcpy(&hh,b+pos+24,4); memcpy(&bf,b+pos+28,4); memcpy(&bwld,b+pos+32,4);
                pos += 36;
                if (!Finite3(c) || !Finite(hw) || !Finite(hh)) continue;
                if (bf < 0.0f) bf = 0.0f; else if (bf > 0.5f) bf = 0.5f;
                prep(c, hw, hh, flags);
                if (!texOK(texPtr)) continue;
                if (bwld > hw) bwld = hw; if (bwld > hh) bwld = hh;
                const float xs[4] = { -hw, -hw+bwld, hw-bwld, hw };
                const float ys[4] = {  hh,  hh-bwld, -hh+bwld, -hh };
                const float us[4] = { 0.0f, bf, 1.0f-bf, 1.0f };
                const float vs[4] = { 0.0f, bf, 1.0f-bf, 1.0f };
                if (batchStage != 1 || batchTex != texPtr || batchFlags != flags) flush();
                batchTex = texPtr; batchFlags = flags; batchStage = 1;
                #define O3D_NSV(xx,yy,uu,vv) VT{ c[0]+rx*(xx)+ux*(yy), c[1]+ry*(xx)+uy*(yy), c[2]+rz*(xx)+uz*(yy), col, (uu),(vv) }
                for (int gy = 0; gy < 3; ++gy) for (int gx = 0; gx < 3; ++gx)
                {
                    if (batchN + 6 > kBatchMax) { flush(); batchTex = texPtr; batchFlags = flags; batchStage = 1; }
                    const VT tl = O3D_NSV(xs[gx],ys[gy],us[gx],vs[gy]),   tr = O3D_NSV(xs[gx+1],ys[gy],us[gx+1],vs[gy]);
                    const VT br = O3D_NSV(xs[gx+1],ys[gy+1],us[gx+1],vs[gy+1]), bl = O3D_NSV(xs[gx],ys[gy+1],us[gx],vs[gy+1]);
                    batch[batchN++]=tl; batch[batchN++]=tr; batch[batchN++]=br;
                    batch[batchN++]=tl; batch[batchN++]=br; batch[batchN++]=bl;
                }
                #undef O3D_NSV
            }
            else if (type == 15)
            {
                if (pos + 44 > end) break;
                uint32_t texPtr = 0, col = 0; float c[3], hw = 0, hh = 0, vel[3], fade = 0;
                memcpy(&texPtr,b+pos,4); memcpy(&col,b+pos+4,4); memcpy(c,b+pos+8,12); memcpy(&hw,b+pos+20,4); memcpy(&hh,b+pos+24,4); memcpy(vel,b+pos+28,12); memcpy(&fade,b+pos+40,4);
                pos += 44;
                uint32_t ttl = 0; memcpy(&ttl, b+8, 4);
                const float el = (ttl > 0) ? (static_cast<float>(ttl) - static_cast<float>(o.expire - now)) : 0.0f;
                const float els = el * 0.001f;
                c[0]+=vel[0]*els; c[1]+=vel[1]*els; c[2]+=vel[2]*els;
                float a = 1.0f;
                if (ttl > 0 && fade >= 0.0f && fade < 1.0f) { const float pr = el / static_cast<float>(ttl); if (pr > fade) a = 1.0f - (pr - fade) / (1.0f - fade); }
                if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
                const uint32_t fc = (col & 0x00FFFFFFu) | (static_cast<uint32_t>(static_cast<float>((col >> 24) & 0xFFu) * a) << 24);
                prep(c, hw, hh, flags);
                quad(c, hw, hh, fc, fc, fc, fc, texPtr, flags);
            }
            else { flush(); break; }
        }
        if (it < items) { this->m_DropBadItem++; this->WarnDrop(2, "truncated or unknown item -- buffer parse aborted", o.id); }
    }
    flush();
}

void occlude3d::ResetStatAccumulators(void)
{
    this->m_StatFrames = 0; this->m_HookMs = 0.0; this->m_EventMs = 0.0;
    this->m_ItemsAcc = 0.0; this->m_OwnersAcc = 0.0; this->m_BytesAcc = 0.0; this->m_StateAcc = 0.0;
}

void occlude3d::RenderHookDraw(void)
{
    if (!this->HasGeometry())
        return;

    IDirect3DDevice8* dev = this->m_Device;
    if (dev == nullptr)
        return;

    __try
    {
        this->RenderHookDrawInner(dev);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void occlude3d::RenderHookDrawInner(IDirect3DDevice8* dev)
{
    D3DMATRIX oldWorld;
    dev->GetTransform(D3DTS_WORLD, &oldWorld);

    DWORD oldFvf = 0;
    dev->GetVertexShader(&oldFvf);
    IDirect3DVertexBuffer8* oldVb = nullptr; UINT oldStride = 0;
    dev->GetStreamSource(0, &oldVb, &oldStride);
    IDirect3DIndexBuffer8* oldIb = nullptr; UINT oldBase = 0;
    dev->GetIndices(&oldIb, &oldBase);
    IDirect3DBaseTexture8* oldTex = nullptr;
    dev->GetTexture(0, &oldTex);

    DWORD rsL, rsZ, rsZw, rsZf, rsFog, rsAb, rsAt, rsCull;
    dev->GetRenderState(D3DRS_LIGHTING,         &rsL);
    dev->GetRenderState(D3DRS_ZENABLE,          &rsZ);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &rsZw);
    dev->GetRenderState(D3DRS_ZFUNC,            &rsZf);
    dev->GetRenderState(D3DRS_FOGENABLE,        &rsFog);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &rsAb);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE,  &rsAt);
    dev->GetRenderState(D3DRS_CULLMODE,         &rsCull);
    DWORD rsSrc, rsDest, rsARef, rsAFunc;
    dev->GetRenderState(D3DRS_SRCBLEND,  &rsSrc);
    dev->GetRenderState(D3DRS_DESTBLEND, &rsDest);
    dev->GetRenderState(D3DRS_ALPHAREF,  &rsARef);
    dev->GetRenderState(D3DRS_ALPHAFUNC, &rsAFunc);
    DWORD tsCop, tsCa1, tsCa2, tsAop, tsAa1, tsAa2, tsCop1;
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &tsCop);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &tsCa1);
    dev->GetTextureStageState(0, D3DTSS_COLORARG2, &tsCa2);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &tsAop);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &tsAa1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &tsAa2);
    dev->GetTextureStageState(1, D3DTSS_COLOROP,   &tsCop1);

    const D3DMATRIX ident = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    dev->SetRenderState(D3DRS_ZENABLE,          TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ZFUNC,            D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_FOGENABLE,        FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
    dev->SetRenderState(D3DRS_ALPHAREF,         0x40);
    dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
    dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);

    LARGE_INTEGER _ds; const bool _st = this->StatsOn();
    if (_st) QueryPerformanceCounter(&_ds);
    this->DrawSubmittedGuarded(dev);
    if (_st && this->m_QpcFreq > 0)
    {
        LARGE_INTEGER _de; QueryPerformanceCounter(&_de);
        this->m_HookMs    += static_cast<double>(_de.QuadPart - _ds.QuadPart) * 1000.0 / static_cast<double>(this->m_QpcFreq);
        this->m_ItemsAcc  += this->m_FrameItems;
        this->m_OwnersAcc += this->m_FrameOwners;
        this->m_StateAcc  += this->m_FrameState;
        this->m_StatFrames++;
    }

    dev->SetTransform(D3DTS_WORLD, &oldWorld);
    dev->SetRenderState(D3DRS_LIGHTING,         rsL);
    dev->SetRenderState(D3DRS_ZENABLE,          rsZ);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     rsZw);
    dev->SetRenderState(D3DRS_ZFUNC,            rsZf);
    dev->SetRenderState(D3DRS_FOGENABLE,        rsFog);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, rsAb);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE,  rsAt);
    dev->SetRenderState(D3DRS_CULLMODE,         rsCull);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   tsCop);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, tsCa1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, tsCa2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   tsAop);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, tsAa1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, tsAa2);
    dev->SetTextureStageState(1, D3DTSS_COLOROP,   tsCop1);
    dev->SetRenderState(D3DRS_SRCBLEND,  rsSrc);
    dev->SetRenderState(D3DRS_DESTBLEND, rsDest);
    dev->SetRenderState(D3DRS_ALPHAREF,  rsARef);
    dev->SetRenderState(D3DRS_ALPHAFUNC, rsAFunc);
    dev->SetVertexShader(oldFvf);
    dev->SetStreamSource(0, oldVb, oldStride);
    dev->SetIndices(oldIb, oldBase);
    dev->SetTexture(0, oldTex);
    if (oldVb != nullptr)  oldVb->Release();
    if (oldIb != nullptr)  oldIb->Release();
    if (oldTex != nullptr) oldTex->Release();

    // Diagnostics report
    if (this->StatsOn() && this->m_StatFrames >= 60)
    {
        const double n = static_cast<double>(this->m_StatFrames);
        this->m_DispItems  = this->m_ItemsAcc / n; this->m_DispOwners = this->m_OwnersAcc / n;
        this->m_DispState  = this->m_StateAcc / n; this->m_DispDrawMs = this->m_HookMs / n;
        this->m_DispRecvMs = this->m_EventMs / n;  this->m_DispBytes  = this->m_BytesAcc / n;
        this->m_HistDrawMs[this->m_HistPos] = static_cast<float>(this->m_DispDrawMs);
        this->m_HistItems[this->m_HistPos]  = static_cast<float>(this->m_DispItems);
        this->m_HistPos = (this->m_HistPos + 1) % kHist;
        if (this->m_Stats && this->m_LogManager != nullptr)
            this->m_LogManager->Logf(static_cast<uint32_t>(Ashita::LogLevel::Info), "occlude3d",
                "STATS avg/frame over %u: items=%.0f owners=%.1f stateCalls=%.0f | drawCPU=%.3f ms  recvCPU=%.3f ms  recvBytes=%.0f",
                this->m_StatFrames, this->m_DispItems, this->m_DispOwners, this->m_DispState, this->m_DispDrawMs, this->m_DispRecvMs, this->m_DispBytes);
        this->ResetStatAccumulators();
    }
}

void occlude3d::Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*)
{
    if (!this->m_Announced)
    {
        this->m_Announced = true;
        this->WriteChat("\x1E\x02" "/o3d" "\x1E\x6A" " opens the status panel, "
                        "\x1E\x02" "/o3d hook" "\x1E\x6A" " toggles the occlusion hook.");
    }

    if (this->m_UiOpen)
        this->RenderUI();
}

// /o3d dashboard
void occlude3d::RenderUI(void)
{
    IGuiManager* g = (this->m_AshitaCore != nullptr) ? this->m_AshitaCore->GetGuiManager() : nullptr;
    if (g == nullptr)
        return;

    const ImVec4 cGreen(0.40f, 0.85f, 0.45f, 1.0f);
    const ImVec4 cAmber(0.95f, 0.65f, 0.20f, 1.0f);
    const ImVec4 cRed  (0.90f, 0.32f, 0.32f, 1.0f);

    g->SetNextWindowSize(ImVec2(400.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!g->Begin("occlude3d", &this->m_UiOpen, 0)) { g->End(); return; }

    const uint32_t now   = GetTickCount();
    const uint32_t drops = this->m_DropOversize + this->m_DropNoSlot + this->m_DropBadItem + this->m_DropBadTex;

    g->Text("occlude3d  v%.1f", static_cast<double>(OCCLUDE3D_VERSION));
    g->SameLine(0.0f, 14.0f);
    if (this->m_Hooked) g->TextColored(cGreen, "hook installed"); else g->TextColored(cRed, "hook OFF");
    char htip[112];
    _snprintf_s(htip, sizeof(htip), _TRUNCATE, "OnMove @ 0x%08X\nsignature matches: %d (expect 1)",
        static_cast<uint32_t>(this->m_NameDrawAddr), this->m_HookMatches);
    g->SetItemTooltip("%s", htip);

    const char* dropsTip =
        "Rejected, never silent:\n"
        "oversize - over 16 KB\n"
        "no-slot - over 32 sources\n"
        "bad-item - malformed data\n"
        "bad-tex - unreadable texture ptr (per-frame)";
    if (drops == 0)
    {
        g->TextColored(cGreen, "no drops");
        g->SetItemTooltip("%s", dropsTip);
    }
    else
    {
        char dfc[5]; FourCC(this->m_LastDropOwner, dfc);
        g->PushStyleColor(ImGuiCol_Text, cRed);
        g->Text("DROPS:  oversize %u   no-slot %u   bad-item %u   bad-tex %u   (last '%s')",
            this->m_DropOversize, this->m_DropNoSlot, this->m_DropBadItem, this->m_DropBadTex, dfc);
        g->PopStyleColor(1);
        g->SetItemTooltip("%s", dropsTip);
        if (g->SmallButton("reset drops"))
            { this->m_DropOversize = 0; this->m_DropNoSlot = 0; this->m_DropBadItem = 0; this->m_DropBadTex = 0; this->m_LastDropOwner = 0; }
    }
    g->Spacing();

    int usedOwners = 0; uint32_t usedBytes = 0;
    for (int i = 0; i < kMaxOwners; ++i)
        if (this->m_Owners[i].used && TickAlive(now, this->m_Owners[i].expire)) { usedOwners++; usedBytes += this->m_Owners[i].size; }
    int usedFonts = 0;
    for (int i = 0; i < kMaxFonts; ++i) if (this->m_Fonts[i].used) usedFonts++;
    const uint32_t totalBytes = static_cast<uint32_t>(sizeof(this->m_Owners));

    auto bar = [&](float frac, const char* overlay, const char* tip) {
        if (frac < 0.0f) frac = 0.0f; if (frac > 1.0f) frac = 1.0f;
        const ImVec4 c = (frac < 0.6f) ? cGreen : (frac < 0.85f) ? cAmber : cRed;
        g->PushStyleColor(ImGuiCol_PlotHistogram, c);
        g->ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
        g->PopStyleColor(1);
        if (tip) g->SetItemTooltip("%s", tip);
    };
    g->Spacing();
    char ov[64];
    _snprintf_s(ov, sizeof(ov), _TRUNCATE, "owners   %d / %d", usedOwners, kMaxOwners);
    bar(static_cast<float>(usedOwners) / static_cast<float>(kMaxOwners), ov,
        "Submission sources in use. Max 32; extras are dropped.");
    _snprintf_s(ov, sizeof(ov), _TRUNCATE, "buffers   %u / %u KB", usedBytes / 1024u, totalBytes / 1024u);
    bar(static_cast<float>(usedBytes) / static_cast<float>(totalBytes), ov,
        "Geometry memory in use. Pool is 512 KB (32 x 16 KB).");
    _snprintf_s(ov, sizeof(ov), _TRUNCATE, "fonts   %d / %d", usedFonts, kMaxFonts);
    bar(static_cast<float>(usedFonts) / static_cast<float>(kMaxFonts), ov,
        "Registered TEXT font atlases. Max 8.");

    g->SeparatorText("Performance (avg/frame)");
    float peakDraw = 0.0f, peakItems = 0.0f;
    for (int i = 0; i < kHist; ++i)
    {
        if (this->m_HistDrawMs[i] > peakDraw) peakDraw = this->m_HistDrawMs[i];
        if (this->m_HistItems[i]  > peakItems) peakItems = this->m_HistItems[i];
    }
    char dov[64], iov[64];
    _snprintf_s(dov, sizeof(dov), _TRUNCATE, "draw %.3f ms   (peak %.3f)", this->m_DispDrawMs, peakDraw);
    _snprintf_s(iov, sizeof(iov), _TRUNCATE, "items %.0f   (peak %.0f)", this->m_DispItems, peakItems);
    g->PlotLines("##drawms", this->m_HistDrawMs, kHist, this->m_HistPos, dov, 0.0f, FLT_MAX, ImVec2(-1.0f, 38.0f));
    g->SetItemTooltip("%s", "Draw CPU per frame (avg; peak over the window).");
    g->PlotLines("##items",  this->m_HistItems,  kHist, this->m_HistPos, iov, 0.0f, FLT_MAX, ImVec2(-1.0f, 38.0f));
    g->SetItemTooltip("%s", "Primitives per frame (avg; peak over the window).");
    const float budget = static_cast<float>(this->m_DispDrawMs) / 16.6667f;
    const ImVec4 bc = (budget < 0.10f) ? cGreen : (budget < 0.25f) ? cAmber : cRed;
    g->Text("frame budget"); g->SameLine();
    g->TextColored(bc, "%.1f%% of a 60 fps frame", budget * 100.0f);
    g->SetItemTooltip("%s", "Draw CPU as a share of one 60 fps frame.");
    g->TextDisabled("recv %.3f ms   state %.0f   bytes %.0f", this->m_DispRecvMs, this->m_DispState, this->m_DispBytes);

    const bool ownersOpen = g->CollapsingHeader("Active owners", ImGuiTreeNodeFlags_DefaultOpen);
    g->SetItemTooltip("%s", "owner - source id\nitems - primitive count\nbytes - buffer used\nttl ms - time left before it expires");
    if (ownersOpen)
    {
        if (g->BeginTable("##owners", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            g->TableSetupColumn("owner"); g->TableSetupColumn("items");
            g->TableSetupColumn("bytes"); g->TableSetupColumn("ttl ms");
            g->TableHeadersRow();
            int shown = 0;
            for (int i = 0; i < kMaxOwners; ++i)
            {
                SubOwner& o = this->m_Owners[i];
                if (!o.used || !TickAlive(now, o.expire)) continue;
                char fc[5]; FourCC(o.id, fc);
                uint32_t items = 0; if (o.size >= 16) memcpy(&items, o.buf + 12, 4);
                g->TableNextRow();
                g->TableNextColumn(); g->Text("%s", fc);
                g->TableNextColumn(); g->Text("%u", items);
                g->TableNextColumn(); g->Text("%u", o.size);
                g->TableNextColumn(); g->Text("%u", o.expire - now);
                shown++;
            }
            g->EndTable();
            if (shown == 0) g->TextDisabled("(no active submissions)");
        }
    }

    const bool fontsOpen = g->CollapsingHeader("Fonts");
    g->SetItemTooltip("%s", "TEXT font atlases. Max 8.\ngrid - cols x rows\nfirst cp - first codepoint\nadvance - glyph width / height");
    if (fontsOpen)
    {
        int shown = 0;
        for (int i = 0; i < kMaxFonts; ++i)
        {
            Font& f = this->m_Fonts[i];
            if (!f.used) continue;
            char fc[5]; FourCC(f.id, fc);
            g->BulletText("'%s'   %ux%u grid   first cp %u   advance %.2f", fc, f.cols, f.rows, f.firstCp, f.advance);
            shown++;
        }
        if (shown == 0) g->TextDisabled("(no fonts registered)");
    }

    g->SeparatorText("Controls");
    auto toggleBtn = [&](const char* label, bool on, float w) -> bool {
        const ImVec4 onC (0.20f, 0.50f, 0.28f, 1.0f), onH(0.26f, 0.60f, 0.34f, 1.0f), onA(0.32f, 0.70f, 0.40f, 1.0f);
        const ImVec4 offC(0.24f, 0.24f, 0.27f, 1.0f), offH(0.32f, 0.32f, 0.36f, 1.0f), offA(0.40f, 0.40f, 0.44f, 1.0f);
        g->PushStyleColor(ImGuiCol_Button,        on ? onC : offC);
        g->PushStyleColor(ImGuiCol_ButtonHovered, on ? onH : offH);
        g->PushStyleColor(ImGuiCol_ButtonActive,  on ? onA : offA);
        const bool clicked = g->Button(label, ImVec2(w, 0.0f));
        g->PopStyleColor(3);
        return clicked;
    };
    const float bw = (g->GetContentRegionAvail().x - 8.0f) / 2.0f;
    char lbl[32];
    _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Hook %s##hk", this->m_Hooked ? "ON" : "OFF");
    if (toggleBtn(lbl, this->m_Hooked, bw))
    {
        if (this->m_Hooked) this->RemoveHook();
        else { const uintptr_t a = this->ResolveNameDraw(); this->InstallHook(a); }
    }
    g->SetItemTooltip("%s", "Toggle the occlusion hook.");
    g->SameLine();
    _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Stats %s##ls", this->m_Stats ? "ON" : "OFF");
    if (toggleBtn(lbl, this->m_Stats, bw)) this->m_Stats = !this->m_Stats;
    g->SetItemTooltip("%s", "Log perf numbers to the Ashita log.");

    g->End();
}


extern "C"
{
    __declspec(noinline) IPlugin* __stdcall expCreatePlugin(const char* args)
    {
        UNREFERENCED_PARAMETER(args);
        try { return new occlude3d(); }
        catch (...) { return nullptr; }
    }

    __declspec(noinline) void __stdcall expDestroyPlugin(void* instance)
    {
        if (instance != nullptr)
            delete static_cast<occlude3d*>(instance);
    }

    __declspec(noinline) double __stdcall expGetInterfaceVersion(void)
    {
        return ASHITA_INTERFACE_VERSION;
    }
}
