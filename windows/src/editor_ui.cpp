// UltraPDF Editor: tool bar, tools, selection with handles, text box, menus.
#include "editor.h"
#ifdef APP_EDITOR
#include <commctrl.h>
#include <uxtheme.h>
#include <sstream>

// ------------------------------------------------------------------ state
enum Tool { ET_SELECT, ET_HILITE, ET_UNDER, ET_STRIKE, ET_SQUIG, ET_TEXT, ET_INK, ET_RECT, ET_ELLIPSE };
enum Gest { G_NONE, G_MOVE, G_RESIZE, G_SHAPE, G_INK };

static int g_tool = ET_SELECT;
static COLORREF g_colHi = RGB(255, 235, 59), g_colLine = RGB(229, 57, 53), g_colText = RGB(0, 0, 0);
static int g_lineW = 2, g_fontSize = 14;
static ws g_font = L"Arial";

static ESel g_sel;                  // selected editor object
static FRect g_selBox;
static int g_selKind = 0;           // EK_* for page-object selections
static HBITMAP g_preview = nullptr; static int g_prevW = 0, g_prevH = 0;

static Gest g_g = G_NONE;
static int g_gPage = 0, g_gHandle = -1;
static FRect g_gBox0, g_gBox;
static float g_gx0 = 0, g_gy0 = 0;
static POINT g_gDown = {0, 0};
static bool g_gMoved = false;
static std::vector<PointF> g_ink;
static ESel g_ctxSel;

struct PalEnt { const wchar_t* name; COLORREF c; };
static const PalEnt kPal[] = {
    {L"Чёрный", RGB(0, 0, 0)}, {L"Красный", RGB(229, 57, 53)}, {L"Синий", RGB(30, 90, 200)}, {L"Зелёный", RGB(46, 160, 67)},
    {L"Оранжевый", RGB(251, 140, 0)}, {L"Фиолетовый", RGB(142, 68, 173)}, {L"Жёлтый", RGB(255, 235, 59)}, {L"Бирюзовый", RGB(0, 172, 193)},
    {L"Розовый", RGB(255, 128, 171)}, {L"Серый", RGB(120, 120, 120)}, {L"Тёмно-синий", RGB(20, 40, 120)}, {L"Белый", RGB(255, 255, 255)}};
static const int kLineW[] = {1, 2, 3, 5, 8};
static const int kFontSz[] = {8, 10, 12, 14, 16, 18, 24, 36, 48};

static bool IsMarkupTool(int t) { return t >= ET_HILITE && t <= ET_SQUIG; }

// the colour the colour button edits right now
static COLORREF* ActiveColor() {
  if (g_sel.kind == 1 && g_selKind == EK_TEXT) return &g_colText;
  if (g_sel.kind == 2 && g_sel.sub == FPDF_ANNOT_HIGHLIGHT) return &g_colHi;
  if (g_sel.kind == 0) {
    if (g_tool == ET_HILITE) return &g_colHi;
    if (g_tool == ET_TEXT) return &g_colText;
  }
  return &g_colLine;
}
static bool SizeIsFont() {
  if (g_sel.kind == 1 && g_selKind == EK_TEXT) return true;
  return g_sel.kind == 0 && g_tool == ET_TEXT;
}

static void Redraw() { ViewInvalidate(); InvalidateRect(A_.main, nullptr, FALSE); }

static void FreePreview() { if (g_preview) { DeleteObject(g_preview); g_preview = nullptr; } }
static void ClearSel() { g_sel = ESel(); g_selKind = 0; FreePreview(); }
static void SetSel(const ESel& s) {
  Tab* t = CurTab();
  g_sel = s;
  g_selKind = 0;
  FreePreview();
  if (!t || !EdSelBox(t, s, &g_selBox)) { g_sel = ESel(); return; }
  if (s.kind == 1) g_selKind = EdSelObjKind(t, s);
}
static void RefreshSel() {
  if (g_sel.kind == 0) return;
  Tab* t = CurTab();
  if (!t || !EdSelBox(t, g_sel, &g_selBox)) ClearSel();
}

// ------------------------------------------------------------------ text box
static HWND g_te = nullptr;
static HFONT g_teFont = nullptr;
static int g_tbTab = 0, g_tbPage = 0;
static float g_tbX = 0, g_tbY = 0;

static void TextBoxResize() {
  if (!g_te) return;
  int len = GetWindowTextLengthW(g_te);
  std::vector<wchar_t> buf(len + 2, 0);
  GetWindowTextW(g_te, buf.data(), len + 1);
  HDC dc = GetDC(g_te);
  HGDIOBJ old = SelectObject(dc, g_teFont);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  int lines = 1, maxW = 0;
  size_t i = 0, n = wcslen(buf.data());
  while (i <= n) {
    size_t j = i;
    while (j < n && buf[j] != L'\r' && buf[j] != L'\n') j++;
    SIZE sz = {0, 0};
    if (j > i) GetTextExtentPoint32W(dc, buf.data() + i, (int)(j - i), &sz);
    maxW = max(maxW, (int)sz.cx);
    if (j >= n) break;
    if (buf[j] == L'\r' && j + 1 < n && buf[j + 1] == L'\n') j++;
    lines++;
    i = j + 1;
  }
  SelectObject(dc, old);
  ReleaseDC(g_te, dc);
  RECT vr;
  GetClientRect(A_.view, &vr);
  POINT vrb = {vr.right, 0};
  ClientToScreen(A_.view, &vrb);
  RECT cur;
  GetWindowRect(g_te, &cur);
  int w = max(S(100), maxW + S(26)), h = lines * tm.tmHeight + S(10);
  w = min(w, max(S(100), (int)vrb.x - (int)cur.left - 4));
  SetWindowPos(g_te, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK TextSub(HWND hw, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR);
static void TextBoxClose(bool commit) {
  if (!g_te) return;
  HWND te = g_te;
  g_te = nullptr;                                     // first: destroying the window sends WM_KILLFOCUS
  ws text;
  int len = GetWindowTextLengthW(te);
  if (len > 0) {
    std::vector<wchar_t> buf(len + 2, 0);
    GetWindowTextW(te, buf.data(), len + 1);
    text = buf.data();
  }
  RemoveWindowSubclass(te, TextSub, 77);
  DestroyWindow(te);
  if (g_teFont) { DeleteObject(g_teFont); g_teFont = nullptr; }
  Tab* t = TabById(g_tbTab);
  if (commit && t && t->doc) {
    bool any = false;
    for (wchar_t c : text) if (c != L' ' && c != L'\r' && c != L'\n' && c != L'\t') any = true;
    if (any) {
      ESel s;
      if (EdAddText(t, g_tbPage, g_tbX, g_tbY, text, g_font, g_fontSize, g_colText, &s)) { g_tool = ET_SELECT; SetSel(s); }
    }
  }
  if (A_.view) SetFocus(A_.view);
  Redraw();
}
void EdCommitText() { if (g_te) TextBoxClose(true); }

static LRESULT CALLBACK TextSub(HWND hw, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) { TextBoxClose(false); return 0; }
      if (wp == VK_RETURN && GetKeyState(VK_CONTROL) < 0) { TextBoxClose(true); return 0; }
      break;
    case WM_KILLFOCUS: {
      auto* f = new std::function<void()>([hw]() { if (g_te == hw && GetFocus() != hw) TextBoxClose(true); });
      PostMessageW(A_.main, WM_APP_TESTCMD, 1, (LPARAM)f);
      break;
    }
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
  }
  LRESULT r = DefSubclassProc(hw, msg, wp, lp);
  if (msg == WM_CHAR || msg == WM_PASTE || msg == WM_CUT || msg == WM_KEYDOWN || msg == WM_UNDO || msg == WM_CLEAR) TextBoxResize();
  return r;
}

static void TextBoxOpen(Tab* t, int page, float x, float y) {
  if (g_te) TextBoxClose(true);
  g_tbTab = t->id; g_tbPage = page; g_tbX = x; g_tbY = y;
  int sx, sy;
  ViewPointToScreen(t, page, x, y, &sx, &sy);
  int px = max(9, (int)lround(g_fontSize * t->scale));
  g_teFont = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, g_font.c_str());
  // a pop-up owned by the main window: the page view may present through a swap chain that would hide a child window
  POINT sp = {sx - 3, sy - 2};
  ClientToScreen(A_.view, &sp);
  g_te = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"", WS_POPUP | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                         sp.x, sp.y, S(120), px + S(10), A_.main, nullptr, A_.inst, nullptr);
  SendMessageW(g_te, WM_SETFONT, (WPARAM)g_teFont, TRUE);
  SetWindowSubclass(g_te, TextSub, 77, 0);
  SetFocus(g_te);
  TextBoxResize();
}

LRESULT EdCtlColor(HDC dc, HWND ctl) {
  static HBRUSH br = nullptr;
  if (!br) br = CreateSolidBrush(RGB(255, 255, 255));
  if (ctl == g_te) {
    SetTextColor(dc, g_colText == RGB(255, 255, 255) ? RGB(0, 0, 0) : g_colText);
    SetBkColor(dc, RGB(255, 255, 255));
    return (LRESULT)br;
  }
  return 0;
}

// ------------------------------------------------------------------ the bar
static std::vector<EdBtn> g_bar;
static RECT g_barR = {0, 0, 0, 0};
static int g_hot = -1, g_press = -1;

bool EdOn() { return G.edMode == 2; }

int EdBarHeight() {
  Tab* t = CurTab();
  return (EdOn() && t && t->doc && !A_.fullscreen) ? S(44) : 0;
}

static void AddB(int& x, int y, int id, Icon ic, const wchar_t* tip, int kind = 0, int w = 36) {
  EdBtn b;
  b.id = id; b.ic = ic; b.tip = tip; b.enabled = true; b.checked = false; b.kind = kind; b.col = 0;
  b.r = {x, y, x + S(w), y + S(36)};
  x += S(w + 2);
  g_bar.push_back(b);
}

void EdBarLayout(RECT bar) {
  g_bar.clear();
  g_barR = bar;
  if (bar.bottom <= bar.top) return;
  int y = bar.top + (bar.bottom - bar.top - S(36)) / 2;
  int x = S(6);
  auto gap = [&]() { x += S(10); };
  AddB(x, y, ID_ED_SAVE, IC_SAVE, L"Сохранить (Ctrl+S)");
  AddB(x, y, ID_ED_UNDO, IC_UNDO, L"Отменить (Ctrl+Z)");
  AddB(x, y, ID_ED_REDO, IC_REDO, L"Повторить (Ctrl+Y)");
  gap();
  AddB(x, y, ID_ED_T_SELECT, IC_CURSOR, L"Выбор и перемещение объектов");
  AddB(x, y, ID_ED_T_HILITE, IC_HILITE, L"Выделить текст цветом");
  AddB(x, y, ID_ED_T_UNDER, IC_UNDER, L"Подчеркнуть текст");
  AddB(x, y, ID_ED_T_STRIKE, IC_STRIKE, L"Зачеркнуть текст");
  AddB(x, y, ID_ED_T_SQUIG, IC_SQUIG, L"Волнистая линия под текстом");
  gap();
  AddB(x, y, ID_ED_T_TEXT, IC_TEXTT, L"Добавить текст");
  AddB(x, y, ID_ED_T_INK, IC_PEN, L"Рисовать от руки");
  AddB(x, y, ID_ED_T_RECT, IC_RECT, L"Прямоугольник");
  AddB(x, y, ID_ED_T_ELLIPSE, IC_ELLIPSE, L"Эллипс");
  AddB(x, y, ID_ED_IMAGE, IC_IMAGE, L"Вставить подпись или печать (картинку)");
  gap();
  AddB(x, y, ID_ED_COLOR, IC_PLUS, L"Цвет", 1, 36);
  AddB(x, y, ID_ED_SIZE, IC_PLUS, L"Толщина линии / размер шрифта", 2, 52);
  gap();
  AddB(x, y, ID_ED_FRONT, IC_TOFRONT, L"На передний план");
  AddB(x, y, ID_ED_FWD, IC_FORWARD, L"Переместить вперёд");
  AddB(x, y, ID_ED_BWD, IC_BACKWARD, L"Переместить назад");
  AddB(x, y, ID_ED_BACK, IC_TOBACK, L"На задний план");
  AddB(x, y, ID_ED_DELETE, IC_TRASH, L"Удалить выбранное (Del)");
  gap();
  AddB(x, y, ID_ED_PAGES, IC_PAGES, L"Страницы: повернуть, удалить, переставить, вставить, разделить");
}

static void BtnState(const EdBtn& b, bool* en, bool* chk) {
  Tab* t = CurTab();
  *en = true; *chk = false;
  bool doc = t && t->doc;
  switch (b.id) {
    case ID_ED_SAVE: *en = doc && EdIsEdited(t); break;
    case ID_ED_UNDO: *en = doc && !t->doc->undo.empty(); break;
    case ID_ED_REDO: *en = doc && !t->doc->redo.empty(); break;
    case ID_ED_T_SELECT: *chk = g_tool == ET_SELECT; break;
    case ID_ED_T_HILITE: *chk = g_tool == ET_HILITE; break;
    case ID_ED_T_UNDER: *chk = g_tool == ET_UNDER; break;
    case ID_ED_T_STRIKE: *chk = g_tool == ET_STRIKE; break;
    case ID_ED_T_SQUIG: *chk = g_tool == ET_SQUIG; break;
    case ID_ED_T_TEXT: *chk = g_tool == ET_TEXT; break;
    case ID_ED_T_INK: *chk = g_tool == ET_INK; break;
    case ID_ED_T_RECT: *chk = g_tool == ET_RECT; break;
    case ID_ED_T_ELLIPSE: *chk = g_tool == ET_ELLIPSE; break;
    case ID_ED_FRONT: case ID_ED_FWD: case ID_ED_BWD: case ID_ED_BACK: *en = g_sel.kind == 1; break;
    case ID_ED_DELETE: *en = g_sel.kind != 0; break;
  }
}

void EdBarPaint(HDC dc, RECT bar) {
  if (bar.bottom <= bar.top) return;
  HBRUSH b = CreateSolidBrush(T.chrome2);
  FillRect(dc, &bar, b);
  DeleteObject(b);
  HPEN pen = CreatePen(PS_SOLID, 1, T.line);
  HGDIOBJ op = SelectObject(dc, pen);
  MoveToEx(dc, 0, bar.bottom - 1, nullptr);
  LineTo(dc, bar.right, bar.bottom - 1);
  SelectObject(dc, op);
  DeleteObject(pen);
  for (size_t i = 0; i < g_bar.size(); i++) {
    const EdBtn& e = g_bar[i];
    bool en, chk;
    BtnState(e, &en, &chk);
    bool hot = (int)i == g_hot, pr = (int)i == g_press && hot;
    if (chk) FillRoundRect(dc, e.r, S(5), T.accent, 55);
    else if (hot && en) FillRoundRect(dc, e.r, S(5), pr ? T.press : T.hover);
    COLORREF c = !en ? T.dim : (chk ? T.accent : T.text);
    if (e.kind == 1) {
      COLORREF cc = *ActiveColor();
      int sz = S(20);
      RECT sw = {(e.r.left + e.r.right) / 2 - sz / 2, (e.r.top + e.r.bottom) / 2 - sz / 2, (e.r.left + e.r.right) / 2 + sz / 2, (e.r.top + e.r.bottom) / 2 + sz / 2};
      FillRoundRect(dc, sw, S(4), cc);
      FrameRoundRect(dc, sw, S(4), T.dark ? RGB(200, 200, 200) : RGB(90, 90, 90), 1.2f);
    } else if (e.kind == 2) {
      ws lab = SizeIsFont() ? FmtT(L"%d", g_fontSize) : FmtT(L"%d pt", g_lineW);
      if (g_sel.kind == 1 && g_selKind == EK_TEXT) { int s = EdSelTextSize(CurTab(), g_sel); if (s > 0) lab = FmtT(L"%d", s); }
      DrawTextW2(dc, lab, e.r, c, UiFont(S(13)), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      FRect cr = {(float)e.r.right - S(14), (float)e.r.top + S(14), (float)e.r.right - S(6), (float)e.r.bottom - S(14)};
      DrawIcon(dc, IC_CHEVD, cr, T.dim, 1.3f);
    } else {
      FRect ir = {(float)e.r.left + S(7), (float)e.r.top + S(7), (float)e.r.right - S(7), (float)e.r.bottom - S(7)};
      DrawIcon(dc, e.ic, ir, c, 1.6f);
    }
  }
}

static int BarHit(int x, int y) {
  POINT pt = {x, y};
  for (size_t i = 0; i < g_bar.size(); i++) if (PtInRect(&g_bar[i].r, pt)) return (int)i;
  return -1;
}

bool EdBarMouse(UINT msg, int x, int y) {
  if (g_barR.bottom <= g_barR.top) return false;
  switch (msg) {
    case WM_MOUSEMOVE: {
      int h = BarHit(x, y);
      if (h != g_hot) { g_hot = h; InvalidateRect(A_.main, &g_barR, FALSE); }
      return false;
    }
    case WM_MOUSELEAVE:
      if (g_hot != -1) { g_hot = -1; InvalidateRect(A_.main, &g_barR, FALSE); }
      return false;
    case WM_LBUTTONDOWN: {
      int h = BarHit(x, y);
      if (h < 0) return false;
      bool en, chk;
      BtnState(g_bar[h], &en, &chk);
      if (en) { g_press = h; SetCapture(A_.main); InvalidateRect(A_.main, &g_barR, FALSE); }
      return true;
    }
    case WM_LBUTTONUP: {
      if (g_press < 0) return false;
      int pr = g_press;
      g_press = -1;
      if (GetCapture() == A_.main) ReleaseCapture();
      InvalidateRect(A_.main, &g_barR, FALSE);
      if (BarHit(x, y) == pr) EdCommand(g_bar[pr].id);
      return true;
    }
  }
  return false;
}

void EdBarTips(std::vector<std::pair<RECT, ws>>* out) {
  for (auto& b : g_bar) out->push_back({b.r, b.tip});
}

// ------------------------------------------------------------------ menus
static RECT BtnRectScreen(int id) {
  for (auto& b : g_bar) if (b.id == id) {
    POINT a = {b.r.left, b.r.bottom}, c = {b.r.right, b.r.top};
    ClientToScreen(A_.main, &a); ClientToScreen(A_.main, &c);
    return RECT{a.x, c.y, c.x, a.y};
  }
  return RECT{0, 0, 0, 0};
}
static int PopupBelow(HMENU m, int btnId) {
  RECT r = BtnRectScreen(btnId);
  TPMPARAMS tp = {sizeof(tp), r};
  return TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom, A_.main, &tp);
}

static HBITMAP SwatchBmp(COLORREF c, int sz) {
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = sz;
  bi.bmiHeader.biHeight = -sz;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!hb || !bits) return nullptr;
  uint32_t* p = (uint32_t*)bits;
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++) {
      bool edge = x == 0 || y == 0 || x == sz - 1 || y == sz - 1;
      uint32_t px = edge ? 0xFF707070u : (0xFF000000u | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c));
      p[y * sz + x] = px;
    }
  return hb;
}

static void ApplyColorToSel(COLORREF c) {
  Tab* t = CurTab();
  if (!t || g_sel.kind == 0) return;
  ESel s = g_sel;
  if (EdSetColorSel(t, s, c)) SetSel(s);
}

static void ShowColorMenu() {
  HMENU m = CreatePopupMenu();
  std::vector<HBITMAP> bms;
  COLORREF cur = *ActiveColor();
  for (int i = 0; i < (int)(sizeof(kPal) / sizeof(kPal[0])); i++) {
    AppendMenuW(m, MF_STRING | (kPal[i].c == cur ? MF_CHECKED : 0), ID_ED_MENU_COLOR + i, kPal[i].name);
    HBITMAP hb = SwatchBmp(kPal[i].c, S(14));
    if (hb) {
      bms.push_back(hb);
      MENUITEMINFOW mi = {sizeof(mi)};
      mi.fMask = MIIM_BITMAP;
      mi.hbmpItem = hb;
      SetMenuItemInfoW(m, ID_ED_MENU_COLOR + i, FALSE, &mi);
    }
  }
  int cmd = PopupBelow(m, ID_ED_COLOR);
  DestroyMenu(m);
  for (HBITMAP h : bms) DeleteObject(h);
  if (cmd >= ID_ED_MENU_COLOR && cmd < ID_ED_MENU_COLOR + 16) {
    COLORREF c = kPal[cmd - ID_ED_MENU_COLOR].c;
    *ActiveColor() = c;
    ApplyColorToSel(c);
    Redraw();
  }
}

static void ShowSizeMenu() {
  HMENU m = CreatePopupMenu();
  bool font = SizeIsFont();
  HMENU fm = nullptr;
  if (font) {
    for (int i = 0; i < (int)(sizeof(kFontSz) / sizeof(kFontSz[0])); i++)
      AppendMenuW(m, MF_STRING | (kFontSz[i] == g_fontSize ? MF_CHECKED : 0), ID_ED_MENU_SIZE + i, FmtT(L"%d pt", kFontSz[i]).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    fm = CreatePopupMenu();
    std::vector<ws> fonts = EdAvailableFonts();
    for (size_t i = 0; i < fonts.size() && i < 8; i++)
      AppendMenuW(fm, MF_STRING | (fonts[i] == g_font ? MF_CHECKED : 0), ID_ED_MENU_FONT + (UINT)i, fonts[i].c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fm, L"Шрифт");
  } else {
    for (int i = 0; i < (int)(sizeof(kLineW) / sizeof(kLineW[0])); i++)
      AppendMenuW(m, MF_STRING | (kLineW[i] == g_lineW ? MF_CHECKED : 0), ID_ED_MENU_SIZE + i, FmtT(L"%d pt", kLineW[i]).c_str());
  }
  int cmd = PopupBelow(m, ID_ED_SIZE);
  DestroyMenu(m);
  Tab* t = CurTab();
  if (cmd >= ID_ED_MENU_SIZE && cmd < ID_ED_MENU_SIZE + 16) {
    int i = cmd - ID_ED_MENU_SIZE;
    if (font) {
      g_fontSize = kFontSz[i];
      if (t && g_sel.kind == 1 && g_selKind == EK_TEXT) { ESel s = g_sel; if (EdSetTextSize(t, s, g_fontSize)) SetSel(s); }
    } else g_lineW = kLineW[i];
    Redraw();
  } else if (cmd >= ID_ED_MENU_FONT && cmd < ID_ED_MENU_FONT + 8) {
    std::vector<ws> fonts = EdAvailableFonts();
    int i = cmd - ID_ED_MENU_FONT;
    if (i < (int)fonts.size()) g_font = fonts[i];
    Redraw();
  }
}

// --- pictures (signature, stamp)
static ws StampDir() {
  ws d = AppDataDir() + L"\\stamps";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}
static std::vector<ws> StampFiles() {
  std::vector<ws> v;
  ws dir = StampDir();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return v;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    ws n = LowerW(fd.cFileName);
    size_t dot = n.rfind(L'.');
    ws ext = dot == ws::npos ? L"" : n.substr(dot);
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".gif") v.push_back(dir + L"\\" + fd.cFileName);
  } while (FindNextFileW(h, &fd) && v.size() < 60);
  FindClose(h);
  std::sort(v.begin(), v.end());
  return v;
}
static HBITMAP ThumbOf(const ws& file, int sz) {
  Gdiplus::Bitmap src(file.c_str());
  if (src.GetLastStatus() != Gdiplus::Ok) return nullptr;
  Gdiplus::Bitmap bmp(sz, sz, PixelFormat32bppPARGB);
  {
    Gdiplus::Graphics g(&bmp);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    double k = min((double)sz / src.GetWidth(), (double)sz / src.GetHeight());
    int w = max(1, (int)(src.GetWidth() * k)), h = max(1, (int)(src.GetHeight() * k));
    g.DrawImage(&src, (sz - w) / 2, (sz - h) / 2, w, h);
  }
  Gdiplus::BitmapData bd;
  Gdiplus::Rect rc(0, 0, sz, sz);
  if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok) return nullptr;
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = sz;
  bi.bmiHeader.biHeight = -sz;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (hb && bits) for (int y = 0; y < sz; y++) memcpy((BYTE*)bits + (size_t)y * sz * 4, (BYTE*)bd.Scan0 + (size_t)y * bd.Stride, (size_t)sz * 4);
  bmp.UnlockBits(&bd);
  return hb;
}

static void InsertPicture(const ws& file) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  int iw, ih;
  if (!EdImageSize(file, &iw, &ih)) { MessageBoxW(A_.main, L"Не удалось прочитать картинку", APP_TITLE, MB_OK | MB_ICONWARNING); return; }
  RECT vr;
  GetClientRect(A_.view, &vr);
  int page;
  float cx, cy;
  bool exact;
  if (!ViewHitPage(t, vr.right / 2, vr.bottom / 2, &page, &cx, &cy, &exact)) return;
  float pw = t->doc->pw[page], ph = t->doc->ph[page];
  float w = min(pw * 0.30f, iw * 0.75f);
  w = max(w, 24.0f);
  float h = w * ih / iw;
  if (h > ph * 0.45f) { h = ph * 0.45f; w = h * iw / ih; }
  FRect r = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
  if (r.l < 0) { r.r -= r.l; r.l = 0; }
  if (r.t < 0) { r.b -= r.t; r.t = 0; }
  if (r.r > pw) { r.l -= r.r - pw; r.r = pw; }
  if (r.b > ph) { r.t -= r.b - ph; r.b = ph; }
  ESel s;
  if (EdAddImage(t, page, r, file, &s)) { g_tool = ET_SELECT; SetSel(s); ShowToast(L"Перетащите картинку на место, углами меняйте размер"); Redraw(); }
  else ShowToast(L"Не удалось вставить картинку");
}

static ws AddToLibrary(const ws& file) {
  ws dir = StampDir();
  ws name = BaseName(file);
  ws dst = dir + L"\\" + name;
  for (int i = 2; FileExists(dst) && i < 1000; i++) {
    ws stem = StripExt(name), ext = name.substr(stem.size());
    dst = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
  }
  if (!CopyFileW(file.c_str(), dst.c_str(), TRUE)) return file;
  return dst;
}

static void ShowImageMenu() {
  std::vector<ws> files = StampFiles();
  HMENU m = CreatePopupMenu(), dm = CreatePopupMenu();
  std::vector<HBITMAP> bms;
  AppendMenuW(m, MF_STRING, ID_ED_IMG_ADD, L"Добавить из файла…");
  if (!files.empty()) {
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < files.size() && i < 64; i++) {
      ws label = StripExt(BaseName(files[i]));
      for (size_t k = 0; k < label.size(); k++) if (label[k] == L'&') { label.insert(k, L"&"); k++; }
      AppendMenuW(m, MF_STRING, ID_ED_MENU_IMG + (UINT)i, label.c_str());
      HBITMAP hb = ThumbOf(files[i], S(32));
      if (hb) {
        bms.push_back(hb);
        MENUITEMINFOW mi = {sizeof(mi)};
        mi.fMask = MIIM_BITMAP;
        mi.hbmpItem = hb;
        SetMenuItemInfoW(m, ID_ED_MENU_IMG + (UINT)i, FALSE, &mi);
      }
      AppendMenuW(dm, MF_STRING, ID_ED_MENU_IMGDEL + (UINT)i, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dm, L"Убрать из списка");
  }
  int cmd = PopupBelow(m, ID_ED_IMAGE);
  DestroyMenu(m);
  for (HBITMAP h : bms) DeleteObject(h);
  if (cmd == ID_ED_IMG_ADD) {
    wchar_t buf[MAX_PATH * 2] = {0};
    OPENFILENAMEW of = {sizeof(of)};
    of.hwndOwner = A_.main;
    of.lpstrFilter = L"Картинки (PNG, JPEG, BMP)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0Все файлы\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH * 2;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    of.lpstrTitle = L"Подпись или печать";
    if (GetOpenFileNameW(&of)) InsertPicture(AddToLibrary(buf));
  } else if (cmd >= ID_ED_MENU_IMG && cmd < ID_ED_MENU_IMG + 64) {
    size_t i = cmd - ID_ED_MENU_IMG;
    if (i < files.size()) InsertPicture(files[i]);
  } else if (cmd >= ID_ED_MENU_IMGDEL && cmd < ID_ED_MENU_IMGDEL + 64) {
    size_t i = cmd - ID_ED_MENU_IMGDEL;
    if (i < files.size()) { DeleteFileW(files[i].c_str()); ShowToast(L"Убрано из списка"); }
  }
}

// --- pages
static ws RangeText(const std::vector<int>& v) {
  ws out;
  size_t i = 0;
  while (i < v.size()) {
    size_t j = i;
    while (j + 1 < v.size() && v[j + 1] == v[j] + 1) j++;
    if (!out.empty()) out += L",";
    out += std::to_wstring(v[i] + 1);
    if (j > i) out += L"-" + std::to_wstring(v[j] + 1);
    i = j + 1;
  }
  return out;
}
static bool PickPdfs(std::vector<ws>* out) {
  std::vector<wchar_t> buf(65536, 0);
  OPENFILENAMEW of = {sizeof(of)};
  of.hwndOwner = A_.main;
  of.lpstrFilter = L"Документы PDF (*.pdf)\0*.pdf\0";
  of.lpstrFile = buf.data();
  of.nMaxFile = (DWORD)buf.size();
  of.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  of.lpstrTitle = L"Вставить страницы из файла";
  if (!GetOpenFileNameW(&of)) return false;
  ws first = buf.data();
  const wchar_t* p = buf.data() + first.size() + 1;
  if (*p == 0) { out->push_back(first); return true; }
  while (*p) { out->push_back(first + L"\\" + p); p += wcslen(p) + 1; }
  return true;
}
static void GoPage(int p) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewGoPage(min(max(p, 0), t->doc->npages - 1), 0, false, false);
}
static ws UniquePath(const ws& dir, const ws& stem) {
  ws p = dir + L"\\" + stem + L".pdf";
  for (int i = 2; FileExists(p) && i < 10000; i++) p = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L").pdf";
  return p;
}

static void PageCommand(int cmd) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  int cur = ViewCurPage(), n = t->doc->npages;
  switch (cmd) {
    case ID_ED_PG_ROTCW: EdRotatePage(t, cur, 1); break;
    case ID_ED_PG_ROTCCW: EdRotatePage(t, cur, -1); break;
    case ID_ED_PG_DEL:
      if (n <= 1) { ShowToast(L"Нельзя удалить единственную страницу"); break; }
      if (EdDeletePages(t, {cur})) GoPage(cur);
      break;
    case ID_ED_PG_DELRANGE: {
      ws in;
      if (!InputText(A_.main, L"Удалить страницы", L"Какие страницы удалить? Например: 2, 4-6 или 8- (до конца).", std::to_wstring(cur + 1), &in)) break;
      std::vector<int> v;
      if (!ParsePageRange(in, n, &v)) { MessageBoxW(A_.main, L"Не удалось разобрать номера страниц.", APP_TITLE, MB_OK | MB_ICONWARNING); break; }
      if ((int)v.size() >= n) { MessageBoxW(A_.main, L"Нельзя удалить все страницы документа.", APP_TITLE, MB_OK | MB_ICONWARNING); break; }
      if (EdDeletePages(t, v)) GoPage(min(v[0], t->doc->npages - 1));
      break;
    }
    case ID_ED_PG_UP: if (cur > 0 && EdMovePage(t, cur, cur - 1)) GoPage(cur - 1); break;
    case ID_ED_PG_DOWN: if (cur + 1 < n && EdMovePage(t, cur, cur + 1)) GoPage(cur + 1); break;
    case ID_ED_PG_BLANK: if (EdBlankPage(t, cur)) GoPage(cur + 1); break;
    case ID_ED_PG_INSERT: case ID_ED_PG_APPEND: {
      std::vector<ws> files;
      if (!PickPdfs(&files)) break;
      int at = cmd == ID_ED_PG_INSERT ? cur + 1 : n;
      if (EdInsertPages(t, at, files)) GoPage(at);
      break;
    }
    case ID_ED_PG_EXTRACT: {
      ws in;
      if (!InputText(A_.main, L"Извлечь страницы", L"Какие страницы сохранить в новый файл? Например: 1-3, 5.", std::to_wstring(cur + 1), &in)) break;
      std::vector<int> v;
      if (!ParsePageRange(in, n, &v)) { MessageBoxW(A_.main, L"Не удалось разобрать номера страниц.", APP_TITLE, MB_OK | MB_ICONWARNING); break; }
      wchar_t buf[MAX_PATH * 2];
      wcsncpy(buf, (StripExt(BaseName(t->path)) + L" (страницы).pdf").c_str(), MAX_PATH);
      OPENFILENAMEW of = {sizeof(of)};
      of.hwndOwner = A_.main;
      of.lpstrFilter = L"Документы PDF (*.pdf)\0*.pdf\0";
      of.lpstrFile = buf;
      of.nMaxFile = MAX_PATH * 2;
      of.lpstrDefExt = L"pdf";
      of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      if (!GetSaveFileNameW(&of)) break;
      if (EdExtractPages(t, WToUtf8(RangeText(v)), buf)) ShowToast(L"Страницы сохранены в новый файл");
      else MessageBoxW(A_.main, L"Не удалось сохранить файл", APP_TITLE, MB_OK | MB_ICONWARNING);
      break;
    }
    case ID_ED_PG_SPLIT: {
      ws in;
      if (!InputText(A_.main, L"Разделить документ", L"Сколько страниц должно быть в каждом новом файле? Файлы появятся рядом с документом.", L"1", &in)) break;
      int per = _wtoi(in.c_str());
      if (per < 1 || per >= n) { MessageBoxW(A_.main, per >= n ? L"В документе не больше страниц, чем в одной части." : L"Введите число страниц, например 1 или 5.", APP_TITLE, MB_OK | MB_ICONWARNING); break; }
      ws dir = DirName(t->path), stem = StripExt(BaseName(t->path));
      int made = 0, parts = (n + per - 1) / per;
      for (int k = 0; k < parts; k++) {
        std::vector<int> v;
        for (int p = k * per; p < min(n, (k + 1) * per); p++) v.push_back(p);
        ws out = UniquePath(dir, stem + L" - часть " + std::to_wstring(k + 1));
        if (EdExtractPages(t, WToUtf8(RangeText(v)), out)) made++;
      }
      ShowToast(FmtT(L"Создано файлов: %d", made));
      break;
    }
  }
}

static void ShowPagesMenu() {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  int cur = ViewCurPage(), n = t->doc->npages;
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_ED_PG_ROTCW, L"Повернуть страницу по часовой");
  AppendMenuW(m, MF_STRING, ID_ED_PG_ROTCCW, L"Повернуть страницу против часовой");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (n > 1 ? 0 : MF_GRAYED), ID_ED_PG_DEL, L"Удалить эту страницу");
  AppendMenuW(m, MF_STRING | (n > 1 ? 0 : MF_GRAYED), ID_ED_PG_DELRANGE, L"Удалить страницы…");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (cur > 0 ? 0 : MF_GRAYED), ID_ED_PG_UP, L"Переместить страницу вверх");
  AppendMenuW(m, MF_STRING | (cur + 1 < n ? 0 : MF_GRAYED), ID_ED_PG_DOWN, L"Переместить страницу вниз");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_ED_PG_BLANK, L"Вставить пустую страницу после этой");
  AppendMenuW(m, MF_STRING, ID_ED_PG_INSERT, L"Вставить страницы из файла после этой…");
  AppendMenuW(m, MF_STRING, ID_ED_PG_APPEND, L"Добавить страницы из файла в конец…");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_ED_PG_EXTRACT, L"Извлечь страницы в новый файл…");
  AppendMenuW(m, MF_STRING | (n > 1 ? 0 : MF_GRAYED), ID_ED_PG_SPLIT, L"Разделить документ на файлы…");
  int cmd = PopupBelow(m, ID_ED_PAGES);
  DestroyMenu(m);
  if (cmd) PageCommand(cmd);
}

// ------------------------------------------------------------------ commands
void EdOnTabChanged() {
  if (g_te) TextBoxClose(false);
  ClearSel();
  g_g = G_NONE;
  g_ink.clear();
  if (GetCapture() == A_.view) ReleaseCapture();
}

static void SetTool(int tool) {
  if (g_te) TextBoxClose(true);
  g_tool = tool;
  if (tool != ET_SELECT) ClearSel();
  g_g = G_NONE;
  if (A_.view) SetFocus(A_.view);
  Redraw();
}

void EdSetMode(int mode) {
  mode = min(max(mode, 0), 2);
  if (mode == G.edMode) return;
  if (g_te) TextBoxClose(true);
  G.edMode = mode;
  G.pro = mode >= 1;
  if (mode != 2) { g_tool = ET_SELECT; ClearSel(); g_g = G_NONE; g_ink.clear(); }
  if (!G.pro) { G.sideTab = 0; ViewClearSel(); }
  MainLayout();
  ViewInvalidate();
  SideSync();
  InvalidateRect(A_.main, nullptr, FALSE);
  UpdateTitle();
  SaveSettings();
}

void EdCommand(int id) {
  if (id >= ID_ED_MODE0 && id <= ID_ED_MODE2) { EdSetMode(id - ID_ED_MODE0); return; }
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  if (g_te && id != ID_ED_T_TEXT) TextBoxClose(true);
  switch (id) {
    case ID_ED_SAVE: EdSave(t); break;
    case ID_ED_SAVEAS_E: EdSaveAs(t); break;
    case ID_ED_UNDO: ClearSel(); EdUndo(t); break;
    case ID_ED_REDO: ClearSel(); EdRedo(t); break;
    case ID_ED_T_SELECT: SetTool(ET_SELECT); break;
    case ID_ED_T_HILITE: case ID_ED_T_UNDER: case ID_ED_T_STRIKE: case ID_ED_T_SQUIG:
      SetTool(ET_HILITE + (id - ID_ED_T_HILITE));
      EdAfterTextSelection();
      break;
    case ID_ED_T_TEXT: SetTool(ET_TEXT); break;
    case ID_ED_T_INK: SetTool(ET_INK); break;
    case ID_ED_T_RECT: SetTool(ET_RECT); break;
    case ID_ED_T_ELLIPSE: SetTool(ET_ELLIPSE); break;
    case ID_ED_IMAGE: ShowImageMenu(); break;
    case ID_ED_COLOR: ShowColorMenu(); break;
    case ID_ED_SIZE: ShowSizeMenu(); break;
    case ID_ED_PAGES: ShowPagesMenu(); break;
    case ID_ED_FRONT: case ID_ED_FWD: case ID_ED_BWD: case ID_ED_BACK:
      if (g_sel.kind == 1) {
        ESel s = g_sel;
        int op = id == ID_ED_FRONT ? 0 : id == ID_ED_FWD ? 1 : id == ID_ED_BWD ? 2 : 3;
        EdZOrder(t, s, op);
        SetSel(s);
      }
      break;
    case ID_ED_DELETE: case ID_ED_CTX_DELETE: {
      ESel s = id == ID_ED_CTX_DELETE ? g_ctxSel : g_sel;
      if (s.kind) { ClearSel(); EdDeleteSel(t, s); }
      break;
    }
  }
  Redraw();
}

bool EdKey(UINT vk, bool ctrl, bool shift) {
  HWND f = GetFocus();
  if (f == g_te || f == A_.pageEdit || f == A_.findEdit) return false;
  Tab* t = CurTab();
  if (!t || !t->doc) return false;
  if (ctrl) {
    switch (vk) {
      case 'S': EdCommand(shift ? ID_ED_SAVEAS_E : ID_ED_SAVE); return true;
      case 'Z': if (!EdOn()) return false; EdCommand(shift ? ID_ED_REDO : ID_ED_UNDO); return true;
      case 'Y': if (!EdOn()) return false; EdCommand(ID_ED_REDO); return true;
      case '1': case '2': case '3': EdSetMode(vk - '1'); return true;
    }
    return false;
  }
  if (!EdOn()) return false;
  switch (vk) {
    case VK_DELETE: if (g_sel.kind) { EdCommand(ID_ED_DELETE); return true; } return false;
    case VK_ESCAPE:
      if (g_g != G_NONE) { g_g = G_NONE; g_ink.clear(); if (GetCapture() == A_.view) ReleaseCapture(); Redraw(); return true; }
      if (g_sel.kind) { ClearSel(); Redraw(); return true; }
      if (g_tool != ET_SELECT) { SetTool(ET_SELECT); return true; }
      return false;
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
      if (g_sel.kind && g_tool == ET_SELECT) {
        float d = shift ? 10.0f : 1.0f;
        float dx = vk == VK_LEFT ? -d : vk == VK_RIGHT ? d : 0, dy = vk == VK_UP ? -d : vk == VK_DOWN ? d : 0;
        FRect b = g_selBox, nb = {b.l + dx, b.t + dy, b.r + dx, b.b + dy};
        ESel s = g_sel;
        if (EdTransformSel(t, s, b, nb)) SetSel(s);
        Redraw();
        return true;
      }
      return false;
  }
  return false;
}

// ------------------------------------------------------------------ mouse in the page view
static int HandleSize() { return S(9); }
static RECT HandleRect(const RECT& box, int h) {
  int hs = HandleSize(), cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
  int x = (h == 0 || h == 6 || h == 7) ? box.left : (h == 2 || h == 3 || h == 4) ? box.right : cx;
  int y = (h == 0 || h == 1 || h == 2) ? box.top : (h == 4 || h == 5 || h == 6) ? box.bottom : cy;
  return RECT{x - hs / 2, y - hs / 2, x + hs / 2 + 1, y + hs / 2 + 1};
}
// resizable handles for the current selection
static bool HandleUsable(int h) {
  bool corner = h == 0 || h == 2 || h == 4 || h == 6;
  if (g_sel.kind == 1 && g_selKind == EK_TEXT) return corner;      // text scales evenly
  if (g_sel.kind == 2 && g_sel.sub == FPDF_ANNOT_INK) return corner;
  if (g_sel.kind == 2 && (g_sel.sub == FPDF_ANNOT_HIGHLIGHT || g_sel.sub == FPDF_ANNOT_UNDERLINE || g_sel.sub == FPDF_ANNOT_STRIKEOUT || g_sel.sub == FPDF_ANNOT_SQUIGGLY)) return false;
  return true;
}
static int HandleAt(Tab* t, int mx, int my) {
  if (g_sel.kind == 0 || g_sel.page < 0) return -1;
  RECT box = ViewRectToScreen(t, g_sel.page, g_selBox);
  POINT pt = {mx, my};
  for (int h = 0; h < 8; h++) {
    if (!HandleUsable(h)) continue;
    RECT r = HandleRect(box, h);
    InflateRect(&r, S(3), S(3));
    if (PtInRect(&r, pt)) return h;
  }
  return -1;
}
static bool InSelBox(Tab* t, int mx, int my) {
  if (g_sel.kind == 0) return false;
  RECT box = ViewRectToScreen(t, g_sel.page, g_selBox);
  InflateRect(&box, S(3), S(3));
  POINT pt = {mx, my};
  return PtInRect(&box, pt) != 0;
}

static FRect ResizeBox(const FRect& b0, int h, float x, float y, bool uniform) {
  FRect n = b0;
  switch (h) {
    case 0: n.l = x; n.t = y; break;
    case 1: n.t = y; break;
    case 2: n.r = x; n.t = y; break;
    case 3: n.r = x; break;
    case 4: n.r = x; n.b = y; break;
    case 5: n.b = y; break;
    case 6: n.l = x; n.b = y; break;
    case 7: n.l = x; break;
  }
  const float MINS = 6.0f;
  bool corner = h == 0 || h == 2 || h == 4 || h == 6;
  if (corner && uniform) {
    float w0 = b0.r - b0.l, h0 = b0.b - b0.t;
    float w = n.r - n.l, hh = n.b - n.t;
    float s = max(w / max(w0, 0.01f), hh / max(h0, 0.01f));
    s = max(s, MINS / max(min(w0, h0), 0.01f));
    float nw = w0 * s, nh = h0 * s;
    bool left = h == 0 || h == 6, top = h == 0 || h == 2;
    n.l = left ? b0.r - nw : b0.l; n.r = left ? b0.r : b0.l + nw;
    n.t = top ? b0.b - nh : b0.t; n.b = top ? b0.b : b0.t + nh;
    return n;
  }
  if (n.r - n.l < MINS) { if (h == 0 || h == 6 || h == 7) n.l = n.r - MINS; else n.r = n.l + MINS; }
  if (n.b - n.t < MINS) { if (h == 0 || h == 1 || h == 2) n.t = n.b - MINS; else n.b = n.t + MINS; }
  return n;
}

static void PtrPage(Tab* t, int page, int mx, int my, float* x, float* y) { ViewScreenToPage(t, page, mx, my, x, y); }

bool EdMouseDown(Tab* t, int mx, int my, bool dbl) {
  (void)dbl;
  if (!EdOn()) return false;
  if (!t || !t->doc) return false;
  if (g_te) { TextBoxClose(true); return true; }               // the click that ends the text box does nothing else
  int page;
  float x, y;
  bool exact;
  bool onPage = ViewHitPage(t, mx, my, &page, &x, &y, &exact) && exact;
  g_gDown = {mx, my};
  g_gMoved = false;
  if (g_tool == ET_SELECT) {
    int h = HandleAt(t, mx, my);
    if (h >= 0) {
      g_g = G_RESIZE; g_gHandle = h; g_gPage = g_sel.page; g_gBox0 = g_selBox; g_gBox = g_selBox;
      if (g_sel.kind == 1 && g_selKind == EK_IMAGE) { FreePreview(); g_preview = EdImagePreview(t, g_sel, &g_prevW, &g_prevH); }
      return true;
    }
    if (onPage) {
      EdHit hit;
      bool inSel = InSelBox(t, mx, my);
      if (inSel || EdHitTest(t, page, x, y, false, &hit)) {
        if (!inSel) SetSel(hit.sel);
        if (g_sel.kind == 0) return false;
        g_g = G_MOVE; g_gPage = g_sel.page; g_gBox0 = g_selBox; g_gBox = g_selBox;
        PtrPage(t, g_gPage, mx, my, &g_gx0, &g_gy0);
        if (g_sel.kind == 1 && g_selKind == EK_IMAGE) { FreePreview(); g_preview = EdImagePreview(t, g_sel, &g_prevW, &g_prevH); }
        Redraw();
        return true;
      }
    }
    if (g_sel.kind) { ClearSel(); Redraw(); }
    return false;
  }
  if (!onPage) return false;
  switch (g_tool) {
    case ET_TEXT: TextBoxOpen(t, page, x, y); return true;
    case ET_INK: g_g = G_INK; g_gPage = page; g_ink.clear(); g_ink.push_back(PointF(x, y)); Redraw(); return true;
    case ET_RECT: case ET_ELLIPSE: g_g = G_SHAPE; g_gPage = page; g_gx0 = x; g_gy0 = y; g_gBox = {x, y, x, y}; Redraw(); return true;
  }
  return false;
}

bool EdMouseMove(Tab* t, int mx, int my, WPARAM wp) {
  (void)wp;
  if (g_g == G_NONE || !t || !t->doc) return false;
  if (g_gPage >= t->doc->npages) { g_g = G_NONE; return false; }
  float x, y;
  PtrPage(t, g_gPage, mx, my, &x, &y);
  float pw = t->doc->pw[g_gPage], ph = t->doc->ph[g_gPage];
  switch (g_g) {
    case G_MOVE: {
      if (!g_gMoved && abs(mx - g_gDown.x) < 3 && abs(my - g_gDown.y) < 3) return true;
      g_gMoved = true;
      float dx = x - g_gx0, dy = y - g_gy0;
      g_gBox = {g_gBox0.l + dx, g_gBox0.t + dy, g_gBox0.r + dx, g_gBox0.b + dy};
      break;
    }
    case G_RESIZE: {
      g_gMoved = true;
      bool uniform = (g_sel.kind == 1 && g_selKind != 0) || GetKeyState(VK_SHIFT) >= 0 ? true : false;
      if (g_sel.kind == 1 && g_selKind == EK_TEXT) uniform = true;
      else uniform = GetKeyState(VK_SHIFT) >= 0 && !(g_sel.kind == 2 && g_sel.sub != FPDF_ANNOT_INK);
      g_gBox = ResizeBox(g_gBox0, g_gHandle, x, y, uniform);
      break;
    }
    case G_SHAPE: {
      x = min(max(x, 0.0f), pw); y = min(max(y, 0.0f), ph);
      g_gBox = {min(g_gx0, x), min(g_gy0, y), max(g_gx0, x), max(g_gy0, y)};
      if (GetKeyState(VK_SHIFT) < 0) {
        float s = max(g_gBox.r - g_gBox.l, g_gBox.b - g_gBox.t);
        g_gBox.r = g_gBox.l + s; g_gBox.b = g_gBox.t + s;
      }
      break;
    }
    case G_INK: {
      x = min(max(x, 0.0f), pw); y = min(max(y, 0.0f), ph);
      if (g_ink.empty() || hypotf(x - g_ink.back().X, y - g_ink.back().Y) >= 0.6f) g_ink.push_back(PointF(x, y));
      break;
    }
    default: break;
  }
  ViewInvalidate();
  return true;
}

bool EdMouseUp(Tab* t, int mx, int my) {
  (void)mx; (void)my;
  if (g_g == G_NONE) return false;
  Gest g = g_g;
  g_g = G_NONE;
  if (!t || !t->doc) { g_ink.clear(); return true; }
  switch (g) {
    case G_MOVE: case G_RESIZE:
      if (g_gMoved && g_sel.kind) {
        ESel s = g_sel;
        FRect b0 = g_gBox0, b1 = g_gBox;
        FreePreview();
        if (EdTransformSel(t, s, b0, b1)) SetSel(s);
      }
      FreePreview();
      break;
    case G_SHAPE:
      if (g_gBox.r - g_gBox.l >= 4 && g_gBox.b - g_gBox.t >= 4) {
        ESel s;
        bool ell = g_tool == ET_ELLIPSE;
        if (EdAddShape(t, g_gPage, g_gBox, ell, g_colLine, g_lineW, &s)) { g_tool = ET_SELECT; SetSel(s); }
      }
      break;
    case G_INK:
      if (g_ink.size() >= 2) EdAddInk(t, g_gPage, g_ink, g_colLine, g_lineW, nullptr);
      g_ink.clear();
      break;
    default: break;
  }
  Redraw();
  return true;
}

bool EdSetCursor(Tab* t, int mx, int my) {
  if (!EdOn()) return false;
  if (!t || !t->doc || !t->layoutOk) return false;
  LPCWSTR cur = nullptr;
  if (g_g == G_MOVE) cur = IDC_SIZEALL;
  else if (g_g == G_RESIZE || (g_g == G_NONE && g_tool == ET_SELECT && g_sel.kind && HandleAt(t, mx, my) >= 0)) {
    int h = g_g == G_RESIZE ? g_gHandle : HandleAt(t, mx, my);
    cur = (h == 0 || h == 4) ? IDC_SIZENWSE : (h == 2 || h == 6) ? IDC_SIZENESW : (h == 1 || h == 5) ? IDC_SIZENS : IDC_SIZEWE;
  } else if (g_g == G_SHAPE || g_g == G_INK) cur = IDC_CROSS;
  else if (g_g == G_NONE) {
    if (g_tool == ET_SELECT) { if (g_sel.kind && InSelBox(t, mx, my)) cur = IDC_SIZEALL; }
    else if (g_tool == ET_TEXT || g_tool == ET_INK || g_tool == ET_RECT || g_tool == ET_ELLIPSE) {
      int p; float x, y; bool ex;
      if (ViewHitPage(t, mx, my, &p, &x, &y, &ex) && ex) cur = g_tool == ET_TEXT ? IDC_IBEAM : IDC_CROSS;
    }
  }
  if (!cur) return false;
  SetCursor(LoadCursorW(nullptr, cur));
  return true;
}

bool EdActive() { return g_g != G_NONE || g_te != nullptr; }

void EdAfterTextSelection() {
  Tab* t = CurTab();
  if (!EdOn() || !t || !t->doc || !IsMarkupTool(g_tool) || !ViewHasSel()) return;
  COLORREF c = g_tool == ET_HILITE ? g_colHi : g_colLine;
  if (EdAddMarkup(t, g_tool - ET_HILITE, c)) ViewClearSel();
}

void EdContextMenu(HMENU m, Tab* t, int mx, int my) {
  g_ctxSel = ESel();
  if (!EdOn()) return;
  int page;
  float x, y;
  bool exact;
  if (!t || !ViewHitPage(t, mx, my, &page, &x, &y, &exact) || !exact) return;
  EdHit hit;
  if (!EdHitTest(t, page, x, y, true, &hit)) return;
  g_ctxSel = hit.sel;
  const wchar_t* label = L"Удалить";
  if (hit.sel.kind == 2) {
    switch (hit.sel.sub) {
      case FPDF_ANNOT_HIGHLIGHT: label = L"Убрать выделение цветом"; break;
      case FPDF_ANNOT_UNDERLINE: label = L"Убрать подчёркивание"; break;
      case FPDF_ANNOT_STRIKEOUT: label = L"Убрать зачёркивание"; break;
      case FPDF_ANNOT_SQUIGGLY: label = L"Убрать волнистую линию"; break;
      default: label = L"Удалить фигуру"; break;
    }
  } else label = hit.sel.sub == EK_IMAGE ? L"Удалить картинку" : L"Удалить текст";
  AppendMenuW(m, MF_STRING, ID_ED_CTX_DELETE, label);
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
}

// ------------------------------------------------------------------ overlay
static void SelRectDraw(Gdiplus::Graphics& g, const RECT& r, bool handles, bool usable[8]) {
  Gdiplus::Color acc(255, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent));
  Gdiplus::Pen pen(acc, 1.4f);
  pen.SetDashStyle(Gdiplus::DashStyleDash);
  g.DrawRectangle(&pen, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)(r.right - r.left), (Gdiplus::REAL)(r.bottom - r.top));
  if (!handles) return;
  Gdiplus::SolidBrush wb(Gdiplus::Color(255, 255, 255, 255));
  Gdiplus::Pen hp(acc, 1.4f);
  for (int h = 0; h < 8; h++) {
    if (!usable[h]) continue;
    RECT hr = HandleRect(r, h);
    g.FillRectangle(&wb, (int)hr.left, (int)hr.top, (int)(hr.right - hr.left - 1), (int)(hr.bottom - hr.top - 1));
    g.DrawRectangle(&hp, (int)hr.left, (int)hr.top, (int)(hr.right - hr.left - 1), (int)(hr.bottom - hr.top - 1));
  }
}

void EdPaintOverlay(HDC dc, RECT rc, Tab* t, int first, int last) {
  (void)rc;
  if (!EdOn()) return;
  if (!t || !t->doc) return;
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  // selection
  if (g_sel.kind && g_sel.page >= first && g_sel.page <= last && g_sel.page < t->doc->npages) {
    FRect b = g_g == G_NONE ? g_selBox : g_gBox0;
    RECT r = ViewRectToScreen(t, g_sel.page, b);
    bool usable[8];
    for (int h = 0; h < 8; h++) usable[h] = HandleUsable(h);
    if (g_g == G_NONE) SelRectDraw(g, r, true, usable);
    else {
      // the original place stays faint while the ghost moves
      Gdiplus::Pen fp(Gdiplus::Color(120, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent)), 1.0f);
      g.DrawRectangle(&fp, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)(r.right - r.left), (Gdiplus::REAL)(r.bottom - r.top));
    }
  }
  // ghost of a moved / resized object
  if ((g_g == G_MOVE || g_g == G_RESIZE) && g_gMoved && g_gPage >= first && g_gPage <= last) {
    RECT r = ViewRectToScreen(t, g_gPage, g_gBox);
    if (g_preview) {
      HDC mdc = CreateCompatibleDC(dc);
      HGDIOBJ old = SelectObject(mdc, g_preview);
      BLENDFUNCTION bf = {AC_SRC_OVER, 0, 215, AC_SRC_ALPHA};
      SetStretchBltMode(dc, HALFTONE);
      AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mdc, 0, 0, g_prevW, g_prevH, bf);
      SelectObject(mdc, old);
      DeleteDC(mdc);
    } else {
      Gdiplus::SolidBrush fb(Gdiplus::Color(40, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent)));
      g.FillRectangle(&fb, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)(r.right - r.left), (Gdiplus::REAL)(r.bottom - r.top));
    }
    bool usable[8];
    for (int h = 0; h < 8; h++) usable[h] = g_g == G_RESIZE && HandleUsable(h);
    SelRectDraw(g, r, g_g == G_RESIZE, usable);
  }
  // shape being drawn
  if (g_g == G_SHAPE && g_gPage >= first && g_gPage <= last) {
    RECT r = ViewRectToScreen(t, g_gPage, g_gBox);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(g_colLine), GetGValue(g_colLine), GetBValue(g_colLine)), (Gdiplus::REAL)max(1.0, g_lineW * t->scale));
    if (g_tool == ET_ELLIPSE) g.DrawEllipse(&pen, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)(r.right - r.left), (Gdiplus::REAL)(r.bottom - r.top));
    else g.DrawRectangle(&pen, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)(r.right - r.left), (Gdiplus::REAL)(r.bottom - r.top));
  }
  // ink being drawn
  if (g_g == G_INK && g_ink.size() >= 2 && g_gPage >= first && g_gPage <= last) {
    std::vector<Gdiplus::PointF> pts;
    for (auto& p : g_ink) {
      int sx, sy;
      ViewPointToScreen(t, g_gPage, p.X, p.Y, &sx, &sy);
      pts.push_back(Gdiplus::PointF((Gdiplus::REAL)sx, (Gdiplus::REAL)sy));
    }
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(g_colLine), GetGValue(g_colLine), GetBValue(g_colLine)), (Gdiplus::REAL)max(1.0, g_lineW * t->scale));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLines(&pen, pts.data(), (int)pts.size());
  }
}


// ------------------------------------------------------------------ test hooks (scripted mouse)
static void TestMouse(UINT msg, WPARAM wp, int page, float x, float y) {
  Tab* t = CurTab();
  ViewEnsureLayout(t);
  int sx, sy;
  ViewPointToScreen(t, page, x, y, &sx, &sy);
  SendMessageW(A_.view, msg, wp, MAKELPARAM(sx, sy));
}
int EdUiTest(const std::string& line) {
  std::istringstream ss(line);
  std::string c;
  ss >> c;
  Tab* t = CurTab();
  if (c == "mode") { int m; ss >> m; EdSetMode(m); return 1; }
  if (c == "btn") { int id; ss >> id; EdCommand(id); return 1; }
  if (c == "tool") {
    std::string n; ss >> n;
    static const char* names[] = {"select", "hilite", "under", "strike", "squig", "text", "ink", "rect", "ellipse"};
    for (int i = 0; i < 9; i++) if (n == names[i]) { EdCommand(ID_ED_T_SELECT + i); return 1; }
    return 0;
  }
  if (!t || !t->doc) return -1;
  if (c == "mdown" || c == "mmove" || c == "mup") {
    int p; float x, y; ss >> p >> x >> y;
    TestMouse(c == "mdown" ? WM_LBUTTONDOWN : c == "mup" ? WM_LBUTTONUP : WM_MOUSEMOVE, c == "mup" ? 0 : MK_LBUTTON, p, x, y);
    return 1;
  }
  if (c == "click") { int p; float x, y; ss >> p >> x >> y; TestMouse(WM_LBUTTONDOWN, MK_LBUTTON, p, x, y); TestMouse(WM_LBUTTONUP, 0, p, x, y); return 1; }
  if (c == "drag") {
    int p; float x0, y0, x1, y1; ss >> p >> x0 >> y0 >> x1 >> y1;
    TestMouse(WM_LBUTTONDOWN, MK_LBUTTON, p, x0, y0);
    for (int i = 1; i <= 8; i++) TestMouse(WM_MOUSEMOVE, MK_LBUTTON, p, x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8);
    TestMouse(WM_LBUTTONUP, 0, p, x1, y1);
    return 1;
  }
  if (c == "textbox") {            // textbox <page> <x> <y> <text with \n>
    int p; float x, y; ss >> p >> x >> y;
    std::string r; std::getline(ss, r);
    if (!r.empty() && r[0] == ' ') r.erase(0, 1);
    for (size_t i = 0; i + 1 < r.size(); i++) if (r[i] == '\\' && r[i + 1] == 'n') r.replace(i, 2, "\r\n");
    EdCommand(ID_ED_T_TEXT);
    TestMouse(WM_LBUTTONDOWN, MK_LBUTTON, p, x, y);
    TestMouse(WM_LBUTTONUP, 0, p, x, y);
    if (!g_te) return 0;
    SetWindowTextW(g_te, Utf8ToW(r).c_str());
    TextBoxResize();
    return 1;
  }
  if (c == "tbinfo") {
    RECT r = {0, 0, 0, 0};
    if (g_te) GetWindowRect(g_te, &r);
    FILE* f = _wfopen(L"Z:\\tmp\\t\\dump.txt", L"ab");
    if (f) { fprintf(f, "tb te=%p vis=%d rect=%ld,%ld,%ld,%ld focus=%d\n", (void*)g_te, g_te ? IsWindowVisible(g_te) : 0, r.left, r.top, r.right, r.bottom, GetFocus() == g_te); fclose(f); }
    return 1;
  }
  if (c == "textcommit") { bool had = g_te != nullptr; TextBoxClose(true); return had; }
  if (c == "setcolor") {
    int i; ss >> i;
    if (i < 0 || i >= (int)(sizeof(kPal) / sizeof(kPal[0]))) return 0;
    *ActiveColor() = kPal[i].c;
    ApplyColorToSel(kPal[i].c);
    return 1;
  }
  if (c == "picture") { std::string f; std::getline(ss, f); if (!f.empty() && f[0] == ' ') f.erase(0, 1); InsertPicture(Utf8ToW(f)); return g_sel.kind != 0; }
  if (c == "setsize") { int v; ss >> v; if (SizeIsFont()) g_fontSize = v; else g_lineW = v; return 1; }
  if (c == "selinfo") {
    char b[200];
    snprintf(b, sizeof b, "sel kind=%d page=%d grp=%d annot=%d sub=%d box=%.1f,%.1f,%.1f,%.1f tool=%d", g_sel.kind, g_sel.page, g_sel.grp, g_sel.annot, g_sel.sub, g_selBox.l, g_selBox.t, g_selBox.r, g_selBox.b, g_tool);
    FILE* f = _wfopen(L"Z:\\tmp\\t\\dump.txt", L"ab");
    if (f) { fprintf(f, "%s\n", b); fclose(f); }
    return g_sel.kind != 0 ? 1 : 0;
  }
  if (c == "dlgexport") {
    auto* f = new std::function<void()>([]() { ExportOpts o; o.preset = 1; ExportOptionsDialog(A_.main, &o, 12); });
    PostMessageW(A_.main, WM_APP_TESTCMD, 1, (LPARAM)f);
    return 1;
  }
  if (c == "pageasync") {          // opens the dialog without blocking the test thread
    std::string n; ss >> n;
    int cmd = n == "split" ? ID_ED_PG_SPLIT : n == "delrange" ? ID_ED_PG_DELRANGE : 0;
    if (!cmd) return 0;
    auto* f = new std::function<void()>([cmd]() { PageCommand(cmd); });
    PostMessageW(A_.main, WM_APP_TESTCMD, 1, (LPARAM)f);
    return 1;
  }
  if (c == "page") { std::string n; ss >> n; int cmd = n == "rotcw" ? ID_ED_PG_ROTCW : n == "del" ? ID_ED_PG_DEL : n == "up" ? ID_ED_PG_UP : n == "down" ? ID_ED_PG_DOWN : n == "blank" ? ID_ED_PG_BLANK : 0; if (!cmd) return 0; PageCommand(cmd); return 1; }
  return -1;
}

#endif
