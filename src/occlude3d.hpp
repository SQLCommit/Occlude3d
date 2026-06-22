/**
 * occlude3d - Shared depth-correct world-space geometry renderer for Ashita addons.
 *
 * Any addon can submit world-space geometry via RaiseEvent('occlude3d', byteTable) and have it drawn
 * OCCLUDED behind terrain/walls. Occlusion is achieved by trampoline-hooking the engine's
 * CXiActorNameDraw::OnMove (where the world depth buffer is bound, just before nameplates render) and
 * drawing there.
 *
 * The submission wire format (O3D2 header + the 16 item types) and the font registry (O3DF) are
 * specified in API.md - that is the canonical reference.
 */
#ifndef OCCLUDE3D_HPP_INCLUDED
#define OCCLUDE3D_HPP_INCLUDED

#include "Ashita.h"

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

    bool     m_Stats;
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
    uint32_t m_DropWarnTick[3];
    void WarnDrop(int cat, const char* reason, uint32_t owner);
    void WriteChat(const char* body, uint8_t bodyColor = 0x6A);

    // UI panel (/o3d).
    bool   m_UiOpen;
    bool   m_Announced;
    void   RenderUI(void);
    bool   StatsOn(void) const { return m_Stats || m_UiOpen; }
    void   ResetStatAccumulators(void);
    double m_DispItems, m_DispOwners, m_DispState, m_DispDrawMs, m_DispRecvMs, m_DispBytes;
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

    void RenderHookDraw(void);

    void HandleEvent(const char* eventName, const void* eventData, uint32_t eventSize) override;

    bool Direct3DInitialize(IDirect3DDevice8* device) override;
    void Direct3DPresent(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override;
};

#endif
