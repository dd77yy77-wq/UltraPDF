// Main window: tabs, toolbar, find bar, sidebar header, commands, keyboard, files, state
#include "app.h"
#include "editor.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

App A_;
static std::vector<ws> g_tipText;
static HWND g_tip = nullptr;

enum { TMR_SAVE = 10, TMR_FIND = 11, TMR_TOAST = 12 };
static const wchar_t* kMainClass = APP_ID L".Main";
HANDLE g_mutex = nullptr;

// ---------------------------------------------------------------- chrome geometry
struct Btn { int id; RECT r; Icon ic; ws tip; bool enabled; bool checked; int kind; };  // kind 0 icon, 1 zoom label
static std::vector<Btn> g_btns;
static std::vector<Btn> g_fbtns;     // find bar buttons
static RECT g_edR = {0, 0, 0, 0};
static RECT g_modeSeg[3];
static int g_hoverMode = -1;
static int ModeBarW() { return S(270); }
static RECT g_tabsR, g_tbR, g_findR, g_hdrR, g_splitR, g_pageLblR, g_findLblR, g_caseR;
static struct TabRect { RECT r, close; } g_tabRects[64];
static RECT g_plusR;
static RECT g_segR[3];
static int g_hoverBtn = -1, g_pressBtn = -1, g_hoverTab = -1, g_hoverSeg = -1;
static int g_dragTab = -1, g_dragStartX = 0;
static bool g_splitDrag = false;
static bool g_chromeTrack = false;
static ws g_toast;
static HBRUSH g_editBrush = nullptr;
static ws g_lastPageText;

static int TabH() { return S(36); }
static int TbH() { return S(44); }
static int FindH() { return S(42); }
static int HdrH() { return S(36); }

Tab* CurTab() { return (A_.cur >= 0 && A_.cur < (int)A_.tabs.size()) ? A_.tabs[A_.cur].get() : nullptr; }
Tab* TabById(int id) {
  for (auto& t : A_.tabs) if (t->id == id) return t.get();
  return nullptr;
}

// ---------------------------------------------------------------- theme plumbing
static void ApplyTitleBar() {
  BOOL dark = T.dark;
  HMODULE dw = LoadLibraryW(L"dwmapi.dll");
  if (!dw) return;
  typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
  SetAttr f = (SetAttr)GetProcAddress(dw, "DwmSetWindowAttribute");
  if (f) {
    if (FAILED(f(A_.main, 20, &dark, sizeof(dark)))) f(A_.main, 19, &dark, sizeof(dark));
  }
  FreeLibrary(dw);
}
static void ApplyMenuTheme() {
  HMODULE ux = LoadLibraryW(L"uxtheme.dll");
  if (!ux) return;
  typedef int(WINAPI * SetMode)(int);
  typedef void(WINAPI * Flush)();
  SetMode sm = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
  Flush fl = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
  if (sm) sm(T.dark ? 2 : 3);
  if (fl) fl();
}
static void ThemeChildren() {
  if (g_editBrush) DeleteObject(g_editBrush);
  g_editBrush = CreateSolidBrush(T.field);
  const wchar_t* th = T.dark ? L"DarkMode_Explorer" : L"Explorer";
  SetWindowTheme(A_.view, th, nullptr);
  SetWindowTheme(A_.side, th, nullptr);
}

static void ReTheme() {
  ApplyTheme(G.themeMode);
  ApplyTitleBar();
  ApplyMenuTheme();
  ThemeChildren();
  InvalidateRect(A_.main, nullptr, TRUE);
  ViewInvalidate();
  SideInvalidate();
  InvalidateRect(A_.pageEdit, nullptr, TRUE);
  InvalidateRect(A_.findEdit, nullptr, TRUE);
}

// ---------------------------------------------------------------- tooltips
static void RebuildTips() {
  if (!g_tip) {
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, A_.main,
                            nullptr, A_.inst, nullptr);
  }
  int cnt = (int)SendMessageW(g_tip, TTM_GETTOOLCOUNT, 0, 0);
  for (int i = cnt - 1; i >= 0; i--) {
    TTTOOLINFOW ti = {sizeof(ti)};
    if (SendMessageW(g_tip, TTM_ENUMTOOLSW, i, (LPARAM)&ti)) SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
  }
  g_tipText.clear();
  g_tipText.reserve(g_btns.size() + g_fbtns.size() + 2);
  uint32_t uid = 1;
  std::vector<Btn> all = g_btns;
  all.insert(all.end(), g_fbtns.begin(), g_fbtns.end());
#ifdef APP_EDITOR
  std::vector<std::pair<RECT, ws>> edTips;
  EdBarTips(&edTips);
  for (auto& et : edTips) {
    Btn b = {};
    b.r = et.first; b.tip = et.second;
    all.push_back(b);
  }
#endif
  for (auto& b : all) {
    if (b.tip.empty()) continue;
    g_tipText.push_back(b.tip);
    TTTOOLINFOW ti = {sizeof(ti)};
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = A_.main;
    ti.uId = uid++;
    ti.rect = b.r;
    ti.lpszText = (LPWSTR)g_tipText.back().c_str();
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  }
}

// ---------------------------------------------------------------- toolbar model
static void AddBtn(int& x, int y, int id, Icon ic, const wchar_t* tip, bool en = true, bool chk = false) {
  Btn b;
  b.id = id; b.ic = ic; b.tip = tip; b.enabled = en; b.checked = chk; b.kind = 0;
  b.r = {x, y, x + S(36), y + S(36)};
  x += S(38);
  g_btns.push_back(b);
}
static void Gap2(int& x) { x += S(10); }

static void BuildToolbar() {
  g_btns.clear();
  Tab* t = CurTab();
  bool doc = t && t->doc;
  int y = g_tbR.top + (TbH() - S(36)) / 2;
  int x = S(6);
  int W = g_tbR.right;
  AddBtn(x, y, ID_OPEN + 1000, IC_MENU, L"Меню");
  if (A_.fullscreen) return;
  if (doc) AddBtn(x, y, ID_SIDEBAR, IC_SIDEBAR, L"Боковая панель (F4)", true, G.sidebar);
  AddBtn(x, y, ID_OPEN, IC_OPEN, L"Открыть (Ctrl+O)");
  g_pageLblR = {0, 0, 0, 0};
  if (doc) {
    Gap2(x);
    AddBtn(x, y, ID_PAGE_PREV, IC_UP, L"Предыдущая страница");
    int pb = x;
    x += S(50);                                // page edit goes here
    int lblw = S(50);
    g_pageLblR = {x, y, x + lblw, y + S(36)};
    SetWindowPos(A_.pageEdit, nullptr, pb, y + S(5), S(48), S(26), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    x += lblw;
    AddBtn(x, y, ID_PAGE_NEXT, IC_DOWN, L"Следующая страница");
    Gap2(x);
    AddBtn(x, y, ID_ZOOM_OUT, IC_ZOUT, L"Уменьшить (Ctrl+−)");
    Btn z;
    z.id = ID_ZOOM_PRESET; z.ic = IC_ZIN; z.enabled = true; z.checked = false; z.kind = 1; z.tip = L"Масштаб";
    z.r = {x, y, x + S(62), y + S(36)};
    x += S(64);
    g_btns.push_back(z);
    AddBtn(x, y, ID_ZOOM_IN, IC_ZIN, L"Увеличить (Ctrl++)");
    AddBtn(x, y, ID_FIT_W, IC_FITW, L"По ширине (Ctrl+9)", true, t->fit == 1);
    AddBtn(x, y, ID_FIT_P, IC_FITP, L"Страница целиком (Ctrl+8)", true, t->fit == 2);
    if (G.pro && W > S(900)) { Gap2(x); AddBtn(x, y, ID_ROT_CW, IC_ROT, L"Повернуть (Ctrl+R)"); }
    Gap2(x);
    AddBtn(x, y, ID_FIND, IC_SEARCH, L"Поиск (Ctrl+F)", true, A_.findOpen);
    AddBtn(x, y, ID_NIGHT_TOGGLE, IC_NIGHT, L"Ночной режим (Ctrl+M)", true, G.readMode == 2);
    if (G.pro) AddBtn(x, y, ID_BOOKMARK, IsBookmarked(t, ViewCurPage()) ? IC_BOOKON : IC_BOOK, L"Закладка (Ctrl+D)", true, IsBookmarked(t, ViewCurPage()));
    // right aligned group
    int rx = W - S(6);
    if (G.pro) {
      Btn b;
      b.enabled = true; b.checked = false; b.kind = 0;
      b.id = ID_FULLSCREEN; b.ic = IC_FULL; b.tip = L"Во весь экран (F11)"; b.r = {rx - S(36), y, rx, y + S(36)}; rx -= S(38);
      if (rx > x + S(120)) g_btns.push_back(b);
      b.id = ID_PRESENT; b.ic = IC_PRES; b.tip = L"Презентация (F5)"; b.r = {rx - S(36), y, rx, y + S(36)}; rx -= S(38);
      if (rx > x + S(120)) g_btns.push_back(b);
      b.id = ID_PRINT; b.ic = IC_PRINT; b.tip = L"Печать (Ctrl+P)"; b.r = {rx - S(36), y, rx, y + S(36)}; rx -= S(38);
      if (rx > x + S(120)) g_btns.push_back(b);
    }
  }
}

void MainUpdatePageBox() {
  Tab* t = CurTab();
  if (!t || !t->doc || !A_.pageEdit) return;
  ws txt = std::to_wstring(ViewCurPage() + 1);
  if (txt != g_lastPageText) {
    g_lastPageText = txt;
    if (GetFocus() != A_.pageEdit) SetWindowTextW(A_.pageEdit, txt.c_str());
    InvalidateRect(A_.main, &g_tbR, FALSE);
  }
}

// ---------------------------------------------------------------- layout
void MainLayout() {
  if (!A_.main || !A_.view) return;
  RECT rc;
  GetClientRect(A_.main, &rc);
  int W = rc.right, H = rc.bottom;
  Tab* t = CurTab();
  if (A_.fullscreen) {
    ShowWindow(A_.side, SW_HIDE);
    ShowWindow(A_.pageEdit, SW_HIDE);
    ShowWindow(A_.findEdit, SW_HIDE);
    MoveWindow(A_.view, 0, 0, W, H, TRUE);
    g_btns.clear();
    g_edR = {0, 0, 0, 0};
    return;
  }
  int y = 0;
  g_tabsR = {0, 0, W, TabH()};
  y = TabH();
  g_tbR = {0, y, W, y + TbH()};
  y += TbH();
#ifdef APP_EDITOR
  {
    int eh = EdBarHeight();
    g_edR = {0, y, W, y + eh};
    y += eh;
    EdBarLayout(g_edR);
  }
#endif
  bool showFind = A_.findOpen && t && t->doc;
  if (showFind) { g_findR = {0, y, W, y + FindH()}; y += FindH(); } else g_findR = {0, 0, 0, 0};
  bool showSide = G.sidebar && t && t->doc;
  int sw = showSide ? S(G.sideW) : 0;
  bool hdr = showSide && G.pro;
  int hh = hdr ? HdrH() : 0;
  g_hdrR = {0, y, sw, y + hh};
  g_splitR = showSide ? RECT{sw, y, sw + S(4), H} : RECT{0, 0, 0, 0};
  if (showSide) {
    MoveWindow(A_.side, 0, y + hh, sw, H - y - hh, TRUE);
    ShowWindow(A_.side, SW_SHOW);
  } else ShowWindow(A_.side, SW_HIDE);
  int vx = showSide ? sw + S(4) : 0;
  MoveWindow(A_.view, vx, y, W - vx, H - y, TRUE);
  if (!(t && t->doc)) ShowWindow(A_.pageEdit, SW_HIDE);
  BuildToolbar();
  if (!(t && t->doc)) ShowWindow(A_.pageEdit, SW_HIDE);
  // find bar controls
  if (showFind) {
    int x = S(12);
    SetWindowPos(A_.findEdit, nullptr, x, g_findR.top + S(7), S(280), S(28), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    x += S(286);
    g_caseR = {x, g_findR.top + S(3), x + S(36), g_findR.top + S(39)};
    x += S(40);
    Btn b;
    b.enabled = true; b.checked = false; b.kind = 0;
    g_fbtns.clear();
    b.id = ID_FIND_PREV; b.ic = IC_UP; b.tip = L"Предыдущее (Shift+F3)"; b.r = {x, g_findR.top + S(3), x + S(36), g_findR.top + S(39)}; x += S(38);
    g_fbtns.push_back(b);
    b.id = ID_FIND_NEXT; b.ic = IC_DOWN; b.tip = L"Следующее (F3)"; b.r = {x, g_findR.top + S(3), x + S(36), g_findR.top + S(39)}; x += S(40);
    g_fbtns.push_back(b);
    g_findLblR = {x, g_findR.top, x + S(220), g_findR.bottom};
    x += S(224);
    b.id = ID_FIND + 5000; b.ic = IC_CLOSE; b.tip = L"Закрыть поиск (Esc)"; b.r = {x, g_findR.top + S(3), x + S(36), g_findR.top + S(39)};
    g_fbtns.push_back(b);
  } else { g_fbtns.clear(); ShowWindow(A_.findEdit, SW_HIDE); }
  // sidebar header segments
  if (hdr) {
    int segw = (sw - S(12)) / 3;
    for (int i = 0; i < 3; i++) g_segR[i] = {S(6) + i * segw, g_hdrR.top + S(4), S(6) + (i + 1) * segw, g_hdrR.bottom - S(4)};
  }
  RebuildTips();
  InvalidateRect(A_.main, nullptr, FALSE);
  ViewSync();
  SideSync();
}

void MainRelayoutAll() {
  for (auto& t : A_.tabs) t->layoutOk = false;
  MainLayout();
  ViewInvalidate();
}
void MainRedrawChrome() {
  if (!A_.main) return;
  // toolbar state (fit buttons etc) may have changed
  BuildToolbar();
  InvalidateRect(A_.main, nullptr, FALSE);
}

void UpdateTitle() {
  Tab* t = CurTab();
  ws s = t ? (EdIsEdited(t) ? L"\u2022 " : L"") + t->title + L" — " APP_TITLE : ws(APP_TITLE);
  SetWindowTextW(A_.main, s.c_str());
}

void ShowToast(const ws& m) {
  g_toast = m;
  SetTimer(A_.main, TMR_TOAST, 1600, nullptr);
  ViewInvalidate();
}
void DrawToast(HDC dc, RECT rc) {
  if (g_toast.empty()) return;
  RECT r = {0, 0, 0, 0};
  HGDIOBJ old = SelectObject(dc, UiFont(S(14)));
  DrawTextW(dc, g_toast.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE);
  SelectObject(dc, old);
  int w = r.right + S(36), h = S(38);
  RECT b = {(rc.right - w) / 2, rc.bottom - S(70), (rc.right + w) / 2, rc.bottom - S(70) + h};
  FillRoundRect(dc, b, S(8), RGB(30, 30, 30), 225);
  DrawTextW2(dc, g_toast, b, RGB(255, 255, 255), UiFont(S(14)), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------- chrome painting
static void DrawBtn(HDC dc, const Btn& b, int idx) {
  bool hot = idx == g_hoverBtn, pr = idx == g_pressBtn && hot;
  if (b.kind == 1) {
    Tab* t = CurTab();
    if (hot) FillRoundRect(dc, b.r, S(5), pr ? T.press : T.hover);
    int pct = t ? (int)lround(t->zoom * 100) : 100;
    DrawTextW2(dc, FmtT(L"%d%%", pct), b.r, T.text, UiFont(S(13)), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  if (b.checked) FillRoundRect(dc, b.r, S(5), T.accent, 55);
  else if (hot && b.enabled) FillRoundRect(dc, b.r, S(5), pr ? T.press : T.hover);
  COLORREF c = !b.enabled ? T.dim : (b.checked ? T.accent : T.text);
  FRect ir = {(float)b.r.left + S(7), (float)b.r.top + S(7), (float)b.r.right - S(7), (float)b.r.bottom - S(7)};
  DrawIcon(dc, b.ic, ir, c, 1.6f);
}

static void DrawTabs(HDC dc) {
  HBRUSH br = CreateSolidBrush(T.tabIdle);
  FillRect(dc, &g_tabsR, br);
  DeleteObject(br);
  int n = (int)A_.tabs.size();
  int plusW = S(34);
  int avail = g_tabsR.right - S(8) - plusW - S(8);
#ifdef APP_EDITOR
  avail -= ModeBarW();
#endif
  int tw = n ? min(S(220), max(S(56), avail / n)) : S(150);
  int x = S(6);
  for (int i = 0; i < n && i < 64; i++) {
    RECT r = {x, S(5), x + tw - S(2), TabH()};
    g_tabRects[i].r = r;
    bool act = i == A_.cur;
    if (act) {
      RECT rr = {r.left, r.top, r.right, r.bottom + S(8)};
      FillRoundRect(dc, rr, S(7), T.tabActive);
    } else if (i == g_hoverTab) {
      RECT rr = {r.left, r.top + S(3), r.right, r.bottom - S(3)};
      FillRoundRect(dc, rr, S(6), T.hover);
    }
    RECT cr = {r.right - S(28), r.top + S(5), r.right - S(6), r.top + S(27)};
    g_tabRects[i].close = cr;
    RECT tr = {r.left + S(10), r.top, r.right - S(30), r.bottom};
    ws title = A_.tabs[i]->title;
    if (EdIsEdited(A_.tabs[i].get())) title = L"\u2022 " + title;
    if (A_.tabs[i]->loading) title = L"… " + title;
    DrawTextW2(dc, title, tr, act ? T.text : T.dim, UiFont(S(13)), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (act || i == g_hoverTab) {
      FRect ir = {(float)cr.left + S(4), (float)cr.top + S(3), (float)cr.right - S(4), (float)cr.bottom - S(3)};
      DrawIcon(dc, IC_CLOSE, ir, T.dim, 1.5f);
    }
    x += tw;
  }
  g_plusR = {x + S(2), S(5), x + S(2) + plusW, TabH() - S(3)};
  if (g_hoverTab == 100) FillRoundRect(dc, g_plusR, S(6), T.hover);
  FRect pr = {(float)g_plusR.left + S(8), (float)g_plusR.top + S(5), (float)g_plusR.right - S(8), (float)g_plusR.bottom - S(5)};
  DrawIcon(dc, IC_PLUS, pr, T.dim, 1.6f);
#ifdef APP_EDITOR
  {
    // three-position switch: viewer / viewer Pro / editor
    static const wchar_t* names[3] = {L"Просмотр", L"Pro", L"Редактор"};
    int ws_[3] = {S(96), S(62), S(100)};
    RECT all = {g_tabsR.right - S(8) - (ws_[0] + ws_[1] + ws_[2]) - S(6), S(6), g_tabsR.right - S(8), TabH() - S(4)};
    FillRoundRect(dc, all, S(9), T.hover);
    int sx = all.left + S(3);
    for (int i = 0; i < 3; i++) {
      g_modeSeg[i] = {sx, all.top + S(3), sx + ws_[i], all.bottom - S(3)};
      sx += ws_[i];
      bool on = G.edMode == i;
      if (on) FillRoundRect(dc, g_modeSeg[i], S(7), T.accent);
      else if (g_hoverMode == i) FillRoundRect(dc, g_modeSeg[i], S(7), T.press);
      DrawTextW2(dc, names[i], g_modeSeg[i], on ? RGB(255, 255, 255) : T.text, UiFont(S(12), on), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }
#endif
}

static ws FindLabel(Tab* t) {
  if (!t || t->query.empty()) return L"";
  if (t->hits.empty()) return t->searchDone ? L"Ничего не найдено" : FmtT(L"Поиск… %d из %d стр.", t->searchScanned, t->doc ? t->doc->npages : 0);
  ws more = t->searchDone ? L"" : L"+";
  if (t->curHit >= 0) return FmtT(L"%d из %d%s", t->curHit + 1, (int)t->hits.size(), more.c_str());
  return FmtT(L"Найдено: %d%s", (int)t->hits.size(), more.c_str());
}

static void PaintMain(HWND hw) {
  PAINTSTRUCT ps;
  HDC dc0 = BeginPaint(hw, &ps);
  RECT rc;
  GetClientRect(hw, &rc);
  if (A_.fullscreen) { EndPaint(hw, &ps); return; }
  HDC dc = CreateCompatibleDC(dc0);
  HBITMAP bm = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
  HGDIOBJ old = SelectObject(dc, bm);
  HBRUSH b = CreateSolidBrush(T.chrome);
  FillRect(dc, &rc, b);
  DeleteObject(b);
  DrawTabs(dc);
  // toolbar
  b = CreateSolidBrush(T.tabActive);
  RECT tb = g_tbR;
  FillRect(dc, &tb, b);
  DeleteObject(b);
  HPEN pen = CreatePen(PS_SOLID, 1, T.line);
  HGDIOBJ op = SelectObject(dc, pen);
  MoveToEx(dc, 0, g_tbR.bottom - 1, nullptr);
  LineTo(dc, rc.right, g_tbR.bottom - 1);
  Tab* t = CurTab();
  for (size_t i = 0; i < g_btns.size(); i++) DrawBtn(dc, g_btns[i], (int)i);
#ifdef APP_EDITOR
  EdBarPaint(dc, g_edR);
#endif
  if (t && t->doc && g_pageLblR.right > g_pageLblR.left) {
    RECT lr = g_pageLblR;
    lr.left += S(4);
    DrawTextW2(dc, FmtT(L"/ %d", t->doc->npages), lr, T.dim, UiFont(S(13)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  // find bar
  if (g_findR.bottom > g_findR.top) {
    b = CreateSolidBrush(T.chrome2);
    FillRect(dc, &g_findR, b);
    DeleteObject(b);
    MoveToEx(dc, 0, g_findR.bottom - 1, nullptr);
    LineTo(dc, rc.right, g_findR.bottom - 1);
    if (G.matchCase) FillRoundRect(dc, g_caseR, S(5), T.accent, 55);
    else if (g_hoverBtn == -2) FillRoundRect(dc, g_caseR, S(5), T.hover);
    DrawTextW2(dc, L"Aa", g_caseR, G.matchCase ? T.accent : T.text, UiFont(S(14), true), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (size_t i = 0; i < g_fbtns.size(); i++) DrawBtn(dc, g_fbtns[i], 1000 + (int)i);
    DrawTextW2(dc, FindLabel(t), g_findLblR, T.dim, UiFont(S(13)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  // sidebar header and splitter
  if (g_hdrR.right > g_hdrR.left && g_hdrR.bottom > g_hdrR.top) {
    b = CreateSolidBrush(T.chrome2);
    FillRect(dc, &g_hdrR, b);
    DeleteObject(b);
    static const wchar_t* names[3] = {L"Страницы", L"Оглавление", L"Закладки"};
    for (int i = 0; i < 3; i++) {
      bool act = G.sideTab == i;
      if (act) FillRoundRect(dc, g_segR[i], S(5), T.accent, 55);
      else if (g_hoverSeg == i) FillRoundRect(dc, g_segR[i], S(5), T.hover);
      DrawTextW2(dc, names[i], g_segR[i], act ? T.accent : T.text, UiFont(S(12), act), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
  }
  if (g_splitR.right > g_splitR.left) {
    b = CreateSolidBrush(T.line);
    RECT l = {g_splitR.left + S(1), g_splitR.top, g_splitR.left + S(2) + 0, g_splitR.bottom};
    l.right = l.left + 1;
    FillRect(dc, &l, b);
    DeleteObject(b);
    b = CreateSolidBrush(T.chrome2);
    RECT s2 = {g_splitR.left + 2, g_splitR.top, g_splitR.right, g_splitR.bottom};
    FillRect(dc, &s2, b);
    DeleteObject(b);
  }
  SelectObject(dc, op);
  DeleteObject(pen);
  BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old);
  DeleteObject(bm);
  DeleteDC(dc);
  EndPaint(hw, &ps);
}

// ---------------------------------------------------------------- tabs
static void SaveTabState(Tab* t) {
  if (!t || !t->doc) return;
  ViewSaveState(t);
  t->saved.path = t->path;
  TouchState(t->saved);
  t->dirty = false;
}

void SaveAllState() {
  G.session.clear();
  for (auto& t : A_.tabs) {
    SaveTabState(t.get());
    G.session.push_back(t->path);
  }
  G.sessionActive = A_.cur;
  if (A_.main && !A_.fullscreen && !IsIconic(A_.main)) {
    WINDOWPLACEMENT wp = {sizeof(wp)};
    GetWindowPlacement(A_.main, &wp);
    G.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    G.winX = wp.rcNormalPosition.left;
    G.winY = wp.rcNormalPosition.top;
    G.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    G.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
  }
  SaveSettings();
}

static void SyncFindEditToTab() {
  Tab* t = CurTab();
  SetWindowTextW(A_.findEdit, t ? t->query.c_str() : L"");
}

void SwitchTab(int idx) {
  if (idx < 0 || idx >= (int)A_.tabs.size()) return;
  Tab* old = CurTab();
#ifdef APP_EDITOR
  EdCommitText();
  EdOnTabChanged();
#endif
  if (old && old->doc) SaveTabState(old);
  SearchCancel();
  A_.cur = idx;
  Tab* t = CurTab();
  g_lastPageText.clear();
  SyncFindEditToTab();
  if (t->doc && !t->searchDone && !t->query.empty()) {
    t->hits.clear();
    t->curHit = -1;
    SearchStart(t->doc, t->query, G.matchCase, t->id);
  }
  UpdateTitle();
  MainLayout();
  ViewSync();
  SideSync();
  MainUpdatePageBox();
  ViewInvalidate();
  InvalidateRect(A_.main, nullptr, FALSE);
  if (t->needPw && !t->doc) {
    t->needPw = false;
    t->loading = true;
    t->error.clear();
    WorkerOpen(t->id, t->path, "");
  }
}

void CloseTab(int idx) {
  if (idx < 0 || idx >= (int)A_.tabs.size()) return;
  Tab* t = A_.tabs[idx].get();
#ifdef APP_EDITOR
  EdCommitText();
  if (!EdConfirmClose(t)) return;
  EdOnTabChanged();
#endif
  if (t->doc) SaveTabState(t);
  SearchCancel();
  CloseDoc(t->doc);
  A_.tabs.erase(A_.tabs.begin() + idx);
  if (A_.tabs.empty()) A_.cur = -1;
  else if (A_.cur > idx || A_.cur >= (int)A_.tabs.size()) A_.cur = min(A_.cur - (A_.cur > idx ? 1 : 0), (int)A_.tabs.size() - 1);
  if (A_.cur < 0 && !A_.tabs.empty()) A_.cur = 0;
  g_lastPageText.clear();
  SyncFindEditToTab();
  if (A_.tabs.empty()) { A_.findOpen = false; if (A_.fullscreen) MainSetFullscreen(false, false); }
  UpdateTitle();
  MainLayout();
  ViewSync();
  SideSync();
  ViewInvalidate();
  SaveSettings();
}

void OpenPath(const ws& path0) {
  wchar_t full[MAX_PATH * 2];
  DWORD n = GetFullPathNameW(path0.c_str(), MAX_PATH * 2, full, nullptr);
  ws path = n ? ws(full, n) : path0;
  ws lp = LowerW(path);
  for (size_t i = 0; i < A_.tabs.size(); i++)
    if (LowerW(A_.tabs[i]->path) == lp) { SwitchTab((int)i); return; }
  if (!FileExists(path)) {
    MessageBoxW(A_.main, (L"Файл не найден:\n" + path).c_str(), APP_TITLE, MB_OK | MB_ICONWARNING);
    return;
  }
  auto t = std::make_unique<Tab>();
  t->id = A_.nextTabId++;
  t->path = path;
  t->title = StripExt(BaseName(path));
  t->saved.path = path;
  int id = t->id;
  Tab* old = CurTab();
  if (old && old->doc) SaveTabState(old);
  A_.tabs.push_back(std::move(t));
  A_.cur = (int)A_.tabs.size() - 1;
  SearchCancel();
  SyncFindEditToTab();
  UpdateTitle();
  MainLayout();
  ViewInvalidate();
  WorkerOpen(id, path, "");
}

void OpenFiles(const std::vector<ws>& paths, bool activate) {
  for (auto& p : paths) OpenPath(p);
  if (activate && A_.main) {
    if (IsIconic(A_.main)) ShowWindow(A_.main, SW_RESTORE);
    SetForegroundWindow(A_.main);
  }
}

void ApplyOpened(OpenResult* r) {
  Tab* t = TabById(r->tabId);
  if (!t) { delete r; return; }
  if (r->error == 2 && t != CurTab()) {
    t->loading = false;
    t->needPw = true;
    t->error = L"Документ защищён паролем. Откройте вкладку, чтобы ввести пароль.";
    delete r;
    InvalidateRect(A_.main, nullptr, FALSE);
    return;
  }
  if (r->error == 2 || r->error == 3) {
    ws pw;
    bool ok = InputPassword(A_.main, BaseName(r->path), r->error == 3, &pw);
    if (ok) {
      t->loading = true;
      WorkerOpen(t->id, t->path, WToUtf8(pw));
      delete r;
      ViewInvalidate();
      return;
    }
    t->loading = false;
    t->error = L"Для этого документа нужен пароль";
  } else if (r->error == 1) {
    t->loading = false;
    t->error = L"Не удалось прочитать файл";
  } else if (r->error != 0) {
    t->loading = false;
    t->error = L"Файл повреждён или это не PDF";
  } else {
    t->loading = false;
    t->doc = r->doc;
    DocState* st = FindState(t->path, false);
    if (st) t->saved = *st;
    t->saved.path = t->path;
    ViewPrepareTab(t);
    TouchState(t->saved);
  }
  delete r;
  if (t == CurTab()) {
    g_lastPageText.clear();
    MainLayout();
    ViewSync();
    SideSync();
    MainUpdatePageBox();
    ViewInvalidate();
  }
  InvalidateRect(A_.main, nullptr, FALSE);
}

// ---------------------------------------------------------------- find
static void StartSearchFromEdit() {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  wchar_t buf[512];
  GetWindowTextW(A_.findEdit, buf, 512);
  t->query = buf;
  t->hits.clear();
  t->curHit = -1;
  t->jumpPending = true;
  t->searchDone = t->query.empty();
  t->searchScanned = 0;
  SearchStart(t->doc, t->query, G.matchCase, t->id);
  ViewInvalidate();
  InvalidateRect(A_.main, &g_findR, FALSE);
}

void MainShowFind(bool show) {
  Tab* t = CurTab();
  if (show && !(t && t->doc)) return;
  A_.findOpen = show;
  MainLayout();
  if (show) {
    SetFocus(A_.findEdit);
    SendMessageW(A_.findEdit, EM_SETSEL, 0, -1);
  } else {
    if (t) { t->hits.clear(); t->curHit = -1; t->query.clear(); t->searchDone = true; }
    SearchCancel();
    SetWindowTextW(A_.findEdit, L"");
    SetFocus(A_.view);
    ViewInvalidate();
  }
}

// ---------------------------------------------------------------- fullscreen
static int g_prevFit = 1;
static double g_prevZoom = 1;
void MainSetFullscreen(bool on, bool pres) {
  Tab* t = CurTab();
  if (on) {
    if (!A_.fullscreen) {
      A_.stylePrev = GetWindowLongW(A_.main, GWL_STYLE);
      A_.wpPrev.length = sizeof(A_.wpPrev);
      GetWindowPlacement(A_.main, &A_.wpPrev);
      MONITORINFO mi = {sizeof(mi)};
      GetMonitorInfoW(MonitorFromWindow(A_.main, MONITOR_DEFAULTTONEAREST), &mi);
      A_.fullscreen = true;
      SetWindowLongW(A_.main, GWL_STYLE, A_.stylePrev & ~(WS_OVERLAPPEDWINDOW));
      SetWindowPos(A_.main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    if (pres && !A_.presentation && t) { g_prevFit = t->fit; g_prevZoom = t->zoom; }
    if (!pres && A_.presentation && t) { t->fit = g_prevFit; t->zoom = g_prevZoom; }
    A_.presentation = pres;
    if (pres && t) t->fit = 2;
  } else if (A_.fullscreen) {
    if (A_.presentation && t) { t->fit = g_prevFit; t->zoom = g_prevZoom; }
    A_.fullscreen = false;
    A_.presentation = false;
    SetWindowLongW(A_.main, GWL_STYLE, A_.stylePrev);
    SetWindowPlacement(A_.main, &A_.wpPrev);
    SetWindowPos(A_.main, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  for (auto& tb : A_.tabs) tb->layoutOk = false;
  MainLayout();
  ViewSync();
  ViewInvalidate();
  if (A_.view) SetFocus(A_.view);
}

// ---------------------------------------------------------------- menus & commands
static bool PickFiles(std::vector<ws>* out) {
  std::vector<wchar_t> buf(65536, 0);
  OPENFILENAMEW of = {sizeof(of)};
  of.hwndOwner = A_.main;
  of.lpstrFilter = L"Документы PDF (*.pdf)\0*.pdf\0Все файлы\0*.*\0";
  of.lpstrFile = buf.data();
  of.nMaxFile = (DWORD)buf.size();
  of.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  of.lpstrTitle = L"Открыть PDF";
  if (!GetOpenFileNameW(&of)) return false;
  ws first = buf.data();
  const wchar_t* p = buf.data() + first.size() + 1;
  if (*p == 0) { out->push_back(first); return true; }
  while (*p) { out->push_back(first + L"\\" + p); p += wcslen(p) + 1; }
  return true;
}

// menu helpers: icons next to items, and "click the button again to close the menu"
static std::vector<HBITMAP> g_menuBmps;
static RECT g_swRect = {0, 0, 0, 0};          // button (screen coordinates) whose menu was closed by a click on that very button
static uint64_t g_swTick = 0;
static void MenuIcon(HMENU m, UINT idOrPos, bool byPos, Icon ic) {
  UINT st = GetMenuState(m, idOrPos, byPos ? MF_BYPOSITION : MF_BYCOMMAND);
  bool gray = st != (UINT)-1 && (st & (MF_GRAYED | MF_DISABLED));
  COLORREF col = gray ? (T.dark ? RGB(110, 110, 110) : RGB(165, 165, 165)) : (T.dark ? RGB(232, 232, 232) : RGB(52, 52, 56));
  HBITMAP hb = MakeMenuIcon(ic, S(16), col);
  if (!hb) return;
  g_menuBmps.push_back(hb);
  MENUITEMINFOW mi = {sizeof(mi)};
  mi.fMask = MIIM_BITMAP;
  mi.hbmpItem = hb;
  SetMenuItemInfoW(m, idOrPos, byPos ? TRUE : FALSE, &mi);
}
static int TrackMenuToggle(HMENU m, int x, int y, RECT btnClient) {
  POINT a = {btnClient.left, btnClient.top}, b = {btnClient.right, btnClient.bottom};
  ClientToScreen(A_.main, &a); ClientToScreen(A_.main, &b);
  RECT bs = {a.x, a.y, b.x, b.y};
  TPMPARAMS tp = {sizeof(tp), bs};
  int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, A_.main, &tp);
  POINT cp;
  GetCursorPos(&cp);
  if (PtInRect(&bs, cp)) { g_swRect = bs; g_swTick = GetTickCount64(); } else g_swTick = 0;
  return cmd;
}
static void FreeMenuIcons() {
  for (HBITMAP h : g_menuBmps) DeleteObject(h);
  g_menuBmps.clear();
}

static void ShowMainMenu(int x, int y, RECT btn) {
  Tab* t = CurTab();
  bool doc = t && t->doc;
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_OPEN, L"Открыть…\tCtrl+O");
  HMENU hist = CreatePopupMenu();
  int n = 0;
  for (auto& d : G.docs) {
    if (n >= 10) break;
    ws label = BaseName(d.path);
    for (size_t i = 0; i < label.size(); i++) if (label[i] == L'&') { label.insert(i, L"&"); i++; }
    AppendMenuW(hist, MF_STRING, ID_HISTORY + n, label.c_str());
    n++;
  }
  if (n == 0) AppendMenuW(hist, MF_STRING | MF_GRAYED, 0, L"(пусто)");
  else { AppendMenuW(hist, MF_SEPARATOR, 0, nullptr); AppendMenuW(hist, MF_STRING, ID_CLEAR_HISTORY, L"Очистить историю"); }
  AppendMenuW(m, MF_POPUP, (UINT_PTR)hist, L"История");
#ifdef APP_EDITOR
  AppendMenuW(m, MF_STRING | (doc && EdIsEdited(t) ? 0 : MF_GRAYED), ID_ED_SAVE, L"Сохранить\tCtrl+S");
#endif
  AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_SAVEAS, L"Сохранить как…\tCtrl+Shift+S");
  AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_SHOWFOLDER, L"Показать в папке");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
#ifndef APP_EDITOR
  AppendMenuW(m, MF_STRING | (G.pro ? MF_CHECKED : 0), ID_PRO, L"Pro-режим");
#endif
  HMENU ap = nullptr;
  if (G.pro) {
    AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_SIDE_BOOKS, L"Закладки");
    AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_SIDE_OUTLINE, L"Оглавление");
    AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_ROT_CW, L"Повернуть по часовой\tCtrl+R");
    AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_ROT_CCW, L"Повернуть против часовой\tCtrl+Shift+R");
    AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_PRINT, L"Печать…\tCtrl+P");
    AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_FULLSCREEN, L"Во весь экран\tF11");
    AppendMenuW(m, MF_STRING | (doc ? 0 : MF_GRAYED), ID_PRESENT, L"Презентация\tF5");
    ap = CreatePopupMenu();
    AppendMenuW(ap, MF_STRING | (G.readMode == 0 ? MF_CHECKED : 0), ID_MODE_DAY, L"Страницы: день");
    AppendMenuW(ap, MF_STRING | (G.readMode == 1 ? MF_CHECKED : 0), ID_MODE_SEPIA, L"Страницы: сепия");
    AppendMenuW(ap, MF_STRING | (G.readMode == 2 ? MF_CHECKED : 0), ID_MODE_NIGHT, L"Страницы: ночь");
    AppendMenuW(ap, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(ap, MF_STRING | (G.themeMode == 0 ? MF_CHECKED : 0), ID_THEME_SYS, L"Окно: как в системе");
    AppendMenuW(ap, MF_STRING | (G.themeMode == 1 ? MF_CHECKED : 0), ID_THEME_LIGHT, L"Окно: светлое");
    AppendMenuW(ap, MF_STRING | (G.themeMode == 2 ? MF_CHECKED : 0), ID_THEME_DARK, L"Окно: тёмное");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ap, L"Внешний вид");
  } else {
    AppendMenuW(m, MF_STRING | (G.readMode == 2 ? MF_CHECKED : 0), ID_NIGHT_TOGGLE, L"Ночной режим\tCtrl+M");
  }
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_ASSOC, L"Сделать программой для PDF…");
  AppendMenuW(m, MF_STRING, ID_UPDATE, L"Проверить обновления");
  HMENU setm = CreatePopupMenu(), rm = CreatePopupMenu();
  AppendMenuW(rm, MF_STRING | (G.renderMode == 0 ? MF_CHECKED : 0), ID_RM_AUTO, L"Авто (по видеокарте)");
  AppendMenuW(rm, MF_STRING | (G.renderMode == 1 ? MF_CHECKED : 0), ID_RM_GDI, L"Классический (без DirectX)");
  AppendMenuW(rm, MF_STRING | (G.renderMode == 2 ? MF_CHECKED : 0), ID_RM_NV, L"Плавный, для NVIDIA / Intel");
  AppendMenuW(rm, MF_STRING | (G.renderMode == 3 ? MF_CHECKED : 0), ID_RM_AMD, L"Плавный, для AMD");
  AppendMenuW(setm, MF_POPUP, (UINT_PTR)rm, L"Рендеринг");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)setm, L"Настройки");
  AppendMenuW(m, MF_STRING, ID_ABOUT, L"О программе");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");
  MenuIcon(m, ID_OPEN, false, IC_OPEN);
  MenuIcon(m, 1, true, IC_HIST);
  MenuIcon(hist, ID_CLEAR_HISTORY, false, IC_TRASH);
#ifdef APP_EDITOR
  MenuIcon(m, ID_ED_SAVE, false, IC_SAVE);
#endif
  MenuIcon(m, ID_SAVEAS, false, IC_SAVEAS);
  MenuIcon(m, ID_SHOWFOLDER, false, IC_SHOW);
#ifndef APP_EDITOR
  MenuIcon(m, ID_PRO, false, IC_STAR);
#endif
  if (G.pro) {
    MenuIcon(m, ID_SIDE_BOOKS, false, IC_BOOK);
    MenuIcon(m, ID_SIDE_OUTLINE, false, IC_LIST);
    MenuIcon(m, ID_ROT_CW, false, IC_ROT);
    MenuIcon(m, ID_ROT_CCW, false, IC_ROTL);
    MenuIcon(m, ID_PRINT, false, IC_PRINT);
    MenuIcon(m, ID_FULLSCREEN, false, IC_FULL);
    MenuIcon(m, ID_PRESENT, false, IC_PRES);
    MenuIcon(ap, ID_MODE_DAY, false, IC_SUN);
    MenuIcon(ap, ID_MODE_SEPIA, false, IC_PAGE);
    MenuIcon(ap, ID_MODE_NIGHT, false, IC_NIGHT);
    MenuIcon(ap, ID_THEME_SYS, false, IC_THEME);
    MenuIcon(ap, ID_THEME_LIGHT, false, IC_SUN);
    MenuIcon(ap, ID_THEME_DARK, false, IC_NIGHT);
    for (int i = 0; i < GetMenuItemCount(m); i++) if (GetSubMenu(m, i) == ap) MenuIcon(m, (UINT)i, true, IC_THEME);
  } else {
    MenuIcon(m, ID_NIGHT_TOGGLE, false, IC_NIGHT);
  }
  MenuIcon(m, ID_ASSOC, false, IC_APPS);
  MenuIcon(m, ID_UPDATE, false, IC_UPD);
  for (int i = 0; i < GetMenuItemCount(m); i++) if (GetSubMenu(m, i) == setm) MenuIcon(m, (UINT)i, true, IC_GEAR);
  MenuIcon(m, ID_ABOUT, false, IC_INFO);
  MenuIcon(m, ID_EXIT, false, IC_EXIT);
  int cmd = TrackMenuToggle(m, x, y, btn);
  DestroyMenu(m);
  FreeMenuIcons();
  if (cmd) DoCmd(cmd);
}

static void ShowZoomMenu(int x, int y, RECT btn) {
  HMENU m = CreatePopupMenu();
  static const int pct[] = {50, 75, 100, 125, 150, 200, 300, 400, 600, 800};
  for (int i = 0; i < 10; i++) AppendMenuW(m, MF_STRING, ID_ZOOM_PRESET + i, FmtT(L"%d%%", pct[i]).c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_FIT_W, L"По ширине");
  AppendMenuW(m, MF_STRING, ID_FIT_P, L"Страница целиком");
  int cmd = TrackMenuToggle(m, x, y, btn);
  DestroyMenu(m);
  if (cmd) DoCmd(cmd);
}

void DoCmd(int id) {
  Tab* t = CurTab();
  bool doc = t && t->doc;
  if (id >= ID_HISTORY && id < ID_HISTORY + 10) {
    int i = id - ID_HISTORY;
    if (i < (int)G.docs.size()) OpenPath(G.docs[i].path);
    return;
  }
  if (id >= ID_ZOOM_PRESET && id < ID_ZOOM_PRESET + 10) {
    static const int pct[] = {50, 75, 100, 125, 150, 200, 300, 400, 600, 800};
    ViewZoom(pct[id - ID_ZOOM_PRESET] / 100.0, -1, -1);
    return;
  }
#ifdef APP_EDITOR
  if (IsEdCmd(id)) { EdCommand(id); return; }
#endif
  switch (id) {
    case ID_OPEN + 1000: {
      RECT r = g_btns.empty() ? RECT{0, 0, 0, 0} : g_btns[0].r;
      POINT pt = {r.left, r.bottom};
      ClientToScreen(A_.main, &pt);
      ShowMainMenu(pt.x, pt.y, r);
      break;
    }
    case ID_OPEN: case ID_NEWTAB_DLG: {
      std::vector<ws> files;
      if (PickFiles(&files)) OpenFiles(files, false);
      break;
    }
    case ID_CLOSE_TAB: CloseTab(A_.cur); break;
    case ID_NEXT_TAB: if (A_.tabs.size() > 1) SwitchTab((A_.cur + 1) % (int)A_.tabs.size()); break;
    case ID_PREV_TAB: if (A_.tabs.size() > 1) SwitchTab((A_.cur + (int)A_.tabs.size() - 1) % (int)A_.tabs.size()); break;
    case ID_PRO:
      G.pro = !G.pro;
      if (!G.pro) { if (G.sideTab != 0) G.sideTab = 0; ViewClearSel(); }
      MainLayout(); ViewInvalidate(); SideSync();
      break;
    case ID_SIDEBAR: G.sidebar = !G.sidebar; MainRelayoutAll(); break;
    case ID_SIDE_PAGES: case ID_SIDE_OUTLINE: case ID_SIDE_BOOKS:
      G.sideTab = id == ID_SIDE_PAGES ? 0 : (id == ID_SIDE_OUTLINE ? 1 : 2);
      if (doc) { if (!G.sidebar) { G.sidebar = true; MainRelayoutAll(); } SideSync(); InvalidateRect(A_.main, nullptr, FALSE); }
      break;
    case ID_FIND: MainShowFind(true); break;
    case ID_FIND + 5000: MainShowFind(false); break;
    case ID_FIND_NEXT: if (doc) ViewFindStep(1); break;
    case ID_FIND_PREV: if (doc) ViewFindStep(-1); break;
    case ID_GOTO: if (doc) { SetFocus(A_.pageEdit); SendMessageW(A_.pageEdit, EM_SETSEL, 0, -1); } break;
    case ID_ZOOM_IN: ViewZoomStep(1); break;
    case ID_ZOOM_OUT: ViewZoomStep(-1); break;
    case ID_ZOOM_100: ViewZoom(1.0, -1, -1); break;
    case ID_FIT_W: ViewSetFit(1); break;
    case ID_FIT_P: ViewSetFit(2); break;
    case ID_ROT_CW: if (G.pro) ViewRotate(1); break;
    case ID_ROT_CCW: if (G.pro) ViewRotate(-1); break;
    case ID_MODE_DAY: case ID_MODE_SEPIA: case ID_MODE_NIGHT:
      G.readMode = id - ID_MODE_DAY;
      ViewInvalidate(); SideInvalidate(); MainRedrawChrome();
      break;
    case ID_NIGHT_TOGGLE:
      G.readMode = G.readMode == 2 ? 0 : 2;
      ViewInvalidate(); SideInvalidate(); MainRedrawChrome();
      break;
    case ID_THEME_SYS: case ID_THEME_LIGHT: case ID_THEME_DARK:
      G.themeMode = id == ID_THEME_SYS ? 0 : (id == ID_THEME_LIGHT ? 1 : 2);
      ReTheme();
      break;
    case ID_RM_AUTO: case ID_RM_GDI: case ID_RM_NV: case ID_RM_AMD:
      G.renderMode = id == ID_RM_AUTO ? 0 : id == ID_RM_GDI ? 1 : id == ID_RM_NV ? 2 : 3;
      ViewRecreate();
      ThemeChildren();
      MainLayout();
      ViewInvalidate();
      break;
    case ID_BOOKMARK: if (doc && G.pro) { ToggleBookmark(t, ViewCurPage()); ShowToast(IsBookmarked(t, ViewCurPage()) ? L"Закладка добавлена" : L"Закладка убрана"); } break;
    case ID_PRINT: if (doc && G.pro) PrintDoc(A_.main); break;
    case ID_FULLSCREEN: if (doc && G.pro) MainSetFullscreen(!(A_.fullscreen && !A_.presentation), false); break;
    case ID_PRESENT: if (doc && G.pro) MainSetFullscreen(!(A_.fullscreen && A_.presentation), true); break;
    case ID_SAVEAS:
#ifdef APP_EDITOR
      if (doc) { EdSaveAs(t); break; }
#endif
      if (doc) {
        wchar_t buf[MAX_PATH * 2];
        wcsncpy(buf, BaseName(t->path).c_str(), MAX_PATH);
        OPENFILENAMEW of = {sizeof(of)};
        of.hwndOwner = A_.main;
        of.lpstrFilter = L"Документы PDF (*.pdf)\0*.pdf\0";
        of.lpstrFile = buf;
        of.nMaxFile = MAX_PATH * 2;
        of.lpstrDefExt = L"pdf";
        of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&of)) {
          if (!CopyFileW(t->path.c_str(), buf, FALSE)) MessageBoxW(A_.main, L"Не удалось сохранить файл", APP_TITLE, MB_OK | MB_ICONWARNING);
          else ShowToast(L"Сохранено");
        }
      }
      break;
    case ID_SHOWFOLDER:
      if (doc) { ws a = L"/select,\"" + t->path + L"\""; ShellExecuteW(A_.main, L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL); }
      break;
    case ID_ABOUT: ShowAbout(A_.main); break;
    case ID_UPDATE: CheckUpdates(true); break;
    case ID_ASSOC: RegisterAssoc(); break;
    case ID_COPY: ViewCopy(); break;
    case ID_SELALL: ViewSelectAll(); break;
    case ID_BACK:
      if (t && !t->back.empty()) { auto b = t->back.back(); t->back.pop_back(); ViewGoPage(b.first, b.second, true, false); }
      break;
    case ID_PAGE_NEXT: ViewPageStep(1); break;
    case ID_PAGE_PREV: ViewPageStep(-1); break;
    case ID_PAGE_FIRST: ViewGoPage(0, 0, true, false); break;
    case ID_PAGE_LAST: if (doc) ViewGoPage(t->doc->npages - 1, 0, true, false); break;
    case ID_CLEAR_HISTORY: G.docs.clear(); SaveSettings(); break;
    case ID_EXIT: SendMessageW(A_.main, WM_CLOSE, 0, 0); break;
  }
}

// ---------------------------------------------------------------- keyboard
static bool HandleKey(MSG& m) {
  if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN) return false;
  if (GetActiveWindow() != A_.main) return false;
  bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
  HWND f = GetFocus();
  bool inEdit = f == A_.pageEdit || f == A_.findEdit;
  UINT vk = (UINT)m.wParam;
  Tab* t = CurTab();
  bool doc = t && t->doc;
  if (alt && !ctrl) {
    if (vk == VK_LEFT) { DoCmd(ID_BACK); return true; }
    return false;
  }
#ifdef APP_EDITOR
  if (EdKey(vk, ctrl, shift)) return true;
#endif
  if (ctrl) {
    switch (vk) {
      case 'O': DoCmd(ID_OPEN); return true;
      case 'W': case VK_F4: DoCmd(ID_CLOSE_TAB); return true;
      case VK_TAB: DoCmd(shift ? ID_PREV_TAB : ID_NEXT_TAB); return true;
      case VK_NEXT: DoCmd(ID_NEXT_TAB); return true;
      case VK_PRIOR: DoCmd(ID_PREV_TAB); return true;
      case 'F': DoCmd(ID_FIND); return true;
      case 'G': DoCmd(ID_GOTO); return true;
      case 'P': DoCmd(ID_PRINT); return true;
      case 'D': DoCmd(ID_BOOKMARK); return true;
      case 'M': G.readMode = (G.readMode + 1) % 3; ViewInvalidate(); SideInvalidate(); MainRedrawChrome(); return true;
      case 'R': DoCmd(shift ? ID_ROT_CCW : ID_ROT_CW); return true;
      case VK_OEM_PLUS: case VK_ADD: DoCmd(ID_ZOOM_IN); return true;
      case VK_OEM_MINUS: case VK_SUBTRACT: DoCmd(ID_ZOOM_OUT); return true;
      case '0': case VK_NUMPAD0: DoCmd(ID_ZOOM_100); return true;
      case '9': case VK_NUMPAD9: DoCmd(ID_FIT_W); return true;
      case '8': case VK_NUMPAD8: DoCmd(ID_FIT_P); return true;
      case 'B': DoCmd(ID_SIDEBAR); return true;
      case 'C': if (!inEdit && G.pro) { ViewCopy(); return true; } return false;
      case 'A': if (!inEdit && G.pro) { ViewSelectAll(); return true; } return false;
      case VK_HOME: if (!inEdit && doc) { ViewGoPage(0, 0, true, false); return true; } return false;
      case VK_END: if (!inEdit && doc) { ViewGoPage(t->doc->npages - 1, 0, true, false); return true; } return false;
    }
    return false;
  }
  switch (vk) {
    case VK_F3: DoCmd(shift ? ID_FIND_PREV : ID_FIND_NEXT); return true;
    case VK_F4: DoCmd(ID_SIDEBAR); return true;
    case VK_F5: DoCmd(ID_PRESENT); return true;
    case VK_F11: DoCmd(ID_FULLSCREEN); return true;
    case VK_ESCAPE:
      if (inEdit) return false;
      if (A_.fullscreen) { MainSetFullscreen(false, false); return true; }
      if (A_.findOpen) { MainShowFind(false); return true; }
      if (ViewHasSel()) { ViewClearSel(); return true; }
      return false;
  }
  if (inEdit || !doc) return false;
  if (f != A_.view && f != A_.main && f != A_.side && f != nullptr) return false;
  int step = S(70);
  bool pres = A_.presentation;
  switch (vk) {
    case VK_DOWN: if (pres) ViewPageStep(1); else ViewScrollBy(0, step, true); return true;
    case VK_UP: if (pres) ViewPageStep(-1); else ViewScrollBy(0, -step, true); return true;
    case VK_RIGHT: if (pres || t->docW <= t->viewW) ViewPageStep(1); else ViewScrollBy(step, 0, true); return true;
    case VK_LEFT: if (pres || t->docW <= t->viewW) ViewPageStep(-1); else ViewScrollBy(-step, 0, true); return true;
    case VK_NEXT: if (pres) ViewPageStep(1); else ViewScrollBy(0, (int)(t->viewH * 0.9), true); return true;
    case VK_PRIOR: if (pres) ViewPageStep(-1); else ViewScrollBy(0, -(int)(t->viewH * 0.9), true); return true;
    case VK_SPACE: case VK_RETURN:
      if (pres) ViewPageStep(shift ? -1 : 1);
      else ViewScrollBy(0, (shift ? -1 : 1) * (int)(t->viewH * 0.9), true);
      return true;
    case VK_HOME: ViewGoPage(0, 0, true, false); return true;
    case VK_END: ViewGoPage(t->doc->npages - 1, 0, true, false); return true;
  }
  return false;
}

// ---------------------------------------------------------------- child controls
static LRESULT CALLBACK EditSub(HWND hw, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
  if (msg == WM_KEYDOWN) {
    if (id == 1) {          // page box
      if (wp == VK_RETURN) {
        wchar_t b[32];
        GetWindowTextW(hw, b, 32);
        int p = _wtoi(b);
        Tab* t = CurTab();
        if (t && t->doc) ViewGoPage(min(max(p, 1), t->doc->npages) - 1, 0, true, false);
        SetFocus(A_.view);
        return 0;
      }
      if (wp == VK_ESCAPE) { g_lastPageText.clear(); MainUpdatePageBox(); SetFocus(A_.view); return 0; }
    } else if (id == 2) {   // find box
      if (wp == VK_RETURN) {
        bool pending = KillTimer(A_.main, TMR_FIND) != 0;
        if (pending) StartSearchFromEdit();
        else ViewFindStep(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        return 0;
      }
      if (wp == VK_ESCAPE) { MainShowFind(false); return 0; }
    }
  }
  if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
  if (msg == WM_KILLFOCUS && id == 1) { g_lastPageText.clear(); MainUpdatePageBox(); }
  return DefSubclassProc(hw, msg, wp, lp);
}

// ---------------------------------------------------------------- mouse on chrome
static int HitBtn(int x, int y) {
  POINT pt = {x, y};
  for (size_t i = 0; i < g_btns.size(); i++)
    if (PtInRect(&g_btns[i].r, pt)) return (int)i;
  for (size_t i = 0; i < g_fbtns.size(); i++)
    if (PtInRect(&g_fbtns[i].r, pt)) return 1000 + (int)i;
  if (g_findR.bottom > g_findR.top && PtInRect(&g_caseR, pt)) return -2;
  return -1;
}
static Btn& BtnAt(int i) { return i >= 1000 ? g_fbtns[i - 1000] : g_btns[i]; }
#ifdef APP_EDITOR
static int HitMode(int x, int y) {
  POINT pt = {x, y};
  for (int i = 0; i < 3; i++) if (PtInRect(&g_modeSeg[i], pt)) return i;
  return -1;
}
#endif
static int HitTab(int x, int y, bool* onClose) {
  POINT pt = {x, y};
  *onClose = false;
  if (y > TabH()) return -1;
  for (int i = 0; i < (int)A_.tabs.size() && i < 64; i++) {
    if (PtInRect(&g_tabRects[i].r, pt)) {
      *onClose = PtInRect(&g_tabRects[i].close, pt) && (i == A_.cur || i == g_hoverTab);
      return i;
    }
  }
  if (PtInRect(&g_plusR, pt)) return 100;
  return -1;
}

static LRESULT CALLBACK MainProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintMain(hw); return 0;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) MainLayout();
      return 0;
    case WM_GETMINMAXINFO: {
      MINMAXINFO* mm = (MINMAXINFO*)lp;
#ifdef APP_EDITOR
      mm->ptMinTrackSize.x = S(1000);
#else
      mm->ptMinTrackSize.x = S(620);
#endif
      mm->ptMinTrackSize.y = S(420);
      return 0;
    }
    case WM_DPICHANGED: {
      A_.dpi = HIWORD(wp);
      RECT* r = (RECT*)lp;
      SetWindowPos(hw, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      MainRelayoutAll();
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0 && G.themeMode == 0) ReTheme();
      return 0;
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)wp;
#ifdef APP_EDITOR
      { LRESULT er = EdCtlColor(dc, (HWND)lp); if (er) return er; }
#endif
      SetTextColor(dc, T.text);
      SetBkColor(dc, T.field);
      return (LRESULT)g_editBrush;
    }
    case WM_NOTIFY: {
      NMHDR* nh = (NMHDR*)lp;
      if (nh && nh->hwndFrom == g_tip && nh->code == TTN_SHOW) {
        // keep the tip clear of the mouse pointer: put it below the pointer image (and never above the button)
        POINT cp;
        GetCursorPos(&cp);
        RECT tr;
        GetWindowRect(g_tip, &tr);
        int tw = tr.right - tr.left, th = tr.bottom - tr.top;
        int x = cp.x - S(8);
        int y = cp.y + max(S(28), GetSystemMetrics(SM_CYCURSOR) - S(2));
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST), &mi);
        if (x + tw > mi.rcWork.right) x = mi.rcWork.right - tw;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + th > mi.rcWork.bottom) y = cp.y - th - S(8);
        SetWindowPos(g_tip, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        return TRUE;
      }
      break;
    }
    case WM_COMMAND:
      if ((HWND)lp == A_.findEdit && HIWORD(wp) == EN_CHANGE) {
        SetTimer(hw, TMR_FIND, 280, nullptr);
        return 0;
      }
      return 0;
    case WM_TIMER:
      if (wp == TMR_FIND) { KillTimer(hw, TMR_FIND); StartSearchFromEdit(); }
      else if (wp == TMR_TOAST) { KillTimer(hw, TMR_TOAST); g_toast.clear(); ViewInvalidate(); }
      else if (wp == TMR_SAVE) {
        bool any = false;
        for (auto& t : A_.tabs) if (t->dirty && t->doc) { SaveTabState(t.get()); any = true; }
        if (any) SaveSettings();
      }
      return 0;
    case WM_APP_OPENED: ApplyOpened((OpenResult*)lp); return 0;
    case WM_APP_REFRESH: WorkerAckRefresh(); ViewInvalidateTiles(); SideInvalidate(); return 0;
    case WM_APP_SEARCH: {
      Tab* t = TabById((int)wp);
      if (!t) return 0;
      SearchSnapshot(t->id, &t->hits, &t->searchDone, &t->searchScanned);
      if (t->jumpPending && !t->hits.empty() && t == CurTab()) { t->jumpPending = false; t->curHit = -1; ViewFindStep(1); }
      if (t->curHit >= (int)t->hits.size()) t->curHit = -1;
      ViewInvalidate();
      InvalidateRect(hw, &g_findR, FALSE);
      return 0;
    }
    case WM_APP_UPDATE: OnUpdateMsg(wp, lp); return 0;
    case WM_APP_TESTCMD: if (lp) { auto* f = (std::function<void()>*)lp; (*f)(); if (wp == 1) delete f; } return 0;
    case WM_DROPFILES: {
      HDROP d = (HDROP)wp;
      UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
      std::vector<ws> files;
      for (UINT i = 0; i < n; i++) {
        wchar_t b[MAX_PATH * 2];
        if (DragQueryFileW(d, i, b, MAX_PATH * 2)) files.push_back(b);
      }
      DragFinish(d);
      OpenFiles(files, true);
      return 0;
    }
    case WM_COPYDATA: {
      COPYDATASTRUCT* cd = (COPYDATASTRUCT*)lp;
      if (cd && cd->dwData == 0x5550 && cd->lpData) {
        ws all((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        std::vector<ws> files;
        size_t i = 0;
        while (i < all.size()) {
          size_t j = all.find(L'\n', i);
          if (j == ws::npos) j = all.size();
          if (j > i) files.push_back(all.substr(i, j - i));
          i = j + 1;
        }
        OpenFiles(files, true);
        if (files.empty()) { if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE); SetForegroundWindow(hw); }
        return TRUE;
      }
      return FALSE;
    }
    case WM_MOUSEMOVE: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (!g_chromeTrack) { TRACKMOUSEEVENT te = {sizeof(te), TME_LEAVE, hw, 0}; TrackMouseEvent(&te); g_chromeTrack = true; }
      if (g_splitDrag) {
        G.sideW = min(max(x * 96 / A_.dpi, 160), 560);
        MainLayout();
        return 0;
      }
      if (g_dragTab >= 0) {
        // reorder tabs while dragging
        bool oc;
        int over = HitTab(x, S(10), &oc);
        if (over >= 0 && over < (int)A_.tabs.size() && over != g_dragTab) {
          std::swap(A_.tabs[over], A_.tabs[g_dragTab]);
          if (A_.cur == g_dragTab) A_.cur = over; else if (A_.cur == over) A_.cur = g_dragTab;
          g_dragTab = over;
          InvalidateRect(hw, &g_tabsR, FALSE);
        }
        return 0;
      }
#ifdef APP_EDITOR
      EdBarMouse(WM_MOUSEMOVE, x, y);
      { int hm = HitMode(x, y); if (hm != g_hoverMode) { g_hoverMode = hm; InvalidateRect(hw, &g_tabsR, FALSE); } }
#endif
      int hb = HitBtn(x, y);
      bool oc;
      int ht = HitTab(x, y, &oc);
      int hs = -1;
      for (int i = 0; i < 3; i++) if (g_hdrR.bottom > g_hdrR.top) { POINT pt = {x, y}; if (PtInRect(&g_segR[i], pt)) hs = i; }
      if (hb != g_hoverBtn || ht != g_hoverTab || hs != g_hoverSeg) {
        g_hoverBtn = hb; g_hoverTab = ht; g_hoverSeg = hs;
        InvalidateRect(hw, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
#ifdef APP_EDITOR
      EdBarMouse(WM_MOUSELEAVE, 0, 0);
#endif
      g_chromeTrack = false;
      g_hoverBtn = g_hoverTab = g_hoverSeg = -1;
#ifdef APP_EDITOR
      g_hoverMode = -1;
#endif
      InvalidateRect(hw, nullptr, FALSE);
      return 0;
    case WM_SETCURSOR: {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(hw, &pt);
      if (g_splitDrag || (g_splitR.right > g_splitR.left && PtInRect(&g_splitR, pt))) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
      break;
    }
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      POINT pt = {x, y};
      SetFocus(A_.view);
      if (g_splitR.right > g_splitR.left && PtInRect(&g_splitR, pt)) { g_splitDrag = true; SetCapture(hw); return 0; }
#ifdef APP_EDITOR
      { int hm = HitMode(x, y); if (hm >= 0) { EdSetMode(hm); return 0; } }
#endif
      bool oc;
      int ht = HitTab(x, y, &oc);
      if (ht == 100) { DoCmd(ID_OPEN); return 0; }
      if (ht >= 0) {
        if (oc) { CloseTab(ht); return 0; }
        if (ht != A_.cur) SwitchTab(ht);
        g_dragTab = ht; g_dragStartX = x;
        SetCapture(hw);
        return 0;
      }
#ifdef APP_EDITOR
      if (EdBarMouse(WM_LBUTTONDOWN, x, y)) return 0;
#endif
      for (int i = 0; i < 3; i++)
        if (g_hdrR.bottom > g_hdrR.top && PtInRect(&g_segR[i], pt)) { G.sideTab = i; SideSync(); InvalidateRect(hw, nullptr, FALSE); return 0; }
      int hb = HitBtn(x, y);
      if (hb == -2) { G.matchCase = !G.matchCase; StartSearchFromEdit(); InvalidateRect(hw, nullptr, FALSE); return 0; }
      if (hb >= 0 && g_swTick && GetTickCount64() - g_swTick < 450) {
        POINT sp = {x, y};
        ClientToScreen(hw, &sp);
        if (PtInRect(&g_swRect, sp)) { g_swTick = 0; return 0; }     // this click only closed the menu
      }
      g_swTick = 0;
      if (hb >= 0 && BtnAt(hb).enabled) { g_pressBtn = hb; SetCapture(hw); InvalidateRect(hw, nullptr, FALSE); }
      return 0;
    }
    case WM_LBUTTONUP: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
#ifdef APP_EDITOR
      if (EdBarMouse(WM_LBUTTONUP, x, y)) return 0;
#endif
      if (GetCapture() == hw) ReleaseCapture();
      if (g_splitDrag) { g_splitDrag = false; return 0; }
      if (g_dragTab >= 0) { g_dragTab = -1; return 0; }
      int pb = g_pressBtn;
      g_pressBtn = -1;
      if (pb >= 0 && HitBtn(x, y) == pb) {
        Btn b = BtnAt(pb);
        if (b.kind == 1) {
          POINT pt = {b.r.left, b.r.bottom};
          ClientToScreen(hw, &pt);
          ShowZoomMenu(pt.x, pt.y, b.r);
        } else DoCmd(b.id);
      }
      InvalidateRect(hw, nullptr, FALSE);
      return 0;
    }
    case WM_MBUTTONUP: {
      bool oc;
      int ht = HitTab(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &oc);
      if (ht >= 0 && ht < (int)A_.tabs.size()) CloseTab(ht);
      return 0;
    }
    case WM_CLOSE:
#ifdef APP_EDITOR
      EdCommitText();
      if (!EdConfirmAll()) return 0;
#endif
      SaveAllState();
      DestroyWindow(hw);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_SETFOCUS: if (A_.view) SetFocus(A_.view); return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

bool AppIsReady() {
  for (auto& tb : A_.tabs) if (tb->loading) return false;
  return ViewSettled();
}

// ---------------------------------------------------------------- window creation
bool MainCreate(HINSTANCE inst, int show, const ws& cmdline) {
  A_.inst = inst;
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = MainProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  wc.lpszClassName = kMainClass;
  RegisterClassExW(&wc);
  ViewRegister(inst);
  SideRegister(inst);
  ApplyTheme(G.themeMode);
  int x = G.winX, y = G.winY;
  if (x != CW_USEDEFAULT) {
    // keep the window on a visible monitor
    RECT r = {x, y, x + G.winW, y + G.winH};
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) { x = y = CW_USEDEFAULT; }
  }
  A_.main = CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, G.winW, G.winH, nullptr,
                            nullptr, inst, nullptr);
  if (!A_.main) return false;
  typedef UINT(WINAPI * GDFW)(HWND);
  GDFW gd = (GDFW)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
  A_.dpi = gd ? gd(A_.main) : 96;
  if (A_.dpi < 48) A_.dpi = 96;
  { wchar_t e[16]; if (GetEnvironmentVariableW(L"ULTRAPDF_DPI", e, 16) > 0) A_.dpi = _wtoi(e); }
  A_.view = ViewCreate(A_.main);
  A_.side = SideCreate(A_.main);
  A_.pageEdit = CreateWindowExW(0, L"EDIT", L"1", WS_CHILD | ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, A_.main, nullptr, inst, nullptr);
  A_.findEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, A_.main, nullptr, inst, nullptr);
  SendMessageW(A_.pageEdit, WM_SETFONT, (WPARAM)UiFont(S(14)), TRUE);
  SendMessageW(A_.findEdit, WM_SETFONT, (WPARAM)UiFont(S(14)), TRUE);
  SendMessageW(A_.findEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Найти в документе");
  SetWindowSubclass(A_.pageEdit, EditSub, 1, 0);
  SetWindowSubclass(A_.findEdit, EditSub, 2, 0);
  ApplyTitleBar();
  ApplyMenuTheme();
  ThemeChildren();
  WorkerStart(A_.main);
  SetTimer(A_.main, TMR_SAVE, 5000, nullptr);
  if (G.maximized) ShowWindow(A_.main, SW_SHOWMAXIMIZED); else ShowWindow(A_.main, show);
  UpdateWindow(A_.main);
  SplashRaise();
  MainLayout();
  // what to open
  std::vector<ws> files;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; i++) {
    if (argv[i][0] == L'-' || argv[i][0] == L'/') continue;
    files.push_back(argv[i]);
  }
  if (argv) LocalFree(argv);
  if (!files.empty()) {
    OpenFiles(files, false);
  } else {
    int act = G.sessionActive;
    std::vector<ws> sess = G.session;
    for (auto& p : sess) if (FileExists(p)) OpenPath(p);
    if (act >= 0 && act < (int)A_.tabs.size() && A_.tabs.size() > 1) {
      // keep the previously active tab
      std::vector<ws> pp;
      for (auto& tb : A_.tabs) pp.push_back(tb->path);
      SwitchTab(act < (int)A_.tabs.size() ? act : 0);
    }
  }
  SetFocus(A_.view);
  return true;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
  // single instance: hand the files over to a running copy
  g_mutex = CreateMutexW(nullptr, TRUE, APP_ID L".Single.Instance");
  bool already = GetLastError() == ERROR_ALREADY_EXISTS;
  wchar_t testEnv[1024];
  bool testing = GetEnvironmentVariableW(L"ULTRAPDF_TEST", testEnv, 1024) > 0;
  if (already && !testing) {
    HWND other = nullptr;
    for (int i = 0; i < 40 && !other; i++) { other = FindWindowW(kMainClass, nullptr); if (!other) Sleep(50); }
    if (other) {
      ws all;
      int argc = 0;
      LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
      for (int i = 1; argv && i < argc; i++) {
        if (argv[i][0] == L'-' || argv[i][0] == L'/') continue;
        wchar_t full[MAX_PATH * 2];
        DWORD n = GetFullPathNameW(argv[i], MAX_PATH * 2, full, nullptr);
        all += ws(full, n) + L"\n";
      }
      if (argv) LocalFree(argv);
      COPYDATASTRUCT cd;
      cd.dwData = 0x5550;
      cd.cbData = (DWORD)(all.size() * sizeof(wchar_t));
      cd.lpData = (PVOID)all.c_str();
      SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, nullptr);
      return 0;
    }
  }
  INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&ic);
  // clean up after a self-update
  ws old = ExePath() + L".old";
  DeleteFileW(old.c_str());
  LoadSettings();
#ifdef APP_EDITOR
  G.pro = G.edMode >= 1;
#endif
  GfxInit();
  A_.dpi = 96;
  if (!testing || GetEnvironmentVariableW(L"ULTRAPDF_SPLASH", nullptr, 0) > 0) { ApplyTheme(G.themeMode); SplashShow(inst); }
  ws err;
  if (!PdfLoad(&err)) {
    SplashClose();
    MessageBoxW(nullptr, err.c_str(), APP_TITLE, MB_OK | MB_ICONERROR);
    return 1;
  }
  if (!MainCreate(inst, show, L"")) return 1;
  if (testing) TestRun(testEnv);
  // background update check, at most once a day
  uint64_t now = (uint64_t)time(nullptr);
  if (!testing && now - G.lastUpdateCheck > 20 * 3600) CheckUpdates(false);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    if (HandleKey(m)) continue;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  WorkerStop();
  GfxShutdown();
  ExitProcess(0);
  return 0;
}
