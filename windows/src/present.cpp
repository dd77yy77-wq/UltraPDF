// Tear-free presentation. The page is still drawn with GDI into a memory bitmap, but instead of BitBlt-ing it into the window
// (DWM can pick the window surface up half-way through the copy: the top of the picture is from one moment, the bottom from
// another, which on a big screen looks like parts of the page moving at different speeds), the finished frame is handed to
// the compositor as a whole through a flip-model DXGI swap chain. Without a usable GPU / Windows 8+ everything falls back to
// the plain BitBlt, so the app works exactly as before there. On AMD graphics (seen on the Ryzen Z2 Extreme handheld) the
// flip chain produced coloured garbage on the real display during zoom (screenshots stay clean), so in the default "auto"
// mode AMD gets a blt-model DXGI chain instead (one atomic GPU copy, no flip/overlay); the menu can force any of the paths.
#include "app.h"
#include <d3d11.h>
#include <dxgi1_2.h>

typedef HRESULT(WINAPI* D3D11CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                              ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain1* g_sc = nullptr;
static HWND g_scWnd = nullptr;
static int g_scW = 0, g_scH = 0;
static bool g_failed = false;            // creation failed once: do not try again until the window is recreated

static void Release() {
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
    DXGI_ADAPTER_DESC ad = {};
    if (G.flipMode == 0 && SUCCEEDED(adapter->GetDesc(&ad)) && ad.VendorId == 0x1002) bltModel = true;   // "auto" on AMD: see the note at the top
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
  g_ctx->UpdateSubresource(tex, 0, nullptr, bits, (UINT)w * 4, 0);
  tex->Release();
  HRESULT hr = g_sc->Present(0, 0);                       // the compositor takes the whole frame at its next vblank
  if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) { Release(); g_failed = true; return false; }
  return true;
}
