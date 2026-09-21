// Render submitted world geometry in CXiActorNameDraw::OnMove while world depth is bound.
#include "occlude3d.hpp"
#include "unload.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <string>
#include <vector>
#include <cstdarg>

#define OCCLUDE3D_VERSION  1.1

#define OCCLUDE3D_SENTINEL "occlude3d"

// Replace the five-byte mov eax,[imm32] prologue with a jump. The detour preserves registers,
// renders, then replays the stolen instruction through a trampoline to entry+5.
namespace
{
    volatile uint32_t g_hookCalls   = 0;
    volatile long     g_inFlight    = 0;         // Active RenderHookDraw calls, including time inside Direct3D.
    void*             g_trampoline  = nullptr;   // Retained for process lifetime; callers may still be inside.
    uintptr_t         g_hookAddr    = 0;
    uint8_t           g_origBytes[5] = { 0 };
    bool              g_installed   = false;     // our JMP is (or may still be) at the entry
    bool              g_retained    = false;     // an unload could not take the hook out: the next load is refused
    bool              g_keepInstance = false;    // a hook call never drained: the instance and its resources are leaked, never freed
    bool              g_pinned      = false;     // this DLL stays mapped until the game closes (pinned before the first write)
    occlude3d*        g_instance    = nullptr;
    plog::FileLog     g_log;                     // Per-character plugin log.
    const char*       levelName(uint32_t level)
    {
        return level == static_cast<uint32_t>(Ashita::LogLevel::Error) ? "error" : level == static_cast<uint32_t>(Ashita::LogLevel::Warn) ? "warn" : "info";
    }
}

// The stub stays pass-through-safe after unload: the instance may be gone (null) and the trampoline is never freed.
static void do_hook_work(void)
{
    g_hookCalls++;
    InterlockedIncrement(&g_inFlight);   // counted before the instance is read, so unload sees every caller that has one
    occlude3d* inst = g_instance;
    if (inst != nullptr)
        inst->RenderHookDraw();
    InterlockedDecrement(&g_inFlight);
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
    , m_Legacy(false)
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
    , m_FrameDraws(0)
    , m_DrawsAcc(0.0)
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
    , m_DispDraws(0.0)
    , m_DispDrawMs(0.0)
    , m_DispRecvMs(0.0)
    , m_DispBytes(0.0)
    , m_HistPos(0)
{
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
    return "https://github.com/SQLCommit/Occlude3d";
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
    this->m_Root = plog::ashitaRoot(reinterpret_cast<const void*>(&hook_OnMove));
    this->m_Run  = plog::thisRun();
    g_log.open(this->m_Root, plog::startupLogPath(this->m_Root, "occlude3d", this->m_Run));
    char ver[16], iface[16];
    _snprintf_s(ver, sizeof ver, _TRUNCATE, "%.1f", OCCLUDE3D_VERSION);
    _snprintf_s(iface, sizeof iface, _TRUNCATE, "%.2f", ASHITA_INTERFACE_VERSION);
    const std::string session = plog::sessionText("occlude3d", ver, plog::ownImageStamp(reinterpret_cast<const void*>(&hook_OnMove)),
                                                  plog::imageStamp(GetModuleHandleA("FFXiMain.dll")), iface, this->m_Run);
    g_log.setSession(session, this->m_Run);
    g_log.write("info", session);
    g_log.start();
    // Refuse a new instance while a retained hook still uses the pinned image.
    if (g_retained)
    {
        this->m_Refused = true;
        this->FollowCharacter();
        this->LogLine(static_cast<uint32_t>(Ashita::LogLevel::Error), "occlude3d", "load refused: the hook from an earlier load is still in (unload could not restore it)");
        char line[300];
        _snprintf_s(line, sizeof line, _TRUNCATE, "an earlier occlude3d could not undo its hook this session; restart the game to load it again. (%s)", this->LogShown().c_str());
        this->WriteChat(line, 0x6A);   // 0x6A: the log path is already in the text, without the "moves" note
        g_log.stop();
        return false;
    }
    g_instance = nullptr;

    if (core != nullptr && core->GetPointerManager() != nullptr)
    {
        core->GetPointerManager()->Add(
            OCCLUDE3D_SENTINEL,
            static_cast<uintptr_t>(OCCLUDE3D_VERSION * 100.0 + 0.5));
    }
    const std::string root = this->m_Root;
    const plog::Run run = this->m_Run;
    g_log.post([root, run]
    {
        plog::deleteFiles(root, { "logs\\occlude3d\\occlude3d.log", "logs\\occlude3d\\occlude3d.log.old" });
        plog::cleanupStartupFiles(root, "occlude3d", run);
    });
    this->FollowCharacter();
    return true;
}

void occlude3d::Release(void)
{
    if (this->m_Refused) { g_log.stop(); return; }
    if (this->m_Shot != 0) this->FinishShot();   // a report still waiting is written with what there is
    for (const auto& r : this->m_Repeats.take())
        g_log.write("warn", r.first + ": " + std::to_string(r.second) + " times this session");
    this->RemoveHook();

    if (g_keepInstance && g_instance == this)   // the same condition expDestroyPlugin uses to skip the delete
    {
        // A hook call may still be drawing with these: they stay alive with the instance.
        g_log.write("warn", "unloaded with a hook call still in flight: instance, textures and fonts are kept, not freed" + plog::runSuffix(this->m_Run));
        if (this->m_AshitaCore != nullptr && this->m_AshitaCore->GetPointerManager() != nullptr)
            this->m_AshitaCore->GetPointerManager()->Delete(OCCLUDE3D_SENTINEL);
        g_log.stop();
        return;
    }

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

    g_log.write("info", std::string(g_retained ? "unloaded; the hook stays in (pass-through) until the game closes" : "unloaded") + plog::runSuffix(this->m_Run));
    // Give the log writer two seconds to stop; retain the DLL if it is still running.
    if (!g_log.stop() && !g_pinned) g_pinned = occ::pinSelf(reinterpret_cast<const void*>(&hook_OnMove));
}

bool occlude3d::Command(int32_t mode, const char* command, bool injected)
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
    this->m_CmdOurs = true;

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
        this->StartShot(1);
    }
    else if (_stricmp(sub, "diag") == 0)
    {
        this->StartShot(2);
    }
    else if (_stricmp(sub, "legacy") == 0)
    {
        const char* v = (args.size() > 2) ? args[2].c_str() : "";
        if (_stricmp(v, "on") == 0)       this->m_Legacy = true;
        else if (_stricmp(v, "off") == 0) this->m_Legacy = false;
        else                              this->m_Legacy = !this->m_Legacy;
        this->ResetStatAccumulators();
    }
    else if (_stricmp(sub, "ui") == 0 || sub[0] == '\0')
    {
        this->m_UiOpen = !this->m_UiOpen;
    }

    {
        char body[160];
        _snprintf_s(body, sizeof(body), _TRUNCATE, "hook=%s, legacy=%s (panel: /o3d).",
            this->m_Hooked ? "ON" : "off", this->m_Legacy ? "ON" : "off");
        this->WriteChat(body);
    }

    return true;
}

bool occlude3d::HandleCommand(int32_t mode, const char* command, bool injected)
{
    this->m_CmdOurs = false;
    try
    {
        return this->Command(mode, command, injected);
    }
    catch (...)
    {
        if (!this->m_CmdOurs) return false;   // it failed before it was known to be ours: leave it to its owner
        g_log.write("error", "HandleCommand: an unexpected error");
        this->WriteChat("the command failed with an unexpected error.", 0x44);
        return true;
    }
}

bool occlude3d::Direct3DInitialize(IDirect3DDevice8* device)
{
    this->m_Device = device;

    this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Info), "occlude3d", "Direct3DInitialize ok (device=0x%p).", static_cast<void*>(device));

    return true;
}

uintptr_t occlude3d::ResolveNameDraw(void)
{
    this->m_HookDetail.clear();
    const uint32_t LL = static_cast<uint32_t>(Ashita::LogLevel::Info);

    HMODULE h = GetModuleHandleA("FFXiMain.dll");
    if (h == nullptr) h = GetModuleHandleA("ffximain.dll");
    if (h == nullptr)
    {
        this->LogLine(LL, "occlude3d", "HOOK: FFXiMain.dll module handle not found.");
        return 0;
    }

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    const IMAGE_NT_HEADERS* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
    const uint8_t* base = reinterpret_cast<const uint8_t*>(h);
    const uint32_t size = nt->OptionalHeader.SizeOfImage;

    this->HookDetail("HOOK: FFXiMain base=0x%08X SizeOfImage=0x%X", reinterpret_cast<uint32_t>(base), size);

    // CXiActorNameDraw::OnMove signature, provided by atom0s.
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
    this->HookDetail("HOOK scan: matches=%d  [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X",
        found, static_cast<uint32_t>(firstAddrs[0]), static_cast<uint32_t>(firstAddrs[1]),
        static_cast<uint32_t>(firstAddrs[2]), static_cast<uint32_t>(firstAddrs[3]));

    if (found != 1)
        this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
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
        this->HookDetail("%s", line);
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
        this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
            "HOOK: prologue at 0x%08X is 0x%02X, expected 0xA1 (mov eax,[imm32]) -- refusing to splice an unexpected instruction.",
            static_cast<uint32_t>(addr), *reinterpret_cast<const uint8_t*>(addr));
        { this->FlushHookDetail(); return false; }
    }

    // Reuse the trampoline for the same entry. Retain old trampolines if the entry changes;
    // a thread may still be executing in one.
    uint8_t* tramp = (g_trampoline != nullptr && g_hookAddr == addr) ? static_cast<uint8_t*>(g_trampoline) : nullptr;
    if (tramp == nullptr)
    {
        tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (tramp == nullptr)
        {
            this->LogLine(LL, "occlude3d", "HOOK: VirtualAlloc failed.");
            { this->FlushHookDetail(); return false; }
        }
    }

    uint8_t* target = reinterpret_cast<uint8_t*>(addr);
    memcpy(g_origBytes, target, 5);
    memcpy(tramp, g_origBytes, 5);
    tramp[5] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + 6) = static_cast<int32_t>((addr + 5) - (reinterpret_cast<uintptr_t>(tramp) + 10));
    FlushInstructionCache(GetCurrentProcess(), tramp, 10);

    // Pin before the first patch so entry jumps, stubs and trampolines always reach mapped code.
    if (!g_pinned) g_pinned = occ::pinSelf(reinterpret_cast<const void*>(&hook_OnMove));
    if (!g_pinned)
    {
        this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Error), "occlude3d", "HOOK: could not pin this DLL (error %lu); the hook is not installed.", GetLastError());
        this->WriteChat("could not keep itself loaded, so the occlusion hook is not installed.", 0x44);
        if (tramp != g_trampoline) VirtualFree(tramp, 0, MEM_RELEASE);   // never published
        { this->FlushHookDetail(); return false; }
    }

    g_trampoline = tramp;   // published before the entry JMP so the stub's jmp [g_trampoline] is valid at once
    g_hookAddr   = addr;
    g_hookCalls  = 0;
    g_instance   = this;

    uint8_t jump[5] = { 0xE9 };
    *reinterpret_cast<int32_t*>(jump + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hook_OnMove) - (addr + 5));
    // Freeze outside the entry before writing: a partial jump would decode as mov from an invalid address.
    const occ::CodeRange entry[1] = { { addr, addr + 5 } };
    bool wrote = false, protectFailed = false;
    char why[600] = "";
    const bool quiet = occ::whenNoThreadIn(entry, 1, nullptr, 0, 500, [&]
    {
        DWORD oldProt = 0;
        if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt)) { protectFailed = true; return; }
        memcpy(target, jump, 5);
        VirtualProtect(target, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
        wrote = occ::bytesAre(addr, jump, 5);
    }, why, sizeof why);
    if (!quiet || protectFailed)
    {
        this->LogLine(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d", !quiet ? "HOOK: no moment without a thread at the entry came in 0.5 s; not installed." : "HOOK: VirtualProtect failed.");
        if (!quiet) { char line[700]; _snprintf_s(line, sizeof line, _TRUNCATE, "HOOK: last try: %s", why); this->LogLine(LL, "occlude3d", line); }
        if (this->m_Repeats.first("hook install"))
        {
            this->m_WarnedUnhooked = true;   // this is the warning; the "geometry submitted" one would repeat it
            this->WriteChat("the occlusion hook could not be installed; nothing it draws will show. Try /o3d hook.", 0x44);
        }
        g_instance = nullptr;
        { this->FlushHookDetail(); return false; }
    }
    if (!wrote)
    {
        this->LogLine(static_cast<uint32_t>(Ashita::LogLevel::Error), "occlude3d", "HOOK: the entry does not read back as our jump; treating the hook as installed and retained.");
        g_installed = true;
        g_retained  = true;
        this->m_Hooked = true;
        { this->FlushHookDetail(); return false; }
    }

    g_installed   = true;
    this->m_Hooked = true;
    this->m_WarnedUnhooked = false;
    this->LogF(LL, "occlude3d", "HOOK installed at 0x%08X (trampoline 0x%08X).",
        static_cast<uint32_t>(addr), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tramp)));
    return true;
}

// Restore only our entry jump under a freeze outside the DLL, entry and trampoline.
// Retain the trampoline and pinned DLL; the stub passes through when g_instance is null.
void occlude3d::RemoveHook(void)
{
    if (!g_installed)
        return;

    const uintptr_t addr = g_hookAddr;
    uint8_t jump[5] = { 0xE9 };
    *reinterpret_cast<int32_t*>(jump + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hook_OnMove) - (addr + 5));

    occ::CodeRange ranges[3];
    size_t n = 0;
    uintptr_t lo = 0, hi = 0;
    if (occ::selfImage(reinterpret_cast<const void*>(&hook_OnMove), lo, hi)) ranges[n++] = occ::CodeRange{ lo, hi };
    else ranges[n++] = occ::CodeRange{ 0, UINTPTR_MAX };   // unknown: treat every thread as busy
    ranges[n++] = occ::CodeRange{ addr, addr + 5 };
    if (g_trampoline != nullptr) ranges[n++] = occ::CodeRange{ reinterpret_cast<uintptr_t>(g_trampoline), reinterpret_cast<uintptr_t>(g_trampoline) + 10 };
    const DWORD writers[1] = { g_log.threadId() };

    // Retry a quiet freeze for up to three seconds. Also require g_inFlight == 0:
    // a hook inside Direct3D lies outside the checked spans but still holds the instance.
    bool owned = false, restored = false, quiet = false;
    const ULONGLONG deadline = GetTickCount64() + 3000;
    do
    {
        bool busy = false;
        const bool ran = occ::whenNoThreadIn(ranges, n, writers, 1, 1, [&]
        {
            // Nothing here allocates or logs: a suspended thread may hold the heap lock.
            if (g_inFlight != 0) { busy = true; return; }
            uint8_t* target = reinterpret_cast<uint8_t*>(addr);
            owned = occ::bytesAre(addr, jump, 5);
            if (!owned) { g_instance = nullptr; return; }
            DWORD oldProt = 0;
            if (VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                memcpy(target, g_origBytes, 5);
                VirtualProtect(target, 5, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), target, 5);
            }
            restored = occ::bytesAre(addr, g_origBytes, 5);
            g_instance = nullptr;   // no thread is in the stub or holds the instance now
        });
        quiet = ran && !busy;
        if (!quiet) Sleep(1);
    } while (!quiet && GetTickCount64() < deadline);

    if (quiet && restored)
    {
        g_installed = false;
        g_retained  = false;
        g_keepInstance = false;   // no hook call holds the instance now: an earlier timeout no longer applies
        this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Info), "occlude3d",
            "HOOK removed at 0x%08X. Total OnMove calls observed: %u", static_cast<uint32_t>(addr), g_hookCalls);
    }
    else
    {
        // The hook stays in: its stub passes straight through to the retained trampoline. A later load is refused.
        g_retained = true;
        if (!quiet)
        {
            // Retain the instance, textures and fonts if a hook may still hold them inside Direct3D.
            g_keepInstance = true;
            this->LogLine(static_cast<uint32_t>(Ashita::LogLevel::Error), "occlude3d",
                "HOOK not removed: no moment without a game thread inside the hook came within 3 s; the entry jump stays in and the instance is kept until the game closes.");
            this->WriteChat("could not remove its hook safely; it stays in until you close the game. Restart the game to load occlude3d again.", 0x44);
        }
        else if (!owned)
        {
            this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Error), "occlude3d",
                "HOOK not removed: the entry at 0x%08X holds bytes occlude3d did not write (another tool hooked it); left alone.", static_cast<uint32_t>(addr));
            this->WriteChat("something else has rewritten the hook site; occlude3d left it alone. Restart the game to load occlude3d again.", 0x44);
        }
        else
        {
            this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Error), "occlude3d",
                "HOOK not removed: the restore at 0x%08X did not read back (protection?); the jump stays in until the game closes.", static_cast<uint32_t>(addr));
            this->WriteChat("could not put the game code back; its hook stays in (harmless) until you close the game.", 0x44);
        }
    }
    this->m_Hooked = false;
}

void occlude3d::HandleEventBody(const char* eventName, const void* eventData, const uint32_t eventSize)
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
    // Diff distinct textures between submissions to avoid repeated references and pointer validations.
    static uint32_t oldTex[kMaxBufTextures], newTex[kMaxBufTextures];
    const int nOld = (this->m_Legacy || !o.used) ? 0 : CollectBufferTextures(o.buf, o.size, oldTex, kMaxBufTextures);
    const int nNew = this->m_Legacy ? -1 : CollectBufferTextures(p, eventSize, newTex, kMaxBufTextures);
    const bool diff = nOld >= 0 && nNew >= 0;
    if (!diff && o.used)
        this->RefBufferTextures(o.buf, o.size, false);
    o.used   = true;
    o.id     = owner;
    o.expire = GetTickCount() + ttl;
    o.size   = eventSize;
    memcpy(o.buf, p, eventSize);
    if (diff)
    {
        for (int i = 0; i < nNew; ++i)
        {
            bool had = false;
            for (int j = 0; j < nOld; ++j) if (oldTex[j] == newTex[i]) { had = true; break; }
            if (!had) this->RefTexture(newTex[i], true);
        }
        for (int j = 0; j < nOld; ++j)
        {
            bool kept = false;
            for (int i = 0; i < nNew; ++i) if (newTex[i] == oldTex[j]) { kept = true; break; }
            if (!kept) this->RefTexture(oldTex[j], false);
        }
    }
    else
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
        this->LogLine(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
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

void occlude3d::HandleEvent(const char* eventName, const void* eventData, const uint32_t eventSize)
{
    try
    {
        this->HandleEventBody(eventName, eventData, eventSize);
    }
    catch (...)
    {
        if (this->m_Repeats.first("event error"))
        {
            g_log.write("error", "HandleEvent: an unexpected error");
            this->WriteChat("a submission failed with an unexpected error.", 0x44);
        }
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

// Collect distinct texture pointers in submission order; return -1 if capacity is exceeded.
int occlude3d::CollectBufferTextures(const uint8_t* b, uint32_t size, uint32_t* out, int cap)
{
    if (b == nullptr || size < 20)
        return 0;
    const uint32_t end = size;
    uint32_t pos = 16, items = 0; memcpy(&items, b + 12, 4);
    int n = 0;
    for (uint32_t it = 0; it < items && pos + 4 <= end; ++it)
    {
        uint32_t tw = 0; memcpy(&tw, b + pos, 4); pos += 4;
        uint32_t bodyLen = 0, tp = 0;
        if (!DecodeItem(tw & 0xFFu, b, pos, end, &bodyLen, &tp)) break;
        pos += bodyLen;
        if (tp == 0) continue;
        bool dup = false;
        for (int s = 0; s < n; ++s) { if (out[s] == tp) { dup = true; break; } }
        if (dup) continue;
        if (n >= cap) return -1;
        out[n++] = tp;
    }
    return n;
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

void occlude3d::LogLine(uint32_t level, const char*, const char* text)
{
    g_log.write(levelName(level), text);
}
void occlude3d::LogF(uint32_t level, const char*, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log.write(levelName(level), buf);
}

void occlude3d::WriteChat(const char* body, uint8_t bodyColor, bool log)
{
    if (body == nullptr) return;
    std::string text = body;
    if (bodyColor == 0x44) text += " Details: " + this->LogShown() + this->LogNote();
    if (log)
    {
        std::string plain;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\x1E') { ++i; continue; }   // a colour escape and its colour byte
            plain += text[i];
        }
        g_log.write(bodyColor == 0x44 ? "warn" : "info", plain);
    }
    if (this->m_AshitaCore == nullptr || this->m_AshitaCore->GetChatManager() == nullptr) return;
    char buffer[400];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
        "\x1E\x51" "[" "\x1E\x06" "occlude3d" "\x1E\x51" "]" "\x1E\x01" " " "\x1E%c" "%s" "\x1E\x01", bodyColor, text.c_str());
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
    UNREFERENCED_PARAMETER(cat);
    char fc[5]; FourCC(owner, fc);
    char cause[200];
    _snprintf_s(cause, sizeof(cause), _TRUNCATE, "dropped submissions from '%s' (%s)", fc, reason);
    if (!this->m_Repeats.first(cause)) return;   // said once per cause; the count is written at unload
    this->LogF(static_cast<uint32_t>(Ashita::LogLevel::Warn), "occlude3d",
        "DROP from owner '%s' (0x%08X): %s. [totals: oversize=%u noSlot=%u badItem=%u]",
        fc, owner, reason, this->m_DropOversize, this->m_DropNoSlot, this->m_DropBadItem);
    char cm[224];
    _snprintf_s(cm, sizeof(cm), _TRUNCATE, "dropped a submission from '%s' -- %s", fc, reason);
    this->WriteChat(cm, 0x44);
}

void occlude3d::HookDetail(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    this->m_HookDetail.emplace_back(buf);
}

void occlude3d::FlushHookDetail(void)
{
    for (const auto& line : this->m_HookDetail) g_log.write("info", line);
}

std::string occlude3d::LogShown(void) { return plog::underRoot(this->m_Root, g_log.path()); }
const char* occlude3d::LogNote(void) { return g_log.atStartupFile() ? " (it moves into your character's log at login)" : ""; }

void occlude3d::FollowCharacter(void)
{
    if (this->m_AshitaCore == nullptr) return;
    IMemoryManager* mm = this->m_AshitaCore->GetMemoryManager();
    IPlayer* player = mm != nullptr ? mm->GetPlayer() : nullptr;
    IParty* party = mm != nullptr ? mm->GetParty() : nullptr;
    if (player == nullptr || party == nullptr || player->GetLoginStatus() != 2) return;
    const char* name = party->GetMemberName(0);
    const uint32_t serverId = party->GetMemberServerId(0);
    if (name == nullptr || name[0] == '\0' || serverId == 0) return;
    const std::string key = plog::characterKey(name, serverId);
    if (key == this->m_CharKey) return;
    if (this->m_Shot != 0) this->FinishShot();   // a report still waiting goes to the log it was asked in
    this->m_CharKey = key;
    g_log.moveToCharacter(plog::characterLogPath(this->m_Root, "occlude3d", key), name);
}

void occlude3d::StartShot(int kind)
{
    if (this->m_Shot != 0) { this->WriteChat("already measuring; the result comes in a moment."); return; }
    this->ResetStatAccumulators();
    this->m_Shot = kind;
    this->m_ShotStart = GetTickCount64();
}

void occlude3d::FinishShot(void)
{
    const int kind = this->m_Shot;
    this->m_Shot = 0;
    const uint32_t frames = this->m_StatFrames;
    const double n = frames > 0 ? static_cast<double>(frames) : 1.0;
    char stats[400];
    if (frames == 0)
        _snprintf_s(stats, sizeof stats, _TRUNCATE, "stats: no frames were drawn by the hook (hook %s)", this->m_Hooked ? "installed, nothing submitted" : "not installed");
    else
        _snprintf_s(stats, sizeof stats, _TRUNCATE,
            "stats over %u frames: items %.0f, owners %.1f, state calls %.0f, draw calls %.0f, draw CPU %.3f ms (slowest %.3f ms), receive CPU %.3f ms, bytes %.0f",
            frames, this->m_ItemsAcc / n, this->m_OwnersAcc / n, this->m_StateAcc / n, this->m_DrawsAcc / n,
            this->m_HookMs / n, this->m_StatWorstMs, this->m_EventMs / n, this->m_BytesAcc / n);
    this->ResetStatAccumulators();
    if (kind == 1)
    {
        this->WriteChat(stats);   // mirrored to the log
        return;
    }
    std::string body;
    auto add = [&body](const char* fmt, ...)
    {
        char line[600];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
        va_end(ap);
        body += "      ";
        body += line;
        body += '\n';
    };
    add("plugin    occlude3d v%.1f  (interface %.2f)", OCCLUDE3D_VERSION, ASHITA_INTERFACE_VERSION);
    add("device    0x%p", static_cast<void*>(this->m_Device));
    if (this->m_Hooked)
        add("hook      installed at 0x%08X, trampoline 0x%08X, calls seen %u, legacy %s", static_cast<uint32_t>(g_hookAddr),
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_trampoline)), g_hookCalls, this->m_Legacy ? "on" : "off");
    else
        add("hook      not installed (%d signature match(es)), legacy %s", this->m_HookMatches, this->m_Legacy ? "on" : "off");
    add("%s", stats);
    for (int i = 0; i < kMaxOwners; ++i)
    {
        if (!this->m_Owners[i].used) continue;
        char fc[5]; FourCC(this->m_Owners[i].id, fc);
        add("addon     '%s' (0x%08X): %u bytes submitted", fc, this->m_Owners[i].id, this->m_Owners[i].size);
    }
    char dfc[5]; FourCC(this->m_LastDropOwner, dfc);
    add("drops     oversize %u, no slot %u, bad item %u, bad texture %u; last from '%s'",
        this->m_DropOversize, this->m_DropNoSlot, this->m_DropBadItem, this->m_DropBadTex, dfc);
    for (const auto& line : this->m_HookDetail) add("%s", line.c_str());
    char who[96];
    _snprintf_s(who, sizeof who, _TRUNCATE, "occlude3d %.1f build %08X", OCCLUDE3D_VERSION, plog::ownImageStamp(reinterpret_cast<const void*>(&hook_OnMove)));
    g_log.writeDiag(who, body);
    char chatLine[300];
    _snprintf_s(chatLine, sizeof chatLine, _TRUNCATE, "Diagnostics written to %s%s.", this->LogShown().c_str(), this->LogNote());
    this->WriteChat(chatLine);
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
    this->m_FrameItems = 0; this->m_FrameOwners = 0; this->m_FrameState = 0; this->m_FrameDraws = 0;

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

    // Cache device state for this hook call; only additive (0x01) and depth-write (0x04) flags affect it.
    // Legacy mode reissues setters, draws colored items separately and breaks texture batches on all flags
    // for comparison with the original call pattern.
    const bool legacy = this->m_Legacy;
    uint32_t curFlags = 0u;
    int      curStage = -1;          // -1 colored, 0 texture colour blended by texture alpha, 1 modulate
    uint32_t curTex   = 0u;
    DWORD    curFvf   = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    auto applyFlags = [&](uint32_t fl) {
        const uint32_t f = fl & 0x05u;
        if (legacy) { dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL); curFlags = ~f; this->m_FrameState += 1; }
        if ((f ^ curFlags) & 0x01u) {
            dev->SetRenderState(D3DRS_DESTBLEND, (f & 0x01u) ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
            this->m_FrameState += 1;
        }
        if ((f ^ curFlags) & 0x04u) {
            dev->SetRenderState(D3DRS_ZWRITEENABLE,    (f & 0x04u) ? TRUE : FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, (f & 0x04u) ? TRUE : FALSE);
            this->m_FrameState += 2;
        }
        curFlags = f;
    };
    auto applyStage = [&](int st) {
        if (st == curStage && !legacy) return;
        if (st == 0) {
            dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_BLENDTEXTUREALPHA);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            this->m_FrameState += 5;
        } else if (st == 1) {
            dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            this->m_FrameState += 6;
        } else {
            // Colored vertices ignore the bound texture, so no unbind is needed.
            dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            this->m_FrameState += 4;
        }
        curStage = st;
    };
    auto applyTex = [&](uint32_t tp) {
        if (tp == curTex && !legacy) return;
        dev->SetTexture(0, reinterpret_cast<IDirect3DBaseTexture8*>(static_cast<uintptr_t>(tp)));
        curTex = tp; this->m_FrameState += 1;
    };
    auto applyFvf = [&](DWORD f) {
        if (f == curFvf && !legacy) return;
        dev->SetVertexShader(f);
        curFvf = f; this->m_FrameState += 1;
    };
    auto colorStage = [&]() {
        if (curStage != -1) applyStage(-1);
        if (curFvf != (D3DFVF_XYZ | D3DFVF_DIFFUSE)) applyFvf(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    };

    // Only one batch may be pending; flush it before switching type to preserve submission order.
    static const int kBatchMax = 8192;
    static VT        batch[kBatchMax];     // textured triangles
    static V         cbatch[kBatchMax];    // colored triangles
    int      batchN = 0, batchStage = -1, cbatchN = 0;
    uint32_t batchTex = 0, batchFlags = 0, cbatchFlags = 0;

    auto flushTex = [&]() {
        if (batchN >= 3 && batchStage >= 0)
        {
            applyFlags(batchFlags); applyTex(batchTex); applyStage(batchStage);
            applyFvf(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, batchN / 3, batch, sizeof(VT));
            this->m_FrameDraws++;
            if (legacy) { applyTex(0); applyStage(-1); applyFvf(D3DFVF_XYZ | D3DFVF_DIFFUSE); }
        }
        batchN = 0; batchStage = -1;
    };
    auto flushCol = [&]() {
        if (cbatchN >= 3)
        {
            applyFlags(cbatchFlags); colorStage();
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, cbatchN / 3, cbatch, sizeof(V));
            this->m_FrameDraws++;
        }
        cbatchN = 0;
    };
    auto flush = [&]() { flushTex(); flushCol(); };
    auto texBegin = [&](uint32_t tp, uint32_t fl, int stage, int need) {
        flushCol();
        const uint32_t f = legacy ? fl : (fl & 0x05u);
        if (batchStage != stage || batchTex != tp || batchFlags != f || batchN + need > kBatchMax) flushTex();
        batchTex = tp; batchFlags = f; batchStage = stage;
    };
    auto colAppend = [&](const V* v, int n, uint32_t fl) {
        if (n < 3) return;
        flushTex();
        const uint32_t f = fl & 0x05u;
        if (cbatchN > 0 && (cbatchFlags != f || cbatchN + n > kBatchMax)) flushCol();
        if (n > kBatchMax)
        {
            applyFlags(f); colorStage();
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, v, sizeof(V));
            this->m_FrameDraws++;
            return;
        }
        cbatchFlags = f;
        memcpy(cbatch + cbatchN, v, static_cast<size_t>(n) * sizeof(V));
        cbatchN += n;
        if (legacy) flushCol();
    };
    auto immediate = [&](uint32_t fl) {
        flush();
        applyFlags(fl); colorStage();
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
    static const int kGoodTexMax = 256;
    uint32_t goodTex[kGoodTexMax]; int nGoodTex = 0;
    auto texOK = [&](uint32_t tp) -> bool {
        if (tp == 0) return false;
        for (int i = 0; i < nGoodTex; ++i) if (goodTex[i] == tp) return true;
        if (ReadablePtr(tp)) { if (nGoodTex < (legacy ? 32 : kGoodTexMax)) goodTex[nGoodTex++] = tp; return true; }
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
            texBegin(texPtr, fl, 1, 6);
            const VT q[6] = {
                { cr[0][0],cr[0][1],cr[0][2], cTL,0,0 }, { cr[1][0],cr[1][1],cr[1][2], cTR,1,0 }, { cr[2][0],cr[2][1],cr[2][2], cBR,1,1 },
                { cr[0][0],cr[0][1],cr[0][2], cTL,0,0 }, { cr[2][0],cr[2][1],cr[2][2], cBR,1,1 }, { cr[3][0],cr[3][1],cr[3][2], cBL,0,1 },
            };
            for (int qi = 0; qi < 6; ++qi) batch[batchN++] = q[qi];
        }
        else
        {
            const V q[6] = {
                { cr[0][0],cr[0][1],cr[0][2], cTL }, { cr[1][0],cr[1][1],cr[1][2], cTR }, { cr[2][0],cr[2][1],cr[2][2], cBR },
                { cr[0][0],cr[0][1],cr[0][2], cTL }, { cr[2][0],cr[2][1],cr[2][2], cBR }, { cr[3][0],cr[3][1],cr[3][2], cBL },
            };
            colAppend(q, 6, fl);
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

            if (type == 0 || type == 1 || type == 2)
                immediate(flags);

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
                        this->m_FrameDraws++;
                        continue;
                    }
                }
                const V v[2] = { { a[0],a[1],a[2], col }, { bp[0],bp[1],bp[2], col } };
                dev->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(V));
                this->m_FrameDraws++;
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
                this->m_FrameDraws++;
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
                this->m_FrameDraws++;
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
                texBegin(texPtr, flags, 0, need);
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
                colAppend(verts, static_cast<int>(vc), flags);
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
                texBegin(texPtr, flags, 1, static_cast<int>(vc));
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
                        if (batchN + 6 > kBatchMax) texBegin(fnt->tex, flags, 1, 6);
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
                texBegin(fnt->tex, flags, 1, 6);
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
                colAppend(fan, fv, flags);
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
                    texBegin(texPtr, flags, 1, 6);
                    const VT q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col, us[0],vs[0] }, { cr[1][0],cr[1][1],cr[1][2], col, us[1],vs[1] }, { cr[2][0],cr[2][1],cr[2][2], col, us[2],vs[2] },
                        { cr[0][0],cr[0][1],cr[0][2], col, us[0],vs[0] }, { cr[2][0],cr[2][1],cr[2][2], col, us[2],vs[2] }, { cr[3][0],cr[3][1],cr[3][2], col, us[3],vs[3] },
                    };
                    for (int qi = 0; qi < 6; ++qi) batch[batchN++] = q[qi];
                }
                else
                {
                    const V q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[1][0],cr[1][1],cr[1][2], col }, { cr[2][0],cr[2][1],cr[2][2], col },
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[2][0],cr[2][1],cr[2][2], col }, { cr[3][0],cr[3][1],cr[3][2], col },
                    };
                    colAppend(q, 6, flags);
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
                colAppend(arcv, vc, flags);
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
                    texBegin(texPtr, flags, 1, 6);
                    const VT q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col,0,0 }, { cr[1][0],cr[1][1],cr[1][2], col,1,0 }, { cr[2][0],cr[2][1],cr[2][2], col,1,1 },
                        { cr[0][0],cr[0][1],cr[0][2], col,0,0 }, { cr[2][0],cr[2][1],cr[2][2], col,1,1 }, { cr[3][0],cr[3][1],cr[3][2], col,0,1 },
                    };
                    for (int qi = 0; qi < 6; ++qi) batch[batchN++] = q[qi];
                }
                else
                {
                    const V q[6] = {
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[1][0],cr[1][1],cr[1][2], col }, { cr[2][0],cr[2][1],cr[2][2], col },
                        { cr[0][0],cr[0][1],cr[0][2], col }, { cr[2][0],cr[2][1],cr[2][2], col }, { cr[3][0],cr[3][1],cr[3][2], col },
                    };
                    colAppend(q, 6, flags);
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
                texBegin(texPtr, flags, 1, 6);
                #define O3D_NSV(xx,yy,uu,vv) VT{ c[0]+rx*(xx)+ux*(yy), c[1]+ry*(xx)+uy*(yy), c[2]+rz*(xx)+uz*(yy), col, (uu),(vv) }
                for (int gy = 0; gy < 3; ++gy) for (int gx = 0; gx < 3; ++gx)
                {
                    if (batchN + 6 > kBatchMax) texBegin(texPtr, flags, 1, 6);
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
    this->m_ItemsAcc = 0.0; this->m_OwnersAcc = 0.0; this->m_BytesAcc = 0.0; this->m_StateAcc = 0.0; this->m_DrawsAcc = 0.0;
    this->m_StatWorstMs = 0.0;
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
        const double frameMs = static_cast<double>(_de.QuadPart - _ds.QuadPart) * 1000.0 / static_cast<double>(this->m_QpcFreq);
        this->m_HookMs    += frameMs;
        if (frameMs > this->m_StatWorstMs) this->m_StatWorstMs = frameMs;
        this->m_ItemsAcc  += this->m_FrameItems;
        this->m_OwnersAcc += this->m_FrameOwners;
        this->m_StateAcc  += this->m_FrameState;
        this->m_DrawsAcc  += this->m_FrameDraws;
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

    if (this->StatsOn() && this->m_StatFrames >= 60 && this->m_Shot == 0)   // the panel's numbers never reset a one-shot's frames
    {
        const double n = static_cast<double>(this->m_StatFrames);
        this->m_DispItems  = this->m_ItemsAcc / n; this->m_DispOwners = this->m_OwnersAcc / n;
        this->m_DispState  = this->m_StateAcc / n; this->m_DispDrawMs = this->m_HookMs / n;
        this->m_DispDraws  = this->m_DrawsAcc / n;
        this->m_DispRecvMs = this->m_EventMs / n;  this->m_DispBytes  = this->m_BytesAcc / n;
        this->m_HistDrawMs[this->m_HistPos] = static_cast<float>(this->m_DispDrawMs);
        this->m_HistItems[this->m_HistPos]  = static_cast<float>(this->m_DispItems);
        this->m_HistPos = (this->m_HistPos + 1) % kHist;
        this->ResetStatAccumulators();
    }
}

void occlude3d::Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*)
{
    if (!this->m_Announced)
    {
        this->m_Announced = true;
        this->WriteChat("\x1E\x02" "/o3d" "\x1E\x6A" " opens the status panel, "
                        "\x1E\x02" "/o3d hook" "\x1E\x6A" " toggles the occlusion hook.", 0x6A, false);
    }
    if (!this->m_FrameDead)
    {
        try
        {
            const ULONGLONG now = GetTickCount64();
            if (this->m_Shot != 0 && (this->m_StatFrames >= 60 || now - this->m_ShotStart > 3000)) this->FinishShot();
            if (now >= this->m_NextCharCheck)
            {
                this->m_NextCharCheck = now + 1000;
                this->FollowCharacter();
                char line[300];
                if (g_log.takeWriteWarning())
                {
                    _snprintf_s(line, sizeof line, _TRUNCATE, "can't write its log (%s).", this->LogShown().c_str());
                    this->WriteChat(line, 0x68);
                }
                if (g_log.takeTrimWarning())
                {
                    _snprintf_s(line, sizeof line, _TRUNCATE, "its log is over 1.5 MB and cannot be trimmed (%s): is another program holding it open?", this->LogShown().c_str());
                    this->WriteChat(line, 0x68);
                }
            }
        }
        catch (...)
        {
            this->m_FrameDead = true;
            g_log.write("error", "Direct3DPresent: an unexpected error; the once-a-second work stops for this session");
            this->WriteChat("an unexpected error stopped its once-a-second check.", 0x44);
        }
    }
    if (this->m_UiOpen)
        this->RenderUI();
}

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
    g->TextDisabled("recv %.3f ms   state %.0f   draws %.0f   bytes %.0f", this->m_DispRecvMs, this->m_DispState, this->m_DispDraws, this->m_DispBytes);

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
    if (toggleBtn("Log stats##ls", this->m_Shot != 0, bw)) this->StartShot(1);
    g->SetItemTooltip("%s", "Measure the next 60 frames and write one line to the log and chat.");

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
        if (instance != nullptr && g_keepInstance && instance == g_instance)
            return;   // a hook call never drained (RemoveHook): the object is leaked rather than pulled from under it
        if (instance != nullptr)
            delete static_cast<occlude3d*>(instance);
    }

    __declspec(noinline) double __stdcall expGetInterfaceVersion(void)
    {
        return ASHITA_INTERFACE_VERSION;
    }
}
