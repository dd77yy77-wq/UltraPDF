// Tear-free presentation. The page is still drawn with GDI into a memory bitmap, but instead of BitBlt-ing it into the window
// (DWM can pick the window surface up half-way through the copy: the top of the picture is from one moment, the bottom from
// another, which on a big screen looks like parts of the page moving at different speeds), the finished frame is handed to
// the compositor as a whole through a flip-model DXGI swap chain. Without a usable GPU / Windows 8+ everything falls back to
// the plain BitBlt, so the app works exactly as before there.
//
// The UI thread only copies the finished frame into a system-memory slot (about a millisecond); a worker thread uploads it to
// the GPU and calls Present. Present can block until the screen takes the previous frame, and on a handheld's integrated GPU
// that stalled the message loop during a pinch (jerky zoom). Frames that the screen cannot show in time are dropped, the newest
// one always wins. Two upload paths exist: UpdateSubresource straight into the back buffer (NVIDIA / Intel), and a ring of
// staging textures + CopyResource (AMD: the direct path left coloured garbage on the real display of a Ryzen Z2 Extreme).
// The menu (Settings > Rendering) can force either path or the classic GDI copy.
#include "app.h"
#include <d3d11.h>
#include <dxgi1_3.h>

typedef HRESULT(WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                              ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain1* g_sc = nullptr;
static HANDLE g_wait = nullptr;                 // frame-latency waitable object (at most one frame queued in front of the screen)
static UINT g_flags = 0;
static const int kRing = 3;
static ID3D11Texture2D* g_up[kRing] = {};       // staging textures of the AMD path
static int g_upI = 0, g_upW = 0, g_upH = 0;
static bool g_staged = false;
static HWND g_scWnd = nullptr;
static int g_scW = 0, g_scH = 0;
static bool g_failed = false;                   // creation failed once: do not try again until the mode is changed / window recreated

// ---- hand-over between the UI thread and the worker
enum { S_FREE, S_FILL, S_READY, S_BUSY };
static const int kSlots = 3;
static BYTE* g_slot[kSlots] = {};
static int g_slotState[kSlots] = {};
static int g_pending = -1;                      // newest finished frame waiting for the worker
static int g_fw = 0, g_fh = 0;                  // size of the frames in the slots
static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static HANDLE g_thread = nullptr, g_evFrame = nullptr, g_evStop = nullptr;
static volatile LONG g_dead = 0;                // the worker hit a fatal D3D error

static void Release() {
  for (auto& u : g_up) if (u) { u->Release(); u = nullptr; }
  g_upW = g_upH = 0;
  if (g_wait) { CloseHandle(g_wait); g_wait = nullptr; }
  if (g_sc) { g_sc->Release(); g_sc = nullptr; }
  if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
  if (g_dev) { g_dev->Release(); g_dev = nullptr; }
  g_scWnd = nullptr;
  g_scW = g_scH = 0;
}

static void StopWorker() {
  if (g_thread) {
    SetEvent(g_evStop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_evFrame) { CloseHandle(g_evFrame); g_evFrame = nullptr; }
  if (g_evStop) { CloseHandle(g_evStop); g_evStop = nullptr; }
  for (int i = 0; i < kSlots; i++) { if (g_slot[i]) { VirtualFree(g_slot[i], 0, MEM_RELEASE); g_slot[i] = nullptr; } g_slotState[i] = S_FREE; }
  g_pending = -1;
  g_fw = g_fh = 0;
}

static bool Create(HWND hw, int w, int h) {
  wchar_t e[8];
  if (GetEnvironmentVariableW(L"ULTRAPDF_NOFLIP", e, 8) > 0) return false;           // escape hatch / tests of the fallback
  { wchar_t m[8]; if (GetEnvironmentVariableW(L"ULTRAPDF_FLIPMODE", m, 8) > 0) G.renderMode = _wtoi(m); }   // test override
  if (G.renderMode == 1) return false;
  HMODULE d3d = LoadLibraryW(L"d3d11.dll");
  if (!d3d) return false;
  D3D11CreateDeviceFn create = (D3D11CreateDeviceFn)GetProcAddress(d3d, "D3D11CreateDevice");
  if (!create) return false;
  D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
  if (FAILED(hr) || !g_dev) { Release(); return false; }
  IDXGIDevice* dxgiDev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* factory = nullptr;
  bool ok = false;
  if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
    DXGI_ADAPTER_DESC ad = {};
    g_staged = G.renderMode == 3 || (G.renderMode == 0 && SUCCEEDED(adapter->GetDesc(&ad)) && ad.VendorId == 0x1002);
  }
  if (adapter && SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory))) {
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = (UINT)max(w, 1);
    d.Height = (UINT)max(h, 1);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_NONE;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL};
    UINT flagSets[] = {DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, 0};
    for (UINT fl : flagSets) {
      for (DXGI_SWAP_EFFECT fx : effects) {
        d.SwapEffect = fx;
        d.Flags = fl;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(g_dev, hw, &d, nullptr, nullptr, &g_sc)) && g_sc) { ok = true; g_flags = fl; break; }
      }
      if (ok) break;
    }
    if (ok && (g_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
      IDXGISwapChain2* sc2 = nullptr;
      if (SUCCEEDED(g_sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) && sc2) {
        sc2->SetMaximumFrameLatency(1);
        g_wait = sc2->GetFrameLatencyWaitableObject();
        sc2->Release();
      }
    }
    if (ok) factory->MakeWindowAssociation(hw, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
  }
  if (factory) factory->Release();
  if (adapter) adapter->Release();
  if (dxgiDev) dxgiDev->Release();
  if (!ok) { Release(); return false; }
  g_scWnd = hw;
  g_scW = w;
  g_scH = h;
  return true;
}

// Worker: upload + Present of the newest frame. Only this thread touches the device context after the first frame.
static bool Upload(const BYTE* bits, int w, int h) {
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return false;
  bool ok = true;
  if (!g_staged) {
    g_ctx->UpdateSubresource(tex, 0, nullptr, bits, (UINT)w * 4, 0);
  } else {
    if (!g_up[0] || g_upW != w || g_upH != h) {
      for (auto& u : g_up) if (u) { u->Release(); u = nullptr; }
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
      td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;       // cached system memory: fast sequential writes
      for (auto& u : g_up) if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &u)) || !u) ok = false;
      g_upW = w; g_upH = h;
    }
    if (ok) {
      g_upI = (g_upI + 1) % kRing;                       // a different texture each frame: the GPU may still be reading the previous one
      ID3D11Texture2D* up = g_up[g_upI];
      D3D11_MAPPED_SUBRESOURCE mp = {};
      if (SUCCEEDED(g_ctx->Map(up, 0, D3D11_MAP_WRITE, 0, &mp)) && mp.pData) {
        if (mp.RowPitch == (UINT)w * 4) memcpy(mp.pData, bits, (size_t)w * h * 4);
        else for (int y = 0; y < h; y++) memcpy((BYTE*)mp.pData + (size_t)y * mp.RowPitch, bits + (size_t)y * w * 4, (size_t)w * 4);
        g_ctx->Unmap(up, 0);
        g_ctx->CopyResource(tex, up);
      } else ok = false;
    }
  }
  tex->Release();
  return ok;
}

static DWORD WINAPI Worker(LPVOID) {
  HANDLE ev[2] = {g_evStop, g_evFrame};
  for (;;) {
    DWORD r = WaitForMultipleObjects(2, ev, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0) break;
    int i = -1;
    EnterCriticalSection(&g_cs);
    if (g_pending >= 0) { i = g_pending; g_pending = -1; g_slotState[i] = S_BUSY; }
    int w = g_fw, h = g_fh;
    LeaveCriticalSection(&g_cs);
    if (i < 0) continue;
    bool ok = true;
    if (w != g_scW || h != g_scH) {
      for (auto& u : g_up) if (u) { u->Release(); u = nullptr; }
      g_upW = g_upH = 0;
      ok = SUCCEEDED(g_sc->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, g_flags));
      if (ok) { g_scW = w; g_scH = h; }
    }
    if (ok) {
      if (g_wait) WaitForSingleObjectEx(g_wait, 50, FALSE);   // the screen has taken the previous frame; never queue more than one
      ok = Upload(g_slot[i], w, h);
    }
    if (ok) {
      HRESULT hr = g_sc->Present(0, 0);                       // the compositor takes the whole frame at its next vblank
      if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) ok = false;
    }
    EnterCriticalSection(&g_cs);
    g_slotState[i] = S_FREE;
    LeaveCriticalSection(&g_cs);
    if (!ok) { InterlockedExchange(&g_dead, 1); break; }
  }
  return 0;
}

static bool StartWorker(int w, int h) {
  if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
  size_t bytes = (size_t)w * h * 4;
  for (int i = 0; i < kSlots; i++) {
    g_slot[i] = (BYTE*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_slotState[i] = S_FREE;
    if (!g_slot[i]) { StopWorker(); return false; }
  }
  g_fw = w; g_fh = h; g_pending = -1;
  g_evFrame = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_evStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  InterlockedExchange(&g_dead, 0);
  g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (!g_thread) { StopWorker(); return false; }
  return true;
}

bool PresentActive() { return g_sc != nullptr && !g_dead; }

void PresentShutdown() { StopWorker(); Release(); g_failed = false; }          // also used when the user changes the rendering mode

// bits: top-down 32-bit BGRX, w*4 bytes per row. Returns false when the caller has to BitBlt instead.
bool PresentFrame(HWND hw, const void* bits, int w, int h) {
  if (!bits || w <= 0 || h <= 0) return false;
  if (g_dead) { StopWorker(); Release(); g_failed = true; return false; }
  if (g_sc && g_scWnd != hw) { StopWorker(); Release(); }
  if (!g_sc) {
    if (g_failed) return false;
    if (!Create(hw, w, h)) { g_failed = true; return false; }
  }
  if (g_thread && (w != g_fw || h != g_fh)) StopWorker();       // new frame size: slots are re-made (the swap chain is resized by the worker)
  if (!g_thread && !StartWorker(w, h)) { Release(); g_failed = true; return false; }
  // newest frame wins: a slot that is neither being read nor being filled; if none is free, the waiting (older) frame is replaced
  int i = -1;
  EnterCriticalSection(&g_cs);
  for (int k = 0; k < kSlots && i < 0; k++) if (g_slotState[k] == S_FREE) i = k;
  if (i < 0 && g_pending >= 0) { i = g_pending; g_pending = -1; }
  if (i >= 0) g_slotState[i] = S_FILL;
  LeaveCriticalSection(&g_cs);
  if (i < 0) return true;                                        // cannot happen with 3 slots; the frame is simply skipped
  memcpy(g_slot[i], bits, (size_t)w * h * 4);
  EnterCriticalSection(&g_cs);
  if (g_pending >= 0) g_slotState[g_pending] = S_FREE;
  g_slotState[i] = S_READY;
  g_pending = i;
  LeaveCriticalSection(&g_cs);
  SetEvent(g_evFrame);
  return true;
}
