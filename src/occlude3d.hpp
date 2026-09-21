// Shared world-space renderer for Ashita addons. Draw in CXiActorNameDraw::OnMove
// to use world depth for occlusion. Submissions use O3D2; font registration uses O3DF.
#ifndef OCCLUDE3D_HPP_INCLUDED
#define OCCLUDE3D_HPP_INCLUDED

#include "Ashita.h"
#include "plugin_log.h"

#include <string>
#include <vector>

class occlude3d final : public IPlugin
{
    IAshitaCore*      m_AshitaCore;
    ILogManager*      m_LogManager;
    IDirect3DDevice8* m_Device;

    uintptr_t m_NameDrawAddr;
    int       m_HookMatches;
    bool      m_Hooked;
    bool      m_TriedAutoHook;
    bool      m_WarnedUnhooked;

    bool     m_Legacy;     // /o3d legacy: the pre-batching device call pattern, for same-session A/B timing
    int64_t  m_QpcFreq;
    uint32_t m_StatFrames;
    double   m_HookMs;
    double   m_EventMs;
    double   m_ItemsAcc;
    double   m_OwnersAcc;
    double   m_BytesAcc;
    double   m_StateAcc;
    uint32_t m_FrameItems;
    uint32_t m_FrameOwners;
    uint32_t m_FrameState;
    uint32_t m_FrameDraws;
    double   m_DrawsAcc;

    static const int kMaxOwners = 32;
    static const int kOwnerBuf  = 16384;
    struct SubOwner { bool used; uint32_t id; uint32_t expire; uint32_t size; uint8_t buf[kOwnerBuf]; };
    SubOwner m_Owners[kMaxOwners];

    static const int kMaxFonts = 8;
    struct Font { bool used; uint32_t id; uint32_t tex; uint32_t cols, rows, firstCp; float advance; };
    Font m_Fonts[kMaxFonts];
    const Font* FindFont(uint32_t id) const;

    uint32_t m_DropOversize;
    uint32_t m_DropNoSlot;
    uint32_t m_DropBadItem;
    uint32_t m_DropBadTex;
    uint32_t m_LastDropOwner;
    void WarnDrop(int cat, const char* reason, uint32_t owner);
    void WriteChat(const char* body, uint8_t bodyColor = 0x6A, bool log = true);   // mirrored to the log unless `log` is false; a failure (0x44) names the log
    void LogLine(uint32_t level, const char* tag, const char* text);   // Writes to the per-character log; tag is unused.
    void LogF(uint32_t level, const char* tag, const char* fmt, ...);
    bool m_Refused = false;   // this instance was refused at load (a retained hook from an earlier load): Release does nothing
    // Per-character log and diagnostic counters.
    std::string m_Root;
    plog::Run   m_Run;
    std::string m_CharKey;
    ULONGLONG   m_NextCharCheck = 0;
    bool        m_FrameDead = false;
    plog::Repeats m_Repeats;                 // a repeated warning: said once per cause, counted
    std::vector<std::string> m_HookDetail;   // the hook scan detail: logged only if the install fails; always in diag
    int         m_Shot = 0;                  // 0 none, 1 /o3d stats, 2 /o3d diag: measuring 60 frames
    ULONGLONG   m_ShotStart = 0;
    double      m_StatWorstMs = 0.0;         // the slowest measured frame's draw time
    void HookDetail(const char* fmt, ...);
    void FlushHookDetail(void);
    void StartShot(int kind);
    void FinishShot(void);                   // writes the stats line or the diag report with what is measured
    void FollowCharacter(void);
    std::string LogShown(void);
    const char* LogNote(void);

    // UI panel (/o3d).
    bool   m_UiOpen;
    bool   m_Announced;
    void   RenderUI(void);
    bool   StatsOn(void) const { return m_UiOpen || m_Shot != 0; }
    void   ResetStatAccumulators(void);
    double m_DispItems, m_DispOwners, m_DispState, m_DispDraws, m_DispDrawMs, m_DispRecvMs, m_DispBytes;
    static const int kHist = 90;
    float m_HistDrawMs[kHist];
    float m_HistItems[kHist];
    int   m_HistPos;

    bool HasGeometry(void) const;
    void RenderHookDrawInner(IDirect3DDevice8* dev);
    void DrawSubmittedGeometry(IDirect3DDevice8* dev);
    void DrawSubmittedGuarded(IDirect3DDevice8* dev);
    void RefTexture(uint32_t texPtr, bool addref);
    void RefBufferTextures(const uint8_t* buf, uint32_t size, bool addref);
    static const int kMaxBufTextures = 512;
    static int CollectBufferTextures(const uint8_t* buf, uint32_t size, uint32_t* out, int cap);

    uintptr_t ResolveNameDraw(void);
    bool InstallHook(uintptr_t addr);
    void RemoveHook(void);

public:
    occlude3d(void);
    ~occlude3d(void) override;

    const char* GetName(void) const override;
    const char* GetAuthor(void) const override;
    const char* GetDescription(void) const override;
    const char* GetLink(void) const override;
    double GetVersion(void) const override;
    double GetInterfaceVersion(void) const override;
    int32_t GetPriority(void) const override;
    uint32_t GetFlags(void) const override;

    bool Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) override;
    void Release(void) override;

    bool HandleCommand(int32_t mode, const char* command, bool injected) override;
    bool Command(int32_t mode, const char* command, bool injected);   // HandleCommand's body, inside the guard
    bool m_CmdOurs = false;

    void RenderHookDraw(void);

    void HandleEvent(const char* eventName, const void* eventData, uint32_t eventSize) override;
    void HandleEventBody(const char* eventName, const void* eventData, uint32_t eventSize);   // inside the guard

    bool Direct3DInitialize(IDirect3DDevice8* device) override;
    void Direct3DPresent(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override;
};

#endif
