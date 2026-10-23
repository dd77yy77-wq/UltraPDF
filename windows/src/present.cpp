// Tear-free presentation. The page is still drawn with GDI into a memory bitmap, but instead of BitBlt-ing it into the window
// (DWM can pick the window surface up half-way through the copy: the top of the picture is from one moment, the bottom from
// another, which on a big screen looks like parts of the page moving at different speeds), the finished frame is handed to
// the compositor as a whole through a flip-model DXGI swap chain. Without a usable GPU / Windows 8+ everything falls back to
// the plain BitBlt, so the app works exactly as before there. The frame is uploaded through a dynamic texture + CopyResource:
// writing it straight into the back buffer with UpdateSubresource produced coloured garbage on the real display of an AMD
// Ryzen Z2 Extreme (screenshots stayed clean). The menu (Appearance > Frame output) can force flip / blt copy / classic GDI.
#include "app.h"
#include <d3d11.h>
#include <dxgi1_2.h>

typedef HRESULT(WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                              ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain1* g_sc = nullptr;
static const int kRing = 3;
static ID3D11Texture2D* g_up[kRing] = {};    // CPU-writable staging textures (ring) the frame is copied through (see PresentFrame)
static int g_upI = 0;
static int g_upW = 0, g_upH = 0;
static HWND g_scWnd = nullptr;
static int g_scW = 0, g_scH = 0;
static bool g_failed = false;            // creation failed once: do not try again until the window is recreated

static void Release() {
  for (auto& u : g_up) if (u) { u->Release(); u = nullptr; }
  g_upW = g_upH = 0;
  if (g_sc) { g_sc->Release(); g_sc = nullptr; }
  if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
  if (g_dev) { g_dev->Release(); g_dev = nullptr; }
  g_scWnd = nullptr;
  g_scW = g_scH = 0;
}

static bool Create(HWND hw, int w, int h) {
  wchar_t e[8];
  if (GetEnvironmentVariableW(L"ULTRAPDF_NOFLIP", e, 8) > 0) return false;           // escape hatch / tests of the fallback
  { wchar_t m[8]; if (GetEnvironmentVariableW(L"ULTRAPDF_FLIPMODE", m, 8) > 0) G.flipMode = _wtoi(m); }   // test override
  if (G.flipMode == 2) return false;
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
  bool bltModel = G.flipMode == 3;
  if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
  }
  if (adapter && SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory))) {
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = (UINT)max(w, 1);
    d.Height = (UINT)max(h, 1);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    if (bltModel) {
      // Blt model: Present() is one GPU copy of the whole frame into the window surface (no half-drawn frames, so still tear-free),
      // but no flip chain / overlay plane is involved.
      d.BufferCount = 1;
      d.Scaling = DXGI_SCALING_STRETCH;
      d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
      DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL};
      for (DXGI_SWAP_EFFECT fx : effects) {
        d.SwapEffect = fx;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(g_dev, hw, &d, nullptr, nullptr, &g_sc)) && g_sc) { ok = true; break; }
      }
    } else {
      d.BufferCount = 2;
      d.Scaling = DXGI_SCALING_NONE;
      d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL};
      for (DXGI_SWAP_EFFECT fx : effects) {
        d.SwapEffect = fx;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(g_dev, hw, &d, nullptr, nullptr, &g_sc)) && g_sc) { ok = true; break; }
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

bool PresentActive() { return g_sc != nullptr; }

void PresentShutdown() { Release(); g_failed = false; }          // also used when the user changes the output mode: the next frame starts afresh

// bits: top-down 32-bit BGRX, w*4 bytes per row. Returns false when the caller has to BitBlt instead.
bool PresentFrame(HWND hw, const void* bits, int w, int h) {
  if (!bits || w <= 0 || h <= 0) return false;
  if (g_sc && g_scWnd != hw) Release();
  if (!g_sc) {
    if (g_failed) return false;
    if (!Create(hw, w, h)) { g_failed = true; return false; }
  }
  if (w != g_scW || h != g_scH) {
    HRESULT hr = g_sc->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { Release(); g_failed = true; return false; }
    g_scW = w;
    g_scH = h;
  }
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) { Release(); g_failed = true; return false; }
  // The frame used to be written straight into the back buffer with UpdateSubresource. On the AMD Ryzen Z2 Extreme that left
  // coloured block garbage on the real display (screenshots stay clean: a compressed-surface problem of the driver). The usual
  // way is used instead: write into an ordinary dynamic texture with Map and let the GPU copy it into the back buffer.
  if (!g_up[0] || g_upW != w || g_upH != h) {
    for (auto& u : g_up) if (u) { u->Release(); u = nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;       // cached system memory: fast sequential writes, no write-combined penalty
    for (auto& u : g_up)
      if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &u)) || !u) { tex->Release(); Release(); g_failed = true; return false; }
    g_upW = w; g_upH = h;
  }
  g_upI = (g_upI + 1) % kRing;                         // a different texture each frame: the GPU may still be reading the previous one
  ID3D11Texture2D* up = g_up[g_upI];
  D3D11_MAPPED_SUBRESOURCE mp = {};
  if (FAILED(g_ctx->Map(up, 0, D3D11_MAP_WRITE, 0, &mp)) || !mp.pData) { tex->Release(); Release(); g_failed = true; return false; }
  if (mp.RowPitch == (UINT)w * 4) memcpy(mp.pData, bits, (size_t)w * h * 4);
  else for (int y = 0; y < h; y++) memcpy((BYTE*)mp.pData + (size_t)y * mp.RowPitch, (const BYTE*)bits + (size_t)y * w * 4, (size_t)w * 4);
  g_ctx->Unmap(up, 0);
  g_ctx->CopyResource(tex, up);
  tex->Release();
  HRESULT hr = g_sc->Present(0, 0);                       // the compositor takes the whole frame at its next vblank
  if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) { Release(); g_failed = true; return false; }
  return true;
}
