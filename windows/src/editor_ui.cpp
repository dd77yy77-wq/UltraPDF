// UltraPDF Editor: tool bar, tools, selection with handles, text box, menus.
#include "editor.h"
#ifdef APP_EDITOR
#include <commctrl.h>
#include <uxtheme.h>
#include <sstream>

// ------------------------------------------------------------------ state
enum Tool { ET_SELECT, ET_HILITE, ET_UNDER, ET_STRIKE, ET_SQUIG, ET_TEXT, ET_INK, ET_RECT, ET_ELLIPSE };
enum Gest { G_NONE, G_MOVE, G_RESIZE, G_SHAPE, G_INK, G_MARQUEE, G_ROTATE };

static int g_tool = ET_SELECT;
static COLORREF g_colHi = RGB(255, 235, 59), g_colLine = RGB(229, 57, 53), g_colText = RGB(0, 0, 0);
static int g_lineW = 2, g_fontSize = 14, g_fontStyle = 0;
static ws g_font = L"Arial";

// the selection: items of one page (editor objects, existing page objects, shapes and drawings)
static ESet g_set;
static std::vector<FRect> g_itemBox;
static FRect g_selBox;
static int g_selKind = 0;           // EK_* when all selected items are page objects of one kind
static bool g_selText = false;      // everything selected is text
static bool g_selHasText = false;   // there is some text in the selection
static bool g_selObjs = false;      // there are page objects (not only shapes / drawings)
static bool g_selAnnots = false;    // there are shapes / drawings / markup
static int g_selAngle = 0;
static HBITMAP g_preview = nullptr; static int g_prevW = 0, g_prevH = 0;

static Gest g_g = G_NONE;
static int g_gPage = 0, g_gHandle = -1;
static FRect g_gBox0, g_gBox;
static float g_gx0 = 0, g_gy0 = 0;
static POINT g_gDown = {0, 0};
static bool g_gMoved = false, g_gCopy = false, g_gAdd = false;
static double g_rotDeg = 0;
static std::vector<PointF> g_ink;
static ESet g_ctxSet;
static int g_clipTab = -1, g_clipPage = -1, g_pasteN = 0;
static int g_testMods = -1;         // scripted tests: bit 0 Shift, bit 1 Ctrl, bit 2 Alt
static bool ModDown(int vk) {
  if (g_testMods >= 0) return (vk == VK_SHIFT && (g_testMods & 1)) || (vk == VK_CONTROL && (g_testMods & 2)) || (vk == VK_MENU && (g_testMods & 4));
  return GetKeyState(vk) < 0;
}

struct PalEnt { const wchar_t* name; COLORREF c; };
static const PalEnt kPal[] = {
    {L"Чёрный", RGB(0, 0, 0)}, {L"Красный", RGB(229, 57, 53)}, {L"Синий", RGB(30, 90, 200)}, {L"Зелёный", RGB(46, 160, 67)},
    {L"Оранжевый", RGB(251, 140, 0)}, {L"Фиолетовый", RGB(142, 68, 173)}, {L"Жёлтый", RGB(255, 235, 59)}, {L"Бирюзовый", RGB(0, 172, 193)},
    {L"Розовый", RGB(255, 128, 171)}, {L"Серый", RGB(120, 120, 120)}, {L"Тёмно-синий", RGB(20, 40, 120)}, {L"Белый", RGB(255, 255, 255)}};
static const int kLineW[] = {1, 2, 3, 5, 8};
static const int kFontSz[] = {8, 10, 12, 14, 16, 18, 24, 36, 48};

static bool IsMarkupTool(int t) { return t >= ET_HILITE && t <= ET_SQUIG; }
static const ESel* One() { return g_set.size() == 1 ? &g_set[0] : nullptr; }
static int SelPage() { return g_set.empty() ? -1 : g_set[0].page; }

// the colour the colour button edits right now
static COLORREF* ActiveColor() {
  if (g_selText) return &g_colText;
  const ESel* o = One();
  if (o && o->kind == 2 && o->sub == FPDF_ANNOT_HIGHLIGHT) return &g_colHi;
  if (g_set.empty()) {
    if (g_tool == ET_HILITE) return &g_colHi;
    if (g_tool == ET_TEXT) return &g_colText;
  }
  return &g_colLine;
}
static bool SizeIsFont() {
  if (g_selText) return true;
  return g_set.empty() && g_tool == ET_TEXT;
}

static void Redraw() { ViewInvalidate(); InvalidateRect(A_.main, nullptr, FALSE); }

static void FreePreview() { if (g_preview) { DeleteObject(g_preview); g_preview = nullptr; } }
static void ClearSel() {
  g_set.clear(); g_itemBox.clear();
  g_selKind = 0; g_selText = g_selHasText = g_selObjs = g_selAnnots = false; g_selAngle = 0;
  FreePreview();
}
static void SetSet(const ESet& s) {
  Tab* t = CurTab();
  ClearSel();
  if (!t || !t->doc) return;
  ESet keep;
  for (auto& it : s) {
    FRect b;
    if (EdSelBox(t, it, &b)) { keep.push_back(it); g_itemBox.push_back(b); }
  }
  if (keep.empty()) { g_itemBox.clear(); return; }
  g_set = keep;
  g_selBox = g_itemBox[0];
  for (auto& b : g_itemBox) {
    g_selBox.l = min(g_selBox.l, b.l); g_selBox.t = min(g_selBox.t, b.t);
    g_selBox.r = max(g_selBox.r, b.r); g_selBox.b = max(g_selBox.b, b.b);
  }
  int common = -1, texts = 0;
  for (auto& it : g_set) {
    if (it.kind == 2) { g_selAnnots = true; common = 0; continue; }
    g_selObjs = true;
    int k = EdSelObjKind(t, it);
    if (k == EK_TEXT) texts++;
    if (common == -1) common = k; else if (common != k) common = 0;
  }
  g_selKind = common < 0 ? 0 : common;
  g_selHasText = texts > 0;
  g_selText = texts == (int)g_set.size();
  if (g_selObjs) g_selAngle = EdSetAngle(t, g_set);
}
static void SetSel(const ESel& s) { ESet v{s}; SetSet(v); }
static void RefreshSel() {
  if (g_set.empty()) return;
  ESet keep = g_set;
  SetSet(keep);
}
static bool SameItem(const ESel& a, const ESel& b) {
  if (a.page != b.page || a.kind != b.kind) return false;
  if (a.kind == 1) return a.grp == b.grp;
  if (a.kind == 2) return a.annot == b.annot;
  return a.obj == b.obj;
}
static bool InSet(const ESel& s) {
  for (auto& it : g_set) if (SameItem(it, s)) return true;
  return false;
}

// ------------------------------------------------------------------ text box
static HWND g_te = nullptr;
static HFONT g_teFont = nullptr;
static int g_tbTab = 0, g_tbPage = 0;
static float g_tbX = 0, g_tbY = 0;
static bool g_tbEdit = false;                       // editing text that exists already
static ESet g_tbSet;
static ws g_tbOrig, g_tbFamily = L"Arial";
static int g_tbStyle = 0, g_tbSize = 14;
static COLORREF g_tbColor = 0;

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
  bool edit = g_tbEdit;
  g_tbEdit = false;
  if (commit && t && t->doc) {
    bool any = false;
    for (wchar_t c : text) if (c != L' ' && c != L'\r' && c != L'\n' && c != L'\t') any = true;
    if (edit) {
      if (text != g_tbOrig) {
        ESet s = g_tbSet;
        ClearSel();
        if (EdReplaceText(t, s, text, g_tbFamily, g_tbStyle, g_tbSize, g_tbColor)) SetSet(s);
      }
    } else if (any) {
      ESel s;
      if (EdAddText(t, g_tbPage, g_tbX, g_tbY, text, g_font, g_fontSize, g_colText, &s, g_fontStyle)) { g_tool = ET_SELECT; SetSel(s); }
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
      if (wp == VK_RETURN && ModDown(VK_CONTROL)) { TextBoxClose(true); return 0; }
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

// a text box at (x, y) of the page: new text, or (editing) the text that is there now
static void TextBoxOpenAt(Tab* t, int page, float x, float y, const ws& init, const ws& family, int style, int size, COLORREF col, const ESet* editing, float minW) {
  if (g_te) TextBoxClose(true);
  g_tbTab = t->id; g_tbPage = page; g_tbX = x; g_tbY = y;
  g_tbEdit = editing != nullptr;
  if (editing) g_tbSet = *editing;
  g_tbOrig = init; g_tbFamily = family; g_tbStyle = style; g_tbSize = size; g_tbColor = col;
  int sx, sy;
  ViewPointToScreen(t, page, x, y, &sx, &sy);
  int px = max(9, (int)lround(size * t->scale));
  g_teFont = CreateFontW(-px, 0, 0, 0, (style & 1) ? FW_BOLD : FW_NORMAL, (style & 2) ? 1 : 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family.c_str());
  // a pop-up owned by the main window: the page view may present through a swap chain that would hide a child window
  POINT sp = {sx - 3, sy - 2};
  ClientToScreen(A_.view, &sp);
  int wmin = max(S(120), (int)lround(minW * t->scale) + S(26));
  // created wide, so that the text is not wrapped while it is put in; TextBoxResize then fits the box to the text
  g_te = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"", WS_POPUP | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                         sp.x, sp.y, S(1400), px + S(10), A_.main, nullptr, A_.inst, nullptr);
  SendMessageW(g_te, WM_SETFONT, (WPARAM)g_teFont, TRUE);
  if (!init.empty()) SetWindowTextW(g_te, init.c_str());
  SetWindowSubclass(g_te, TextSub, 77, 0);
  SetFocus(g_te);
  SendMessageW(g_te, EM_SETSEL, (WPARAM)init.size(), (LPARAM)init.size());
  TextBoxResize();
  RECT cur;
  GetWindowRect(g_te, &cur);
  if (cur.right - cur.left < wmin) SetWindowPos(g_te, nullptr, 0, 0, wmin, cur.bottom - cur.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void TextBoxOpen(Tab* t, int page, float x, float y) {
  TextBoxOpenAt(t, page, x, y, L"", g_font, g_fontStyle, g_fontSize, g_colText, nullptr, 0);
}
// opens the text under the selection for editing
static bool StartTextEdit() {
  Tab* t = CurTab();
  if (!t || !t->doc || g_set.empty() || !g_selHasText) return false;
  EdTextInfo ti;
  if (!EdGetText(t, g_set, &ti)) return false;
  ESet s = g_set;
  int page = SelPage();
  TextBoxOpenAt(t, page, ti.box.l, ti.box.t, ti.text, ti.family, ti.style, ti.size, ti.color, &s, ti.box.r - ti.box.l);
  Redraw();
  return true;
}

LRESULT EdCtlColor(HDC dc, HWND ctl) {
  static HBRUSH br = nullptr;
  if (!br) br = CreateSolidBrush(RGB(255, 255, 255));
  if (ctl == g_te) {
    COLORREF c = g_tbColor;
    SetTextColor(dc, c == RGB(255, 255, 255) ? RGB(0, 0, 0) : c);
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
  AddB(x, y, ID_ED_OBJECT, IC_OBJECT, L"Объект: копировать, группировать, выровнять, повернуть, прозрачность");
  AddB(x, y, ID_ED_EDITTEXT, IC_TXTEDIT, L"Править текст (F2 или двойной щелчок)");
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
    case ID_ED_FRONT: case ID_ED_FWD: case ID_ED_BWD: case ID_ED_BACK: *en = g_selObjs; break;
    case ID_ED_DELETE: *en = !g_set.empty(); break;
    case ID_ED_EDITTEXT: *en = g_selHasText; break;
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
      if (g_selText && !g_set.empty()) { int s = EdSelTextSize(CurTab(), g_set[0]); if (s > 0) lab = FmtT(L"%d", s); }
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
  if (!t || g_set.empty()) return;
  ESet s = g_set;
  if (EdSetColorSet(t, s, c)) SetSet(s);
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

// style and face of the text that the size / font menu works on
static void CurrentFace(ws* family, int* style) {
  *family = g_font; *style = g_fontStyle;
  Tab* t = CurTab();
  EdTextInfo ti;
  if (g_selText && t && EdGetText(t, g_set, &ti)) { *family = ti.family; *style = ti.style; }
}
static void ApplyFace(const ws& family, int style) {
  Tab* t = CurTab();
  if (g_selText && t) {
    ESet s = g_set;
    ClearSel();
    if (EdSetTextStyle(t, s, family, style)) SetSet(s);
    else SetSet(s);
  } else { g_font = family; g_fontStyle = style; }
  Redraw();
}

static void ShowSizeMenu() {
  HMENU m = CreatePopupMenu();
  bool font = SizeIsFont();
  HMENU fm = nullptr;
  ws curFam;
  int curStyle = 0;
  CurrentFace(&curFam, &curStyle);
  int curSize = g_fontSize;
  if (g_selText && !g_set.empty()) { int s = EdSelTextSize(CurTab(), g_set[0]); if (s > 0) curSize = s; }
  if (font) {
    for (int i = 0; i < (int)(sizeof(kFontSz) / sizeof(kFontSz[0])); i++)
      AppendMenuW(m, MF_STRING | (kFontSz[i] == curSize ? MF_CHECKED : 0), ID_ED_MENU_SIZE + i, FmtT(L"%d pt", kFontSz[i]).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    fm = CreatePopupMenu();
    std::vector<ws> fonts = EdAvailableFonts();
    for (size_t i = 0; i < fonts.size() && i < 8; i++)
      AppendMenuW(fm, MF_STRING | (fonts[i] == curFam ? MF_CHECKED : 0), ID_ED_MENU_FONT + (UINT)i, fonts[i].c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fm, L"Шрифт");
    AppendMenuW(m, MF_STRING | ((curStyle & 1) ? MF_CHECKED : 0), ID_ED_OB_BOLD, L"Жирный\tCtrl+B");
    AppendMenuW(m, MF_STRING | ((curStyle & 2) ? MF_CHECKED : 0), ID_ED_OB_ITALIC, L"Курсив\tCtrl+I");
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
      if (t && g_selText) { ESet s = g_set; if (EdSetTextSizeSet(t, s, g_fontSize)) SetSet(s); }
    } else g_lineW = kLineW[i];
    Redraw();
  } else if (cmd >= ID_ED_MENU_FONT && cmd < ID_ED_MENU_FONT + 8) {
    std::vector<ws> fonts = EdAvailableFonts();
    int i = cmd - ID_ED_MENU_FONT;
    if (i < (int)fonts.size()) ApplyFace(fonts[i], curStyle);
  } else if (cmd == ID_ED_OB_BOLD) ApplyFace(curFam, curStyle ^ 1);
  else if (cmd == ID_ED_OB_ITALIC) ApplyFace(curFam, curStyle ^ 2);
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

// fit all pages to a paper size
static bool g_fitEnlarge = true;
static int g_fitMargin = 0;                         // mm
struct FitItem { const wchar_t* name; int paper, orient; };
static const FitItem kFit[] = {
    {L"A4 (книжная или альбомная — как у страницы)", 0, 0}, {L"A4 книжная", 0, 1}, {L"A4 альбомная", 0, 2},
    {L"A3 (как у страницы)", 1, 0}, {L"A5 (как у страницы)", 2, 0}, {L"Letter (как у страницы)", 3, 0}, {L"Legal (как у страницы)", 4, 0}};

static void PageCommand(int cmd) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  int cur = ViewCurPage(), n = t->doc->npages;
  if (cmd >= ID_ED_PG_FIT && cmd < ID_ED_PG_FIT + (int)(sizeof(kFit) / sizeof(kFit[0]))) {
    FitOpts fo;
    fo.paper = kFit[cmd - ID_ED_PG_FIT].paper; fo.orient = kFit[cmd - ID_ED_PG_FIT].orient;
    fo.marginMm = (float)g_fitMargin; fo.enlarge = g_fitEnlarge;
    int changed = 0;
    if (EdFitPages(t, fo, &changed)) ShowToast(FmtT(L"Приведено страниц: %d", changed));
    return;
  }
  if (cmd == ID_ED_PG_FITOPT) { g_fitEnlarge = !g_fitEnlarge; return; }
  if (cmd == ID_ED_PG_FITMARGIN) { g_fitMargin = g_fitMargin ? 0 : 10; return; }
  if (cmd == ID_ED_PG_FITSAVE) { EdSaveAs(t, true); return; }
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
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  HMENU fm = CreatePopupMenu();
  for (int i = 0; i < (int)(sizeof(kFit) / sizeof(kFit[0])); i++) AppendMenuW(fm, MF_STRING, ID_ED_PG_FIT + i, kFit[i].name);
  AppendMenuW(fm, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(fm, MF_STRING | (g_fitEnlarge ? MF_CHECKED : 0), ID_ED_PG_FITOPT, L"Увеличивать маленькие страницы");
  AppendMenuW(fm, MF_STRING | (g_fitMargin ? MF_CHECKED : 0), ID_ED_PG_FITMARGIN, L"Поля 10 мм");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)fm, L"Привести все страницы к одному формату");
  AppendMenuW(m, MF_STRING, ID_ED_PG_FITSAVE, L"Сохранить копию: все страницы A4 с нужным разрешением…");
  int cmd = PopupBelow(m, ID_ED_PAGES);
  DestroyMenu(m);
  if (cmd) PageCommand(cmd);
}

// ------------------------------------------------------------------ objects: copy, paste, group, align, turn, transparency
static bool PngClsid(CLSID* c) {
  UINT n = 0, sz = 0;
  Gdiplus::GetImageEncodersSize(&n, &sz);
  if (!sz) return false;
  std::vector<char> buf(sz);
  auto* ie = (Gdiplus::ImageCodecInfo*)buf.data();
  Gdiplus::GetImageEncoders(n, sz, ie);
  for (UINT i = 0; i < n; i++) if (wcscmp(ie[i].MimeType, L"image/png") == 0) { *c = ie[i].Clsid; return true; }
  return false;
}
static bool ClipboardHasImage() { return IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB); }
// a picture from the system clipboard (a screenshot, a copy from another program) goes onto the page like a stamp
static void PasteSystemImage() {
  if (!ClipboardHasImage()) { ShowToast(L"В буфере обмена нет объектов для вставки"); return; }
  if (!OpenClipboard(A_.main)) return;
  ws file;
  HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP);
  if (hb) {
    Gdiplus::Bitmap bmp(hb, nullptr);
    CLSID png;
    wchar_t tmp[MAX_PATH];
    if (bmp.GetLastStatus() == Gdiplus::Ok && PngClsid(&png) && GetTempPathW(MAX_PATH, tmp)) {
      ws f = ws(tmp) + L"UltraPDF-paste.png";
      if (bmp.Save(f.c_str(), &png, nullptr) == Gdiplus::Ok) file = f;
    }
  }
  CloseClipboard();
  if (!file.empty()) InsertPicture(file);
}

static bool DoCopy(bool cut, bool quiet = false) {
  Tab* t = CurTab();
  if (!t || !t->doc || g_set.empty()) return false;
  if (!EdCopy(t, g_set)) return false;
  g_clipTab = t->id; g_clipPage = SelPage(); g_pasteN = 0;
  if (cut) { ESet s = g_set; ClearSel(); EdDeleteSet(t, s); }
  else if (!quiet) ShowToast(L"Скопировано");
  return true;
}
static void DoPaste(bool duplicate) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  if (!EdClipHas()) { PasteSystemImage(); return; }
  int page = !g_set.empty() ? SelPage() : ViewCurPage();
  page = min(max(page, 0), t->doc->npages - 1);
  float d = 0;
  if (duplicate || (t->id == g_clipTab && page == g_clipPage)) d = 12.0f * (float)(++g_pasteN);
  ESet res;
  ClearSel();
  if (EdPaste(t, page, d, d, &res)) { g_tool = ET_SELECT; SetSet(res); }
}
static void DoDuplicate() {
  if (g_set.empty()) return;
  int page = SelPage();
  if (!DoCopy(false, true)) return;
  Tab* t = CurTab();
  (void)page;
  g_pasteN = 0;
  float d = 12.0f * (float)(++g_pasteN);
  ESet res;
  ClearSel();
  if (EdPaste(t, g_clipPage, d, d, &res)) SetSet(res);
}
static void SelectAllOnPage() {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  int page = !g_set.empty() ? SelPage() : ViewCurPage();
  page = min(max(page, 0), t->doc->npages - 1);
  ESet res;
  if (EdMarquee(t, page, FRect{-1, -1, t->doc->pw[page] + 1, t->doc->ph[page] + 1}, &res)) SetSet(res);
  else ClearSel();
}
static void RotateSel(double deg) {
  Tab* t = CurTab();
  if (!t || g_set.empty()) return;
  ESet s = g_set;
  if (EdRotateSet(t, s, deg)) SetSet(s);
}

static void ObjCommand(int id) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  switch (id) {
    case ID_ED_OB_COPY: DoCopy(false); break;
    case ID_ED_OB_CUT: DoCopy(true); break;
    case ID_ED_OB_PASTE: DoPaste(false); break;
    case ID_ED_OB_DUP: DoDuplicate(); break;
    case ID_ED_OB_SELALL: SelectAllOnPage(); break;
    case ID_ED_OB_EDITTEXT: StartTextEdit(); break;
    case ID_ED_OB_GROUP: { ESet s = g_set; if (EdGroup(t, s)) SetSet(s); break; }
    case ID_ED_OB_UNGROUP: { ESet s = g_set; if (EdUngroup(t, s)) SetSet(s); break; }
    case ID_ED_OB_ROT_CW: RotateSel(90); break;
    case ID_ED_OB_ROT_CCW: RotateSel(-90); break;
    case ID_ED_OB_ROT_180: RotateSel(180); break;
    case ID_ED_OB_ROT_ANY: {
      ws in;
      if (!InputText(A_.main, L"Повернуть", L"На сколько градусов повернуть по часовой стрелке? Например 15 или -30.", L"15", &in)) break;
      double v = wcstod(in.c_str(), nullptr);
      if (fabs(v) > 0.01) RotateSel(v);
      break;
    }
    case ID_ED_OB_BOLD: case ID_ED_OB_ITALIC: {
      if (!g_selText) break;
      ws fam; int st;
      CurrentFace(&fam, &st);
      ApplyFace(fam, st ^ (id == ID_ED_OB_BOLD ? 1 : 2));
      break;
    }
    default:
      if (id >= ID_ED_OB_AL_LEFT && id <= ID_ED_OB_DIST_V) {
        ESet s = g_set;
        if (EdAlign(t, s, id - ID_ED_OB_AL_LEFT)) SetSet(s);
        else if (id >= ID_ED_OB_DIST_V - 1 && s.size() < 3) ShowToast(L"Для распределения нужно выделить не меньше трёх объектов");
      } else if (id >= ID_ED_OB_OPAC && id <= ID_ED_OB_OPAC + 4) {
        static const int pct[] = {100, 75, 50, 25};
        int v;
        if (id - ID_ED_OB_OPAC < 4) v = pct[id - ID_ED_OB_OPAC];
        else {
          ws in;
          if (!InputText(A_.main, L"Прозрачность", L"Непрозрачность в процентах: 100 — обычный цвет, 50 — наполовину прозрачный, 0 — невидимый.", std::to_wstring(EdSetOpacityOf(t, g_set)), &in)) break;
          v = _wtoi(in.c_str());
        }
        ESet s = g_set;
        if (EdSetOpacity(t, s, v)) SetSet(s);
      }
  }
}

static void ShowObjectMenu() {
  Tab* t = CurTab();
  bool has = !g_set.empty(), multi = g_set.size() >= 2;
  auto en = [](bool b) { return (UINT)(b ? 0 : MF_GRAYED); };
  HMENU m = CreatePopupMenu(), al = CreatePopupMenu(), ro = CreatePopupMenu(), op = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | en(g_selHasText), ID_ED_OB_EDITTEXT, L"Править текст\tF2");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | en(has), ID_ED_OB_COPY, L"Копировать\tCtrl+C");
  AppendMenuW(m, MF_STRING | en(has), ID_ED_OB_CUT, L"Вырезать\tCtrl+X");
  AppendMenuW(m, MF_STRING | en(EdClipHas() || ClipboardHasImage()), ID_ED_OB_PASTE, L"Вставить\tCtrl+V");
  AppendMenuW(m, MF_STRING | en(has), ID_ED_OB_DUP, L"Дублировать\tCtrl+D");
  AppendMenuW(m, MF_STRING, ID_ED_OB_SELALL, L"Выделить всё на странице\tCtrl+A");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | en(multi && !g_selAnnots), ID_ED_OB_GROUP, L"Сгруппировать\tCtrl+G");
  AppendMenuW(m, MF_STRING | en(has), ID_ED_OB_UNGROUP, L"Разгруппировать\tCtrl+Shift+G");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  static const wchar_t* alNames[] = {L"По левому краю", L"По центру", L"По правому краю", L"По верхнему краю", L"По середине", L"По нижнему краю",
                                     L"Распределить по горизонтали", L"Распределить по вертикали"};
  for (int i = 0; i < 8; i++) {
    if (i == 6) AppendMenuW(al, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(al, MF_STRING | en(has && (i < 6 || g_set.size() >= 3)), ID_ED_OB_AL_LEFT + i, alNames[i]);
  }
  AppendMenuW(m, MF_POPUP | en(has), (UINT_PTR)al, g_set.size() == 1 ? L"Выровнять по странице" : L"Выровнять");
  AppendMenuW(ro, MF_STRING, ID_ED_OB_ROT_CW, L"На 90° по часовой");
  AppendMenuW(ro, MF_STRING, ID_ED_OB_ROT_CCW, L"На 90° против часовой");
  AppendMenuW(ro, MF_STRING, ID_ED_OB_ROT_180, L"На 180°");
  AppendMenuW(ro, MF_STRING, ID_ED_OB_ROT_ANY, L"На угол…");
  AppendMenuW(m, MF_POPUP | en(g_selObjs), (UINT_PTR)ro, L"Повернуть");
  int cur = g_selObjs && t ? EdSetOpacityOf(t, g_set) : 100;
  static const int pct[] = {100, 75, 50, 25};
  for (int i = 0; i < 4; i++) AppendMenuW(op, MF_STRING | (cur == pct[i] ? MF_CHECKED : 0), ID_ED_OB_OPAC + i, FmtT(L"%d %%", pct[i]).c_str());
  AppendMenuW(op, MF_STRING, ID_ED_OB_OPAC + 4, L"Другая…");
  AppendMenuW(m, MF_POPUP | en(g_selObjs), (UINT_PTR)op, L"Прозрачность");
  int cmd = PopupBelow(m, ID_ED_OBJECT);
  DestroyMenu(m);
  if (cmd) ObjCommand(cmd);
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
    case ID_ED_OBJECT: ShowObjectMenu(); break;
    case ID_ED_EDITTEXT: StartTextEdit(); break;
    case ID_ED_FRONT: case ID_ED_FWD: case ID_ED_BWD: case ID_ED_BACK:
      if (g_selObjs) {
        ESet s = g_set;
        int op = id == ID_ED_FRONT ? 0 : id == ID_ED_FWD ? 1 : id == ID_ED_BWD ? 2 : 3;
        EdZOrderSet(t, s, op);
        SetSet(s);
      }
      break;
    case ID_ED_DELETE: case ID_ED_CTX_DELETE: {
      ESet s = id == ID_ED_CTX_DELETE ? g_ctxSet : g_set;
      if (!s.empty()) { ClearSel(); EdDeleteSet(t, s); }
      break;
    }
    default:
      if (id >= ID_ED_OB_COPY && id <= ID_ED_OB_ITALIC) ObjCommand(id);
      else if (id >= ID_ED_PG_FIT && id <= ID_ED_PG_FITSAVE) PageCommand(id);
      break;
  }
  Redraw();
}

bool EdKey(UINT vk, bool ctrl, bool shift) {
  HWND f = GetFocus();
  if (f == g_te || f == A_.pageEdit || f == A_.findEdit) return false;
  Tab* t = CurTab();
  if (!t || !t->doc) return false;
  bool sel = EdOn() && g_tool == ET_SELECT;
  if (ctrl) {
    switch (vk) {
      case 'S': EdCommand(shift ? ID_ED_SAVEAS_E : ID_ED_SAVE); return true;
      case 'Z': if (!EdOn()) return false; EdCommand(shift ? ID_ED_REDO : ID_ED_UNDO); return true;
      case 'Y': if (!EdOn()) return false; EdCommand(ID_ED_REDO); return true;
      case '1': case '2': case '3': EdSetMode(vk - '1'); return true;
      case 'C': if (sel && !g_set.empty()) { ObjCommand(ID_ED_OB_COPY); Redraw(); return true; } return false;
      case 'X': if (sel && !g_set.empty()) { ObjCommand(ID_ED_OB_CUT); Redraw(); return true; } return false;
      case 'V': if (sel && (EdClipHas() || ClipboardHasImage())) { ObjCommand(ID_ED_OB_PASTE); Redraw(); return true; } return false;
      case 'D': if (sel && !g_set.empty()) { ObjCommand(ID_ED_OB_DUP); Redraw(); return true; } return false;
      case 'A': if (sel) { ObjCommand(ID_ED_OB_SELALL); Redraw(); return true; } return false;
      case 'G': if (sel && !g_set.empty()) { ObjCommand(shift ? ID_ED_OB_UNGROUP : ID_ED_OB_GROUP); Redraw(); return true; } return false;
      case 'B': case 'I': if (sel && g_selText) { ObjCommand(vk == 'B' ? ID_ED_OB_BOLD : ID_ED_OB_ITALIC); Redraw(); return true; } return false;
    }
    return false;
  }
  if (!EdOn()) return false;
  switch (vk) {
    case VK_F2: case VK_RETURN: if (g_selHasText && g_tool == ET_SELECT) return StartTextEdit(); return false;
    case VK_DELETE: if (!g_set.empty()) { EdCommand(ID_ED_DELETE); return true; } return false;
    case VK_ESCAPE:
      if (g_g != G_NONE) { g_g = G_NONE; g_ink.clear(); if (GetCapture() == A_.view) ReleaseCapture(); Redraw(); return true; }
      if (!g_set.empty()) { ClearSel(); Redraw(); return true; }
      if (g_tool != ET_SELECT) { SetTool(ET_SELECT); return true; }
      return false;
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
      if (!g_set.empty() && g_tool == ET_SELECT) {
        float d = shift ? 10.0f : 1.0f;
        float dx = vk == VK_LEFT ? -d : vk == VK_RIGHT ? d : 0, dy = vk == VK_UP ? -d : vk == VK_DOWN ? d : 0;
        FRect b = g_selBox, nb = {b.l + dx, b.t + dy, b.r + dx, b.b + dy};
        ESet s = g_set;
        if (EdTransformSet(t, s, b, nb)) SetSet(s);
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
static bool CanRotate() { return g_selObjs && !g_selAnnots; }
static RECT RotHandleRect(const RECT& box) {
  int cx = (box.left + box.right) / 2, y = box.top - S(26), r = S(6);
  return RECT{cx - r, y - r, cx + r + 1, y + r + 1};
}
static bool IsMarkupSub(int sub) { return sub == FPDF_ANNOT_HIGHLIGHT || sub == FPDF_ANNOT_UNDERLINE || sub == FPDF_ANNOT_STRIKEOUT || sub == FPDF_ANNOT_SQUIGGLY; }
// resizable handles for the current selection
static bool HandleUsable(int h) {
  if (g_set.empty()) return false;
  bool corner = h == 0 || h == 2 || h == 4 || h == 6;
  bool onlyMarkup = true;
  for (auto& it : g_set) if (!(it.kind == 2 && IsMarkupSub(it.sub))) onlyMarkup = false;
  if (onlyMarkup) return false;
  if (g_selHasText || g_selAngle != 0 || g_set.size() > 1) return corner;      // text scales evenly; a turned picture keeps its shape
  const ESel* o = One();
  if (o && o->kind == 2 && o->sub == FPDF_ANNOT_INK) return corner;
  return true;
}
static int HandleAt(Tab* t, int mx, int my) {
  if (g_set.empty() || SelPage() < 0) return -1;
  RECT box = ViewRectToScreen(t, SelPage(), g_selBox);
  POINT pt = {mx, my};
  for (int h = 0; h < 8; h++) {
    if (!HandleUsable(h)) continue;
    RECT r = HandleRect(box, h);
    InflateRect(&r, S(3), S(3));
    if (PtInRect(&r, pt)) return h;
  }
  return -1;
}
static bool RotHandleAt(Tab* t, int mx, int my) {
  if (!CanRotate() || SelPage() < 0) return false;
  RECT box = ViewRectToScreen(t, SelPage(), g_selBox);
  RECT r = RotHandleRect(box);
  InflateRect(&r, S(3), S(3));
  POINT pt = {mx, my};
  return PtInRect(&r, pt) != 0;
}
static bool InSelBox(Tab* t, int mx, int my) {
  if (g_set.empty()) return false;
  RECT box = ViewRectToScreen(t, SelPage(), g_selBox);
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

static void LoadPreview(Tab* t) {
  FreePreview();
  if (g_set.size() == 1 && g_selKind == EK_IMAGE && g_selAngle == 0) g_preview = EdImagePreview(t, g_set, &g_prevW, &g_prevH);
}
static void BeginMove(Tab* t, int mx, int my) {
  g_g = G_MOVE; g_gPage = SelPage(); g_gBox0 = g_selBox; g_gBox = g_selBox;
  g_gCopy = ModDown(VK_CONTROL);
  PtrPage(t, g_gPage, mx, my, &g_gx0, &g_gy0);
  LoadPreview(t);
}

bool EdMouseDown(Tab* t, int mx, int my, bool dbl) {
  if (!EdOn()) return false;
  if (!t || !t->doc) return false;
  if (g_te) { TextBoxClose(true); return true; }               // the click that ends the text box does nothing else
  int page;
  float x, y;
  bool exact;
  bool onPage = ViewHitPage(t, mx, my, &page, &x, &y, &exact) && exact;
  g_gDown = {mx, my};
  g_gMoved = false;
  g_gCopy = false;
  if (g_tool == ET_SELECT) {
    bool shift = ModDown(VK_SHIFT), alt = ModDown(VK_MENU);
    if (RotHandleAt(t, mx, my)) {
      g_g = G_ROTATE; g_gPage = SelPage(); g_rotDeg = 0; g_gBox0 = g_selBox; g_gBox = g_selBox;
      return true;
    }
    int h = HandleAt(t, mx, my);
    if (h >= 0) {
      g_g = G_RESIZE; g_gHandle = h; g_gPage = SelPage(); g_gBox0 = g_selBox; g_gBox = g_selBox;
      LoadPreview(t);
      return true;
    }
    if (onPage) {
      EdHit hit;
      bool got = EdHitTest(t, page, x, y, false, &hit, alt);
      if (dbl && got) {
        // double click on text: the whole line goes into a text box
        if (hit.sel.kind != 2 && (hit.sel.sub == EK_TEXT || EdSelObjKind(t, hit.sel) == EK_TEXT)) {
          ESet line;
          if (hit.sel.kind == 3) EdExpandLine(t, hit.sel, &line); else line.push_back(hit.sel);
          SetSet(line);
          StartTextEdit();
          return true;
        }
      }
      if (shift && got) {
        // add to / take out of the selection
        ESet s = g_set;
        if (!s.empty() && s[0].page != page) s.clear();
        bool had = false;
        for (size_t i = 0; i < s.size(); i++) if (SameItem(s[i], hit.sel)) { s.erase(s.begin() + i); had = true; break; }
        if (!had) s.push_back(hit.sel);
        SetSet(s);
        Redraw();
        return true;
      }
      bool inSel = InSelBox(t, mx, my) && (!got || InSet(hit.sel));
      if (inSel || got) {
        if (!inSel) SetSel(hit.sel);
        if (g_set.empty()) return false;
        BeginMove(t, mx, my);
        Redraw();
        return true;
      }
      // empty place on the page: a frame to select with
      if (!shift) { ClearSel(); }
      g_g = G_MARQUEE; g_gPage = page; g_gx0 = x; g_gy0 = y; g_gBox = {x, y, x, y}; g_gAdd = shift;
      Redraw();
      return true;
    }
    if (!g_set.empty()) { ClearSel(); Redraw(); }
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
      bool uniform = !ModDown(VK_SHIFT);
      const ESel* o = One();
      if (g_selHasText || g_selAngle != 0 || g_set.size() > 1) uniform = true;
      else if (o && o->kind == 2 && o->sub != FPDF_ANNOT_INK) uniform = false;
      g_gBox = ResizeBox(g_gBox0, g_gHandle, x, y, uniform);
      break;
    }
    case G_ROTATE: {
      double cx = (g_gBox0.l + g_gBox0.r) * 0.5, cy = (g_gBox0.t + g_gBox0.b) * 0.5;
      double a = atan2((double)y - cy, (double)x - cx) * 180.0 / 3.14159265358979323846 + 90.0;
      while (a > 180) a -= 360;
      while (a < -180) a += 360;
      if (ModDown(VK_SHIFT)) a = floor(a / 15.0 + 0.5) * 15.0;
      g_rotDeg = a;
      g_gMoved = fabs(a) > 0.3;
      break;
    }
    case G_MARQUEE: {
      g_gMoved = true;
      x = min(max(x, 0.0f), pw); y = min(max(y, 0.0f), ph);
      g_gBox = {min(g_gx0, x), min(g_gy0, y), max(g_gx0, x), max(g_gy0, y)};
      break;
    }
    case G_SHAPE: {
      x = min(max(x, 0.0f), pw); y = min(max(y, 0.0f), ph);
      g_gBox = {min(g_gx0, x), min(g_gy0, y), max(g_gx0, x), max(g_gy0, y)};
      if (ModDown(VK_SHIFT)) {
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
      if (g_gMoved && !g_set.empty()) {
        ESet s = g_set;
        FRect b0 = g_gBox0, b1 = g_gBox;
        FreePreview();
        if (g == G_MOVE && g_gCopy) {
          // Ctrl + drag: a copy is left where the original was and the original moves... the copy is what moves
          if (EdCopy(t, s)) {
            g_clipTab = t->id; g_clipPage = g_gPage; g_pasteN = 0;
            ESet res;
            ClearSel();
            if (EdPaste(t, g_gPage, b1.l - b0.l, b1.t - b0.t, &res)) SetSet(res);
          }
        } else if (EdTransformSet(t, s, b0, b1)) SetSet(s);
        else RefreshSel();
      }
      FreePreview();
      break;
    case G_ROTATE:
      if (g_gMoved && !g_set.empty()) {
        ESet s = g_set;
        if (EdRotateSet(t, s, g_rotDeg)) SetSet(s);
      }
      g_rotDeg = 0;
      break;
    case G_MARQUEE: {
      ESet res;
      if (g_gBox.r - g_gBox.l >= 3 || g_gBox.b - g_gBox.t >= 3) {
        EdMarquee(t, g_gPage, g_gBox, &res);
        if (g_gAdd && !g_set.empty() && SelPage() == g_gPage) {
          ESet all = g_set;
          for (auto& it : res) if (!InSet(it)) all.push_back(it);
          res = all;
        }
        SetSet(res);
      }
      break;
    }
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
  else if (g_g == G_ROTATE) cur = IDC_HAND;
  else if (g_g == G_MARQUEE) cur = IDC_CROSS;
  else if (g_g == G_RESIZE || (g_g == G_NONE && g_tool == ET_SELECT && !g_set.empty() && HandleAt(t, mx, my) >= 0)) {
    int h = g_g == G_RESIZE ? g_gHandle : HandleAt(t, mx, my);
    cur = (h == 0 || h == 4) ? IDC_SIZENWSE : (h == 2 || h == 6) ? IDC_SIZENESW : (h == 1 || h == 5) ? IDC_SIZENS : IDC_SIZEWE;
  } else if (g_g == G_SHAPE || g_g == G_INK) cur = IDC_CROSS;
  else if (g_g == G_NONE) {
    if (g_tool == ET_SELECT) {
      if (RotHandleAt(t, mx, my)) cur = IDC_HAND;
      else if (!g_set.empty() && InSelBox(t, mx, my)) cur = IDC_SIZEALL;
    } else if (g_tool == ET_TEXT || g_tool == ET_INK || g_tool == ET_RECT || g_tool == ET_ELLIPSE) {
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
  g_ctxSet.clear();
  if (!EdOn() || !t) return;
  int page;
  float x, y;
  bool exact;
  if (!ViewHitPage(t, mx, my, &page, &x, &y, &exact) || !exact) return;
  EdHit hit;
  bool got = EdHitTest(t, page, x, y, true, &hit);
  if (got && !InSet(hit.sel)) { SetSel(hit.sel); Redraw(); }
  if (!got && !InSelBox(t, mx, my)) { if (!g_set.empty()) { ClearSel(); Redraw(); } }
  if (!g_set.empty()) {
    g_ctxSet = g_set;
    const wchar_t* label = L"Удалить";
    const ESel* o = One();
    if (o && o->kind == 2) {
      switch (o->sub) {
        case FPDF_ANNOT_HIGHLIGHT: label = L"Убрать выделение цветом"; break;
        case FPDF_ANNOT_UNDERLINE: label = L"Убрать подчёркивание"; break;
        case FPDF_ANNOT_STRIKEOUT: label = L"Убрать зачёркивание"; break;
        case FPDF_ANNOT_SQUIGGLY: label = L"Убрать волнистую линию"; break;
        default: label = L"Удалить фигуру"; break;
      }
    } else if (g_set.size() > 1) label = L"Удалить выбранное";
    else label = g_selKind == EK_IMAGE ? L"Удалить картинку" : g_selKind == EK_TEXT ? L"Удалить текст" : L"Удалить";
    if (g_selHasText) AppendMenuW(m, MF_STRING, ID_ED_OB_EDITTEXT, L"Править текст");
    if (!(o && o->kind == 2 && IsMarkupSub(o->sub))) {
      AppendMenuW(m, MF_STRING, ID_ED_OB_COPY, L"Копировать объект");
      AppendMenuW(m, MF_STRING, ID_ED_OB_DUP, L"Дублировать");
    }
    AppendMenuW(m, MF_STRING, ID_ED_CTX_DELETE, label);
  }
  if (EdClipHas() || ClipboardHasImage()) AppendMenuW(m, MF_STRING, ID_ED_OB_PASTE, L"Вставить");
  AppendMenuW(m, MF_STRING, ID_ED_OB_SELALL, L"Выбрать все объекты страницы");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
}

// ------------------------------------------------------------------ overlay
static void SelRectDraw(Gdiplus::Graphics& g, const RECT& r, bool handles, bool usable[8], bool rot) {
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
  if (rot) {
    RECT rh = RotHandleRect(r);
    int cx = (r.left + r.right) / 2;
    Gdiplus::Pen lp(acc, 1.0f);
    g.DrawLine(&lp, cx, r.top, cx, (int)(rh.bottom - 1));
    g.FillEllipse(&wb, (int)rh.left, (int)rh.top, (int)(rh.right - rh.left - 1), (int)(rh.bottom - rh.top - 1));
    g.DrawEllipse(&hp, (int)rh.left, (int)rh.top, (int)(rh.right - rh.left - 1), (int)(rh.bottom - rh.top - 1));
  }
}

void EdPaintOverlay(HDC dc, RECT rc, Tab* t, int first, int last) {
  (void)rc;
  if (!EdOn()) return;
  if (!t || !t->doc) return;
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::Color accA(120, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent));
  // selection
  if (!g_set.empty() && SelPage() >= first && SelPage() <= last && SelPage() < t->doc->npages) {
    FRect b = g_g == G_NONE ? g_selBox : g_gBox0;
    RECT r = ViewRectToScreen(t, SelPage(), b);
    bool usable[8];
    for (int h = 0; h < 8; h++) usable[h] = HandleUsable(h);
    if (g_g == G_NONE) {
      if (g_set.size() > 1) {
        Gdiplus::Pen ip(accA, 1.0f);
        for (auto& ib : g_itemBox) {
          RECT ir = ViewRectToScreen(t, SelPage(), ib);
          g.DrawRectangle(&ip, (Gdiplus::REAL)ir.left, (Gdiplus::REAL)ir.top, (Gdiplus::REAL)(ir.right - ir.left), (Gdiplus::REAL)(ir.bottom - ir.top));
        }
      }
      SelRectDraw(g, r, true, usable, CanRotate());
    } else {
      // the original place stays faint while the ghost moves
      Gdiplus::Pen fp(accA, 1.0f);
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
    SelRectDraw(g, r, g_g == G_RESIZE, usable, false);
  }
  // turning
  if (g_g == G_ROTATE && g_gMoved && g_gPage >= first && g_gPage <= last) {
    double cx = (g_gBox0.l + g_gBox0.r) * 0.5, cy = (g_gBox0.t + g_gBox0.b) * 0.5, a = g_rotDeg * 3.14159265358979323846 / 180.0;
    double cs = cos(a), sn = sin(a);
    float xs[4] = {g_gBox0.l, g_gBox0.r, g_gBox0.r, g_gBox0.l}, ys[4] = {g_gBox0.t, g_gBox0.t, g_gBox0.b, g_gBox0.b};
    Gdiplus::PointF pts[4];
    for (int i = 0; i < 4; i++) {
      double px = cx + (xs[i] - cx) * cs - (ys[i] - cy) * sn, py = cy + (xs[i] - cx) * sn + (ys[i] - cy) * cs;
      int sx, sy;
      ViewPointToScreen(t, g_gPage, (float)px, (float)py, &sx, &sy);
      pts[i] = Gdiplus::PointF((Gdiplus::REAL)sx, (Gdiplus::REAL)sy);
    }
    Gdiplus::SolidBrush fb(Gdiplus::Color(40, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent)));
    g.FillPolygon(&fb, pts, 4);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent)), 1.4f);
    g.DrawPolygon(&pen, pts, 4);
    wchar_t lab[32];
    swprintf(lab, 32, L"%d°", (int)lround(g_rotDeg));
    Gdiplus::Font f(L"Segoe UI", 10.0f);
    Gdiplus::SolidBrush tb(Gdiplus::Color(255, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent)));
    g.DrawString(lab, -1, &f, Gdiplus::PointF(pts[0].X + 6, pts[0].Y - 18), &tb);
  }
  // frame being drawn to select
  if (g_g == G_MARQUEE && g_gMoved && g_gPage >= first && g_gPage <= last) {
    RECT r = ViewRectToScreen(t, g_gPage, g_gBox);
    Gdiplus::SolidBrush fb(Gdiplus::Color(35, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent)));
    g.FillRectangle(&fb, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)(r.right - r.left), (Gdiplus::REAL)(r.bottom - r.top));
    Gdiplus::Pen pen(Gdiplus::Color(220, GetRValue(T.accent), GetGValue(T.accent), GetBValue(T.accent)), 1.0f);
    g.DrawRectangle(&pen, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)(r.right - r.left), (Gdiplus::REAL)(r.bottom - r.top));
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
static void TestLog(const char* fmt, ...) {
  FILE* f = _wfopen(L"Z:\\tmp\\t\\dump.txt", L"ab");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}
static UINT KeyByName(const std::string& n) {
  if (n.size() == 1) return (UINT)toupper((unsigned char)n[0]);
  if (n == "f2") return VK_F2;
  if (n == "delete") return VK_DELETE;
  if (n == "esc") return VK_ESCAPE;
  if (n == "enter") return VK_RETURN;
  if (n == "left") return VK_LEFT;
  if (n == "right") return VK_RIGHT;
  if (n == "up") return VK_UP;
  if (n == "down") return VK_DOWN;
  return 0;
}
int EdUiTest(const std::string& line) {
  std::istringstream ss(line);
  std::string c;
  ss >> c;
  Tab* t = CurTab();
  if (c == "mode") { int m; ss >> m; EdSetMode(m); return 1; }
  if (c == "btn") { int id; ss >> id; EdCommand(id); return 1; }
  if (c == "mods") { ss >> g_testMods; return 1; }
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
  if (c == "dclick") {
    int p; float x, y; ss >> p >> x >> y;
    TestMouse(WM_LBUTTONDOWN, MK_LBUTTON, p, x, y); TestMouse(WM_LBUTTONUP, 0, p, x, y);
    TestMouse(WM_LBUTTONDBLCLK, MK_LBUTTON, p, x, y); TestMouse(WM_LBUTTONUP, 0, p, x, y);
    return 1;
  }
  if (c == "drag") {
    int p; float x0, y0, x1, y1; ss >> p >> x0 >> y0 >> x1 >> y1;
    TestMouse(WM_LBUTTONDOWN, MK_LBUTTON, p, x0, y0);
    for (int i = 1; i <= 8; i++) TestMouse(WM_MOUSEMOVE, MK_LBUTTON, p, x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8);
    TestMouse(WM_LBUTTONUP, 0, p, x1, y1);
    return 1;
  }
  if (c == "rotdrag") {            // rotdrag <deg>: takes the rotation handle and turns the selection
    double deg; ss >> deg;
    if (g_set.empty() || !CanRotate()) return 0;
    ViewEnsureLayout(t);
    RECT box = ViewRectToScreen(t, SelPage(), g_selBox);
    RECT rh = RotHandleRect(box);
    int hx = (rh.left + rh.right) / 2, hy = (rh.top + rh.bottom) / 2;
    SendMessageW(A_.view, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(hx, hy));
    float cx = (g_selBox.l + g_selBox.r) * 0.5f, cy = (g_selBox.t + g_selBox.b) * 0.5f;
    double a = deg * 3.14159265358979323846 / 180.0;
    for (int i = 1; i <= 6; i++) {
      double aa = a * i / 6;
      TestMouse(WM_MOUSEMOVE, MK_LBUTTON, SelPage(), cx + (float)(80 * sin(aa)), cy - (float)(80 * cos(aa)));
    }
    TestMouse(WM_LBUTTONUP, 0, SelPage(), cx + (float)(80 * sin(a)), cy - (float)(80 * cos(a)));
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
  if (c == "edittext") {           // edittext <text>: types into the open text box (editing existing text)
    std::string r; std::getline(ss, r);
    if (!r.empty() && r[0] == ' ') r.erase(0, 1);
    for (size_t i = 0; i + 1 < r.size(); i++) if (r[i] == '\\' && r[i + 1] == 'n') r.replace(i, 2, "\r\n");
    if (!g_te) return 0;
    SetWindowTextW(g_te, Utf8ToW(r).c_str());
    TextBoxResize();
    return 1;
  }
  if (c == "tbinfo") {
    RECT r = {0, 0, 0, 0};
    if (g_te) GetWindowRect(g_te, &r);
    TestLog("tb te=%p vis=%d rect=%ld,%ld,%ld,%ld focus=%d edit=%d", (void*)g_te, g_te ? IsWindowVisible(g_te) : 0, r.left, r.top, r.right, r.bottom, GetFocus() == g_te, g_tbEdit ? 1 : 0);
    if (g_te) {
      int len = GetWindowTextLengthW(g_te);
      std::vector<wchar_t> buf(len + 2, 0);
      GetWindowTextW(g_te, buf.data(), len + 1);
      TestLog("tbtext [%s]", WToUtf8(buf.data()).c_str());
      RECT fr = {0, 0, 0, 0}, cr = {0, 0, 0, 0};
      SendMessageW(g_te, EM_GETRECT, 0, (LPARAM)&fr);
      GetClientRect(g_te, &cr);
      TestLog("tbedit lines=%d first=%d fmt=%ld,%ld,%ld,%ld client=%ld,%ld,%ld,%ld", (int)SendMessageW(g_te, EM_GETLINECOUNT, 0, 0), (int)SendMessageW(g_te, EM_GETFIRSTVISIBLELINE, 0, 0),
              fr.left, fr.top, fr.right, fr.bottom, cr.left, cr.top, cr.right, cr.bottom);
    }
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
  if (c == "picture") { std::string f; std::getline(ss, f); if (!f.empty() && f[0] == ' ') f.erase(0, 1); InsertPicture(Utf8ToW(f)); return !g_set.empty(); }
  if (c == "setsize") { int v; ss >> v; if (SizeIsFont()) { g_fontSize = v; if (g_selText) { ESet s = g_set; if (EdSetTextSizeSet(t, s, v)) SetSet(s); } } else g_lineW = v; return 1; }
  if (c == "key") {                // key <name> [ctrl] [shift]
    std::string n, m1, m2; ss >> n >> m1 >> m2;
    bool ctrl = m1 == "ctrl" || m2 == "ctrl", shift = m1 == "shift" || m2 == "shift";
    UINT vk = KeyByName(n);
    if (!vk) return 0;
    bool r = EdKey(vk, ctrl, shift);
    Redraw();
    return r;
  }
  if (c == "obj") {                // obj <name> [arg]
    std::string n; ss >> n;
    double arg = 0; ss >> arg;
    static const std::pair<const char*, int> names[] = {{"copy", ID_ED_OB_COPY}, {"cut", ID_ED_OB_CUT}, {"paste", ID_ED_OB_PASTE}, {"dup", ID_ED_OB_DUP},
        {"group", ID_ED_OB_GROUP}, {"ungroup", ID_ED_OB_UNGROUP}, {"selall", ID_ED_OB_SELALL}, {"edittext", ID_ED_OB_EDITTEXT}, {"bold", ID_ED_OB_BOLD},
        {"italic", ID_ED_OB_ITALIC}, {"cw", ID_ED_OB_ROT_CW}, {"ccw", ID_ED_OB_ROT_CCW}, {"r180", ID_ED_OB_ROT_180}};
    for (auto& p : names) if (n == p.first) { ObjCommand(p.second); Redraw(); return 1; }
    if (n == "rot") { RotateSel(arg); Redraw(); return 1; }
    if (n == "align") { ObjCommand(ID_ED_OB_AL_LEFT + (int)arg); Redraw(); return 1; }
    if (n == "opac") { ESet s = g_set; bool ok = EdSetOpacity(t, s, (int)arg); if (ok) SetSet(s); Redraw(); return ok; }
    if (n == "textsize") { ESet s = g_set; bool ok = EdSetTextSizeSet(t, s, (int)arg); if (ok) SetSet(s); Redraw(); return ok; }
    if (n == "z") { ESet s = g_set; bool ok = EdZOrderSet(t, s, (int)arg); if (ok) SetSet(s); Redraw(); return ok; }
    if (n == "move") { std::string dy; ss >> dy; FRect b = g_selBox, nb = {b.l + (float)arg, b.t + (float)atof(dy.c_str()), b.r + (float)arg, b.b + (float)atof(dy.c_str())}; ESet s = g_set; bool ok = EdTransformSet(t, s, b, nb); if (ok) SetSet(s); Redraw(); return ok; }
    if (n == "delete") { EdCommand(ID_ED_DELETE); return 1; }
    return 0;
  }
  if (c == "face") {               // face <family> <style>: font of the selected text (or of new text)
    std::string fam; int st = 0; ss >> fam >> st;
    for (char& ch : fam) if (ch == '_') ch = ' ';
    ApplyFace(Utf8ToW(fam), st);
    return 1;
  }
  if (c == "selinfo") {
    TestLog("sel n=%d kind=%d text=%d hasText=%d objs=%d annots=%d angle=%d box=%.1f,%.1f,%.1f,%.1f tool=%d", (int)g_set.size(), g_selKind, g_selText, g_selHasText,
            g_selObjs, g_selAnnots, g_selAngle, g_selBox.l, g_selBox.t, g_selBox.r, g_selBox.b, g_tool);
    for (auto& it : g_set) TestLog("  item page=%d kind=%d grp=%d obj=%d annot=%d sub=%d", it.page, it.kind, it.grp, it.obj, it.annot, it.sub);
    EdTextInfo ti;
    if (g_selHasText && EdGetText(t, g_set, &ti))
      TestLog("  text [%s] fam=%s style=%d size=%d lines=%d", WToUtf8(ti.text).c_str(), WToUtf8(ti.family).c_str(), ti.style, ti.size, ti.lines);
    return !g_set.empty() ? 1 : 0;
  }
  if (c == "dlgexport") {
    auto* f = new std::function<void()>([]() { ExportOpts o; o.preset = 1; o.paper = 1; ExportOptionsDialog(A_.main, &o, 12); });
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
  if (c == "fit") {                // fit <item 0..6> : the Pages menu command "all pages to paper size"
    int k = 0; ss >> k;
    PageCommand(ID_ED_PG_FIT + k);
    return 1;
  }
  if (c == "fitopt") { int en = 1, mg = 0; ss >> en >> mg; g_fitEnlarge = en != 0; g_fitMargin = mg; return 1; }
  return -1;
}

#endif
