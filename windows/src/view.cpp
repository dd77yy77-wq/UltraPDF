#include <dwmapi.h>
#include <atomic>
// The document view: layout, scrolling, zoom, tiles, selection, links, search highlights
#include "app.h"

static HWND g_hw = nullptr;
static int g_vw = 0, g_vh = 0;
static HBITMAP g_back = nullptr;
static HDC g_backDC = nullptr;
static int g_backW = 0, g_backH = 0;
static uint64_t g_lastTick = 0;
static std::atomic<bool> g_anim{false};
static std::atomic<bool> g_vsPending{false};
static HANDLE g_vsEvent = nullptr;
static uint64_t g_lastQpc = 0;
static double g_lastQpcF = 0;
static double QpcMs() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }

// Animation tick source: a thread waits for the DWM composition (vblank) and posts a message, so the frame is
// drawn right after the compositor took the previous one (no tearing, even frame pacing).
static DWORD WINAPI VsyncThread(LPVOID) {
  for (;;) {
    WaitForSingleObject(g_vsEvent, INFINITE);
    while (g_anim) {
      double t0 = QpcMs();
      DwmFlush();
      if (QpcMs() - t0 < 4.0) Sleep(12);
      if (g_hw && !g_vsPending.exchange(true)) PostMessageW(g_hw, WM_APP_VSYNC, 0, 0);
    }
  }
  return 0;
}
extern int g_ticks; extern int g_timerRet;
static uint64_t g_zoomBusy = 0;

enum { TMR_ANIM = 1, TMR_DRAG = 2, TMR_ZOOM = 3 };
enum Drag { D_NONE, D_PAN, D_SEL, D_PENDLINK };
static Drag g_drag = D_NONE;
static POINT g_downPt, g_lastPt;
static double g_downSx, g_downSy;
static int g_clickCount = 0;
static uint32_t g_lastClickTime = 0;
static POINT g_lastClickPt;
static Link g_pendLink;
static bool g_trackLeave = false;

struct StartItem { RECT r; ws path; int cmd; };
static std::vector<StartItem> g_start;
static int g_startHover = -1;
static RECT g_closeBtn = {0, 0, 0, 0};

static int Margin() { return S(14); }
static int Gap() { return S(10); }

// ---------------------------------------------------------------- geometry helpers
int PageAtDocY(Tab* t, double y) {
  int n = (int)t->ytop.size();
  if (n == 0) return 0;
  int lo = 0, hi = n - 1;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (t->ytop[mid] <= y) lo = mid; else hi = mid - 1;
  }
  return lo;
}
static int PageLeft(Tab* t, int i) { return (max(t->docW, t->viewW) - t->pwpx[i]) / 2; }

static void PtToLocal(Tab* t, int p, float x, float y, float* px, float* py) {
  float w = t->doc->pw[p], h = t->doc->ph[p], rx, ry;
  switch (t->rot & 3) {
    case 0: rx = x; ry = y; break;
    case 1: rx = h - y; ry = x; break;
    case 2: rx = w - x; ry = h - y; break;
    default: rx = y; ry = w - x; break;
  }
  *px = (float)(rx * t->scale);
  *py = (float)(ry * t->scale);
}
static void LocalToPt(Tab* t, int p, float px, float py, float* x, float* y) {
  float w = t->doc->pw[p], h = t->doc->ph[p];
  float rx = (float)(px / t->scale), ry = (float)(py / t->scale);
  switch (t->rot & 3) {
    case 0: *x = rx; *y = ry; break;
    case 1: *x = ry; *y = h - rx; break;
    case 2: *x = w - rx; *y = h - ry; break;
    default: *x = w - ry; *y = rx; break;
  }
}
static RECT RectToScreen(Tab* t, int p, const FRect& r) {
  float ax, ay, bx, by;
  PtToLocal(t, p, r.l, r.t, &ax, &ay);
  PtToLocal(t, p, r.r, r.b, &bx, &by);
  int ox = PageLeft(t, p) - (int)lround(t->sx);
  int oy = t->ytop[p] - (int)lround(t->sy);
  RECT o;
  o.left = ox + (int)floorf(min(ax, bx));
  o.right = ox + (int)ceilf(max(ax, bx));
  o.top = oy + (int)floorf(min(ay, by));
  o.bottom = oy + (int)ceilf(max(ay, by));
  return o;
}

static void ClampScroll(Tab* t) {
  double maxY = max(0, t->docH - t->viewH), maxX = max(0, t->docW - t->viewW);
  t->sx = min(max(t->sx, 0.0), maxX);
  t->sy = min(max(t->sy, 0.0), maxY);
  t->tx = min(max(t->tx, 0.0), maxX);
  t->ty = min(max(t->ty, 0.0), maxY);
}

static void CenterX(Tab* t) { t->sx = max(0, (t->docW - t->viewW) / 2); t->tx = t->sx; }

static double FitScale(Tab* t, int mode) {
  int p = min(max(t->fitRef, 0), t->doc->npages - 1);
  double w = (t->rot & 1) ? t->doc->ph[p] : t->doc->pw[p];
  double h = (t->rot & 1) ? t->doc->pw[p] : t->doc->ph[p];
  double aw = max(50, t->viewW - 2 * Margin());
  if (A_.presentation) aw = t->viewW;
  double sc = aw / w;
  if (mode == 2) {
    double ah = max(50, t->viewH - 2 * Gap());
    if (A_.presentation) ah = t->viewH;
    sc = min(sc, ah / h);
  }
  return sc;
}

static void Relayout(Tab* t) {
  if (!t->doc) return;
  t->viewW = max(g_vw, 400);
  t->viewH = max(g_vh, 300);
  if (t->fit) t->zoom = FitScale(t, t->fit) * 72.0 / Dpi();
  t->zoom = min(max(t->zoom, 0.05), 32.0);
  t->scale = t->zoom * Dpi() / 72.0;
  int n = t->doc->npages;
  t->ytop.assign(n, 0);
  t->pwpx.assign(n, 0);
  t->phpx.assign(n, 0);
  int gap = A_.presentation ? 0 : Gap();
  int y = A_.presentation ? 0 : Gap();
  int maxw = 0;
  for (int i = 0; i < n; i++) {
    double w = (t->rot & 1) ? t->doc->ph[i] : t->doc->pw[i];
    double h = (t->rot & 1) ? t->doc->pw[i] : t->doc->ph[i];
    t->pwpx[i] = max(1, (int)lround(w * t->scale));
    t->phpx[i] = max(1, (int)lround(h * t->scale));
    t->ytop[i] = y;
    y += t->phpx[i] + gap;
    maxw = max(maxw, t->pwpx[i]);
  }
  t->docH = y + (A_.presentation ? 0 : 0);
  t->docW = maxw + (A_.presentation ? 0 : 2 * Margin());
  t->layoutOk = true;
}

void ViewEnsureLayout(Tab* t) {
  if (!t || !t->doc) return;
  RECT rc;
  GetClientRect(g_hw, &rc);
  g_vw = rc.right;
  g_vh = rc.bottom;
  double expected = t->fit ? -1 : t->zoom * Dpi() / 72.0;
  if (t->layoutOk && t->viewW == g_vw && t->viewH == g_vh && (t->fit ? true : fabs(t->scale - expected) < 1e-9) &&
      (int)t->ytop.size() == t->doc->npages)
    return;
  Relayout(t);
}

void ViewSaveState(Tab* t) {
  if (!t->doc || !t->layoutOk) return;
  t->saved.path = t->path;
  int p = PageAtDocY(t, t->sy + S(20));
  t->saved.page = p;
  t->saved.off = (float)((t->sy - t->ytop[p]) / t->scale);
  t->saved.zoom = t->zoom;
  t->saved.fit = t->fit;
  t->saved.rot = t->rot;
}

int ViewCurPage() {
  Tab* t = CurTab();
  if (!t || !t->doc || !t->layoutOk) return 0;
  double probe = t->sy + min(t->viewH / 3.0, (double)S(260));
  if (A_.presentation) probe = t->sy + t->viewH / 2.0;
  return PageAtDocY(t, probe);
}

// ---------------------------------------------------------------- scrollbars
void ViewSync() {
  Tab* t = CurTab();
  if (!g_hw) return;
  SCROLLINFO si = {};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
  if (t && t->doc && t->layoutOk && !A_.presentation) {
    si.nMin = 0; si.nMax = max(t->docH - 1, 0); si.nPage = t->viewH; si.nPos = (int)lround(t->sy);
    SetScrollInfo(g_hw, SB_VERT, &si, TRUE);
    bool needH = t->docW > t->viewW;
    ShowScrollBar(g_hw, SB_HORZ, needH);
    if (needH) {
      si.nMax = t->docW - 1; si.nPage = t->viewW; si.nPos = (int)lround(t->sx);
      SetScrollInfo(g_hw, SB_HORZ, &si, TRUE);
    }
  } else {
    si.nMin = 0; si.nMax = 0; si.nPage = 1; si.nPos = 0;
    SetScrollInfo(g_hw, SB_VERT, &si, TRUE);
    ShowScrollBar(g_hw, SB_HORZ, FALSE);
    ShowScrollBar(g_hw, SB_VERT, !A_.presentation);
  }
}

void ViewInvalidate() { if (g_hw) InvalidateRect(g_hw, nullptr, FALSE); }

static void StartAnim() {
  if (!g_anim) {
    if (!g_vsEvent) {
      g_vsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      CloseHandle(CreateThread(nullptr, 0, VsyncThread, nullptr, 0, nullptr));
    }
    g_lastQpc = (uint64_t)QpcMs();
    g_lastQpcF = QpcMs();
    g_anim = true;
    g_timerRet = 1;
    SetEvent(g_vsEvent);
  }
}

static void AfterScrollChange(Tab* t) {
  t->dirty = true;
  ViewSync();
  MainUpdatePageBox();
  SideScrollToCurrent();
  ViewInvalidate();
}

void ViewScrollBy(int dx, int dy, bool animate) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  if (animate) {
    t->ty += dy; t->tx += dx;
    ClampScroll(t);
    StartAnim();
  } else {
    t->sy += dy; t->sx += dx;
    t->ty = t->sy; t->tx = t->sx;
    ClampScroll(t);
    AfterScrollChange(t);
  }
}

static void ScrollTo(Tab* t, double x, double y, bool animate) {
  t->tx = x; t->ty = y;
  ClampScroll(t);
  if (animate) StartAnim();
  else { t->sx = t->tx; t->sy = t->ty; AfterScrollChange(t); }
}

void ViewGoPage(int page, float yPt, bool animate, bool pushBack) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  page = min(max(page, 0), t->doc->npages - 1);
  if (pushBack) {
    int cp = PageAtDocY(t, t->sy + S(20));
    t->back.push_back({cp, (float)((t->sy - t->ytop[cp]) / t->scale)});
    if (t->back.size() > 50) t->back.erase(t->back.begin());
  }
  double y = t->ytop[page] + yPt * t->scale;
  if (A_.presentation) y = t->ytop[page] - max(0, (t->viewH - t->phpx[page]) / 2);
  ScrollTo(t, t->sx, y, animate);
}

void ViewPageStep(int dir) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  int p = ViewCurPage();
  if (A_.presentation) { ViewGoPage(p + dir, 0, false, false); return; }
  // continuous mode: previous/next page start; if we are in the middle of a page, "previous" goes to its top
  if (dir < 0 && t->sy > t->ytop[p] + S(30)) ViewGoPage(p, 0, true, false);
  else ViewGoPage(p + dir, 0, true, false);
}
void ViewPresentStep(int dir) { ViewPageStep(dir); }

// ---------------------------------------------------------------- zoom, fit, rotate
struct Anchor { int page; double xPt, yPt; };
static Anchor GetAnchor(Tab* t, int ax, int ay) {
  Anchor a;
  double docY = t->sy + ay;
  a.page = PageAtDocY(t, docY);
  a.yPt = (docY - t->ytop[a.page]) / t->scale;
  a.xPt = (t->sx + ax - PageLeft(t, a.page)) / t->scale;
  return a;
}
static void SetAnchor(Tab* t, const Anchor& a, int ax, int ay) {
  if (a.page >= (int)t->ytop.size()) return;
  t->sy = t->ytop[a.page] + a.yPt * t->scale - ay;
  t->sx = PageLeft(t, a.page) + a.xPt * t->scale - ax;
  t->ty = t->sy; t->tx = t->sx;
  ClampScroll(t);
}

void ViewZoom(double z, int ax, int ay) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  if (ax < 0) { ax = t->viewW / 2; ay = t->viewH / 2; }
  z = min(max(z, 0.1), 16.0);
  Anchor a = GetAnchor(t, ax, ay);
  t->fit = 0;
  t->zoom = z;
  t->fitRef = a.page;
  Relayout(t);
  SetAnchor(t, a, ax, ay);
  g_zoomBusy = NowMs() + 140;
  SetTimer(g_hw, TMR_ZOOM, 150, nullptr);
  AfterScrollChange(t);
  MainRedrawChrome();
}

void ViewZoomStep(int dir, int ax, int ay) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  static const double steps[] = {0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 12.0, 16.0};
  double z = t->zoom;
  double nz = z;
  if (dir > 0) { for (double s : steps) if (s > z * 1.01) { nz = s; break; } if (nz == z) nz = 16; }
  else { for (int i = (int)(sizeof(steps) / sizeof(steps[0])) - 1; i >= 0; i--) if (steps[i] < z / 1.01) { nz = steps[i]; break; } if (nz == z) nz = 0.1; }
  ViewZoom(nz, ax, ay);
}

void ViewSetFit(int fit) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  int p = ViewCurPage();
  double off = (t->sy - t->ytop[p]) / t->scale;
  t->fit = fit;
  t->fitRef = p;
  Relayout(t);
  if (fit == 2) t->sy = t->ytop[p] - Gap() + 0.0;
  else t->sy = t->ytop[p] + off * t->scale;
  t->sx = max(0, (t->docW - t->viewW) / 2);
  t->ty = t->sy; t->tx = t->sx;
  ClampScroll(t);
  AfterScrollChange(t);
  MainRedrawChrome();
}

void ViewRotate(int dir) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  ViewEnsureLayout(t);
  int p = ViewCurPage();
  t->rot = (t->rot + (dir > 0 ? 1 : 3)) & 3;
  t->fitRef = p;
  t->sel.active = false;
  Relayout(t);
  t->sy = t->ytop[p] - (A_.presentation ? 0 : Gap());
  t->sx = max(0, (t->docW - t->viewW) / 2);
  t->ty = t->sy; t->tx = t->sx;
  ClampScroll(t);
  AfterScrollChange(t);
  MainRedrawChrome();
}

void ViewPrepareTab(Tab* t) {
  if (t->restored || !t->doc) return;
  t->restored = true;
  DocState* st = FindState(t->path, false);
  RECT rc;
  GetClientRect(g_hw, &rc);
  g_vw = rc.right; g_vh = rc.bottom;
  if (st) {
    t->saved = *st;
    t->rot = st->rot & 3;
    t->fit = st->fit;
    t->zoom = st->zoom;
    t->fitRef = min(max(st->page, 0), t->doc->npages - 1);
    t->layoutOk = false;
    Relayout(t);
    int p = t->fitRef;
    t->sy = t->ytop[p] + st->off * t->scale;
    t->sx = max(0, (t->docW - t->viewW) / 2);
  } else {
    t->fit = 1; t->fitRef = 0; t->zoom = 1.0;
    Relayout(t);
    t->sy = 0;
    t->sx = max(0, (t->docW - t->viewW) / 2);
  }
  t->tx = t->sx; t->ty = t->sy;
  ClampScroll(t);
}

// ---------------------------------------------------------------- text / selection helpers
static bool IsWordCh(wchar_t c) { return iswalnum(c) || c == L'_' || c == L'\'' || c == 0xAD; }

static int NearestChar(const PageData& pd, float x, float y, bool* inside, bool* after) {
  int best = -1;
  float bd = 1e30f;
  *inside = false;
  *after = false;
  int n = (int)pd.box.size();
  for (int i = 0; i < n; i++) {
    const FRect& b = pd.box[i];
    if (b.r <= b.l || b.b <= b.t) continue;
    float dx = x < b.l ? b.l - x : (x > b.r ? x - b.r : 0);
    float dy = y < b.t ? b.t - y : (y > b.b ? y - b.b : 0);
    float d = dy * dy * 16 + dx * dx;
    if (d < bd) { bd = d; best = i; if (d == 0) break; }
  }
  if (best >= 0) {
    const FRect& b = pd.box[best];
    *inside = bd == 0;
    *after = x > (b.l + b.r) / 2;
  }
  return best;
}

// finds the page under (or nearest to) a view point; returns local pt
static bool HitPage(Tab* t, int mx, int my, int* page, float* xPt, float* yPt, bool* exact) {
  if (!t->doc || !t->layoutOk) return false;
  double docY = t->sy + my;
  int p = PageAtDocY(t, docY);
  *exact = true;
  if (docY > t->ytop[p] + t->phpx[p]) {
    // in the gap below p: nearest of p and p+1
    if (p + 1 < t->doc->npages && docY - (t->ytop[p] + t->phpx[p]) > t->ytop[p + 1] - docY) p++;
    *exact = false;
  }
  double lx = t->sx + mx - PageLeft(t, p);
  double ly = docY - t->ytop[p];
  if (lx < 0 || lx > t->pwpx[p] || ly < 0 || ly > t->phpx[p]) *exact = false;
  lx = min(max(lx, 0.0), (double)t->pwpx[p]);
  ly = min(max(ly, 0.0), (double)t->phpx[p]);
  float x, y;
  LocalToPt(t, p, (float)lx, (float)ly, &x, &y);
  *page = p; *xPt = x; *yPt = y;
  return true;
}

static const Link* LinkAt(Tab* t, int p, float x, float y) {
  auto pd = GetPageDataNow(t->doc, p, false);
  if (!pd) return nullptr;
  for (const Link& l : pd->links) {
    float tol = 0.5f;
    if (x >= l.r.l - tol && x <= l.r.r + tol && y >= l.r.t - tol && y <= l.r.b + tol) return &l;
  }
  return nullptr;
}

static bool CaretAt(Tab* t, int mx, int my, int* page, int* caret, bool* inside) {
  float x, y;
  bool exact;
  if (!HitPage(t, mx, my, page, &x, &y, &exact)) return false;
  auto pd = GetPageDataNow(t->doc, *page, true);
  if (!pd || pd->box.empty()) { *caret = 0; *inside = false; return pd != nullptr; }
  bool after;
  int c = NearestChar(*pd, x, y, inside, &after);
  if (c < 0) { *caret = 0; *inside = false; return true; }
  *caret = c + (after ? 1 : 0);
  if (!exact) *inside = false;
  return true;
}

bool ViewHasSel() {
  Tab* t = CurTab();
  return t && t->sel.active && !(t->sel.p0 == t->sel.p1 && t->sel.c0 == t->sel.c1);
}
void ViewClearSel() {
  Tab* t = CurTab();
  if (t && t->sel.active) { t->sel.active = false; ViewInvalidate(); }
}
static void SelOrder(const Sel& s, int* pa, int* ca, int* pb, int* cb) {
  bool fwd = s.p0 < s.p1 || (s.p0 == s.p1 && s.c0 <= s.c1);
  if (fwd) { *pa = s.p0; *ca = s.c0; *pb = s.p1; *cb = s.c1; }
  else { *pa = s.p1; *ca = s.c1; *pb = s.p0; *cb = s.c0; }
}

void ViewSelectAll() {
  Tab* t = CurTab();
  if (!t || !t->doc || !G.pro) return;
  int p = ViewCurPage();
  auto pd = GetPageDataNow(t->doc, p, true);
  if (!pd || pd->ch.empty()) return;
  t->sel.active = true;
  t->sel.p0 = p; t->sel.c0 = 0; t->sel.p1 = p; t->sel.c1 = (int)pd->ch.size();
  ViewInvalidate();
}

void ViewCopy() {
  Tab* t = CurTab();
  if (!t || !t->doc || !ViewHasSel()) return;
  int pa, ca, pb, cb;
  SelOrder(t->sel, &pa, &ca, &pb, &cb);
  ws out;
  for (int p = pa; p <= pb; p++) {
    auto pd = GetPageDataNow(t->doc, p, true);
    if (!pd) continue;
    int s = p == pa ? ca : 0, e = p == pb ? cb : (int)pd->ch.size();
    s = max(s, 0); e = min(e, (int)pd->ch.size());
    for (int i = s; i < e; i++) out += pd->ch[i] ? pd->ch[i] : L' ';
    if (p != pb && !out.empty()) out += L"\r\n";
  }
  if (out.empty()) return;
  if (OpenClipboard(A_.main)) {
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (out.size() + 1) * sizeof(wchar_t));
    if (g) {
      wchar_t* p = (wchar_t*)GlobalLock(g);
      memcpy(p, out.c_str(), (out.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(g);
      SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
  }
  ShowToast(L"Скопировано");
}

// ---------------------------------------------------------------- search
void ViewSearchChanged() {
  Tab* t = CurTab();
  if (!t) return;
  ViewInvalidate();
}

void ViewShowHit(int idx) {
  Tab* t = CurTab();
  if (!t || !t->doc || idx < 0 || idx >= (int)t->hits.size()) return;
  ViewEnsureLayout(t);
  t->curHit = idx;
  const SearchHit& h = t->hits[idx];
  if (h.rects.empty()) { ViewGoPage(h.page, 0, true, false); return; }
  FRect r = h.rects[0];
  float ax, ay;
  PtToLocal(t, h.page, r.l, r.t, &ax, &ay);
  double docY = t->ytop[h.page] + ay;
  double docX = PageLeft(t, h.page) + ax;
  bool visY = docY - t->sy > S(30) && docY - t->sy < t->viewH - S(60);
  bool visX = docX - t->sx > S(10) && docX - t->sx < t->viewW - S(40);
  double ny = visY ? t->ty : docY - t->viewH / 3.0;
  double nx = visX ? t->tx : docX - t->viewW / 3.0;
  ScrollTo(t, nx, ny, true);
  ViewInvalidate();
  MainRedrawChrome();
}

void ViewFindStep(int dir) {
  Tab* t = CurTab();
  if (!t || t->hits.empty()) return;
  int n = (int)t->hits.size();
  int i = t->curHit;
  if (i < 0) {
    // first hit at or after the visible area
    int p = ViewCurPage();
    i = 0;
    for (int k = 0; k < n; k++) if (t->hits[k].page >= p) { i = k; break; }
    if (dir < 0) i = (i + n - 1) % n;
  } else {
    i = (i + dir + n) % n;
  }
  ViewShowHit(i);
}

// ---------------------------------------------------------------- links
void ViewFollowLink(const Link& l) {
  Tab* t = CurTab();
  if (!t) return;
  if (!l.url.empty()) {
    ws u = LowerW(l.url);
    if (u.rfind(L"http://", 0) == 0 || u.rfind(L"https://", 0) == 0 || u.rfind(L"mailto:", 0) == 0 || u.rfind(L"www.", 0) == 0) {
      ws url = l.url;
      if (u.rfind(L"www.", 0) == 0) url = L"https://" + url;
      ShellExecuteW(A_.main, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return;
  }
  if (l.page >= 0) {
    float y = 0;
    bool has = l.hasY && ResolveDestY(t->doc, l.page, l.yPdf, &y);
    ViewGoPage(l.page, has ? max(0.0f, y - 12) : 0, true, true);
  }
}

// ---------------------------------------------------------------- painting
static COLORREF PageBase() { return G.readMode == 1 ? RGB(244, 232, 200) : (G.readMode == 2 ? RGB(16, 16, 16) : RGB(255, 255, 255)); }

static void EnsureBack(HDC dc, int w, int h) {
  if (g_back && g_backW == w && g_backH == h) return;
  if (g_back) DeleteObject(g_back);
  if (g_backDC) DeleteDC(g_backDC);
  g_backDC = CreateCompatibleDC(dc);
  g_back = CreateCompatibleBitmap(dc, max(w, 1), max(h, 1));
  SelectObject(g_backDC, g_back);
  g_backW = w; g_backH = h;
}

static void PaintStart(HDC dc, RECT rc) {
  g_start.clear();
  int cx = (rc.right - rc.left) / 2;
  int top = max(S(40), (rc.bottom - S(560)) / 2);
  DrawAppIcon(dc, cx - S(48), top, S(96));
  RECT r = {rc.left, top + S(106), rc.right, top + S(140)};
  DrawTextW2(dc, L"UltraPDF", r, T.text, UiFont(S(26), true), DT_CENTER | DT_SINGLELINE);
  r = {rc.left, top + S(144), rc.right, top + S(170)};
  DrawTextW2(dc, L"Перетащите PDF в окно или нажмите «Открыть»", r, T.dim, UiFont(S(14)), DT_CENTER | DT_SINGLELINE);
  RECT b = {cx - S(80), top + S(184), cx + S(80), top + S(222)};
  bool hot = g_startHover == 0;
  FillRoundRect(dc, b, S(6), hot ? RGB(max(0, GetRValue(T.accent) - 20), max(0, GetGValue(T.accent) - 20), max(0, GetBValue(T.accent) - 20)) : T.accent);
  DrawTextW2(dc, L"Открыть…", b, RGB(255, 255, 255), UiFont(S(14), true), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  StartItem si;
  si.r = b; si.cmd = ID_OPEN;
  g_start.push_back(si);
  int y = top + S(250);
  if (!G.docs.empty()) {
    int lw = min(S(520), rc.right - rc.left - S(40));
    int lx = cx - lw / 2;
    RECT h = {lx, y, lx + lw, y + S(24)};
    DrawTextW2(dc, L"Недавние документы", h, T.dim, UiFont(S(13), true), DT_LEFT | DT_SINGLELINE);
    y += S(28);
    int shown = 0;
    for (auto& d : G.docs) {
      if (shown >= 10) break;
      RECT ir = {lx, y, lx + lw, y + S(32)};
      int idx = (int)g_start.size();
      if (g_startHover == idx) FillRoundRect(dc, ir, S(5), T.hover);
      RECT tr = {ir.left + S(10), ir.top, ir.right - S(10), ir.bottom};
      ws nm = BaseName(d.path);
      DrawTextW2(dc, nm, tr, T.text, UiFont(S(14)), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      StartItem it;
      it.r = ir; it.path = d.path; it.cmd = 0;
      g_start.push_back(it);
      y += S(34);
      shown++;
    }
  }
}

static void PaintDoc(HDC dc, RECT rc, Tab* t) {
  ViewEnsureLayout(t);
  if (!t->layoutOk) return;
  int vw = rc.right, vh = rc.bottom;
  int n = t->doc->npages;
  int first = PageAtDocY(t, t->sy);
  bool zooming = NowMs() < g_zoomBusy;
  COLORREF base = PageBase();
  COLORREF border = G.readMode == 2 ? RGB(70, 70, 70) : (T.dark ? RGB(60, 60, 60) : RGB(190, 190, 190));
  HBRUSH baseBr = CreateSolidBrush(base);
  HBRUSH borderBr = CreateSolidBrush(border);
  HDC mdc = CreateCompatibleDC(dc);
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  int docId = t->doc->id;
  int rotq = t->rot & 3;
  int zq = (int)lround(t->scale * 10000);
  ReqBegin(CONS_VIEW);
  CacheLock();
  int last = first;
  for (int i = first; i < n; i++) {
    int py = t->ytop[i] - (int)lround(t->sy);
    if (py >= vh) break;
    last = i;
    int pw = t->pwpx[i], ph = t->phpx[i];
    int px = PageLeft(t, i) - (int)lround(t->sx);
    RECT pr = {px, py, px + pw, py + ph};
    RECT br2 = {px - 1, py - 1, px + pw + 1, py + ph + 1};
    if (!A_.presentation) FrameRect(dc, &br2, borderBr);
    FillRect(dc, &pr, baseBr);
    bool single = pw <= 1500 && ph <= 1700;
    int tw = single ? pw : 1024, th = single ? ph : 768;
    int nx = (pw + tw - 1) / tw, ny = (ph + th - 1) / th;
    // preview as a fallback
    TileKey pk;
    pk.doc = docId; pk.page = i; pk.rot = rotq; pk.mode = G.readMode; pk.kind = 1; pk.zq = 0;
    Tile pv;
    bool havePv = CacheGetLocked(pk, &pv);
    bool needPv = false;
    for (int ty = 0; ty < ny; ty++) {
      int oy = ty * th;
      int h = min(th, ph - oy);
      if (py + oy >= vh || py + oy + h <= 0) continue;
      for (int tx = 0; tx < nx; tx++) {
        int ox = tx * tw;
        int w = min(tw, pw - ox);
        if (px + ox >= vw || px + ox + w <= 0) continue;
        TileKey k;
        k.doc = docId; k.page = i; k.tx = tx; k.ty = ty; k.zq = zq; k.rot = rotq; k.mode = G.readMode; k.kind = 0;
        Tile tl;
        if (CacheGetLocked(k, &tl)) {
          HGDIOBJ old = SelectObject(mdc, tl.bmp);
          BitBlt(dc, px + ox, py + oy, w, h, mdc, 0, 0, SRCCOPY);
          SelectObject(mdc, old);
        } else {
          if (!zooming) ReqTile(CONS_VIEW, t->doc, k, pw, ph, ox, oy, w, h, 1);
          needPv = true;
          if (havePv) {
            HGDIOBJ old = SelectObject(mdc, pv.bmp);
            int sx0 = (int)((double)ox * pv.w / pw), sy0 = (int)((double)oy * pv.h / ph);
            int sw = max(1, (int)((double)w * pv.w / pw)), sh = max(1, (int)((double)h * pv.h / ph));
            StretchBlt(dc, px + ox, py + oy, w, h, mdc, sx0, sy0, sw, sh, SRCCOPY);
            SelectObject(mdc, old);
          }
        }
      }
    }
    if (needPv && !havePv && !zooming) {
      double pvs = min(1.0, 760.0 / max(pw, 1));
      pvs = min(pvs, 1100.0 / max(ph, 1));
      int vw2 = max(8, (int)(pw * pvs)), vh2 = max(8, (int)(ph * pvs));
      if (!(single && pw <= 800)) ReqTile(CONS_VIEW, t->doc, pk, vw2, vh2, 0, 0, vw2, vh2, 0);
    }
    if (G.pro && !zooming && !GetPageDataNow(t->doc, i, false)) ReqPageData(CONS_VIEW, t->doc, i, 6);
  }
  // prefetch: whole small pages (by memory budget) and tiles of big pages around the view, nearest first, ahead first
  if (!zooming) {
    static int dirS = 1;
    if (t->ty > t->sy + 0.5) dirS = 1; else if (t->ty < t->sy - 0.5) dirS = -1;
    std::vector<int> ord;
    if (dirS > 0) { for (int i = first; i < n; i++) ord.push_back(i); for (int i = first - 1; i >= 0; i--) ord.push_back(i); }
    else { for (int i = last; i >= 0; i--) ord.push_back(i); for (int i = last + 1; i < n; i++) ord.push_back(i); }
    int mAhead = vh * 8 / 5, mBehind = vh * 3 / 5;
    int mTop = dirS > 0 ? mBehind : mAhead, mBot = dirS > 0 ? mAhead : mBehind;
    double bytesAhead = 0, bytesBehind = 0;
    const double LIM_AHEAD = 190e6, LIM_BEHIND = 60e6;
    int rankA = 0, rankB = 0, jobs = 0;
    for (int i : ord) {
      if (jobs >= 48 || (bytesAhead >= LIM_AHEAD && bytesBehind >= LIM_BEHIND)) break;
      bool vis = i >= first && i <= last;
      bool ahead = vis || (dirS > 0 ? i > last : i < first);
      int pw = t->pwpx[i], ph = t->phpx[i];
      bool single = pw <= 1500 && ph <= 1700;
      int py = t->ytop[i] - (int)lround(t->sy);
      int px = PageLeft(t, i) - (int)lround(t->sx);
      if (!vis && (ahead ? bytesAhead >= LIM_AHEAD : bytesBehind >= LIM_BEHIND)) continue;
      if (!vis) {
        TileKey pk;
        pk.doc = docId; pk.page = i; pk.rot = rotq; pk.mode = G.readMode; pk.kind = 1; pk.zq = 0;
        Tile pv;
        if (!(single && pw <= 800) && !CacheGetLocked(pk, &pv)) {
          double pvs = min(1.0, 760.0 / max(pw, 1));
          pvs = min(pvs, 1100.0 / max(ph, 1));
          int vw2 = max(8, (int)(pw * pvs)), vh2 = max(8, (int)(ph * pvs));
          ReqTile(CONS_VIEW, t->doc, pk, vw2, vh2, 0, 0, vw2, vh2, 2);
          jobs++;
        }
      }
      if (single) {
        if (vis) continue;                                  // handled above
        (ahead ? bytesAhead : bytesBehind) += (double)pw * ph * 4;
        TileKey k;
        k.doc = docId; k.page = i; k.tx = 0; k.ty = 0; k.zq = zq; k.rot = rotq; k.mode = G.readMode; k.kind = 0;
        Tile tl;
        if (!CacheGetLocked(k, &tl)) {
          ReqTile(CONS_VIEW, t->doc, k, pw, ph, 0, 0, pw, ph, ahead ? 10 + rankA : 100 + rankB);
          jobs++;
        }
        if (ahead) rankA++; else rankB++;
        continue;
      }
      if (py >= vh + mBot || py + ph <= -mTop) continue;
      int tw = 1024, th = 768;
      int nx = (pw + tw - 1) / tw, ny = (ph + th - 1) / th;
      for (int ty = 0; ty < ny; ty++) {
        int oy = ty * th, h = min(th, ph - oy);
        int top = py + oy, bot = top + h;
        if (bot <= -mTop || top >= vh + mBot) continue;
        if (top < vh && bot > 0) continue;                 // visible: handled above
        bool tahead = dirS > 0 ? top >= vh : bot <= 0;
        for (int tx = 0; tx < nx; tx++) {
          int ox = tx * tw, w = min(tw, pw - ox);
          if (px + ox >= vw || px + ox + w <= 0) continue;
          TileKey k;
          k.doc = docId; k.page = i; k.tx = tx; k.ty = ty; k.zq = zq; k.rot = rotq; k.mode = G.readMode; k.kind = 0;
          Tile tl;
          if (CacheGetLocked(k, &tl)) continue;
          ReqTile(CONS_VIEW, t->doc, k, pw, ph, ox, oy, w, h, tahead ? 10 + rankA : 100 + rankB);
          jobs++;
        }
      }
      if (ahead) rankA++; else rankB++;
    }
  }
  CacheUnlock();
  ReqEnd(CONS_VIEW);
  DeleteDC(mdc);
  DeleteObject(baseBr);
  DeleteObject(borderBr);

  // overlays: search hits
  if (!t->hits.empty()) {
    auto lo = std::lower_bound(t->hits.begin(), t->hits.end(), first, [](const SearchHit& h, int p) { return h.page < p; });
    for (auto it = lo; it != t->hits.end() && it->page <= last; ++it) {
      bool curH = (int)(it - t->hits.begin()) == t->curHit;
      for (const FRect& rf : it->rects) {
        RECT r = RectToScreen(t, it->page, rf);
        if (curH) { FillRectA(dc, r, RGB(255, 140, 0), 120); FrameRoundRect(dc, r, 2, RGB(230, 110, 0), 1.5f); }
        else FillRectA(dc, r, RGB(255, 214, 0), 105);
      }
    }
  }
  // overlay: selection
  if (G.pro && ViewHasSel()) {
    int pa, ca, pb, cb;
    SelOrder(t->sel, &pa, &ca, &pb, &cb);
    COLORREF selc = T.accent;
    for (int p = max(pa, first); p <= min(pb, last); p++) {
      auto pd = GetPageDataNow(t->doc, p, false);
      if (!pd) continue;
      int s = p == pa ? ca : 0, e = p == pb ? cb : (int)pd->ch.size();
      e = min(e, (int)pd->box.size());
      FRect run;
      bool have = false;
      auto flush = [&]() {
        if (have) { RECT r = RectToScreen(t, p, run); FillRectA(dc, r, selc, 95); }
        have = false;
      };
      for (int i = max(s, 0); i < e; i++) {
        const FRect& b = pd->box[i];
        if (b.r <= b.l || b.b <= b.t) continue;
        if (have) {
          float oh = min(run.b, b.b) - max(run.t, b.t);
          float mh = min(run.b - run.t, b.b - b.t);
          if (oh > mh * 0.5f && b.l < run.r + (b.b - b.t) * 2) {
            run.l = min(run.l, b.l); run.r = max(run.r, b.r); run.t = min(run.t, b.t); run.b = max(run.b, b.b);
            continue;
          }
          flush();
        }
        run = b; have = true;
      }
      flush();
    }
  }
}

static void PaintFullscreenButton(HDC dc, RECT rc) {
  int sz = S(44);
  g_closeBtn = {rc.right - sz - S(8), S(8), rc.right - S(8), S(8) + sz};
  FillRoundRect(dc, g_closeBtn, sz / 2, RGB(0, 0, 0), 170);
  FrameRoundRect(dc, g_closeBtn, sz / 2, RGB(255, 255, 255), 1.5f);
  FRect ir = {(float)g_closeBtn.left + S(11), (float)g_closeBtn.top + S(11), (float)g_closeBtn.right - S(11), (float)g_closeBtn.bottom - S(11)};
  DrawIcon(dc, IC_CLOSE, ir, RGB(255, 255, 255), 2.0f);
}

double g_paintAvg = 0; int g_paintN = 0; int g_ticks = 0; int g_timerRet = -5; int g_paintTotal = 0;
static void OnPaint(HWND hw) {
  uint64_t t0 = GetTickCount64();
  PAINTSTRUCT ps;
  HDC dc0 = BeginPaint(hw, &ps);
  RECT rc;
  GetClientRect(hw, &rc);
  EnsureBack(dc0, rc.right, rc.bottom);
  HDC dc = g_backDC;
  Tab* t = CurTab();
  COLORREF bg = A_.presentation ? RGB(0, 0, 0) : T.viewbg;
  if (t && t->doc && G.readMode == 2 && !A_.presentation) bg = RGB(34, 34, 34);
  HBRUSH b = CreateSolidBrush(bg);
  FillRect(dc, &rc, b);
  DeleteObject(b);
  if (!t) {
    PaintStart(dc, rc);
  } else if (t->loading) {
    DrawTextW2(dc, L"Загрузка…", rc, T.dim, UiFont(S(16)), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  } else if (!t->doc) {
    DrawTextW2(dc, t->error.empty() ? L"Не удалось открыть документ" : t->error, rc, T.dim, UiFont(S(15)), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  } else {
    PaintDoc(dc, rc, t);
  }
  if (A_.fullscreen) PaintFullscreenButton(dc, rc);
  DrawToast(dc, rc);
  BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
  EndPaint(hw, &ps);
  g_paintAvg = (g_paintAvg * g_paintN + (double)(GetTickCount64() - t0)) / (g_paintN + 1);
  if (g_paintN < 50) g_paintN++;
  g_paintTotal++;
}

// ---------------------------------------------------------------- mouse and keyboard
static void UpdateSelFocus(Tab* t, int mx, int my) {
  int p, c;
  bool inside;
  if (!CaretAt(t, mx, my, &p, &c, &inside)) return;
  t->sel.p1 = p;
  t->sel.c1 = c;
  t->sel.active = true;
  ViewInvalidate();
}

static void SelectWordAt(Tab* t, int mx, int my, bool line) {
  int p;
  float x, y;
  bool exact;
  if (!HitPage(t, mx, my, &p, &x, &y, &exact)) return;
  auto pd = GetPageDataNow(t->doc, p, true);
  if (!pd) return;
  bool inside, after;
  int c = NearestChar(*pd, x, y, &inside, &after);
  if (c < 0) return;
  int n = (int)pd->ch.size();
  int s = c, e = c + 1;
  if (line) {
    while (s > 0 && pd->ch[s - 1] != L'\n' && pd->ch[s - 1] != L'\r') s--;
    while (e < n && pd->ch[e] != L'\n' && pd->ch[e] != L'\r') e++;
  } else if (IsWordCh(pd->ch[c])) {
    while (s > 0 && IsWordCh(pd->ch[s - 1])) s--;
    while (e < n && IsWordCh(pd->ch[e])) e++;
  }
  t->sel.active = true;
  t->sel.p0 = p; t->sel.c0 = s; t->sel.p1 = p; t->sel.c1 = e;
  ViewInvalidate();
}

static int StartHit(int x, int y) {
  for (size_t i = 0; i < g_start.size(); i++)
    if (PtInRect(&g_start[i].r, {x, y})) return (int)i;
  return -1;
}

static LRESULT OnSetCursor(HWND hw) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(hw, &pt);
  Tab* t = CurTab();
  LPCWSTR cur = IDC_ARROW;
  if (A_.fullscreen && PtInRect(&g_closeBtn, pt)) cur = IDC_HAND;
  else if (!t) { if (StartHit(pt.x, pt.y) >= 0) cur = IDC_HAND; }
  else if (t->doc && t->layoutOk) {
    if (g_drag == D_PAN) cur = IDC_SIZEALL;
    else if (g_drag == D_SEL) cur = IDC_IBEAM;
    else {
      int p;
      float x, y;
      bool exact;
      if (G.pro && HitPage(t, pt.x, pt.y, &p, &x, &y, &exact) && exact) {
        const Link* l = LinkAt(t, p, x, y);
        if (l) cur = IDC_HAND;
        else {
          auto pd = GetPageDataNow(t->doc, p, false);
          if (pd) {
            bool inside, after;
            NearestChar(*pd, x, y, &inside, &after);
            if (inside) cur = IDC_IBEAM;
          }
        }
      }
    }
  }
  SetCursor(LoadCursorW(nullptr, cur));
  return TRUE;
}

static void ShowCtxMenu(HWND hw, int sx, int sy) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  HMENU m = CreatePopupMenu();
  if (G.pro) {
    AppendMenuW(m, MF_STRING | (ViewHasSel() ? 0 : MF_GRAYED), ID_COPY, L"Копировать\tCtrl+C");
    AppendMenuW(m, MF_STRING, ID_SELALL, L"Выделить всё на странице\tCtrl+A");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (IsBookmarked(t, ViewCurPage()) ? MF_CHECKED : 0), ID_BOOKMARK, L"Закладка на этой странице\tCtrl+D");
    AppendMenuW(m, MF_STRING, ID_ROT_CW, L"Повернуть по часовой\tCtrl+R");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  }
  AppendMenuW(m, MF_STRING, ID_FIT_W, L"По ширине\tCtrl+9");
  AppendMenuW(m, MF_STRING, ID_FIT_P, L"Страница целиком\tCtrl+8");
  AppendMenuW(m, MF_STRING, ID_ZOOM_100, L"Реальный размер\tCtrl+0");
  if (t->back.size()) AppendMenuW(m, MF_STRING, ID_BACK, L"Назад\tAlt+←");
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, A_.main, nullptr);
  DestroyMenu(m);
  if (cmd) DoCmd(cmd);
}

static LRESULT CALLBACK ViewProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: g_hw = hw; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: OnPaint(hw); return 0;
    case WM_SIZE: {
      g_vw = LOWORD(lp); g_vh = HIWORD(lp);
      Tab* t = CurTab();
      if (t && t->doc) {
        int p = ViewCurPage();
        bool had = t->layoutOk && p < (int)t->ytop.size();
        double offPx = had ? t->sy - t->ytop[p] : 0, offPt = had ? offPx / t->scale : 0;
        t->layoutOk = false;
        ViewEnsureLayout(t);
        if (t->ytop.size() > (size_t)p) {
          t->sy = t->ytop[p] + (offPx < 0 ? offPx : offPt * t->scale);
          t->ty = t->sy;
        }
        if (t->fit) CenterX(t);
        ClampScroll(t);
        ViewSync();
        MainUpdatePageBox();
      }
      ViewInvalidate();
      return 0;
    }
    case WM_VSCROLL: case WM_HSCROLL: {
      Tab* t = CurTab();
      if (!t || !t->doc) return 0;
      bool vert = msg == WM_VSCROLL;
      SCROLLINFO si = {};
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(hw, vert ? SB_VERT : SB_HORZ, &si);
      double pos = vert ? t->sy : t->sx;
      int step = S(50);
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= step; break;
        case SB_LINEDOWN: pos += step; break;
        case SB_PAGEUP: pos -= (vert ? t->viewH : t->viewW) * 0.9; break;
        case SB_PAGEDOWN: pos += (vert ? t->viewH : t->viewW) * 0.9; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = 1e9; break;
        default: return 0;
      }
      if (vert) t->sy = pos; else t->sx = pos;
      t->tx = t->sx; t->ty = t->sy;
      ClampScroll(t);
      AfterScrollChange(t);
      return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
      Tab* t = CurTab();
      if (!t || !t->doc) return 0;
      int delta = GET_WHEEL_DELTA_WPARAM(wp);
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hw, &pt);
      ViewEnsureLayout(t);
      if (msg == WM_MOUSEWHEEL && (LOWORD(wp) & MK_CONTROL)) {
        ViewZoom(t->zoom * pow(1.0 + 0.1, delta / 120.0), pt.x, pt.y);
        return 0;
      }
      if (A_.presentation) {
        static int acc = 0;
        acc += delta;
        if (abs(acc) >= 120) { ViewPageStep(acc > 0 ? -1 : 1); acc = 0; }
        return 0;
      }
      double amt = -delta / 120.0 * S(100);
      if (msg == WM_MOUSEHWHEEL) ViewScrollBy((int)(-amt), 0, true);
      else if (LOWORD(wp) & MK_SHIFT) ViewScrollBy((int)amt, 0, true);
      else ViewScrollBy(0, (int)amt, true);
      return 0;
    }
    case WM_APP_VSYNC: {
      g_vsPending = false;
      if (!g_anim) return 0;
      g_ticks++;
      Tab* t = CurTab();
      double now = QpcMs();
      double dt = now - g_lastQpcF;
      g_lastQpcF = now;
      if (!t || !t->doc) { g_anim = false; return 0; }
      double k = 1.0 - exp(-min(max(dt, 1.0), 100.0) / 70.0);
      double dy = t->ty - t->sy, dx = t->tx - t->sx;
      if (fabs(dy) < 0.6 && fabs(dx) < 0.6) { t->sy = t->ty; t->sx = t->tx; g_anim = false; }
      else { t->sy += dy * k; t->sx += dx * k; }
      AfterScrollChange(t);
      UpdateWindow(hw);
      return 0;
    }
    case WM_TIMER: {
      if (false) {
      } else if (wp == TMR_ZOOM) {
        KillTimer(hw, TMR_ZOOM);
        ViewInvalidate();
      } else if (wp == TMR_DRAG) {
        Tab* t = CurTab();
        if (t && g_drag == D_SEL) {
          RECT rc;
          GetClientRect(hw, &rc);
          int dy = 0;
          if (g_lastPt.y < S(20)) dy = -(S(20) - g_lastPt.y) / 2 - 4;
          else if (g_lastPt.y > rc.bottom - S(20)) dy = (g_lastPt.y - (rc.bottom - S(20))) / 2 + 4;
          if (dy) { ViewScrollBy(0, dy, false); UpdateSelFocus(t, g_lastPt.x, g_lastPt.y); }
        }
      }
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
      SetFocus(hw);
      int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
      Tab* t = CurTab();
      if (A_.fullscreen && PtInRect(&g_closeBtn, {mx, my})) { MainSetFullscreen(false, false); return 0; }
      if (!t) {
        int i = StartHit(mx, my);
        if (i >= 0) {
          if (g_start[i].cmd) DoCmd(g_start[i].cmd); else OpenPath(g_start[i].path);
        }
        return 0;
      }
      if (!t->doc) return 0;
      ViewEnsureLayout(t);
      uint32_t now = GetMessageTime();
      POINT pt = {mx, my};
      bool multi = (now - g_lastClickTime) < GetDoubleClickTime() && abs(mx - g_lastClickPt.x) < 6 && abs(my - g_lastClickPt.y) < 6;
      g_clickCount = multi ? g_clickCount + 1 : 1;
      g_lastClickTime = now; g_lastClickPt = pt;
      g_downPt = pt; g_lastPt = pt;
      g_downSx = t->sx; g_downSy = t->sy;
      SetCapture(hw);
      if (A_.presentation) { g_drag = D_NONE; ReleaseCapture(); ViewPageStep(1); return 0; }
      int p;
      float x, y;
      bool exact;
      g_drag = D_PAN;
      if (G.pro && HitPage(t, mx, my, &p, &x, &y, &exact) && exact) {
        const Link* l = LinkAt(t, p, x, y);
        if (l) { g_pendLink = *l; g_drag = D_PENDLINK; }
        else {
          auto pd = GetPageDataNow(t->doc, p, true);
          if (pd) {
            bool inside, after;
            NearestChar(*pd, x, y, &inside, &after);
            if (inside) {
              if (g_clickCount == 2) { SelectWordAt(t, mx, my, false); g_drag = D_NONE; }
              else if (g_clickCount >= 3) { SelectWordAt(t, mx, my, true); g_drag = D_NONE; g_clickCount = 0; }
              else {
                int pp, cc;
                bool ins;
                if (CaretAt(t, mx, my, &pp, &cc, &ins)) {
                  t->sel.active = true;
                  t->sel.p0 = t->sel.p1 = pp;
                  t->sel.c0 = t->sel.c1 = cc;
                  g_drag = D_SEL;
                  SetTimer(hw, TMR_DRAG, 40, nullptr);
                }
              }
            }
          }
        }
      }
      if (g_drag == D_PAN) ViewClearSel();
      ViewInvalidate();
      return 0;
    }
    case WM_MOUSEMOVE: {
      int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
      Tab* t = CurTab();
      if (!g_trackLeave) { TRACKMOUSEEVENT te = {sizeof(te), TME_LEAVE, hw, 0}; TrackMouseEvent(&te); g_trackLeave = true; }
      if (!t) {
        int h = StartHit(mx, my);
        if (h != g_startHover) { g_startHover = h; ViewInvalidate(); }
        return 0;
      }
      g_lastPt = {mx, my};
      if (g_drag == D_PENDLINK) {
        if (abs(mx - g_downPt.x) > 4 || abs(my - g_downPt.y) > 4) g_drag = D_PAN;
      }
      if (g_drag == D_PAN && (wp & MK_LBUTTON)) {
        t->sx = g_downSx - (mx - g_downPt.x);
        t->sy = g_downSy - (my - g_downPt.y);
        t->tx = t->sx; t->ty = t->sy;
        ClampScroll(t);
        AfterScrollChange(t);
      } else if (g_drag == D_SEL) {
        UpdateSelFocus(t, mx, my);
      }
      return 0;
    }
    case WM_MOUSELEAVE: g_trackLeave = false; if (g_startHover != -1) { g_startHover = -1; ViewInvalidate(); } return 0;
    case WM_LBUTTONUP: {
      Drag was = g_drag;
      Link pend = g_pendLink;
      g_drag = D_NONE;
      if (GetCapture() == hw) ReleaseCapture();
      KillTimer(hw, TMR_DRAG);
      if (was == D_PENDLINK) ViewFollowLink(pend);
      else if (was == D_SEL) {
        Tab* t = CurTab();
        if (t && t->sel.p0 == t->sel.p1 && t->sel.c0 == t->sel.c1) t->sel.active = false;
      }
      ViewInvalidate();
      return 0;
    }
    case WM_MBUTTONDOWN: {
      Tab* t = CurTab();
      if (!t || !t->doc) return 0;
      SetFocus(hw);
      SetCapture(hw);
      g_drag = D_PAN;
      g_downPt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      g_downSx = t->sx; g_downSy = t->sy;
      return 0;
    }
    case WM_MBUTTONUP: if (GetCapture() == hw) ReleaseCapture(); g_drag = D_NONE; return 0;
    case WM_CAPTURECHANGED: if ((HWND)lp != hw) { g_drag = D_NONE; KillTimer(hw, TMR_DRAG); } return 0;
    case WM_RBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ClientToScreen(hw, &pt);
      ShowCtxMenu(hw, pt.x, pt.y);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) return OnSetCursor(hw);
      break;
    case WM_DROPFILES: return SendMessageW(A_.main, WM_DROPFILES, wp, lp);
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

bool ViewRegister(HINSTANCE inst) {
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = ViewProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"UltraPDF.View";
  return RegisterClassExW(&wc) != 0;
}
HWND ViewCreate(HWND parent) {
  return CreateWindowExW(WS_EX_ACCEPTFILES, L"UltraPDF.View", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 100, 100, parent,
                         nullptr, A_.inst, nullptr);
}
