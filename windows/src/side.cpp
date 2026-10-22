// Sidebar content: thumbnails, outline, bookmarks
#include "app.h"

static HWND g_sw = nullptr;
static HBITMAP g_sback = nullptr;
static HDC g_sbackDC = nullptr;
static int g_sbW = 0, g_sbH = 0;
static int g_scroll = 0, g_content = 0;
static std::vector<int> g_itop;      // thumbs: item tops
static int g_thumbW = 100;
static int g_lastCur = -1;
static int g_hover = -1;
static bool g_trk = false;
static Tab* g_layoutTab = nullptr;
static int g_layoutKey = 0;

struct Row { RECT r; int idx; int kind; };   // kind: 0 item, 1 chevron, 2 delete
static std::vector<Row> g_rows;

static int RowH() { return S(30); }

static std::vector<int> OutlineVisible(Tab* t) {
  std::vector<int> v;
  if (!t || !t->doc) return v;
  auto& o = t->doc->outline;
  int skipBelow = -1;
  for (int i = 0; i < (int)o.size(); i++) {
    if (skipBelow >= 0) {
      if (o[i].level > skipBelow) continue;
      skipBelow = -1;
    }
    v.push_back(i);
    if (o[i].hasKids && !o[i].open) skipBelow = o[i].level;
  }
  return v;
}

static void Layout() {
  Tab* t = CurTab();
  RECT rc;
  GetClientRect(g_sw, &rc);
  int w = rc.right, h = rc.bottom;
  g_content = 0;
  g_itop.clear();
  if (t && t->doc) {
    if (G.sideTab == 0) {
      g_thumbW = max(S(60), w - S(44) - GetSystemMetrics(SM_CXVSCROLL));
      int y = S(10);
      for (int i = 0; i < t->doc->npages; i++) {
        double rw = (t->rot & 1) ? t->doc->ph[i] : t->doc->pw[i];
        double rh = (t->rot & 1) ? t->doc->pw[i] : t->doc->ph[i];
        int th = max(8, (int)lround(g_thumbW * rh / rw));
        th = min(th, S(520));
        g_itop.push_back(y);
        y += th + S(30);
      }
      g_content = y;
    } else if (G.sideTab == 1) {
      g_content = (int)OutlineVisible(t).size() * RowH() + S(8);
    } else {
      g_content = (int)t->saved.bookmarks.size() * RowH() + S(8);
    }
  }
  SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
  g_scroll = min(max(g_scroll, 0), max(0, g_content - h));
  si.nMin = 0; si.nMax = max(0, g_content - 1); si.nPage = h; si.nPos = g_scroll;
  SetScrollInfo(g_sw, SB_VERT, &si, TRUE);
}

void SideSync() {
  if (!g_sw) return;
  Layout();
  InvalidateRect(g_sw, nullptr, FALSE);
  UpdateWindow(g_sw);
}
void SideInvalidate() { if (g_sw) InvalidateRect(g_sw, nullptr, FALSE); }
void SideRebuildOutline(Tab*) { SideSync(); }

void SideScrollToCurrent() {
  if (!g_sw) return;
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  int cur = ViewCurPage();
  if (cur == g_lastCur) return;
  g_lastCur = cur;
  if (G.sideTab == 0 && G.sidebar && cur < (int)g_itop.size()) {
    RECT rc;
    GetClientRect(g_sw, &rc);
    int top = g_itop[cur], bot = top + S(200);
    if (top < g_scroll + S(10)) g_scroll = max(0, top - S(10));
    else if (bot > g_scroll + rc.bottom) g_scroll = top - S(10);
    Layout();
  }
  SideInvalidate();
}

static void EnsureBack(HDC dc, int w, int h) {
  if (g_sback && g_sbW == w && g_sbH == h) return;
  if (g_sback) DeleteObject(g_sback);
  if (g_sbackDC) DeleteDC(g_sbackDC);
  g_sbackDC = CreateCompatibleDC(dc);
  g_sback = CreateCompatibleBitmap(dc, max(w, 1), max(h, 1));
  SelectObject(g_sbackDC, g_sback);
  g_sbW = w; g_sbH = h;
}

static void PaintSide(HWND hw) {
  PAINTSTRUCT ps;
  HDC dc0 = BeginPaint(hw, &ps);
  RECT rc;
  GetClientRect(hw, &rc);
  EnsureBack(dc0, rc.right, rc.bottom);
  HDC dc = g_sbackDC;
  HBRUSH bg = CreateSolidBrush(T.chrome2);
  FillRect(dc, &rc, bg);
  DeleteObject(bg);
  g_rows.clear();
  Tab* t = CurTab();
  if (!t || !t->doc) { BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY); EndPaint(hw, &ps); return; }
  if (g_layoutTab != t) { g_layoutTab = t; Layout(); }
  int cur = ViewCurPage();
  int w = rc.right, h = rc.bottom;
  if (G.sideTab == 0) {
    ReqBegin(CONS_SIDE);
    CacheLock();
    HDC mdc = CreateCompatibleDC(dc);
    SetStretchBltMode(dc, HALFTONE);
    int n = t->doc->npages;
    int zq = g_thumbW;
    for (int i = 0; i < n && i < (int)g_itop.size(); i++) {
      int top = g_itop[i] - g_scroll;
      double rw = (t->rot & 1) ? t->doc->ph[i] : t->doc->pw[i];
      double rh = (t->rot & 1) ? t->doc->pw[i] : t->doc->ph[i];
      int th = min(max(8, (int)lround(g_thumbW * rh / rw)), S(520));
      int tw = g_thumbW;
      if (th == S(520)) tw = max(8, (int)lround(th * rw / rh));
      if (top > h + S(300)) break;
      if (top + th + S(30) < -S(300)) continue;
      int left = (w - GetSystemMetrics(SM_CXVSCROLL) * 0 - tw) / 2;
      RECT outer = {left - S(5), top - S(5), left + tw + S(5), top + th + S(5)};
      if (i == cur) { FillRoundRect(dc, outer, S(4), T.accent, 60); FrameRoundRect(dc, outer, S(4), T.accent, 2.0f); }
      else if (i == g_hover) FillRoundRect(dc, outer, S(4), T.hover);
      RECT pr = {left, top, left + tw, top + th};
      COLORREF pb = G.readMode == 1 ? RGB(244, 232, 200) : (G.readMode == 2 ? RGB(16, 16, 16) : RGB(255, 255, 255));
      HBRUSH b = CreateSolidBrush(pb);
      FillRect(dc, &pr, b);
      DeleteObject(b);
      if (top + th > -S(20) && top < h + S(20)) {
        TileKey k;
        k.doc = t->doc->id; k.page = i; k.zq = zq; k.rot = t->rot & 3; k.mode = G.readMode; k.kind = 2;
        Tile tl;
        if (CacheGetLocked(k, &tl)) {
          HGDIOBJ old = SelectObject(mdc, tl.bmp);
          StretchBlt(dc, left, top, tw, th, mdc, 0, 0, tl.w, tl.h, SRCCOPY);
          SelectObject(mdc, old);
        } else {
          ReqTile(CONS_SIDE, t->doc, k, tw, th, 0, 0, tw, th, 5);
        }
        FrameRect(dc, &pr, (HBRUSH)GetStockObject(GRAY_BRUSH));
      }
      RECT nr = {left - S(5), top + th + S(5), left + tw + S(5), top + th + S(26)};
      DrawTextW2(dc, std::to_wstring(i + 1), nr, i == cur ? T.accent : T.dim, UiFont(S(12), i == cur), DT_CENTER | DT_SINGLELINE);
      Row r = {outer, i, 0};
      g_rows.push_back(r);
    }
    CacheUnlock();
    DeleteDC(mdc);
    ReqEnd(CONS_SIDE);
  } else if (G.sideTab == 1) {
    auto vis = OutlineVisible(t);
    if (vis.empty()) {
      RECT r = {S(12), S(16), w - S(12), S(80)};
      DrawTextW2(dc, L"В документе нет оглавления", r, T.dim, UiFont(S(13)), DT_CENTER | DT_WORDBREAK);
    }
    for (int k = 0; k < (int)vis.size(); k++) {
      int top = S(4) + k * RowH() - g_scroll;
      if (top > h) break;
      if (top + RowH() < 0) continue;
      const OutlineItem& it = t->doc->outline[vis[k]];
      RECT row = {S(4), top, w - S(4), top + RowH()};
      if (g_hover == k) FillRoundRect(dc, row, S(4), T.hover);
      int ind = S(8) + it.level * S(14);
      if (it.hasKids) {
        RECT cr = {ind - S(2), top, ind + S(18), top + RowH()};
        FRect ir = {(float)ind, (float)top + S(7), (float)ind + S(16), (float)top + S(23)};
        DrawIcon(dc, it.open ? IC_CHEVD : IC_CHEV, ir, T.dim, 1.5f);
        Row r = {cr, k, 1};
        g_rows.push_back(r);
      }
      RECT tr = {ind + S(20), top, w - S(10), top + RowH()};
      DrawTextW2(dc, it.title.empty() ? L"(без названия)" : it.title, tr, T.text, UiFont(S(13)), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      Row r = {row, k, 0};
      g_rows.push_back(r);
    }
  } else {
    auto& bm = t->saved.bookmarks;
    if (bm.empty()) {
      RECT r = {S(12), S(16), w - S(12), S(100)};
      DrawTextW2(dc, L"Закладок нет.\nCtrl+D — добавить закладку на текущую страницу", r, T.dim, UiFont(S(13)), DT_CENTER | DT_WORDBREAK);
    }
    for (int k = 0; k < (int)bm.size(); k++) {
      int top = S(4) + k * RowH() - g_scroll;
      if (top > h) break;
      if (top + RowH() < 0) continue;
      RECT row = {S(4), top, w - S(4), top + RowH()};
      if (g_hover == k) FillRoundRect(dc, row, S(4), T.hover);
      FRect ir = {(float)S(12), (float)top + S(6), (float)S(30), (float)top + S(24)};
      DrawIcon(dc, IC_BOOKON, ir, T.accent, 1.5f);
      RECT tr = {S(38), top, w - S(40), top + RowH()};
      DrawTextW2(dc, FmtT(L"Страница %d", bm[k] + 1), tr, T.text, UiFont(S(13)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      RECT dr = {w - S(36), top, w - S(8), top + RowH()};
      if (g_hover == k) {
        FRect xr = {(float)dr.left + S(6), (float)top + S(7), (float)dr.right - S(6), (float)top + S(23)};
        DrawIcon(dc, IC_CLOSE, xr, T.dim, 1.5f);
      }
      Row r = {row, k, 0};
      g_rows.push_back(r);
      Row d = {dr, k, 2};
      g_rows.push_back(d);
    }
  }
  BitBlt(dc0, 0, 0, w, h, dc, 0, 0, SRCCOPY);
  EndPaint(hw, &ps);
}

static int HitRow(int x, int y, int* kind) {
  // later rows (chevron/delete) take priority
  for (int i = (int)g_rows.size() - 1; i >= 0; i--) {
    if (g_rows[i].kind != 0 && PtInRect(&g_rows[i].r, {x, y})) { *kind = g_rows[i].kind; return g_rows[i].idx; }
  }
  for (int i = 0; i < (int)g_rows.size(); i++)
    if (g_rows[i].kind == 0 && PtInRect(&g_rows[i].r, {x, y})) { *kind = 0; return g_rows[i].idx; }
  return -1;
}

static LRESULT CALLBACK SideProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: g_sw = hw; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DROPFILES: return SendMessageW(A_.main, WM_DROPFILES, wp, lp);
    case WM_PAINT: PaintSide(hw); return 0;
    case WM_SIZE: g_layoutTab = nullptr; Layout(); InvalidateRect(hw, nullptr, FALSE); return 0;
    case WM_VSCROLL: {
      SCROLLINFO si = {sizeof(si), SIF_ALL};
      GetScrollInfo(hw, SB_VERT, &si);
      int pos = g_scroll;
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= S(40); break;
        case SB_LINEDOWN: pos += S(40); break;
        case SB_PAGEUP: pos -= si.nPage; break;
        case SB_PAGEDOWN: pos += si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: return 0;
      }
      g_scroll = pos;
      Layout();
      InvalidateRect(hw, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(60) / 120;
      Layout();
      InvalidateRect(hw, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!g_trk) { TRACKMOUSEEVENT te = {sizeof(te), TME_LEAVE, hw, 0}; TrackMouseEvent(&te); g_trk = true; }
      int kind;
      int i = HitRow(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &kind);
      if (i != g_hover) { g_hover = i; InvalidateRect(hw, nullptr, FALSE); }
      return 0;
    }
    case WM_MOUSELEAVE: g_trk = false; if (g_hover != -1) { g_hover = -1; InvalidateRect(hw, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
      SetFocus(A_.view);
      Tab* t = CurTab();
      if (!t || !t->doc) return 0;
      int kind;
      int i = HitRow(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &kind);
      if (i < 0) return 0;
      if (G.sideTab == 0) {
        ViewGoPage(i, 0, true, false);
      } else if (G.sideTab == 1) {
        auto vis = OutlineVisible(t);
        if (i >= (int)vis.size()) return 0;
        OutlineItem& it = t->doc->outline[vis[i]];
        if (kind == 1) { it.open = !it.open; Layout(); InvalidateRect(hw, nullptr, FALSE); }
        else if (it.page >= 0) {
          float y = 0;
          bool has = it.hasY && ResolveDestY(t->doc, it.page, it.yPdf, &y);
          ViewGoPage(it.page, has ? max(0.0f, y - 10) : 0, true, true);
        }
      } else {
        auto& bm = t->saved.bookmarks;
        if (i >= (int)bm.size()) return 0;
        if (kind == 2) { bm.erase(bm.begin() + i); t->dirty = true; Layout(); InvalidateRect(hw, nullptr, FALSE); MainRedrawChrome(); }
        else ViewGoPage(bm[i], 0, true, true);
      }
      return 0;
    }
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

bool SideRegister(HINSTANCE inst) {
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = SideProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"UltraPDF.Side";
  return RegisterClassExW(&wc) != 0;
}
HWND SideCreate(HWND parent) {
  return CreateWindowExW(WS_EX_ACCEPTFILES, L"UltraPDF.Side", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 100, 100, parent,
                         nullptr, A_.inst, nullptr);
}

bool IsBookmarked(Tab* t, int page) {
  for (int b : t->saved.bookmarks) if (b == page) return true;
  return false;
}
void ToggleBookmark(Tab* t, int page) {
  auto& v = t->saved.bookmarks;
  auto it = std::find(v.begin(), v.end(), page);
  if (it != v.end()) v.erase(it);
  else { v.push_back(page); std::sort(v.begin(), v.end()); }
  t->dirty = true;
  g_layoutTab = nullptr;
  SideSync();
  MainRedrawChrome();
}
