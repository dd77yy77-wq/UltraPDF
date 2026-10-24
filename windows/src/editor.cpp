// UltraPDF Editor: the editing engine. Edits are made on the live PDFium document under g_pdf; after each one the document is
// serialized and re-loaded (annotation appearances are generated on load, and the bytes are the undo history).
#include "editor.h"
#include <thread>
#ifdef APP_EDITOR
#include <fstream>
#include <sstream>

// ------------------------------------------------------------------ page geometry
// PDF user space <-> "page points": top-left origin, page orientation (/Rotate applied), the way the viewer addresses a page.
struct PXf {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;      // pdf = (a*x + c*y + e, b*x + d*y + f)
  void ToPdf(double x, double y, double* px, double* py) const { *px = a * x + c * y + e; *py = b * x + d * y + f; }
  void ToTl(double px, double py, double* x, double* y) const {
    double det = a * d - b * c;
    if (fabs(det) < 1e-12) { *x = px; *y = py; return; }
    double dx = px - e, dy = py - f;
    *x = (d * dx - c * dy) / det;
    *y = (-b * dx + a * dy) / det;
  }
};
static PXf MakeXf(FPDF_PAGE pg, float pw, float ph) {
  const int Q = 100;
  int W = max(1, (int)lround(pw * Q)), H = max(1, (int)lround(ph * Q));
  double x0, y0, x1, y1, x2, y2;
  PA.FPDF_DeviceToPage(pg, 0, 0, W, H, 0, 0, 0, &x0, &y0);
  PA.FPDF_DeviceToPage(pg, 0, 0, W, H, 0, W, 0, &x1, &y1);
  PA.FPDF_DeviceToPage(pg, 0, 0, W, H, 0, 0, H, &x2, &y2);
  PXf m;
  double sw = W / (double)Q, sh = H / (double)Q;
  m.a = (x1 - x0) / sw; m.b = (y1 - y0) / sw;
  m.c = (x2 - x0) / sh; m.d = (y2 - y0) / sh;
  m.e = x0; m.f = y0;
  return m;
}
static FRect TlBoxOfPdf(const PXf& xf, float l, float b, float r, float t) {
  double xs[4], ys[4];
  xf.ToTl(l, b, &xs[0], &ys[0]);
  xf.ToTl(r, b, &xs[1], &ys[1]);
  xf.ToTl(r, t, &xs[2], &ys[2]);
  xf.ToTl(l, t, &xs[3], &ys[3]);
  FRect o;
  o.l = (float)min(min(xs[0], xs[1]), min(xs[2], xs[3]));
  o.r = (float)max(max(xs[0], xs[1]), max(xs[2], xs[3]));
  o.t = (float)min(min(ys[0], ys[1]), min(ys[2], ys[3]));
  o.b = (float)max(max(ys[0], ys[1]), max(ys[2], ys[3]));
  return o;
}
static void PdfRectOfTl(const PXf& xf, const FRect& r, float* l, float* b, float* rr, float* t) {
  double xs[4], ys[4];
  xf.ToPdf(r.l, r.t, &xs[0], &ys[0]);
  xf.ToPdf(r.r, r.t, &xs[1], &ys[1]);
  xf.ToPdf(r.r, r.b, &xs[2], &ys[2]);
  xf.ToPdf(r.l, r.b, &xs[3], &ys[3]);
  *l = (float)min(min(xs[0], xs[1]), min(xs[2], xs[3]));
  *rr = (float)max(max(xs[0], xs[1]), max(xs[2], xs[3]));
  *b = (float)min(min(ys[0], ys[1]), min(ys[2], ys[3]));
  *t = (float)max(max(ys[0], ys[1]), max(ys[2], ys[3]));
}

// ------------------------------------------------------------------ content marks: editor-made page objects are tagged
static const char* kMark = "UPE";
static int g_grpSeq = 0;
static int NewGroupId() {
  g_grpSeq++;
  return (int)(((uint32_t)(time(nullptr) & 0x3FFFFF) << 8) | (uint32_t)(g_grpSeq & 0xFF)) & 0x7FFFFFFF;
}
static void TagObject(Doc* d, FPDF_PAGEOBJECT o, int kind, int grp, int extra) {
  FPDF_PAGEOBJECTMARK m = PA.FPDFPageObj_AddMark(o, kMark);
  if (!m) return;
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "k", kind);
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "g", grp);
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "s", extra);
}
static FPDF_PAGEOBJECTMARK FindTagMark(FPDF_PAGEOBJECT o) {
  int n = PA.FPDFPageObj_CountMarks(o);
  for (int i = 0; i < n; i++) {
    FPDF_PAGEOBJECTMARK m = PA.FPDFPageObj_GetMark(o, i);
    if (!m) continue;
    unsigned long need = 0;
    if (!PA.FPDFPageObjMark_GetName(m, nullptr, 0, &need) || need != 8) continue;
    unsigned short buf[8] = {};
    if (!PA.FPDFPageObjMark_GetName(m, buf, need, &need)) continue;
    if (buf[0] == 'U' && buf[1] == 'P' && buf[2] == 'E' && buf[3] == 0) return m;
  }
  return nullptr;
}
static bool ReadTag(FPDF_PAGEOBJECT o, int* kind, int* grp, int* extra) {
  FPDF_PAGEOBJECTMARK m = FindTagMark(o);
  if (!m) return false;
  int k = 0, g = 0, s = 0;
  if (!PA.FPDFPageObjMark_GetParamIntValue(m, "g", &g)) return false;
  PA.FPDFPageObjMark_GetParamIntValue(m, "k", &k);
  PA.FPDFPageObjMark_GetParamIntValue(m, "s", &s);
  if (kind) *kind = k;
  if (grp) *grp = g;
  if (extra) *extra = s;
  return true;
}

struct TagObj { int idx; int kind, grp, extra; FRect box; FPDF_PAGEOBJECT o; };
static void EnumTagged(FPDF_PAGE pg, const PXf& xf, std::vector<TagObj>* out) {
  int n = PA.FPDFPage_CountObjects(pg);
  for (int i = 0; i < n; i++) {
    FPDF_PAGEOBJECT o = PA.FPDFPage_GetObject(pg, i);
    if (!o) continue;
    TagObj t;
    if (PA.FPDFPageObj_CountMarks(o) <= 0 || !ReadTag(o, &t.kind, &t.grp, &t.extra)) continue;
    float l, b, r, tp;
    if (!PA.FPDFPageObj_GetBounds(o, &l, &b, &r, &tp)) continue;
    t.idx = i; t.o = o;
    t.box = TlBoxOfPdf(xf, l, b, r, tp);
    out->push_back(t);
  }
}
static FRect UnionBox(const std::vector<TagObj>& v, int grp, bool* any) {
  FRect u;
  bool have = false;
  for (auto& t : v) {
    if (t.grp != grp) continue;
    if (!have) { u = t.box; have = true; }
    else { u.l = min(u.l, t.box.l); u.t = min(u.t, t.box.t); u.r = max(u.r, t.box.r); u.b = max(u.b, t.box.b); }
  }
  if (any) *any = have;
  return u;
}

// ------------------------------------------------------------------ transactions
static void TrimUndo(Doc* d) {
  size_t total = 0;
  for (auto& s : d->undo) total += s.size();
  int dropped = 0;
  while (!d->undo.empty() && (d->undo.size() > 60 || total > (size_t)600 * 1024 * 1024)) {
    total -= d->undo.front().size();
    d->undo.erase(d->undo.begin());
    dropped++;
  }
  if (dropped) { d->saveMark -= dropped; if (d->saveMark < 0) d->saveMark = -1; }
}

static void AfterEdit(Tab* t, bool structural, const std::vector<int>& pages) {
  Doc* d = t->doc.get();
  DocClearPageData(d);
  if (structural) CachePurgeDoc(d->id);
  else for (int p : pages) CachePurgePage(d->id, p);
  t->hits.clear();
  t->curHit = -1;
  t->sel.active = false;
  if (structural) EdHandleDocReplaced(t, true);
  else {
    ViewInvalidate();
    SideInvalidate();
    MainRedrawChrome();
  }
}

// runs `fn` on the live document; when it succeeds the result becomes the new state (one undo step), otherwise nothing changes
static bool EdEdit(Tab* t, bool structural, const std::vector<int>& pages, const std::function<bool(Doc*)>& fn) {
  if (!t || !t->doc) return false;
  Doc* d = t->doc.get();
  SearchCancel();
  bool ok;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    std::string before = d->src;                          // fn may reload the document on the way (scratch pages), so keep the start state here
    ok = fn(d);
    std::string nb;
    if (ok) ok = DocSerialize(d, &nb);
    if (!ok) {
      DocReload(d, std::string(before));                  // drop whatever the failed edit left behind
    } else if (!DocReload(d, std::move(nb))) {
      ok = false;
      DocReload(d, std::string(before));
    } else {
      if (d->saveMark > (int)d->undo.size()) d->saveMark = -1;
      d->undo.push_back(std::move(before));
      d->redo.clear();
      TrimUndo(d);
    }
    d->edited = d->undo.size() != (size_t)d->saveMark || d->saveMark < 0;
  }
  if (!ok) { ShowToast(L"Не удалось применить изменение"); structural = true; }
  AfterEdit(t, structural, pages);
  return ok;
}

bool EdUndo(Tab* t) {
  if (!t || !t->doc || t->doc->undo.empty()) return false;
  Doc* d = t->doc.get();
  SearchCancel();
  bool ok = false;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    std::string prev = std::move(d->undo.back());
    d->undo.pop_back();
    std::string old;
    std::string keep = prev;
    if (DocReload(d, std::move(prev), &old)) { d->redo.push_back(std::move(old)); ok = true; }
    else d->undo.push_back(std::move(keep));
    d->edited = d->undo.size() != (size_t)d->saveMark || d->saveMark < 0;
  }
  AfterEdit(t, true, {});
  return ok;
}
bool EdRedo(Tab* t) {
  if (!t || !t->doc || t->doc->redo.empty()) return false;
  Doc* d = t->doc.get();
  SearchCancel();
  bool ok = false;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    std::string next = std::move(d->redo.back());
    d->redo.pop_back();
    std::string old;
    std::string keep = next;
    if (DocReload(d, std::move(next), &old)) { d->undo.push_back(std::move(old)); ok = true; }
    else d->redo.push_back(std::move(keep));
    d->edited = d->undo.size() != (size_t)d->saveMark || d->saveMark < 0;
  }
  AfterEdit(t, true, {});
  return ok;
}

// ------------------------------------------------------------------ fonts
struct FontFile { const wchar_t* key; const wchar_t* file; const wchar_t* alt; };
static const FontFile kFonts[] = {
    {L"Arial", L"arial.ttf", L"\\usr\\share\\fonts\\truetype\\liberation\\LiberationSans-Regular.ttf"},
    {L"Times New Roman", L"times.ttf", L"\\usr\\share\\fonts\\truetype\\liberation\\LiberationSerif-Regular.ttf"},
    {L"Courier New", L"cour.ttf", L"\\usr\\share\\fonts\\truetype\\liberation\\LiberationMono-Regular.ttf"},
    {L"Segoe UI", L"segoeui.ttf", L"\\usr\\share\\fonts\\truetype\\dejavu\\DejaVuSans.ttf"},
    {L"Calibri", L"calibri.ttf", L"\\usr\\share\\fonts\\truetype\\crosextra\\Carlito-Regular.ttf"},
};
static ws FontPath(const wchar_t* key) {
  wchar_t win[MAX_PATH];
  GetWindowsDirectoryW(win, MAX_PATH);
  for (auto& f : kFonts) {
    if (wcscmp(f.key, key) != 0) continue;
    ws p = ws(win) + L"\\Fonts\\" + f.file;
    if (FileExists(p)) return p;
    ws q = ws(L"Z:") + f.alt;                              // Wine, for the tests
    if (FileExists(q)) return q;
    return L"";
  }
  return L"";
}
std::vector<ws> EdAvailableFonts() {
  std::vector<ws> v;
  for (auto& f : kFonts) if (!FontPath(f.key).empty()) v.push_back(f.key);
  if (v.empty()) v.push_back(L"Arial");
  return v;
}
static FPDF_FONT LoadDocFont(Doc* d, const ws& key) {
  auto it = d->fonts.find(key);
  if (it != d->fonts.end()) return (FPDF_FONT)it->second;
  ws path = FontPath(key.c_str());
  if (path.empty()) path = FontPath(L"Arial");
  if (path.empty()) return nullptr;
  std::string data;
  if (!ReadFileAll(path, &data) || data.empty()) return nullptr;
  FPDF_FONT f = PA.FPDFText_LoadFont(d->h, (const uint8_t*)data.data(), (uint32_t)data.size(), FPDF_FONT_TRUETYPE, 1);
  if (f) d->fonts[key] = f;
  return f;
}

// ------------------------------------------------------------------ helpers
static void ColorOf(COLORREF c, unsigned* r, unsigned* g, unsigned* b) { *r = GetRValue(c); *g = GetGValue(c); *b = GetBValue(c); }
static COLORREF g_unused;

static FPDF_PAGE PageOf(Doc* d, int p) { return (p >= 0 && p < d->npages) ? d->CachedPage(p) : nullptr; }

// ------------------------------------------------------------------ hit test / bounds
static bool IsEditableAnnot(int sub, bool markup) {
  if (sub == FPDF_ANNOT_SQUARE || sub == FPDF_ANNOT_CIRCLE || sub == FPDF_ANNOT_INK) return true;
  if (markup && (sub == FPDF_ANNOT_HIGHLIGHT || sub == FPDF_ANNOT_UNDERLINE || sub == FPDF_ANNOT_STRIKEOUT || sub == FPDF_ANNOT_SQUIGGLY)) return true;
  return false;
}

bool EdHitTest(Tab* t, int page, float x, float y, bool markup, EdHit* out) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages) return false;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, page);
  if (!pg) return false;
  PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
  float tol = 2.5f;
  // annotations are on top of the page content
  int na = PA.FPDFPage_GetAnnotCount(pg);
  for (int i = na - 1; i >= 0; i--) {
    FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, i);
    if (!an) continue;
    int sub = PA.FPDFAnnot_GetSubtype(an);
    FS_RECTF rc;
    bool ok = IsEditableAnnot(sub, markup) && PA.FPDFAnnot_GetRect(an, &rc);
    PA.FPDFPage_CloseAnnot(an);
    if (!ok) continue;
    FRect b = TlBoxOfPdf(xf, rc.left, rc.bottom, rc.right, rc.top);
    bool hit;
    float bt = 5.0f;
    if (sub == FPDF_ANNOT_SQUARE || sub == FPDF_ANNOT_CIRCLE) {
      bool inOuter = x >= b.l - bt && x <= b.r + bt && y >= b.t - bt && y <= b.b + bt;
      bool inInner = x > b.l + bt && x < b.r - bt && y > b.t + bt && y < b.b - bt;
      hit = inOuter && !inInner;
    } else hit = x >= b.l - tol && x <= b.r + tol && y >= b.t - tol && y <= b.b + tol;
    if (hit) {
      if (out) { out->sel = ESel(); out->sel.page = page; out->sel.kind = 2; out->sel.annot = i; out->sel.sub = sub; out->box = b; }
      return true;
    }
  }
  std::vector<TagObj> v;
  EnumTagged(pg, xf, &v);
  for (int i = (int)v.size() - 1; i >= 0; i--) {
    const TagObj& o = v[i];
    if (x >= o.box.l - tol && x <= o.box.r + tol && y >= o.box.t - tol && y <= o.box.b + tol) {
      if (out) {
        bool any;
        out->sel = ESel(); out->sel.page = page; out->sel.kind = 1; out->sel.grp = o.grp; out->sel.sub = o.kind;
        out->box = UnionBox(v, o.grp, &any);
      }
      return true;
    }
  }
  return false;
}

bool EdSelBox(Tab* t, const ESel& s, FRect* box) {
  if (!t || !t->doc || s.kind == 0 || s.page < 0 || s.page >= t->doc->npages) return false;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg) return false;
  PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
  if (s.kind == 2) {
    FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, s.annot);
    if (!an) return false;
    FS_RECTF rc;
    bool ok = PA.FPDFAnnot_GetRect(an, &rc) != 0;
    PA.FPDFPage_CloseAnnot(an);
    if (!ok) return false;
    *box = TlBoxOfPdf(xf, rc.left, rc.bottom, rc.right, rc.top);
    return true;
  }
  std::vector<TagObj> v;
  EnumTagged(pg, xf, &v);
  bool any;
  FRect u = UnionBox(v, s.grp, &any);
  if (!any) return false;
  *box = u;
  return true;
}

int EdSelObjKind(Tab* t, const ESel& s) {
  if (!t || !t->doc || s.kind != 1) return 0;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg) return 0;
  PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
  std::vector<TagObj> v;
  EnumTagged(pg, xf, &v);
  for (auto& o : v) if (o.grp == s.grp) return o.kind;
  return 0;
}
int EdSelTextSize(Tab* t, const ESel& s) {
  if (!t || !t->doc || s.kind != 1) return 0;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg) return 0;
  PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
  std::vector<TagObj> v;
  EnumTagged(pg, xf, &v);
  for (auto& o : v) if (o.grp == s.grp && o.kind == EK_TEXT) return (o.extra + 5) / 10;
  return 0;
}

// ------------------------------------------------------------------ markup annotations
static void SelRunRects(Tab* t, int p, int s, int e, std::vector<FRect>* out) {
  auto pd = GetPageDataNow(t->doc, p, true);
  if (!pd) return;
  e = min(e, (int)pd->box.size());
  FRect run;
  bool have = false;
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
      out->push_back(run);
    }
    run = b; have = true;
  }
  if (have) out->push_back(run);
}

bool EdAddMarkup(Tab* t, int which, COLORREF c) {
  if (!t || !t->doc || !t->sel.active) return false;
  int pa, ca, pb, cb;
  ViewSelOrder(t->sel, &pa, &ca, &pb, &cb);
  std::map<int, std::vector<FRect>> per;
  for (int p = pa; p <= pb && p < t->doc->npages; p++) {
    auto pd = GetPageDataNow(t->doc, p, true);
    if (!pd) continue;
    int s = p == pa ? ca : 0, e = p == pb ? cb : (int)pd->ch.size();
    SelRunRects(t, p, s, e, &per[p]);
  }
  bool any = false;
  std::vector<int> pages;
  for (auto& kv : per) if (!kv.second.empty()) { any = true; pages.push_back(kv.first); }
  if (!any) return false;
  static const FPDF_ANNOTATION_SUBTYPE subs[4] = {FPDF_ANNOT_HIGHLIGHT, FPDF_ANNOT_UNDERLINE, FPDF_ANNOT_STRIKEOUT, FPDF_ANNOT_SQUIGGLY};
  FPDF_ANNOTATION_SUBTYPE sub = subs[which & 3];
  return EdEdit(t, false, pages, [&](Doc* d) {
    for (auto& kv : per) {
      if (kv.second.empty()) continue;
      FPDF_PAGE pg = PageOf(d, kv.first);
      if (!pg) return false;
      PXf xf = MakeXf(pg, d->pw[kv.first], d->ph[kv.first]);
      FPDF_ANNOTATION an = PA.FPDFPage_CreateAnnot(pg, sub);
      if (!an) return false;
      float L = 1e9f, B = 1e9f, R = -1e9f, T = -1e9f;
      for (const FRect& r : kv.second) {
        double x[4], y[4];
        xf.ToPdf(r.l, r.t, &x[0], &y[0]);
        xf.ToPdf(r.r, r.t, &x[1], &y[1]);
        xf.ToPdf(r.l, r.b, &x[2], &y[2]);
        xf.ToPdf(r.r, r.b, &x[3], &y[3]);
        FS_QUADPOINTSF q = {(float)x[0], (float)y[0], (float)x[1], (float)y[1], (float)x[2], (float)y[2], (float)x[3], (float)y[3]};
        PA.FPDFAnnot_AppendAttachmentPoints(an, &q);
        for (int k = 0; k < 4; k++) { L = min(L, (float)x[k]); R = max(R, (float)x[k]); B = min(B, (float)y[k]); T = max(T, (float)y[k]); }
      }
      FS_RECTF rc = {L, T, R, B};
      PA.FPDFAnnot_SetRect(an, &rc);
      unsigned r, g, b;
      ColorOf(c, &r, &g, &b);
      PA.FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);
      PA.FPDFAnnot_SetFlags(an, 4);
      PA.FPDFPage_CloseAnnot(an);
    }
    return true;
  });
}

bool EdAddShape(Tab* t, int page, FRect r, bool ellipse, COLORREF c, int widthPt, ESel* out) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages) return false;
  int newIdx = -1;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    FPDF_ANNOTATION an = PA.FPDFPage_CreateAnnot(pg, ellipse ? FPDF_ANNOT_CIRCLE : FPDF_ANNOT_SQUARE);
    if (!an) return false;
    float l, b, rr, tp;
    PdfRectOfTl(xf, r, &l, &b, &rr, &tp);
    FS_RECTF rc = {l, tp, rr, b};
    PA.FPDFAnnot_SetRect(an, &rc);
    unsigned R, G, B;
    ColorOf(c, &R, &G, &B);
    PA.FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, R, G, B, 255);
    PA.FPDFAnnot_SetBorder(an, 0, 0, (float)widthPt);
    PA.FPDFAnnot_SetFlags(an, 4);
    newIdx = PA.FPDFPage_GetAnnotCount(pg) - 1;
    PA.FPDFPage_CloseAnnot(an);
    return true;
  });
  if (ok && out) { *out = ESel(); out->page = page; out->kind = 2; out->annot = newIdx; out->sub = ellipse ? FPDF_ANNOT_CIRCLE : FPDF_ANNOT_SQUARE; }
  return ok;
}

bool EdAddInk(Tab* t, int page, const std::vector<PointF>& pts, COLORREF c, int widthPt, ESel* out) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages || pts.size() < 2) return false;
  int newIdx = -1;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    FPDF_ANNOTATION an = PA.FPDFPage_CreateAnnot(pg, FPDF_ANNOT_INK);
    if (!an) return false;
    std::vector<FS_POINTF> v;
    float L = 1e9f, B = 1e9f, R = -1e9f, T = -1e9f;
    for (auto& p : pts) {
      double x, y;
      xf.ToPdf(p.X, p.Y, &x, &y);
      v.push_back({(float)x, (float)y});
      L = min(L, (float)x); R = max(R, (float)x); B = min(B, (float)y); T = max(T, (float)y);
    }
    float pad = widthPt * 0.5f + 1;
    FS_RECTF rc = {L - pad, T + pad, R + pad, B - pad};
    PA.FPDFAnnot_SetRect(an, &rc);
    if (PA.FPDFAnnot_AddInkStroke(an, v.data(), v.size()) < 0) { PA.FPDFPage_CloseAnnot(an); return false; }
    unsigned r, g, b;
    ColorOf(c, &r, &g, &b);
    PA.FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);
    PA.FPDFAnnot_SetBorder(an, 0, 0, (float)widthPt);
    PA.FPDFAnnot_SetFlags(an, 4);
    newIdx = PA.FPDFPage_GetAnnotCount(pg) - 1;
    PA.FPDFPage_CloseAnnot(an);
    return true;
  });
  if (ok && out) { *out = ESel(); out->page = page; out->kind = 2; out->annot = newIdx; out->sub = FPDF_ANNOT_INK; }
  return ok;
}

// ------------------------------------------------------------------ text and images (page objects)
// New page objects would each get a content stream of their own, after everything else on the page, and could never be moved below
// it. Objects that PDFium has *parsed* belong to stream 0 together with the page's own content, where the list order is the
// stacking order. So they are built on a scratch page, the document is round-tripped, and the parsed objects are moved to the page.
static bool AdoptObjects(Doc* d, int page, const std::function<bool(FPDF_PAGE tmp)>& build) {
  int n = d->npages;
  DocClosePageCache(d);
  FPDF_PAGE tmp = PA.FPDFPage_New(d->h, n, 612, 792);
  if (!tmp) return false;
  bool ok = build(tmp) && PA.FPDFPage_GenerateContent(tmp) != 0;
  PA.FPDF_ClosePage(tmp);
  if (!ok) return false;
  std::string nb;
  if (!DocSerialize(d, &nb) || !DocReload(d, std::move(nb))) return false;
  if (d->npages != n + 1) return false;
  FPDF_PAGE sp = d->CachedPage(n);
  FPDF_PAGE tp = d->CachedPage(page);
  if (!sp || !tp) return false;
  int cnt = PA.FPDFPage_CountObjects(sp);
  if (cnt <= 0) return false;
  std::vector<FPDF_PAGEOBJECT> objs;
  for (int i = 0; i < cnt; i++) objs.push_back(PA.FPDFPage_GetObject(sp, i));
  for (FPDF_PAGEOBJECT o : objs) PA.FPDFPage_RemoveObject(sp, o);
  for (FPDF_PAGEOBJECT o : objs) PA.FPDFPage_InsertObject(tp, o);
  bool g = PA.FPDFPage_GenerateContent(tp) != 0;
  DocClosePageCache(d);
  PA.FPDFPage_Delete(d->h, n);
  return g;
}

bool EdAddText(Tab* t, int page, float x, float y, const ws& text, const ws& font, int size, COLORREF c, ESel* out) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages || text.empty()) return false;
  // lines
  std::vector<ws> lines;
  {
    ws cur;
    for (wchar_t ch : text) {
      if (ch == L'\r') continue;
      if (ch == L'\n') { lines.push_back(cur); cur.clear(); } else cur += ch;
    }
    lines.push_back(cur);
  }
  int grp = NewGroupId();
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    return AdoptObjects(d, page, [&](FPDF_PAGE tmp) {
      FPDF_FONT f = LoadDocFont(d, font);
      if (!f) return false;
      float lh = size * 1.2f;
      int made = 0;
      for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].empty()) continue;
        FPDF_PAGEOBJECT o = PA.FPDFPageObj_CreateTextObj(d->h, f, (float)size);
        if (!o) return false;
        if (!PA.FPDFText_SetText(o, (FPDF_WIDESTRING)lines[i].c_str())) { PA.FPDFPageObj_Destroy(o); return false; }
        unsigned r, g, b;
        ColorOf(c, &r, &g, &b);
        PA.FPDFPageObj_SetFillColor(o, r, g, b, 255);
        // baseline of line i in page points; the text axes follow the page orientation
        double bx = x, by = y + size * 0.88 + lh * (double)i;
        double px, py;
        xf.ToPdf(bx, by, &px, &py);
        FS_MATRIX m = {(float)xf.a, (float)xf.b, (float)-xf.c, (float)-xf.d, (float)px, (float)py};
        PA.FPDFPageObj_SetMatrix(o, &m);
        TagObject(d, o, EK_TEXT, grp, size * 10);
        PA.FPDFPage_InsertObject(tmp, o);
        made++;
      }
      return made > 0;
    });
  });
  if (ok && out) { *out = ESel(); out->page = page; out->kind = 1; out->grp = grp; out->sub = EK_TEXT; }
  return ok;
}

bool EdImageSize(const ws& file, int* w, int* h) {
  Gdiplus::Bitmap bmp(file.c_str());
  if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
  *w = (int)bmp.GetWidth();
  *h = (int)bmp.GetHeight();
  return *w > 0 && *h > 0;
}

// decodes any picture GDI+ knows (PNG with transparency first of all) into a PDFium BGRA bitmap
static FPDF_BITMAP LoadBitmapBGRA(const ws& file) {
  Gdiplus::Bitmap src(file.c_str());
  if (src.GetLastStatus() != Gdiplus::Ok) return nullptr;
  int w = (int)src.GetWidth(), h = (int)src.GetHeight();
  if (w < 1 || h < 1) return nullptr;
  const int MAXD = 2400;
  Gdiplus::Bitmap* use = &src;
  std::unique_ptr<Gdiplus::Bitmap> scaled;
  if (max(w, h) > MAXD) {
    double k = (double)MAXD / max(w, h);
    int nw = max(1, (int)(w * k)), nh = max(1, (int)(h * k));
    scaled.reset(new Gdiplus::Bitmap(nw, nh, PixelFormat32bppARGB));
    Gdiplus::Graphics g(scaled.get());
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.DrawImage(&src, Gdiplus::Rect(0, 0, nw, nh), 0, 0, w, h, Gdiplus::UnitPixel);
    use = scaled.get();
    w = nw; h = nh;
  }
  Gdiplus::BitmapData bd;
  Gdiplus::Rect rc(0, 0, w, h);
  if (use->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return nullptr;
  FPDF_BITMAP bm = PA.FPDFBitmap_Create(w, h, 1);
  if (bm) {
    unsigned char* dst = (unsigned char*)PA.FPDFBitmap_GetBuffer(bm);
    int ds = PA.FPDFBitmap_GetStride(bm);
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * ds, (unsigned char*)bd.Scan0 + (ptrdiff_t)y * bd.Stride, (size_t)w * 4);
  }
  use->UnlockBits(&bd);
  return bm;
}

bool EdAddImage(Tab* t, int page, FRect r, const ws& file, ESel* out) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages) return false;
  int grp = NewGroupId();
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    return AdoptObjects(d, page, [&](FPDF_PAGE tmp) {
      FPDF_BITMAP bm = LoadBitmapBGRA(file);
      if (!bm) return false;
      FPDF_PAGEOBJECT o = PA.FPDFPageObj_NewImageObj(d->h);
      if (!o) { PA.FPDFBitmap_Destroy(bm); return false; }
      FPDF_PAGE pages[1] = {tmp};
      bool good = PA.FPDFImageObj_SetBitmap(pages, 1, o, bm) != 0;
      PA.FPDFBitmap_Destroy(bm);
      if (!good) { PA.FPDFPageObj_Destroy(o); return false; }
      double w = r.r - r.l, h = r.b - r.t, px, py;
      xf.ToPdf(r.l, r.b, &px, &py);                       // lower left corner of the picture on the screen
      FS_MATRIX m = {(float)(xf.a * w), (float)(xf.b * w), (float)(-xf.c * h), (float)(-xf.d * h), (float)px, (float)py};
      PA.FPDFPageObj_SetMatrix(o, &m);
      TagObject(d, o, EK_IMAGE, grp, 0);
      PA.FPDFPage_InsertObject(tmp, o);
      return true;
    });
  });
  if (ok && out) { *out = ESel(); out->page = page; out->kind = 1; out->grp = grp; out->sub = EK_IMAGE; }
  return ok;
}


// a small premultiplied picture of an image object, for the drag preview (upright pages only)
HBITMAP EdImagePreview(Tab* t, const ESel& s, int* ow, int* oh) {
  if (!t || !t->doc || s.kind != 1) return nullptr;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg || PA.FPDFPage_GetRotation(pg) != 0) return nullptr;
  PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
  std::vector<TagObj> v;
  EnumTagged(pg, xf, &v);
  for (auto& o : v) {
    if (o.grp != s.grp || o.kind != EK_IMAGE) continue;
    FPDF_BITMAP bm = PA.FPDFImageObj_GetRenderedBitmap(d->h, pg, o.o);
    if (!bm) return nullptr;
    int w = PA.FPDFBitmap_GetWidth(bm), h = PA.FPDFBitmap_GetHeight(bm), stride = PA.FPDFBitmap_GetStride(bm), fmt = PA.FPDFBitmap_GetFormat(bm);
    const unsigned char* src = (const unsigned char*)PA.FPDFBitmap_GetBuffer(bm);
    int bpp = fmt == FPDFBitmap_BGR ? 3 : (fmt == FPDFBitmap_Gray ? 1 : 4);
    if (w < 1 || h < 1 || !src) { PA.FPDFBitmap_Destroy(bm); return nullptr; }
    int step = max(1, max(w, h) / 600);
    int dw = (w + step - 1) / step, dh = (h + step - 1) / step;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hb && bits) {
      for (int y = 0; y < dh; y++) {
        unsigned char* dp = (unsigned char*)bits + (size_t)y * dw * 4;
        const unsigned char* sp = src + (size_t)(y * step) * stride;
        for (int x = 0; x < dw; x++) {
          const unsigned char* px = sp + (size_t)(x * step) * bpp;
          unsigned b = px[0], g = bpp >= 3 ? px[1] : px[0], r = bpp >= 3 ? px[2] : px[0], a = (bpp == 4 && fmt == FPDFBitmap_BGRA) ? px[3] : 255;
          dp[x * 4 + 0] = (unsigned char)(b * a / 255);
          dp[x * 4 + 1] = (unsigned char)(g * a / 255);
          dp[x * 4 + 2] = (unsigned char)(r * a / 255);
          dp[x * 4 + 3] = (unsigned char)a;
        }
      }
      *ow = dw; *oh = dh;
    }
    PA.FPDFBitmap_Destroy(bm);
    return hb;
  }
  return nullptr;
}

// ------------------------------------------------------------------ editing a selection
// affine map of the page points that sends oldBox onto newBox, expressed as a PDF-space matrix
static FS_MATRIX BoxMatrix(const PXf& xf, const FRect& ob, const FRect& nb) {
  double sx = (ob.r - ob.l) > 1e-6 ? (nb.r - nb.l) / (ob.r - ob.l) : 1;
  double sy = (ob.b - ob.t) > 1e-6 ? (nb.b - nb.t) / (ob.b - ob.t) : 1;
  auto map = [&](double px, double py, double* qx, double* qy) {
    double x, y;
    xf.ToTl(px, py, &x, &y);
    x = nb.l + (x - ob.l) * sx;
    y = nb.t + (y - ob.t) * sy;
    xf.ToPdf(x, y, qx, qy);
  };
  double x0, y0, x1, y1, x2, y2;
  map(0, 0, &x0, &y0);
  map(1, 0, &x1, &y1);
  map(0, 1, &x2, &y2);
  FS_MATRIX m = {(float)(x1 - x0), (float)(y1 - y0), (float)(x2 - x0), (float)(y2 - y0), (float)x0, (float)y0};
  return m;
}

bool EdTransformSel(Tab* t, const ESel& s, FRect ob, FRect nb) {
  if (!t || !t->doc || s.kind == 0) return false;
  return EdEdit(t, false, {s.page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, s.page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
    if (s.kind == 2) {
      FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, s.annot);
      if (!an) return false;
      float l, b, r, tp;
      PdfRectOfTl(xf, nb, &l, &b, &r, &tp);
      FS_RECTF rc = {l, tp, r, b};
      bool ok = PA.FPDFAnnot_SetRect(an, &rc) != 0;
      PA.FPDFPage_CloseAnnot(an);
      return ok;
    }
    FS_MATRIX m = BoxMatrix(xf, ob, nb);
    std::vector<TagObj> v;
    EnumTagged(pg, xf, &v);
    double sc = (ob.r - ob.l) > 1e-6 ? (nb.r - nb.l) / (ob.r - ob.l) : 1;
    int moved = 0;
    for (auto& o : v) {
      if (o.grp != s.grp) continue;
      PA.FPDFPageObj_Transform(o.o, m.a, m.b, m.c, m.d, m.e, m.f);
      if (o.kind == EK_TEXT && fabs(sc - 1.0) > 1e-4) {
        FPDF_PAGEOBJECTMARK mk = FindTagMark(o.o);
        if (mk) PA.FPDFPageObjMark_SetIntParam(d->h, o.o, mk, "s", (int)lround(o.extra * sc));
      }
      moved++;
    }
    if (!moved) return false;
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
}

bool EdSetTextSize(Tab* t, const ESel& s, int size) {
  if (!t || !t->doc || s.kind != 1) return false;
  FRect box;
  if (!EdSelBox(t, s, &box)) return false;
  int cur = EdSelTextSize(t, s);
  if (cur <= 0 || cur == size) return false;
  double k = (double)size / cur;
  FRect nb = box;
  nb.r = box.l + (box.r - box.l) * (float)k;
  nb.b = box.t + (box.b - box.t) * (float)k;
  return EdTransformSel(t, s, box, nb);
}

bool EdDeleteSel(Tab* t, const ESel& s) {
  if (!t || !t->doc || s.kind == 0) return false;
  return EdEdit(t, false, {s.page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, s.page);
    if (!pg) return false;
    if (s.kind == 2) return PA.FPDFPage_RemoveAnnot(pg, s.annot) != 0;
    PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
    std::vector<TagObj> v;
    EnumTagged(pg, xf, &v);
    int removed = 0;
    for (int i = (int)v.size() - 1; i >= 0; i--) {
      if (v[i].grp != s.grp) continue;
      if (PA.FPDFPage_RemoveObject(pg, v[i].o)) { PA.FPDFPageObj_Destroy(v[i].o); removed++; }
    }
    if (!removed) return false;
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
}

bool EdSetColorSel(Tab* t, const ESel& s, COLORREF c) {
  if (!t || !t->doc || s.kind == 0) return false;
  return EdEdit(t, false, {s.page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, s.page);
    if (!pg) return false;
    unsigned r, g, b;
    ColorOf(c, &r, &g, &b);
    if (s.kind == 2) {
      FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, s.annot);
      if (!an) return false;
      // the stored appearance has the old colour in it: re-setting the colour drops it and PDFium builds a new one on load
      bool ok = PA.FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, r, g, b, 255) != 0;
      PA.FPDFPage_CloseAnnot(an);
      return ok;
    }
    PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
    std::vector<TagObj> v;
    EnumTagged(pg, xf, &v);
    int n = 0;
    for (auto& o : v) if (o.grp == s.grp && o.kind == EK_TEXT) { PA.FPDFPageObj_SetFillColor(o.o, r, g, b, 255); n++; }
    if (!n) return false;
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
}

// layers: 0 to front, 1 one step forward (above the next object it overlaps), 2 one step back, 3 to back
bool EdZOrder(Tab* t, const ESel& s, int op) {
  if (!t || !t->doc || s.kind != 1) return false;
  bool changed = false;
  bool ok = EdEdit(t, false, {s.page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, s.page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
    std::vector<TagObj> v;
    EnumTagged(pg, xf, &v);
    std::vector<int> S;
    for (auto& o : v) if (o.grp == s.grp) S.push_back(o.idx);
    if (S.empty()) return false;
    int n = PA.FPDFPage_CountObjects(pg);
    std::vector<FPDF_PAGEOBJECT> objs(n);
    for (int i = 0; i < n; i++) objs[i] = PA.FPDFPage_GetObject(pg, i);
    std::set<int> inS(S.begin(), S.end());
    // bounds of the group (pdf space)
    float gl = 1e9f, gb = 1e9f, gr = -1e9f, gt = -1e9f;
    for (int i : S) {
      float l, b, r, tp;
      if (!PA.FPDFPageObj_GetBounds(objs[i], &l, &b, &r, &tp)) continue;
      gl = min(gl, l); gb = min(gb, b); gr = max(gr, r); gt = max(gt, tp);
    }
    auto overlaps = [&](int i) {
      float l, b, r, tp;
      if (!PA.FPDFPageObj_GetBounds(objs[i], &l, &b, &r, &tp)) return false;
      return l < gr && r > gl && b < gt && tp > gb;
    };
    int below = 0;    // members of S under a given index
    auto countBelow = [&](int idx) { int c = 0; for (int i : S) if (i < idx) c++; return c; };
    (void)below;
    int p0;   // position among the others where the group goes
    int others = n - (int)S.size();
    if (op == 0) p0 = others;
    else if (op == 3) p0 = 0;
    else if (op == 1) {
      int X = -1;
      for (int i = S.back() + 1; i < n; i++) if (!inS.count(i) && overlaps(i)) { X = i; break; }
      if (X < 0) return true;
      p0 = X - countBelow(X) + 1;
    } else {
      int X = -1;
      for (int i = S.front() - 1; i >= 0; i--) if (!inS.count(i) && overlaps(i)) { X = i; break; }
      if (X < 0) return true;
      p0 = X - countBelow(X);
    }
    // is it already there?
    bool same = true;
    for (size_t k = 0; k < S.size(); k++) if (S[k] != p0 + (int)k) same = false;
    if (same) return true;
    for (int k = (int)S.size() - 1; k >= 0; k--) PA.FPDFPage_RemoveObject(pg, objs[S[k]]);
    for (size_t k = 0; k < S.size(); k++) PA.FPDFPage_InsertObjectAtIndex(pg, objs[S[k]], (size_t)(p0 + (int)k));
    changed = true;
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
  (void)changed;
  return ok;
}

// ------------------------------------------------------------------ page operations
bool ParsePageRange(const ws& s, int n, std::vector<int>* pages) {
  std::set<int> out;
  size_t i = 0;
  auto skip = [&]() { while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) i++; };
  auto num = [&](int* v) {
    skip();
    if (i >= s.size() || s[i] < L'0' || s[i] > L'9') return false;
    long long x = 0;
    while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') { x = x * 10 + (s[i] - L'0'); if (x > 1000000) x = 1000000; i++; }
    *v = (int)x;
    return true;
  };
  skip();
  if (i >= s.size()) return false;
  while (i < s.size()) {
    int a, b;
    skip();
    if (i < s.size() && (s[i] == L'-' || s[i] == 0x2013)) { a = 1; }          // "-5"
    else if (!num(&a)) return false;
    b = a;
    skip();
    if (i < s.size() && (s[i] == L'-' || s[i] == 0x2013)) {
      i++;
      skip();
      if (i >= s.size() || s[i] == L',' || s[i] == L';') b = n;
      else if (!num(&b)) return false;
    }
    if (a < 1 || b < a || b > n) return false;
    for (int p = a; p <= b; p++) out.insert(p - 1);
    skip();
    if (i < s.size()) { if (s[i] == L',' || s[i] == L';') i++; else return false; }
  }
  if (out.empty()) return false;
  pages->assign(out.begin(), out.end());
  return true;
}

bool EdRotatePage(Tab* t, int page, int delta) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages) return false;
  return EdEdit(t, true, {}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    int r = PA.FPDFPage_GetRotation(pg);
    PA.FPDFPage_SetRotation(pg, ((r + delta) % 4 + 4) % 4);
    return true;
  });
}

bool EdDeletePages(Tab* t, std::vector<int> pages) {
  if (!t || !t->doc || pages.empty()) return false;
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  if ((int)pages.size() >= t->doc->npages) { ShowToast(L"Нельзя удалить все страницы"); return false; }
  return EdEdit(t, true, {}, [&](Doc* d) {
    DocClosePageCache(d);
    for (int i = (int)pages.size() - 1; i >= 0; i--) {
      if (pages[i] < 0 || pages[i] >= d->npages) continue;
      PA.FPDFPage_Delete(d->h, pages[i]);
    }
    return true;
  });
}

bool EdMovePage(Tab* t, int from, int to) {
  if (!t || !t->doc || from < 0 || to < 0 || from >= t->doc->npages || to >= t->doc->npages || from == to) return false;
  return EdEdit(t, true, {}, [&](Doc* d) {
    DocClosePageCache(d);
    int idx[1] = {from};
    return PA.FPDF_MovePages(d->h, idx, 1, to) != 0;
  });
}

bool EdBlankPage(Tab* t, int after) {
  if (!t || !t->doc || after < -1 || after >= t->doc->npages) return false;
  return EdEdit(t, true, {}, [&](Doc* d) {
    DocClosePageCache(d);
    int ref = max(after, 0);
    double w = d->pw[ref], h = d->ph[ref];
    FPDF_PAGE np = PA.FPDFPage_New(d->h, after + 1, w, h);
    if (!np) return false;
    PA.FPDF_ClosePage(np);
    return true;
  });
}

bool EdInsertPages(Tab* t, int at, const std::vector<ws>& files) {
  if (!t || !t->doc || files.empty()) return false;
  at = min(max(at, 0), t->doc->npages);
  int failed = 0;
  bool ok = EdEdit(t, true, {}, [&](Doc* d) {
    DocClosePageCache(d);
    int pos = at, added = 0;
    for (const ws& f : files) {
      std::string data;
      if (!ReadFileAll(f, &data) || data.empty()) { failed++; continue; }
      FPDF_DOCUMENT sd = PA.FPDF_LoadMemDocument(data.data(), (int)data.size(), nullptr);
      if (!sd) { failed++; continue; }
      int cnt = PA.FPDF_GetPageCount(sd);
      if (cnt > 0 && PA.FPDF_ImportPages(d->h, sd, nullptr, pos)) { pos += cnt; added += cnt; }
      else failed++;
      PA.FPDF_CloseDocument(sd);
    }
    return added > 0;
  });
  if (failed && ok) ShowToast(L"Часть файлов не удалось вставить");
  else if (!ok) ShowToast(L"Не удалось вставить страницы");
  return ok;
}

bool EdExtractPages(Tab* t, const std::string& range, const ws& outFile) {
  if (!t || !t->doc) return false;
  std::string data;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    FPDF_DOCUMENT nd = PA.FPDF_CreateNewDocument();
    if (!nd) return false;
    bool ok = PA.FPDF_ImportPages(nd, t->doc->h, range.c_str(), 0) != 0 && PA.FPDF_GetPageCount(nd) > 0;
    if (ok) ok = SerializeHandle(nd, &data);
    PA.FPDF_CloseDocument(nd);
    if (!ok) return false;
  }
  return WriteFileAtomic(outFile, data);
}

// ------------------------------------------------------------------ saving
bool EdIsEdited(Tab* t) { return t && t->doc && t->doc->edited; }

bool EdSave(Tab* t) {
  if (!t || !t->doc) return false;
  Doc* d = t->doc.get();
  if (!d->edited) return true;
  std::string data;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    data = d->src;
  }
  if (!WriteFileAtomic(t->path, data)) {
    MessageBoxW(A_.main, (L"Не удалось сохранить файл:\n" + t->path + L"\n\nВозможно, он открыт в другой программе или папка защищена от записи.").c_str(), APP_TITLE, MB_OK | MB_ICONWARNING);
    return false;
  }
  d->saveMark = (int)d->undo.size();
  d->edited = false;
  ShowToast(L"Сохранено");
  MainRedrawChrome();
  UpdateTitle();
  return true;
}

static ExportOpts g_lastExport;

// builds an export copy of the document and writes it to `out` (progress dialog unless `quiet`); `report` gets a user-readable result
static bool RunExport(Tab* t, const ExportOpts& o, const ws& out, bool quiet, ws* report) {
  Doc* d = t->doc.get();
  std::string src, pass;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    src = d->src;
    pass = d->pass;
  }
  ExportJob job;
  std::thread th([&]() { ExportBuild(src, pass, o, &job); job.done = true; });
  if (quiet) { while (!job.done) Sleep(20); }
  else ProgressDialog(A_.main, L"Подготовка файла", &job);
  th.join();
  if (job.out.empty()) { *report = job.err.empty() ? L"Не удалось подготовить файл" : job.err; return false; }
  if (!WriteFileAtomic(out, job.out)) { *report = L"Не удалось записать файл:\n" + out; return false; }
  double mb = job.out.size() / 1048576.0;
  wchar_t sz[64];
  swprintf(sz, 64, L"%.2f МБ", mb);
  *report = L"Сохранено, " + ws(sz);
  if (job.usedDpi > 0) *report += L" (" + std::to_wstring(job.usedDpi.load()) + L" dpi)";
  if (!job.note.empty()) *report += L"\n" + job.note;
  return true;
}

bool EdSaveAs(Tab* t) {
  if (!t || !t->doc) return false;
  Doc* d = t->doc.get();
  ExportOpts o = g_lastExport;
  if (!ExportOptionsDialog(A_.main, &o, d->npages)) return false;
  g_lastExport = o;
  ws stem = StripExt(BaseName(t->path));
  if (o.mode != 0 && o.preset >= 1 && o.preset <= 4 && stem.find(L" на ") == ws::npos) stem += L" на " + std::to_wstring(d->npages) + L"л.";
  wchar_t buf[MAX_PATH * 2];
  wcsncpy(buf, (stem + L".pdf").c_str(), MAX_PATH);
  OPENFILENAMEW of = {sizeof(of)};
  of.hwndOwner = A_.main;
  of.lpstrFilter = L"Документы PDF (*.pdf)\0*.pdf\0";
  of.lpstrFile = buf;
  of.nMaxFile = MAX_PATH * 2;
  of.lpstrDefExt = L"pdf";
  of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&of)) return false;
  if (o.mode != 0) {
    // an export copy: the document in the window stays as it is
    ws rep;
    bool ok = RunExport(t, o, buf, false, &rep);
    if (ok && rep.find(L'\n') == ws::npos) ShowToast(rep);
    else MessageBoxW(A_.main, rep.c_str(), APP_TITLE, ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
    return ok;
  }
  std::string data;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    data = d->src;
  }
  if (!WriteFileAtomic(buf, data)) {
    MessageBoxW(A_.main, L"Не удалось сохранить файл", APP_TITLE, MB_OK | MB_ICONWARNING);
    return false;
  }
  t->path = buf;
  t->title = StripExt(BaseName(t->path));
  t->saved.path = t->path;
  d->path = t->path;
  d->saveMark = (int)d->undo.size();
  d->edited = false;
  ShowToast(L"Сохранено");
  MainRedrawChrome();
  UpdateTitle();
  return true;
}

bool EdExportTest(Tab* t, const ExportOpts& o, const ws& out, bool quiet, ws* report) { return RunExport(t, o, out, quiet, report); }

bool EdConfirmClose(Tab* t) {
  if (!EdIsEdited(t)) return true;
  if (GetEnvironmentVariableW(L"ULTRAPDF_NOPROMPT", nullptr, 0) > 0) return true;       // scripted tests
  ws msg = L"Сохранить изменения в документе «" + t->title + L"»?";
  int r = MessageBoxW(A_.main, msg.c_str(), APP_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
  if (r == IDCANCEL) return false;
  if (r == IDYES) return EdSave(t);
  return true;
}
bool EdConfirmAll() {
  for (auto& t : A_.tabs) {
    if (!EdIsEdited(t.get())) continue;
    int idx = -1;
    for (size_t i = 0; i < A_.tabs.size(); i++) if (A_.tabs[i].get() == t.get()) idx = (int)i;
    if (idx >= 0 && idx != A_.cur) SwitchTab(idx);
    if (!EdConfirmClose(t.get())) return false;
  }
  return true;
}

void EdHandleDocReplaced(Tab* t, bool pagesChanged) {
  if (!t || !t->doc) return;
  t->layoutOk = false;
  t->hits.clear();
  t->curHit = -1;
  t->sel.active = false;
  if (pagesChanged) {
    SideRebuildOutline(t);
    if (t == CurTab()) {
      ViewEnsureLayout(t);
      ViewScrollBy(0, 0, false);          // a new layout (rotated page, other fit zoom) can leave the old scroll position out of range
      ViewSync();
      SideSync();
    }
  }
  EdOnTabChanged();
  ViewInvalidate();
  SideInvalidate();
  MainRedrawChrome();
  MainUpdatePageBox();
  UpdateTitle();
}

// ------------------------------------------------------------------ test hook: "ed <command>" lines of a test script
bool EdTestCommand(const std::string& line) {
  { int r = EdUiTest(line); if (r >= 0) return r != 0; }
  std::istringstream ss(line);
  std::string c;
  ss >> c;
  Tab* t = CurTab();
  if (!t || !t->doc) return false;
  auto rest = [&]() { std::string r; std::getline(ss, r); if (!r.empty() && r[0] == ' ') r.erase(0, 1); return r; };
  if (c == "export" || c == "exportasync") {            // export <mode> <dpi> <color> <quality> <maxMB> <path>
    ExportOpts o; int dpi;
    ss >> o.mode >> dpi >> o.color >> o.quality >> o.maxMB;
    o.dpi = dpi;
    std::string f = rest();
    ws rep;
    if (c == "exportasync") {      // same, with the progress dialog, without blocking the test thread
      ws out = Utf8ToW(f);
      auto* fn = new std::function<void()>([o, out]() { ws r; EdExportTest(CurTab(), o, out, false, &r); });
      PostMessageW(A_.main, WM_APP_TESTCMD, 1, (LPARAM)fn);
      return true;
    }
    bool ok = EdExportTest(t, o, Utf8ToW(f), true, &rep);
    FILE* lf = _wfopen(L"Z:\\tmp\\t\\dump.txt", L"ab");
    if (lf) { fprintf(lf, "export ok=%d %s\n", ok, WToUtf8(rep).c_str()); fclose(lf); }
    return ok;
  }
  if (c == "image") {
    int p; float x, y, w, h; std::string f;
    ss >> p >> x >> y >> w >> h;
    f = rest();
    FRect r = {x, y, x + w, y + h};
    return EdAddImage(t, p, r, Utf8ToW(f), nullptr);
  }
  if (c == "text") {
    int p, size; float x, y; unsigned col = 0;
    ss >> p >> x >> y >> size >> std::hex >> col >> std::dec;
    std::string s = rest();
    for (size_t i = 0; i + 1 < s.size(); i++) if (s[i] == '\\' && s[i + 1] == 'n') { s.replace(i, 2, "\n"); }
    return EdAddText(t, p, x, y, Utf8ToW(s), L"Arial", size, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255), nullptr);
  }
  if (c == "shape") {
    int p, ell, w; float x, y, ww, hh; unsigned col;
    ss >> p >> ell >> x >> y >> ww >> hh >> w >> std::hex >> col;
    return EdAddShape(t, p, {x, y, x + ww, y + hh}, ell != 0, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255), w, nullptr);
  }
  if (c == "ink") {
    int p, w; unsigned col;
    ss >> p >> w >> std::hex >> col >> std::dec;
    std::vector<PointF> pts;
    float x, y;
    while (ss >> x >> y) pts.push_back(PointF(x, y));
    return EdAddInk(t, p, pts, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255), w, nullptr);
  }
  if (c == "markup") {              // markup <which> <page> <c0> <c1> <color>
    int which, p, c0, c1; unsigned col;
    ss >> which >> p >> c0 >> c1 >> std::hex >> col;
    t->sel.active = true; t->sel.p0 = t->sel.p1 = p; t->sel.c0 = c0; t->sel.c1 = c1;
    return EdAddMarkup(t, which, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255));
  }
  if (c == "select") {              // select <page> <x> <y>  (then z/move/delete act on it)
    extern ESel g_testSel;
    int p; float x, y;
    ss >> p >> x >> y;
    EdHit h;
    if (!EdHitTest(t, p, x, y, true, &h)) return false;
    g_testSel = h.sel;
    return true;
  }
  if (c == "z") { extern ESel g_testSel; int op; ss >> op; return EdZOrder(t, g_testSel, op); }
  if (c == "move") {
    extern ESel g_testSel;
    float dx, dy; ss >> dx >> dy;
    FRect b;
    if (!EdSelBox(t, g_testSel, &b)) return false;
    FRect n = {b.l + dx, b.t + dy, b.r + dx, b.b + dy};
    return EdTransformSel(t, g_testSel, b, n);
  }
  if (c == "scale") {
    extern ESel g_testSel;
    float k; ss >> k;
    FRect b;
    if (!EdSelBox(t, g_testSel, &b)) return false;
    FRect n = {b.l, b.t, b.l + (b.r - b.l) * k, b.t + (b.b - b.t) * k};
    return EdTransformSel(t, g_testSel, b, n);
  }
  if (c == "delete") { extern ESel g_testSel; return EdDeleteSel(t, g_testSel); }
  if (c == "color") { extern ESel g_testSel; unsigned col; ss >> std::hex >> col; return EdSetColorSel(t, g_testSel, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255)); }
  if (c == "textsize") { extern ESel g_testSel; int s; ss >> s; return EdSetTextSize(t, g_testSel, s); }
  if (c == "undo") return EdUndo(t);
  if (c == "redo") return EdRedo(t);
  if (c == "save") { std::string f = rest(); if (f.empty()) return EdSave(t); std::string d; { std::lock_guard<std::recursive_mutex> lk(g_pdf); d = t->doc->src; } return WriteFileAtomic(Utf8ToW(f), d); }
  if (c == "rotate") { int p, dl; ss >> p >> dl; return EdRotatePage(t, p, dl); }
  if (c == "delpage") { std::vector<int> v; int p; while (ss >> p) v.push_back(p); return EdDeletePages(t, v); }
  if (c == "movepage") { int a, b; ss >> a >> b; return EdMovePage(t, a, b); }
  if (c == "blank") { int a; ss >> a; return EdBlankPage(t, a); }
  if (c == "insert") { int at; ss >> at; std::string f = rest(); return EdInsertPages(t, at, {Utf8ToW(f)}); }
  if (c == "extract") { std::string rg; ss >> rg; std::string f = rest(); return EdExtractPages(t, rg, Utf8ToW(f)); }
  if (c == "dump") {                 // dump: lists the tagged objects of a page
    int p; ss >> p;
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    FPDF_PAGE pg = PageOf(t->doc.get(), p);
    if (!pg) return false;
    PXf xf = MakeXf(pg, t->doc->pw[p], t->doc->ph[p]);
    std::vector<TagObj> v;
    EnumTagged(pg, xf, &v);
    FILE* f = _wfopen(L"Z:\\tmp\\t\\dump.txt", L"a");
    if (f) {
      fprintf(f, "page %d objects=%d annots=%d tagged=%d undo=%d redo=%d edited=%d\n", p, PA.FPDFPage_CountObjects(pg), PA.FPDFPage_GetAnnotCount(pg), (int)v.size(), (int)t->doc->undo.size(), (int)t->doc->redo.size(), t->doc->edited ? 1 : 0);
      for (auto& o : v) fprintf(f, "  idx=%d kind=%d grp=%d extra=%d box=%.1f,%.1f,%.1f,%.1f\n", o.idx, o.kind, o.grp, o.extra, o.box.l, o.box.t, o.box.r, o.box.b);
      fclose(f);
    }
    return true;
  }
  return false;
}
ESel g_testSel;

#endif
