// Windows: OLE IDropTarget registered on the TCP/MCP window, replacing REAPER's own

#include "platform/platform.h"
#include "settings.h"

#include <ole2.h>
#include <shellapi.h>

#ifndef MK_ALT
#define MK_ALT 0x20
#endif

static bool g_oleInited = false;

// OLE gives UTF-16; REAPER APIs and Cartridge pending_load expect UTF-8
static std::string WideToUtf8(const wchar_t* wide)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// grfKeyState can miss keys pressed during the drag; also ask the keyboard state
static void ClearAssignedModifier(Modifiers& m, int key)
{
    switch (key)
    {
    case ModKey_Shift: m.shift = false; break;
    case ModKey_Ctrl:  m.ctrl = false; break;
    case ModKey_Alt:   m.alt = false; break;
    default: break;
    }
}

static Modifiers ModifiersFromKeyState(DWORD grfKeyState)
{
    Modifiers m;
    BYTE ks[256] = {};
    const bool haveKs = GetKeyboardState(ks) != 0;

    auto down = [&](int vk, DWORD mk) -> bool {
        if (grfKeyState & mk) return true;
        if (GetAsyncKeyState(vk) & 0x8000) return true;
        if (GetKeyState(vk) & 0x8000) return true;
        if (haveKs && (ks[vk] & 0x80)) return true;
        return false;
    };

    m.shift = down(VK_SHIFT, MK_SHIFT);
    m.ctrl = down(VK_CONTROL, MK_CONTROL);
    m.alt = (grfKeyState & MK_ALT) != 0
        || down(VK_LMENU, 0)
        || down(VK_RMENU, 0)
        || down(VK_MENU, 0);
    return m;
}

static std::vector<std::string> DroppedFiles(IDataObject* pDataObj)
{
    std::vector<std::string> files;
    if (!pDataObj) return files;

    FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg = {};
    if (FAILED(pDataObj->GetData(&fmt, &stg)))
        return files;

    HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
    if (hDrop)
    {
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            std::wstring path(len, L'\0');
            if (len && DragQueryFileW(hDrop, i, &path[0], len + 1))
            {
                std::string utf8 = WideToUtf8(path.c_str());
                if (!utf8.empty())
                    files.push_back(utf8);
            }
        }
        GlobalUnlock(stg.hGlobal);
    }
    ReleaseStgMedium(&stg);
    return files;
}

class OleDropTarget : public IDropTarget
{
public:
    explicit OleDropTarget(DragHandler* handler) : m_handler(handler) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        m_handler->OnDragEnter((int)DroppedFiles(pDataObj).size(), pt.x, pt.y, ModifiersFromKeyState(grfKeyState));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        m_handler->OnDragOver(pt.x, pt.y, ModifiersFromKeyState(grfKeyState));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        m_handler->OnDragLeave();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        m_handler->OnDrop(DroppedFiles(pDataObj), pt.x, pt.y, ModifiersFromKeyState(grfKeyState));
        return S_OK;
    }

private:
    LONG m_ref = 1;
    DragHandler* m_handler;
};

class OleDropTargetRegistration : public DropTarget
{
public:
    OleDropTargetRegistration(HWND hwnd, OleDropTarget* target) : m_hwnd(hwnd), m_target(target) {}

    ~OleDropTargetRegistration() override
    {
        if (IsWindow(m_hwnd))
            RevokeDragDrop(m_hwnd);
        m_target->Release();
    }

private:
    HWND m_hwnd;
    OleDropTarget* m_target;
};

std::unique_ptr<DropTarget> DropTarget::Attach(HWND hwnd, DragHandler* handler)
{
    if (!hwnd || !handler) return nullptr;
    if (!g_oleInited)
    {
        OleInitialize(nullptr);
        g_oleInited = true;
    }

    OleDropTarget* target = new OleDropTarget(handler);
    // If REAPER already owns the target, steal it
    HRESULT hr = RegisterDragDrop(hwnd, target);
    if (hr == DRAGDROP_E_ALREADYREGISTERED)
    {
        RevokeDragDrop(hwnd);
        hr = RegisterDragDrop(hwnd, target);
    }
    if (FAILED(hr))
    {
        target->Release();
        return nullptr;
    }
    return std::unique_ptr<DropTarget>(new OleDropTargetRegistration(hwnd, target));
}


static IDropTarget* WindowOleDropTarget(HWND hwnd)
{
    return reinterpret_cast<IDropTarget*>(GetPropW(hwnd, L"OleDropTargetInterface"));
}

static int CountDroppedFiles(IDataObject* pDataObj)
{
    return (int)DroppedFiles(pDataObj).size();
}

// Passthrough proxy: native unless shouldIntercept (Alt on arrange)
class OlePassthroughTarget : public IDropTarget
{
public:
    OlePassthroughTarget(DragHandler* handler, IDropTarget* native, DropTarget::InterceptFn intercept)
        : m_handler(handler), m_native(native), m_intercept(intercept)
    {
        m_native->AddRef();
    }

    ~OlePassthroughTarget()
    {
        ResetDrag();
        m_native->Release();
    }

    IDropTarget* Native() const { return m_native; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        ResetDrag();
        m_data = data;
        if (m_data) m_data->AddRef();
        m_fileCount = CountDroppedFiles(data);
        return Route(keys, pt, effect);
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override
    {
        return Route(keys, pt, effect);
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        HRESULT hr = S_OK;
        if (m_ours)
        {
            m_handler->OnDragLeave();
            m_ours = false;
        }
        if (m_nativeActive)
        {
            hr = m_native->DragLeave();
            m_nativeActive = false;
        }
        ResetDrag();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        HRESULT hr = Route(keys, pt, effect);
        if (SUCCEEDED(hr))
        {
            if (m_ours)
            {
                if (effect) *effect = DROPEFFECT_COPY;
                Modifiers mods = ModifiersFromKeyState(keys);
                ClearAssignedModifier(mods, g_settings.modSurface);
                m_handler->OnDrop(DroppedFiles(data), pt.x, pt.y, mods);
                m_handler->OnDragLeave();
                m_ours = false;
            }
            else if (m_nativeActive)
            {
                hr = m_native->Drop(data, keys, pt, effect);
                m_nativeActive = false;
            }
        }
        ResetDrag();
        return hr;
    }

private:
    bool WantsOurs(DWORD keys, POINTL pt) const
    {
        if (!m_intercept || m_fileCount <= 0)
            return false;
        return m_intercept(ModifiersFromKeyState(keys), pt.x, pt.y);
    }

    HRESULT Route(DWORD keys, POINTL pt, DWORD* effect)
    {
        if (WantsOurs(keys, pt))
        {
            if (m_nativeActive)
            {
                m_native->DragLeave();
                m_nativeActive = false;
            }
            Modifiers mods = ModifiersFromKeyState(keys);
            ClearAssignedModifier(mods, g_settings.modSurface);
            if (!m_ours)
            {
                m_handler->OnDragEnter(m_fileCount, pt.x, pt.y, mods);
                m_ours = true;
            }
            else
                m_handler->OnDragOver(pt.x, pt.y, mods);
            if (effect) *effect = DROPEFFECT_COPY;
            return S_OK;
        }

        if (m_ours)
        {
            m_handler->OnDragLeave();
            m_ours = false;
        }
        if (!m_nativeActive)
        {
            HRESULT hr = m_native->DragEnter(m_data, keys, pt, effect);
            m_nativeActive = SUCCEEDED(hr);
            return hr;
        }
        return m_native->DragOver(keys, pt, effect);
    }

    void ResetDrag()
    {
        if (m_data) m_data->Release();
        m_data = nullptr;
        m_fileCount = 0;
        m_nativeActive = false;
        m_ours = false;
    }

    LONG m_ref = 1;
    DragHandler* m_handler;
    IDropTarget* m_native;
    DropTarget::InterceptFn m_intercept;
    IDataObject* m_data = nullptr;
    int m_fileCount = 0;
    bool m_nativeActive = false;
    bool m_ours = false;
};

class OlePassthroughRegistration : public DropTarget
{
public:
    OlePassthroughRegistration(HWND hwnd, OlePassthroughTarget* proxy)
        : m_hwnd(hwnd), m_proxy(proxy) {}

    ~OlePassthroughRegistration() override
    {
        if (IsWindow(m_hwnd) && WindowOleDropTarget(m_hwnd) == static_cast<IDropTarget*>(m_proxy))
        {
            IDropTarget* native = m_proxy->Native();
            RevokeDragDrop(m_hwnd);
            RegisterDragDrop(m_hwnd, native);
        }
        m_proxy->Release();
    }

private:
    HWND m_hwnd;
    OlePassthroughTarget* m_proxy;
};

std::unique_ptr<DropTarget> DropTarget::AttachPassthrough(
    HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, bool allowParentWalk)
{
    if (!hwnd || !handler || !shouldIntercept) return nullptr;
    if (!g_oleInited)
    {
        OleInitialize(nullptr);
        g_oleInited = true;
    }

    HWND oleHwnd = hwnd;
    IDropTarget* native = WindowOleDropTarget(oleHwnd);
    if (!native && allowParentWalk)
    {
        // REAPER often registers arrange-related OLE on a parent (sometimes REAPERwnd),
        // not on the track-list / arrange child. See handoff notes for v0.6.
        HWND root = GetAncestor(hwnd, GA_ROOT);
        HWND walk = GetParent(hwnd);
        while (!native && walk)
        {
            native = WindowOleDropTarget(walk);
            if (native)
            {
                oleHwnd = walk;
                break;
            }
            if (walk == root)
                break;
            walk = GetParent(walk);
        }
    }
    if (!native)
        return nullptr;

    OlePassthroughTarget* proxy = new OlePassthroughTarget(handler, native, shouldIntercept);
    if (FAILED(RevokeDragDrop(oleHwnd)))
    {
        proxy->Release();
        return nullptr;
    }
    if (FAILED(RegisterDragDrop(oleHwnd, proxy)))
    {
        RegisterDragDrop(oleHwnd, native);
        proxy->Release();
        return nullptr;
    }
    return std::unique_ptr<DropTarget>(new OlePassthroughRegistration(oleHwnd, proxy));
}


// ---------------------------------------------------------------------------
// Claim: register on a window that may have no OLE (TCP/MCP). Forward to parent.
// ---------------------------------------------------------------------------
static IDropTarget* FindParentOleTarget(HWND hwnd, HWND* oleHwndOut)
{
    HWND root = GetAncestor(hwnd, GA_ROOT);
    for (HWND p = GetParent(hwnd); p; p = GetParent(p))
    {
        if (IDropTarget* t = WindowOleDropTarget(p))
        {
            if (oleHwndOut) *oleHwndOut = p;
            return t;
        }
        if (p == root) break;
    }
    if (oleHwndOut) *oleHwndOut = nullptr;
    return nullptr;
}

class OleClaimTarget : public IDropTarget
{
public:
    OleClaimTarget(DragHandler* handler, IDropTarget* forward, DropTarget::InterceptFn intercept)
        : m_handler(handler), m_forward(forward), m_intercept(intercept)
    {
        if (m_forward) m_forward->AddRef();
    }

    ~OleClaimTarget()
    {
        ResetDrag();
        if (m_forward) m_forward->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        ResetDrag();
        m_data = data;
        if (m_data) m_data->AddRef();
        m_fileCount = CountDroppedFiles(data);
        return Route(keys, pt, effect);
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override
    {
        return Route(keys, pt, effect);
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        HRESULT hr = S_OK;
        if (m_ours)
        {
            m_handler->OnDragLeave();
            m_ours = false;
        }
        if (m_fwdActive && m_forward)
        {
            hr = m_forward->DragLeave();
            m_fwdActive = false;
        }
        ResetDrag();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        HRESULT hr = Route(keys, pt, effect);
        if (SUCCEEDED(hr))
        {
            if (m_ours)
            {
                if (effect) *effect = DROPEFFECT_COPY;
                Modifiers mods = ModifiersFromKeyState(keys);
                // Alt is the bypass key on TCP — do not treat as Add/Replace toggle
                ClearAssignedModifier(mods, g_settings.modSurface);
                m_handler->OnDrop(DroppedFiles(data), pt.x, pt.y, mods);
                m_handler->OnDragLeave();
                m_ours = false;
            }
            else if (m_fwdActive && m_forward)
            {
                hr = m_forward->Drop(data, keys, pt, effect);
                m_fwdActive = false;
            }
        }
        ResetDrag();
        return hr;
    }

private:
    bool WantsOurs(DWORD keys, POINTL pt) const
    {
        if (!m_intercept || m_fileCount <= 0)
            return false;
        return m_intercept(ModifiersFromKeyState(keys), pt.x, pt.y);
    }

    HRESULT Route(DWORD keys, POINTL pt, DWORD* effect)
    {
        if (WantsOurs(keys, pt))
        {
            if (m_fwdActive && m_forward)
            {
                m_forward->DragLeave();
                m_fwdActive = false;
            }
            Modifiers mods = ModifiersFromKeyState(keys);
            ClearAssignedModifier(mods, g_settings.modSurface);
            if (!m_ours)
            {
                m_handler->OnDragEnter(m_fileCount, pt.x, pt.y, mods);
                m_ours = true;
            }
            else
                m_handler->OnDragOver(pt.x, pt.y, mods);
            if (effect) *effect = DROPEFFECT_COPY;
            return S_OK;
        }

        if (m_ours)
        {
            m_handler->OnDragLeave();
            m_ours = false;
        }

        if (!m_forward)
        {
            if (effect) *effect = DROPEFFECT_NONE;
            return S_OK;
        }

        if (!m_fwdActive)
        {
            HRESULT hr = m_forward->DragEnter(m_data ? m_data : nullptr, keys, pt, effect);
            m_fwdActive = SUCCEEDED(hr);
            return hr;
        }
        return m_forward->DragOver(keys, pt, effect);
    }

    void ResetDrag()
    {
        if (m_data) m_data->Release();
        m_data = nullptr;
        m_fileCount = 0;
        m_fwdActive = false;
        m_ours = false;
    }

    LONG m_ref = 1;
    DragHandler* m_handler;
    IDropTarget* m_forward;
    DropTarget::InterceptFn m_intercept;
    IDataObject* m_data = nullptr;
    int m_fileCount = 0;
    bool m_fwdActive = false;
    bool m_ours = false;
};

class OleClaimRegistration : public DropTarget
{
public:
    OleClaimRegistration(HWND hwnd, OleClaimTarget* target)
        : m_hwnd(hwnd), m_target(target) {}

    ~OleClaimRegistration() override
    {
        if (IsWindow(m_hwnd))
            RevokeDragDrop(m_hwnd);
        m_target->Release();
    }

private:
    HWND m_hwnd;
    OleClaimTarget* m_target;
};

std::unique_ptr<DropTarget> DropTarget::AttachClaim(
    HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept)
{
    if (!hwnd || !handler || !shouldIntercept) return nullptr;
    if (!g_oleInited)
    {
        OleInitialize(nullptr);
        g_oleInited = true;
    }

    // Prefer existing OLE on this hwnd; else parent (main) — REAPER's real handler
    HWND oleHwnd = nullptr;
    IDropTarget* forward = WindowOleDropTarget(hwnd);
    if (forward)
        oleHwnd = hwnd;
    else
        forward = FindParentOleTarget(hwnd, &oleHwnd);

    if (!forward)
        return nullptr;

    OleClaimTarget* target = new OleClaimTarget(handler, forward, shouldIntercept);

    // Register on the *claim* hwnd (TCP/MCP), not on the parent
    if (WindowOleDropTarget(hwnd))
        RevokeDragDrop(hwnd);

    HRESULT hr = RegisterDragDrop(hwnd, target);
    if (FAILED(hr))
    {
        target->Release();
        return nullptr;
    }
    return std::unique_ptr<DropTarget>(new OleClaimRegistration(hwnd, target));
}
