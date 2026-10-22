// Start-up splash: logo + name, shown at least 1 s, closed once the window is ready (documents opened, first page drawn)
#include "app.h"

static HWND g_splash = nullptr;
static uint64_t g_shown = 0;
static int g_alpha = 0;
static int g_fadeOut = 0;
static int g_w = 0, g_h = 0;
static double g_sc = 1.0;
static const wchar_t kSplashClass[] = L"UltraPDF.Splash";
enum { TMR_SPL = 1 };
static uint64_t kMinMs = 1000;            // never closes earlier than this (ULTRAPDF_SPLASH_MS overrides it for tests)
static const uint64_t kMaxMs = 8000;      // never hangs longer than this (huge file, broken disk)

static int Sp(int v) { return (int)lround(v * g_sc); }

static void Paint(HWND hw) {
  PAINTSTRUCT ps;
  HDC dc0 = BeginPaint(hw, &ps);
  HDC dc = CreateCompatibleDC(dc0);
  HBITMAP bm = CreateCompatibleBitmap(dc0, g_w, g_h);
  HGDIOBJ old = SelectObject(dc, bm);
  RECT rc = {0, 0, g_w, g_h};
  bool dark = T.dark;
  COLORREF bg = dark ? RGB(30, 31, 34) : RGB(255, 255, 255);
  COLORREF line = dark ? RGB(70, 72, 78) : RGB(205, 208, 214);
  HBRUSH b = CreateSolidBrush(bg);
  FillRect(dc, &rc, b);
  DeleteObject(b);
  HBRUSH lb = CreateSolidBrush(line);
  FrameRect(dc, &rc, lb);
  DeleteObject(lb);
  int icon = Sp(112);
  DrawAppIcon(dc, (g_w - icon) / 2, Sp(30), icon);
  RECT r = {0, Sp(30) + icon + Sp(8), g_w, Sp(30) + icon + Sp(56)};
  DrawTextW2(dc, L"UltraPDF", r, BrandRed(dark), UiFont(Sp(33), true), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  r = {0, Sp(30) + icon + Sp(58), g_w, Sp(30) + icon + Sp(82)};
  DrawTextW2(dc, L"Загрузка…", r, dark ? RGB(160, 162, 168) : RGB(110, 113, 120), UiFont(Sp(14)), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  BitBlt(dc0, 0, 0, g_w, g_h, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bm);
  DeleteDC(dc);
  EndPaint(hw, &ps);
}

static LRESULT CALLBACK SplashProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(hw); return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;       // clicks pass through to whatever is below
    case WM_TIMER: {
      uint64_t el = GetTickCount64() - g_shown;
      if (!g_fadeOut) {
        if (g_alpha < 255) { g_alpha = min(255, g_alpha + 45); SetLayeredWindowAttributes(hw, 0, (BYTE)g_alpha, LWA_ALPHA); }
        if (el >= kMinMs && (AppIsReady() || el >= kMaxMs)) g_fadeOut = 1;
      } else {
        g_alpha -= 40;
        if (g_alpha <= 0) { SplashClose(); return 0; }
        SetLayeredWindowAttributes(hw, 0, (BYTE)g_alpha, LWA_ALPHA);
      }
      return 0;
    }
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

void SplashShow(HINSTANCE inst) {
  if (g_splash) return;
  HDC sdc = GetDC(nullptr);
  int dpi = GetDeviceCaps(sdc, LOGPIXELSX);
  ReleaseDC(nullptr, sdc);
  g_sc = dpi > 48 ? dpi / 96.0 : 1.0;
  g_w = Sp(340);
  g_h = Sp(296);
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = SplashProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kSplashClass;
  RegisterClassExW(&wc);
  RECT wa;
  POINT pt;
  GetCursorPos(&pt);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
  wa = mi.rcWork;
  int x = wa.left + (wa.right - wa.left - g_w) / 2, y = wa.top + (wa.bottom - wa.top - g_h) / 2;
  g_splash = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kSplashClass, L"UltraPDF", WS_POPUP, x, y, g_w, g_h,
                             nullptr, nullptr, inst, nullptr);
  if (!g_splash) return;
  HRGN rg = CreateRoundRectRgn(0, 0, g_w + 1, g_h + 1, Sp(18), Sp(18));
  SetWindowRgn(g_splash, rg, FALSE);
  g_alpha = 0;
  SetLayeredWindowAttributes(g_splash, 0, 0, LWA_ALPHA);
  ShowWindow(g_splash, SW_SHOWNOACTIVATE);
  UpdateWindow(g_splash);
  g_shown = GetTickCount64();
  { wchar_t e[16]; if (GetEnvironmentVariableW(L"ULTRAPDF_SPLASH_MS", e, 16) > 0) kMinMs = (uint64_t)_wtoi(e); }
  g_alpha = 120;                                    // appears at once, finishes fading in during the first ticks
  SetLayeredWindowAttributes(g_splash, 0, (BYTE)g_alpha, LWA_ALPHA);
  SetTimer(g_splash, TMR_SPL, 16, nullptr);
}

void SplashClose() {
  if (!g_splash) return;
  KillTimer(g_splash, TMR_SPL);
  DestroyWindow(g_splash);
  g_splash = nullptr;
}

bool SplashActive() { return g_splash != nullptr; }

void SplashRaise() { if (g_splash) SetWindowPos(g_splash, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); }
