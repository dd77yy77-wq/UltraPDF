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
// mark "UPE": k kind, g group (what is selected / moved together), s size*10 for text, u the object's own unit (so a group can be taken apart),
// f font code (family*4+style), r angle*10 the editor turned it by, a opacity in percent
static const char* kMark = "UPE";
static int g_grpSeq = 0;
static int NewGroupId() {
  g_grpSeq++;
  return (int)(((uint32_t)(time(nullptr) & 0x3FFFFF) << 8) | (uint32_t)(g_grpSeq & 0xFF)) & 0x7FFFFFFF;
}
struct TagInfo { int kind = 0, grp = 0, extra = 0, unit = 0, font = 0, rot = 0, op = 100; };
static void TagObject(Doc* d, FPDF_PAGEOBJECT o, int kind, int grp, int extra, int font = 0) {
  FPDF_PAGEOBJECTMARK m = PA.FPDFPageObj_AddMark(o, kMark);
  if (!m) return;
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "k", kind);
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "g", grp);
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "s", extra);
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "u", grp);
  PA.FPDFPageObjMark_SetIntParam(d->h, o, m, "f", font);
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
static bool ReadTagInfo(FPDF_PAGEOBJECT o, TagInfo* ti) {
  FPDF_PAGEOBJECTMARK m = FindTagMark(o);
  if (!m) return false;
  int g = 0;
  if (!PA.FPDFPageObjMark_GetParamIntValue(m, "g", &g)) return false;
  TagInfo t;
  t.grp = g;
  PA.FPDFPageObjMark_GetParamIntValue(m, "k", &t.kind);
  PA.FPDFPageObjMark_GetParamIntValue(m, "s", &t.extra);
  if (!PA.FPDFPageObjMark_GetParamIntValue(m, "u", &t.unit)) t.unit = g;
  PA.FPDFPageObjMark_GetParamIntValue(m, "f", &t.font);
  PA.FPDFPageObjMark_GetParamIntValue(m, "r", &t.rot);
  if (!PA.FPDFPageObjMark_GetParamIntValue(m, "a", &t.op)) t.op = 100;
  *ti = t;
  return true;
}
static void SetMarkInt(Doc* d, FPDF_PAGEOBJECT o, const char* key, int v) {
  FPDF_PAGEOBJECTMARK m = FindTagMark(o);
  if (m) PA.FPDFPageObjMark_SetIntParam(d->h, o, m, key, v);
}

static int KindOfType(int type) {
  switch (type) {
    case FPDF_PAGEOBJ_TEXT: return EK_TEXT;
    case FPDF_PAGEOBJ_IMAGE: return EK_IMAGE;
    case FPDF_PAGEOBJ_PATH: return EK_PATH;
    default: return EK_FORM;
  }
}

struct TagObj {
  int idx = 0;
  int kind = 0, grp = 0, extra = 0, unit = 0, font = 0, rot = 0, op = 100;
  int type = 0;
  bool tagged = false;
  FRect box;
  FPDF_PAGEOBJECT o = nullptr;
};
static void EnumAll(FPDF_PAGE pg, const PXf& xf, std::vector<TagObj>* out, bool taggedOnly) {
  int n = PA.FPDFPage_CountObjects(pg);
  for (int i = 0; i < n; i++) {
    FPDF_PAGEOBJECT o = PA.FPDFPage_GetObject(pg, i);
    if (!o) continue;
    TagObj t;
    TagInfo ti;
    bool tg = PA.FPDFPageObj_CountMarks(o) > 0 && ReadTagInfo(o, &ti);
    if (taggedOnly && !tg) continue;
    float l, b, r, tp;
    if (!PA.FPDFPageObj_GetBounds(o, &l, &b, &r, &tp)) continue;
    t.idx = i; t.o = o; t.type = PA.FPDFPageObj_GetType(o);
    t.tagged = tg;
    if (tg) { t.kind = ti.kind; t.grp = ti.grp; t.extra = ti.extra; t.unit = ti.unit; t.font = ti.font; t.rot = ti.rot; t.op = ti.op; }
    else t.kind = KindOfType(t.type);
    t.box = TlBoxOfPdf(xf, l, b, r, tp);
    out->push_back(t);
  }
}
static void EnumTagged(FPDF_PAGE pg, const PXf& xf, std::vector<TagObj>* out) { EnumAll(pg, xf, out, true); }
static FRect UnionRect(const FRect& a, const FRect& b) {
  FRect u;
  u.l = min(a.l, b.l); u.t = min(a.t, b.t); u.r = max(a.r, b.r); u.b = max(a.b, b.b);
  return u;
}
static FRect UnionBox(const std::vector<TagObj>& v, int grp, bool* any) {
  FRect u;
  bool have = false;
  for (auto& t : v) {
    if (!t.tagged || t.grp != grp) continue;
    if (!have) { u = t.box; have = true; }
    else u = UnionRect(u, t.box);
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
// four faces per family: regular, bold, italic, bold italic (style bits: 1 bold, 2 italic). The second set is for Wine on the test machine.
struct FontFam { const wchar_t* key; const wchar_t* file[4]; const wchar_t* alt[4]; };
static const FontFam kFonts[] = {
    {L"Arial", {L"arial.ttf", L"arialbd.ttf", L"ariali.ttf", L"arialbi.ttf"},
     {L"liberation\\LiberationSans-Regular.ttf", L"liberation\\LiberationSans-Bold.ttf", L"liberation\\LiberationSans-Italic.ttf", L"liberation\\LiberationSans-BoldItalic.ttf"}},
    {L"Times New Roman", {L"times.ttf", L"timesbd.ttf", L"timesi.ttf", L"timesbi.ttf"},
     {L"liberation\\LiberationSerif-Regular.ttf", L"liberation\\LiberationSerif-Bold.ttf", L"liberation\\LiberationSerif-Italic.ttf", L"liberation\\LiberationSerif-BoldItalic.ttf"}},
    {L"Courier New", {L"cour.ttf", L"courbd.ttf", L"couri.ttf", L"courbi.ttf"},
     {L"liberation\\LiberationMono-Regular.ttf", L"liberation\\LiberationMono-Bold.ttf", L"liberation\\LiberationMono-Italic.ttf", L"liberation\\LiberationMono-BoldItalic.ttf"}},
    {L"Segoe UI", {L"segoeui.ttf", L"segoeuib.ttf", L"segoeuii.ttf", L"segoeuiz.ttf"},
     {L"dejavu\\DejaVuSans.ttf", L"dejavu\\DejaVuSans-Bold.ttf", L"dejavu\\DejaVuSans-Oblique.ttf", L"dejavu\\DejaVuSans-BoldOblique.ttf"}},
    {L"Calibri", {L"calibri.ttf", L"calibrib.ttf", L"calibrii.ttf", L"calibriz.ttf"},
     {L"crosextra\\Carlito-Regular.ttf", L"crosextra\\Carlito-Bold.ttf", L"crosextra\\Carlito-Italic.ttf", L"crosextra\\Carlito-BoldItalic.ttf"}},
};
static const int kNFonts = (int)(sizeof(kFonts) / sizeof(kFonts[0]));
static int FamilyIndex(const ws& key) {
  for (int i = 0; i < kNFonts; i++) if (key == kFonts[i].key) return i;
  return -1;
}
static ws FontPathAt(int fam, int style) {
  wchar_t win[MAX_PATH];
  GetWindowsDirectoryW(win, MAX_PATH);
  const FontFam& f = kFonts[fam];
  for (int s : {style & 3, 0}) {
    ws p = ws(win) + L"\\Fonts\\" + f.file[s];
    if (FileExists(p)) return p;
    ws q = ws(L"Z:\\usr\\share\\fonts\\truetype\\") + f.alt[s];
    if (FileExists(q)) return q;
  }
  return L"";
}
static ws FontPath(const wchar_t* key) { int i = FamilyIndex(key); return i < 0 ? L"" : FontPathAt(i, 0); }
std::vector<ws> EdAvailableFonts() {
  std::vector<ws> v;
  for (int i = 0; i < kNFonts; i++) if (!FontPathAt(i, 0).empty()) v.push_back(kFonts[i].key);
  if (v.empty()) v.push_back(L"Arial");
  return v;
}
static FPDF_FONT LoadDocFont(Doc* d, const ws& family, int style) {
  int fam = FamilyIndex(family);
  if (fam < 0) fam = 0;
  ws key = ws(kFonts[fam].key) + L"|" + std::to_wstring(style & 3);
  auto it = d->fonts.find(key);
  if (it != d->fonts.end()) return (FPDF_FONT)it->second;
  ws path = FontPathAt(fam, style);
  if (path.empty()) path = FontPathAt(0, style);
  if (path.empty()) return nullptr;
  std::string data;
  if (!ReadFileAll(path, &data) || data.empty()) return nullptr;
  FPDF_FONT f = PA.FPDFText_LoadFont(d->h, (const uint8_t*)data.data(), (uint32_t)data.size(), FPDF_FONT_TRUETYPE, 1);
  if (f) d->fonts[key] = f;
  return f;
}
// which of our families / styles resembles a font of the document
static void MapPdfFont(FPDF_FONT f, int* fam, int* style) {
  *fam = 0; *style = 0;
  if (!f) return;
  char nm[256] = {};
  std::string name;
  size_t n = PA.FPDFFont_GetFamilyName(f, nm, sizeof nm);
  if (n > 1) name = nm;
  char bn[256] = {};
  n = PA.FPDFFont_GetBaseFontName(f, bn, sizeof bn);
  if (n > 1) { name += " "; name += bn; }
  for (char& c : name) c = (char)tolower((unsigned char)c);
  auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
  int flags = PA.FPDFFont_GetFlags(f);
  if (has("times") || (has("serif") && !has("sans")) || has("georgia") || has("garamond") || has("cambria") || has("book") || has("palatino") || has("minion") || has("roman")) *fam = 1;
  else if (has("courier") || has("mono") || has("consolas") || has("typewriter")) *fam = 2;
  else if (has("calibri") || has("carlito")) *fam = 4;
  else if (has("segoe") || has("tahoma") || has("verdana") || has("dejavu")) *fam = 3;
  else if (has("arial") || has("helvetica") || has("sans") || has("liberation")) *fam = 0;
  else if (flags & 2) *fam = 1;           // PDF font flag Serif
  else if (flags & 1) *fam = 2;           // FixedPitch
  bool bold = has("bold") || has("black") || has("heavy") || has("semibold") || PA.FPDFFont_GetWeight(f) >= 600 || (flags & 0x40000) != 0;
  int ang = 0;
  bool italic = has("italic") || has("oblique") || (PA.FPDFFont_GetItalicAngle(f, &ang) && ang != 0) || (flags & 0x40) != 0;
  *style = (bold ? 1 : 0) | (italic ? 2 : 0);
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

// objects of the page's own content that may be picked with the mouse: no invisible text (scan layers), nothing that covers the page
static bool RawSelectable(const TagObj& o, float pw, float ph, bool deep) {
  if (o.tagged) return true;
  float w = o.box.r - o.box.l, h = o.box.b - o.box.t;
  if (w < 0.01f && h < 0.01f) return false;
  float area = w * h, page = max(pw * ph, 1.0f);
  switch (o.type) {
    case FPDF_PAGEOBJ_TEXT: return PA.FPDFTextObj_GetTextRenderMode(o.o) != 3 && w > 0.3f;
    case FPDF_PAGEOBJ_IMAGE: return deep || area < page * 0.92f;
    case FPDF_PAGEOBJ_PATH: return deep || area < page * 0.2f;
    case FPDF_PAGEOBJ_FORM: return deep || area < page * 0.7f;
    default: return false;
  }
}

static void FillSel(ESel* s, int page, const TagObj& o) {
  *s = ESel();
  s->page = page;
  if (o.tagged) { s->kind = 1; s->grp = o.grp; s->sub = o.kind; }
  else { s->kind = 3; s->obj = o.idx; s->sub = o.kind; }
}

bool EdHitTest(Tab* t, int page, float x, float y, bool markup, EdHit* out, bool deep) {
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
  EnumAll(pg, xf, &v, false);
  float pw = d->pw[page], ph = d->ph[page];
  int found = -1;
  for (int i = (int)v.size() - 1; i >= 0; i--) {
    const TagObj& o = v[i];
    if (!RawSelectable(o, pw, ph, deep)) continue;
    float tl = o.type == FPDF_PAGEOBJ_TEXT && !o.tagged ? 1.0f : tol;
    if (x >= o.box.l - tl && x <= o.box.r + tl && y >= o.box.t - tl && y <= o.box.b + tl) { found = i; break; }
  }
  if (found < 0) return false;
  if (!v[found].tagged && v[found].type == FPDF_PAGEOBJ_TEXT) {
    // lines of text have boxes that overlap a little: take the line whose middle is nearest to the pointer
    float best = 1e9f;
    for (int i = (int)v.size() - 1; i >= 0; i--) {
      const TagObj& o = v[i];
      if (o.tagged || o.type != FPDF_PAGEOBJ_TEXT || !RawSelectable(o, pw, ph, deep)) continue;
      if (!(x >= o.box.l - 1.0f && x <= o.box.r + 1.0f && y >= o.box.t - 1.0f && y <= o.box.b + 1.0f)) continue;
      float dist = fabsf(y - (o.box.t + o.box.b) * 0.5f);
      if (dist < best - 0.01f) { best = dist; found = i; }
    }
  }
  const TagObj& o = v[found];
  if (out) {
    FillSel(&out->sel, page, o);
    if (o.tagged) { bool any; out->box = UnionBox(v, o.grp, &any); } else out->box = o.box;
  }
  return true;
}

bool EdMarquee(Tab* t, int page, FRect r, ESet* out) {
  out->clear();
  if (!t || !t->doc || page < 0 || page >= t->doc->npages) return false;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, page);
  if (!pg) return false;
  PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
  auto touches = [&](const FRect& b) { return b.l <= r.r && b.r >= r.l && b.t <= r.b && b.b >= r.t; };
  int na = PA.FPDFPage_GetAnnotCount(pg);
  for (int i = 0; i < na; i++) {
    FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, i);
    if (!an) continue;
    int sub = PA.FPDFAnnot_GetSubtype(an);
    FS_RECTF rc;
    bool ok = IsEditableAnnot(sub, false) && PA.FPDFAnnot_GetRect(an, &rc);
    PA.FPDFPage_CloseAnnot(an);
    if (!ok) continue;
    FRect b = TlBoxOfPdf(xf, rc.left, rc.bottom, rc.right, rc.top);
    if (!touches(b)) continue;
    ESel s;
    s.page = page; s.kind = 2; s.annot = i; s.sub = sub;
    out->push_back(s);
  }
  std::vector<TagObj> v;
  EnumAll(pg, xf, &v, false);
  std::set<int> seen;
  float pw = d->pw[page], ph = d->ph[page];
  for (auto& o : v) {
    if (!RawSelectable(o, pw, ph, false)) continue;
    if (o.tagged) {
      bool any;
      FRect u = UnionBox(v, o.grp, &any);
      if (!touches(u) || seen.count(o.grp)) continue;
      seen.insert(o.grp);
    } else if (!touches(o.box)) continue;
    ESel s;
    FillSel(&s, page, o);
    out->push_back(s);
  }
  return !out->empty();
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
  if (s.kind == 3) {
    if (s.obj < 0 || s.obj >= PA.FPDFPage_CountObjects(pg)) return false;
    FPDF_PAGEOBJECT o = PA.FPDFPage_GetObject(pg, s.obj);
    float l, b, r, tp;
    if (!o || !PA.FPDFPageObj_GetBounds(o, &l, &b, &r, &tp)) return false;
    *box = TlBoxOfPdf(xf, l, b, r, tp);
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

bool EdSetBox(Tab* t, const ESet& s, FRect* box) {
  bool have = false;
  for (auto& it : s) {
    FRect b;
    if (!EdSelBox(t, it, &b)) continue;
    *box = have ? UnionRect(*box, b) : b;
    have = true;
  }
  return have;
}

// kind of the objects behind a selection: EK_*, or 0 for a mixture / annotation
int EdSelObjKind(Tab* t, const ESel& s) {
  if (!t || !t->doc || (s.kind != 1 && s.kind != 3)) return 0;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg) return 0;
  if (s.kind == 3) {
    FPDF_PAGEOBJECT o = s.obj >= 0 && s.obj < PA.FPDFPage_CountObjects(pg) ? PA.FPDFPage_GetObject(pg, s.obj) : nullptr;
    return o ? KindOfType(PA.FPDFPageObj_GetType(o)) : 0;
  }
  PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
  std::vector<TagObj> v;
  EnumTagged(pg, xf, &v);
  int k = 0;
  for (auto& o : v) {
    if (o.grp != s.grp) continue;
    if (k == 0) k = o.kind;
    else if (k != o.kind) return 0;
  }
  return k;
}
static float TextEffSize(FPDF_PAGEOBJECT o) {
  float fs = 0;
  PA.FPDFTextObj_GetFontSize(o, &fs);
  FS_MATRIX m = {1, 0, 0, 1, 0, 0};
  PA.FPDFPageObj_GetMatrix(o, &m);
  double sc = hypot((double)m.a, (double)m.b);
  if (sc < 1e-6) sc = 1;
  return (float)(fs * sc);
}
int EdSelTextSize(Tab* t, const ESel& s) {
  if (!t || !t->doc || (s.kind != 1 && s.kind != 3)) return 0;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg) return 0;
  if (s.kind == 3) {
    FPDF_PAGEOBJECT o = s.obj >= 0 && s.obj < PA.FPDFPage_CountObjects(pg) ? PA.FPDFPage_GetObject(pg, s.obj) : nullptr;
    if (!o || PA.FPDFPageObj_GetType(o) != FPDF_PAGEOBJ_TEXT) return 0;
    return (int)lroundf(TextEffSize(o));
  }
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

// map of the page points (top-left origin) given by `tl`, written as a PDF-space matrix for FPDFPageObj_Transform
static FS_MATRIX PdfAffine(const PXf& xf, const std::function<void(double, double, double*, double*)>& tl) {
  auto map = [&](double px, double py, double* qx, double* qy) {
    double x, y, x2, y2;
    xf.ToTl(px, py, &x, &y);
    tl(x, y, &x2, &y2);
    xf.ToPdf(x2, y2, qx, qy);
  };
  double x0, y0, x1, y1, x2, y2;
  map(0, 0, &x0, &y0);
  map(1, 0, &x1, &y1);
  map(0, 1, &x2, &y2);
  FS_MATRIX m = {(float)(x1 - x0), (float)(y1 - y0), (float)(x2 - x0), (float)(y2 - y0), (float)x0, (float)y0};
  return m;
}
// clockwise turn by `deg` around (cx, cy) in page points
static FS_MATRIX RotMatrix(const PXf& xf, double cx, double cy, double deg) {
  double a = deg * 3.14159265358979323846 / 180.0, cs = cos(a), sn = sin(a);
  return PdfAffine(xf, [=](double x, double y, double* ox, double* oy) {
    *ox = cx + (x - cx) * cs - (y - cy) * sn;
    *oy = cy + (x - cx) * sn + (y - cy) * cs;
  });
}
static void SetObjAlpha(FPDF_PAGEOBJECT o, int type, int pct) {
  unsigned r = 0, g = 0, b = 0, a = 255;
  unsigned al = (unsigned)min(max(pct, 0), 100) * 255 / 100;
  if (PA.FPDFPageObj_GetFillColor(o, &r, &g, &b, &a)) PA.FPDFPageObj_SetFillColor(o, r, g, b, al);
  else PA.FPDFPageObj_SetFillColor(o, 0, 0, 0, al);
  if (type == FPDF_PAGEOBJ_PATH && PA.FPDFPageObj_GetStrokeColor(o, &r, &g, &b, &a)) PA.FPDFPageObj_SetStrokeColor(o, r, g, b, al);
}

// text lines as objects on the scratch page; line i has its baseline at base0 + i*pitch (page points, top-left origin)
static bool BuildTextLines(Doc* d, FPDF_PAGE tmp, const PXf& xf, float x, float base0, float pitch, const std::vector<ws>& lines, const ws& family, int style,
                           float size, COLORREF c, int grp, int opacity) {
  FPDF_FONT f = LoadDocFont(d, family, style);
  if (!f) return false;
  int fam = FamilyIndex(family);
  if (fam < 0) fam = 0;
  int made = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].empty()) continue;
    FPDF_PAGEOBJECT o = PA.FPDFPageObj_CreateTextObj(d->h, f, size);
    if (!o) return false;
    if (!PA.FPDFText_SetText(o, (FPDF_WIDESTRING)lines[i].c_str())) { PA.FPDFPageObj_Destroy(o); return false; }
    unsigned r, g, b;
    ColorOf(c, &r, &g, &b);
    PA.FPDFPageObj_SetFillColor(o, r, g, b, (unsigned)min(max(opacity, 0), 100) * 255 / 100);
    double px, py;
    xf.ToPdf(x, base0 + pitch * (double)i, &px, &py);
    FS_MATRIX m = {(float)xf.a, (float)xf.b, (float)-xf.c, (float)-xf.d, (float)px, (float)py};
    PA.FPDFPageObj_SetMatrix(o, &m);
    TagObject(d, o, EK_TEXT, grp, (int)lroundf(size * 10), fam * 4 + (style & 3));
    if (opacity != 100) SetMarkInt(d, o, "a", opacity);
    PA.FPDFPage_InsertObject(tmp, o);
    made++;
  }
  return made > 0;
}

static std::vector<ws> SplitLines(const ws& text) {
  std::vector<ws> lines;
  ws cur;
  for (wchar_t ch : text) {
    if (ch == L'\r') continue;
    if (ch == L'\n') { lines.push_back(cur); cur.clear(); } else cur += ch;
  }
  lines.push_back(cur);
  return lines;
}

bool EdAddText(Tab* t, int page, float x, float y, const ws& text, const ws& font, int size, COLORREF c, ESel* out, int style) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages || text.empty()) return false;
  std::vector<ws> lines = SplitLines(text);
  int grp = NewGroupId();
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    return AdoptObjects(d, page, [&](FPDF_PAGE tmp) {
      return BuildTextLines(d, tmp, xf, x, y + size * 0.88f, size * 1.2f, lines, font, style, (float)size, c, grp, 100);
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

// a picture object on the scratch page, filling `r` (page points) and turned clockwise by rotDeg around its centre
static bool BuildImage(Doc* d, FPDF_PAGE tmp, const PXf& xf, FPDF_BITMAP bm, const FRect& r, double rotDeg, int grp, int opacity) {
  FPDF_PAGEOBJECT o = PA.FPDFPageObj_NewImageObj(d->h);
  if (!o) return false;
  FPDF_PAGE pages[1] = {tmp};
  if (!PA.FPDFImageObj_SetBitmap(pages, 1, o, bm)) { PA.FPDFPageObj_Destroy(o); return false; }
  double w = r.r - r.l, h = r.b - r.t, px, py;
  xf.ToPdf(r.l, r.b, &px, &py);                       // lower left corner of the picture on the screen
  FS_MATRIX m = {(float)(xf.a * w), (float)(xf.b * w), (float)(-xf.c * h), (float)(-xf.d * h), (float)px, (float)py};
  PA.FPDFPageObj_SetMatrix(o, &m);
  if (fabs(rotDeg) > 0.01) {
    FS_MATRIX rm = RotMatrix(xf, (r.l + r.r) * 0.5, (r.t + r.b) * 0.5, rotDeg);
    PA.FPDFPageObj_Transform(o, rm.a, rm.b, rm.c, rm.d, rm.e, rm.f);
  }
  TagObject(d, o, EK_IMAGE, grp, 0, 0);
  if (fabs(rotDeg) > 0.01) SetMarkInt(d, o, "r", ((int)lround(rotDeg * 10) % 3600 + 3600) % 3600);
  if (opacity != 100) { SetObjAlpha(o, FPDF_PAGEOBJ_IMAGE, opacity); SetMarkInt(d, o, "a", opacity); }
  PA.FPDFPage_InsertObject(tmp, o);
  return true;
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
      bool good = BuildImage(d, tmp, xf, bm, r, 0, grp, 100);
      PA.FPDFBitmap_Destroy(bm);
      return good;
    });
  });
  if (ok && out) { *out = ESel(); out->page = page; out->kind = 1; out->grp = grp; out->sub = EK_IMAGE; }
  return ok;
}

// a small premultiplied picture of an image object, for the drag preview (upright pages only)
HBITMAP EdImagePreview(Tab* t, const ESet& set, int* ow, int* oh) {
  if (!t || !t->doc || set.size() != 1 || (set[0].kind != 1 && set[0].kind != 3)) return nullptr;
  const ESel& s = set[0];
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg || PA.FPDFPage_GetRotation(pg) != 0) return nullptr;
  PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
  std::vector<TagObj> v;
  EnumAll(pg, xf, &v, false);
  for (auto& o : v) {
    bool mine = s.kind == 1 ? (o.tagged && o.grp == s.grp) : (o.idx == s.obj);
    if (!mine || o.type != FPDF_PAGEOBJ_IMAGE) continue;
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
  return PdfAffine(xf, [=](double x, double y, double* ox, double* oy) {
    *ox = nb.l + (x - ob.l) * sx;
    *oy = nb.t + (y - ob.t) * sy;
  });
}

// what a selection stands for on its page right now
struct Resolved {
  std::vector<TagObj> mem;          // member page objects, in stacking order
  std::vector<int> annots;          // annotation indices
  std::set<int> grps;
  FRect box;
  bool haveBox = false;
  void Add(const FRect& b) { box = haveBox ? UnionRect(box, b) : b; haveBox = true; }
};

// adopt = true: existing objects of the page get the editor's tag (they become ordinary editor objects) and `set` is rewritten to say so
static bool Resolve(Doc* d, FPDF_PAGE pg, const PXf& xf, ESet& set, bool adopt, Resolved* R) {
  int cnt = PA.FPDFPage_CountObjects(pg);
  if (adopt) {
    for (auto& s : set) {
      if (s.kind != 3) continue;
      FPDF_PAGEOBJECT o = s.obj >= 0 && s.obj < cnt ? PA.FPDFPage_GetObject(pg, s.obj) : nullptr;
      if (!o) return false;
      int type = PA.FPDFPageObj_GetType(o), kind = KindOfType(type), extra = 0, font = 0;
      if (type == FPDF_PAGEOBJ_TEXT) {
        extra = (int)lroundf(TextEffSize(o) * 10);
        int fam, st;
        MapPdfFont(PA.FPDFTextObj_GetFont(o), &fam, &st);
        font = fam * 4 + st;
      }
      int g = NewGroupId();
      TagObject(d, o, kind, g, extra, font);
      s.kind = 1; s.grp = g; s.sub = kind; s.obj = -1;
    }
  }
  std::set<int> raw;
  for (auto& s : set) {
    if (s.kind == 1) R->grps.insert(s.grp);
    else if (s.kind == 3) raw.insert(s.obj);
    else if (s.kind == 2) R->annots.push_back(s.annot);
  }
  std::vector<TagObj> all;
  EnumAll(pg, xf, &all, !raw.empty() ? false : true);
  for (auto& o : all) {
    if ((o.tagged && R->grps.count(o.grp)) || (!o.tagged && raw.count(o.idx))) { R->mem.push_back(o); R->Add(o.box); }
  }
  std::vector<int> okAnnots;
  for (int a : R->annots) {
    FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, a);
    if (!an) continue;
    FS_RECTF rc;
    bool ok = PA.FPDFAnnot_GetRect(an, &rc) != 0;
    PA.FPDFPage_CloseAnnot(an);
    if (!ok) continue;
    okAnnots.push_back(a);
    R->Add(TlBoxOfPdf(xf, rc.left, rc.bottom, rc.right, rc.top));
  }
  R->annots = okAnnots;
  return !R->mem.empty() || !R->annots.empty();
}

static bool SetPageOf(const ESet& s, int* page) {
  if (s.empty() || s[0].page < 0) return false;
  *page = s[0].page;
  return true;
}

bool EdTransformSet(Tab* t, ESet& s, FRect ob, FRect nb) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page) || page >= t->doc->npages) return false;
  ESet work = s;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R)) return false;
    double sx = (ob.r - ob.l) > 1e-6 ? (nb.r - nb.l) / (ob.r - ob.l) : 1;
    double sy = (ob.b - ob.t) > 1e-6 ? (nb.b - nb.t) / (ob.b - ob.t) : 1;
    FS_MATRIX m = BoxMatrix(xf, ob, nb);
    for (auto& o : R.mem) {
      PA.FPDFPageObj_Transform(o.o, m.a, m.b, m.c, m.d, m.e, m.f);
      if (o.kind == EK_TEXT && fabs(sx - 1.0) > 1e-4) SetMarkInt(d, o.o, "s", (int)lround(o.extra * sx));
    }
    for (int a : R.annots) {
      FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, a);
      if (!an) continue;
      FS_RECTF rc;
      if (PA.FPDFAnnot_GetRect(an, &rc)) {
        FRect b = TlBoxOfPdf(xf, rc.left, rc.bottom, rc.right, rc.top);
        FRect n = {(float)(nb.l + (b.l - ob.l) * sx), (float)(nb.t + (b.t - ob.t) * sy), (float)(nb.l + (b.r - ob.l) * sx), (float)(nb.t + (b.b - ob.t) * sy)};
        float l, bt, r, tp;
        PdfRectOfTl(xf, n, &l, &bt, &r, &tp);
        FS_RECTF nr = {l, tp, r, bt};
        PA.FPDFAnnot_SetRect(an, &nr);
      }
      PA.FPDFPage_CloseAnnot(an);
    }
    if (!R.mem.empty()) return PA.FPDFPage_GenerateContent(pg) != 0;
    return true;
  });
  if (ok) s = work;
  return ok;
}
bool EdTransformSel(Tab* t, const ESel& s, FRect ob, FRect nb) { ESet set{s}; return EdTransformSet(t, set, ob, nb); }

// text size: every text group is scaled around its own top-left corner
bool EdSetTextSizeSet(Tab* t, ESet& s, int size) {
  int page;
  if (!t || !t->doc || size < 1 || !SetPageOf(s, &page)) return false;
  ESet work = s;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R)) return false;
    int changed = 0;
    for (int g : R.grps) {
      FRect ub;
      bool have = false;
      int cur = 0;
      for (auto& o : R.mem) {
        if (o.grp != g || o.kind != EK_TEXT) continue;
        ub = have ? UnionRect(ub, o.box) : o.box;
        have = true;
        if (!cur) cur = (o.extra + 5) / 10;
      }
      if (!have || cur <= 0 || cur == size) continue;
      double k = (double)size / cur;
      FRect nb = {ub.l, ub.t, (float)(ub.l + (ub.r - ub.l) * k), (float)(ub.t + (ub.b - ub.t) * k)};
      FS_MATRIX m = BoxMatrix(xf, ub, nb);
      for (auto& o : R.mem) {
        if (o.grp != g || o.kind != EK_TEXT) continue;
        PA.FPDFPageObj_Transform(o.o, m.a, m.b, m.c, m.d, m.e, m.f);
        SetMarkInt(d, o.o, "s", (int)lround(o.extra * k));
      }
      changed++;
    }
    if (!changed) return false;
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
  if (ok) s = work;
  return ok;
}
bool EdSetTextSize(Tab* t, const ESel& s, int size) { ESet set{s}; return EdSetTextSizeSet(t, set, size); }

bool EdDeleteSet(Tab* t, const ESet& s) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  ESet work = s;
  return EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, false, &R)) return false;
    std::vector<int> an = R.annots;
    std::sort(an.begin(), an.end());
    for (int i = (int)an.size() - 1; i >= 0; i--) PA.FPDFPage_RemoveAnnot(pg, an[i]);
    int removed = 0;
    for (int i = (int)R.mem.size() - 1; i >= 0; i--) {
      if (PA.FPDFPage_RemoveObject(pg, R.mem[i].o)) { PA.FPDFPageObj_Destroy(R.mem[i].o); removed++; }
    }
    if (removed) return PA.FPDFPage_GenerateContent(pg) != 0;
    return !an.empty();
  });
}
bool EdDeleteSel(Tab* t, const ESel& s) { ESet set{s}; return EdDeleteSet(t, set); }

bool EdSetColorSet(Tab* t, ESet& s, COLORREF c) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  ESet work = s;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    unsigned r, g, b;
    ColorOf(c, &r, &g, &b);
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R)) return false;
    int n = 0;
    for (int a : R.annots) {
      FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, a);
      if (!an) continue;
      // the stored appearance has the old colour in it: re-setting the colour drops it and PDFium builds a new one on load
      if (PA.FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, r, g, b, 255)) n++;
      PA.FPDFPage_CloseAnnot(an);
    }
    for (auto& o : R.mem) {
      unsigned cr, cg, cb, ca = 255;
      if (o.type == FPDF_PAGEOBJ_TEXT) {
        PA.FPDFPageObj_GetFillColor(o.o, &cr, &cg, &cb, &ca);
        PA.FPDFPageObj_SetFillColor(o.o, r, g, b, ca);
        n++;
      } else if (o.type == FPDF_PAGEOBJ_PATH) {
        int fm = 0;
        FPDF_BOOL stroke = 0;
        PA.FPDFPath_GetDrawMode(o.o, &fm, &stroke);
        if (fm) { PA.FPDFPageObj_GetFillColor(o.o, &cr, &cg, &cb, &ca); PA.FPDFPageObj_SetFillColor(o.o, r, g, b, ca); }
        if (stroke || !fm) { PA.FPDFPageObj_GetStrokeColor(o.o, &cr, &cg, &cb, &ca); PA.FPDFPageObj_SetStrokeColor(o.o, r, g, b, ca); }
        n++;
      }
    }
    if (!n) return false;
    if (!R.mem.empty()) return PA.FPDFPage_GenerateContent(pg) != 0;
    return true;
  });
  if (ok) s = work;
  return ok;
}
bool EdSetColorSel(Tab* t, const ESel& s, COLORREF c) { ESet set{s}; return EdSetColorSet(t, set, c); }

// layers: 0 to front, 1 one step forward (above the next object it overlaps), 2 one step back, 3 to back
bool EdZOrderSet(Tab* t, ESet& s, int op) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  ESet work = s;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R)) return false;
    std::vector<int> S;
    for (auto& o : R.mem) S.push_back(o.idx);
    if (S.empty()) return false;
    int n = PA.FPDFPage_CountObjects(pg);
    std::vector<FPDF_PAGEOBJECT> objs(n);
    for (int i = 0; i < n; i++) objs[i] = PA.FPDFPage_GetObject(pg, i);
    std::set<int> inS(S.begin(), S.end());
    // bounds of the selection (pdf space)
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
    auto countBelow = [&](int idx) { int c = 0; for (int i : S) if (i < idx) c++; return c; };
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
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
  if (ok) s = work;
  return ok;
}
bool EdZOrder(Tab* t, const ESel& s, int op) { ESet set{s}; return EdZOrderSet(t, set, op); }

// ---- groups
bool EdGroup(Tab* t, ESet& s) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  for (auto& it : s) if (it.kind == 2) { ShowToast(L"Рисунки и фигуры нельзя объединять в группу"); return false; }
  if (s.size() < 2) { ShowToast(L"Выделите несколько объектов (рамкой или с клавишей Shift)"); return false; }
  ESet work = s;
  int ng = NewGroupId();
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R) || R.mem.empty()) return false;
    for (auto& o : R.mem) {
      SetMarkInt(d, o.o, "u", o.unit);
      SetMarkInt(d, o.o, "g", ng);
    }
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
  if (ok) {
    s.clear();
    ESel g;
    g.page = page; g.kind = 1; g.grp = ng; g.sub = 0;
    s.push_back(g);
  }
  return ok;
}
bool EdUngroup(Tab* t, ESet& s) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  ESet work = s;
  ESet res;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R) || R.mem.empty()) return false;
    std::set<int> units;
    bool any = false;
    for (auto& o : R.mem) {
      if (o.unit != o.grp) { SetMarkInt(d, o.o, "g", o.unit); any = true; }
      units.insert(o.unit);
    }
    if (!any) return false;
    for (int u : units) { ESel g; g.page = page; g.kind = 1; g.grp = u; res.push_back(g); }
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
  if (ok) s = res;
  else ShowToast(L"Это не группа");
  return ok;
}

// ---- align / spread
bool EdAlign(Tab* t, ESet& s, int op) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  ESet work = s;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved all;
    if (!Resolve(d, pg, xf, work, true, &all)) return false;
    // one unit per selected item: its objects or its annotation
    struct Unit { std::vector<int> mem; int annot = -1; FRect box; double dx = 0, dy = 0; };
    std::vector<Unit> U;
    for (auto& it : work) {
      Unit u;
      bool have = false;
      if (it.kind == 1) {
        for (size_t i = 0; i < all.mem.size(); i++) if (all.mem[i].grp == it.grp) { u.mem.push_back((int)i); u.box = have ? UnionRect(u.box, all.mem[i].box) : all.mem[i].box; have = true; }
      } else if (it.kind == 2) {
        FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, it.annot);
        FS_RECTF rc;
        if (an && PA.FPDFAnnot_GetRect(an, &rc)) { u.annot = it.annot; u.box = TlBoxOfPdf(xf, rc.left, rc.bottom, rc.right, rc.top); have = true; }
        if (an) PA.FPDFPage_CloseAnnot(an);
      }
      if (have) U.push_back(u);
    }
    if (U.empty()) return false;
    if (op >= 6 && U.size() < 3) return false;
    FRect ref = U[0].box;
    if (U.size() == 1) ref = {0, 0, d->pw[page], d->ph[page]};
    else for (auto& u : U) ref = UnionRect(ref, u.box);
    if (op < 6) {
      for (auto& u : U) {
        const FRect& b = u.box;
        switch (op) {
          case 0: u.dx = ref.l - b.l; break;
          case 1: u.dx = (ref.l + ref.r) / 2 - (b.l + b.r) / 2; break;
          case 2: u.dx = ref.r - b.r; break;
          case 3: u.dy = ref.t - b.t; break;
          case 4: u.dy = (ref.t + ref.b) / 2 - (b.t + b.b) / 2; break;
          case 5: u.dy = ref.b - b.b; break;
        }
      }
    } else {
      bool h = op == 6;
      std::vector<int> ord(U.size());
      for (size_t i = 0; i < ord.size(); i++) ord[i] = (int)i;
      std::sort(ord.begin(), ord.end(), [&](int a, int b) { return h ? U[a].box.l + U[a].box.r < U[b].box.l + U[b].box.r : U[a].box.t + U[a].box.b < U[b].box.t + U[b].box.b; });
      double total = 0;
      for (auto& u : U) total += h ? (u.box.r - u.box.l) : (u.box.b - u.box.t);
      double span = h ? ref.r - ref.l : ref.b - ref.t;
      double gap = (span - total) / (double)(U.size() - 1);
      double pos = h ? ref.l : ref.t;
      for (int i : ord) {
        Unit& u = U[i];
        double sz = h ? (u.box.r - u.box.l) : (u.box.b - u.box.t);
        if (h) u.dx = pos - u.box.l; else u.dy = pos - u.box.t;
        pos += sz + gap;
      }
    }
    int moved = 0, objs = 0;
    for (auto& u : U) {
      if (fabs(u.dx) < 0.01 && fabs(u.dy) < 0.01) continue;
      FRect nb = {(float)(u.box.l + u.dx), (float)(u.box.t + u.dy), (float)(u.box.r + u.dx), (float)(u.box.b + u.dy)};
      if (u.annot >= 0) {
        FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, u.annot);
        if (an) {
          float l, bt, r, tp;
          PdfRectOfTl(xf, nb, &l, &bt, &r, &tp);
          FS_RECTF nr = {l, tp, r, bt};
          PA.FPDFAnnot_SetRect(an, &nr);
          PA.FPDFPage_CloseAnnot(an);
        }
      } else {
        FS_MATRIX m = BoxMatrix(xf, u.box, nb);
        for (int i : u.mem) { PA.FPDFPageObj_Transform(all.mem[i].o, m.a, m.b, m.c, m.d, m.e, m.f); objs++; }
      }
      moved++;
    }
    if (!moved) return false;
    if (objs) return PA.FPDFPage_GenerateContent(pg) != 0;
    return true;
  });
  if (ok) s = work;
  return ok;
}

// ---- turning and transparency
bool EdRotateSet(Tab* t, ESet& s, double deg) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  ESet work = s;
  bool skipped = false;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R) || R.mem.empty()) return false;
    skipped = !R.annots.empty();
    FRect ub = R.mem[0].box;
    for (auto& o : R.mem) ub = UnionRect(ub, o.box);
    FS_MATRIX m = RotMatrix(xf, (ub.l + ub.r) * 0.5, (ub.t + ub.b) * 0.5, deg);
    for (auto& o : R.mem) {
      PA.FPDFPageObj_Transform(o.o, m.a, m.b, m.c, m.d, m.e, m.f);
      int r = (((o.rot + (int)lround(deg * 10)) % 3600) + 3600) % 3600;
      SetMarkInt(d, o.o, "r", r);
    }
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
  if (ok) s = work;
  else if (!s.empty()) ShowToast(L"Этот объект нельзя повернуть на произвольный угол");
  if (ok && skipped) ShowToast(L"Рисунки от руки и фигуры не поворачиваются");
  return ok;
}

bool EdSetOpacity(Tab* t, ESet& s, int percent) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  percent = min(max(percent, 0), 100);
  ESet work = s;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, true, &R) || R.mem.empty()) return false;
    for (auto& o : R.mem) {
      SetObjAlpha(o.o, o.type, percent);
      SetMarkInt(d, o.o, "a", percent);
    }
    return PA.FPDFPage_GenerateContent(pg) != 0;
  });
  if (ok) s = work;
  else ShowToast(L"Прозрачность задаётся для текста, картинок и линий; фигуры и рисунки от руки пока не поддерживаются");
  return ok;
}

static bool FirstMember(Tab* t, const ESet& s, TagObj* out) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page) || page >= t->doc->npages) return false;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, page);
  if (!pg) return false;
  PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
  ESet w = s;
  Resolved R;
  if (!Resolve(d, pg, xf, w, false, &R) || R.mem.empty()) return false;
  *out = R.mem[0];
  return true;
}
int EdSetAngle(Tab* t, const ESet& s) {
  TagObj o;
  return FirstMember(t, s, &o) ? ((o.rot + 5) / 10) % 360 : 0;
}
int EdSetOpacityOf(Tab* t, const ESet& s) {
  TagObj o;
  return FirstMember(t, s, &o) ? o.op : 100;
}

// ---- copy / paste: the objects are described by value, so they can be put on any page of any document
namespace {
struct ClipItem {
  int type = 0;                      // 1 text, 2 picture, 3 rectangle / ellipse, 4 ink
  int unit = 0;
  FRect box;                         // page points
  ws text, family;
  int style = 0;
  float size = 12, baseline = 0;     // text: box.l is the left edge of the first letter
  COLORREF color = 0;
  int bw = 0, bh = 0;
  std::vector<uint8_t> bgra;
  double angle = 0;                  // picture: clockwise, around the centre of `box`
  int sub = 0, width = 1;            // annotation
  std::vector<PointF> pts;
  int opacity = 100;
};
}
static std::vector<ClipItem> g_clip;

bool EdClipHas() { return !g_clip.empty(); }

static ws TextOfObj(FPDF_TEXTPAGE tp, FPDF_PAGEOBJECT o) {
  if (!tp) return L"";
  unsigned long need = PA.FPDFTextObj_GetText(o, tp, nullptr, 0);
  if (need <= 2) return L"";
  std::vector<unsigned short> buf(need / 2 + 4, 0);
  PA.FPDFTextObj_GetText(o, tp, (FPDF_WCHAR*)buf.data(), need);
  return ws((const wchar_t*)buf.data());
}

struct TxtRec {
  TagObj o;
  ws text;
  double bx = 0, by = 0;
  float size = 12;
  COLORREF color = 0;
  int fam = 0, style = 0;
};
static void ReadTextRec(const PXf& xf, FPDF_TEXTPAGE tp, const TagObj& to, TxtRec* r) {
  r->o = to;
  r->text = TextOfObj(tp, to.o);
  FS_MATRIX m = {1, 0, 0, 1, 0, 0};
  PA.FPDFPageObj_GetMatrix(to.o, &m);
  xf.ToTl(m.e, m.f, &r->bx, &r->by);
  r->size = to.tagged && to.kind == EK_TEXT && to.extra > 0 ? to.extra / 10.0f : TextEffSize(to.o);
  unsigned cr = 0, cg = 0, cb = 0, ca = 255;
  PA.FPDFPageObj_GetFillColor(to.o, &cr, &cg, &cb, &ca);
  r->color = RGB(cr, cg, cb);
  if (to.tagged && to.kind == EK_TEXT) { r->fam = min(max(to.font / 4, 0), kNFonts - 1); r->style = to.font & 3; }
  else MapPdfFont(PA.FPDFTextObj_GetFont(to.o), &r->fam, &r->style);
}
// the pieces of text of a selection, grouped into lines (top to bottom, left to right)
static void LineUp(std::vector<TxtRec>& v, std::vector<std::vector<int>>* lines) {
  std::vector<int> ord(v.size());
  for (size_t i = 0; i < ord.size(); i++) ord[i] = (int)i;
  std::sort(ord.begin(), ord.end(), [&](int a, int b) { return v[a].by < v[b].by; });
  for (int i : ord) {
    if (!lines->empty()) {
      const TxtRec& first = v[lines->back()[0]];
      if (fabs(v[i].by - first.by) <= 0.45 * max(first.size, v[i].size)) { lines->back().push_back(i); continue; }
    }
    lines->push_back({i});
  }
  for (auto& ln : *lines) std::sort(ln.begin(), ln.end(), [&](int a, int b) { return v[a].o.box.l < v[b].o.box.l; });
}
static ws JoinLine(const std::vector<TxtRec>& v, const std::vector<int>& ln) {
  ws out;
  for (size_t k = 0; k < ln.size(); k++) {
    const TxtRec& r = v[ln[k]];
    if (k > 0) {
      const TxtRec& p = v[ln[k - 1]];
      float gap = r.o.box.l - p.o.box.r;
      bool sp = (!out.empty() && out.back() == L' ') || (!r.text.empty() && r.text[0] == L' ');
      if (!sp && gap > 0.12f * r.size) out += L' ';
    }
    out += r.text;
  }
  return out;
}

static void FillTextRecs(Doc* d, FPDF_PAGE pg, const PXf& xf, const Resolved& R, std::vector<TxtRec>* out) {
  (void)d;
  FPDF_TEXTPAGE tp = PA.FPDFText_LoadPage(pg);
  for (auto& o : R.mem) {
    if (o.type != FPDF_PAGEOBJ_TEXT) continue;
    TxtRec r;
    ReadTextRec(xf, tp, o, &r);
    out->push_back(r);
  }
  if (tp) PA.FPDFText_ClosePage(tp);
}

bool EdGetText(Tab* t, const ESet& s, EdTextInfo* out) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page) || page >= t->doc->npages) return false;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, page);
  if (!pg) return false;
  PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
  ESet w = s;
  Resolved R;
  if (!Resolve(d, pg, xf, w, false, &R)) return false;
  std::vector<TxtRec> v;
  FillTextRecs(d, pg, xf, R, &v);
  if (v.empty()) return false;
  std::vector<std::vector<int>> lines;
  LineUp(v, &lines);
  EdTextInfo ti;
  FRect u = v[0].o.box;
  for (size_t i = 0; i < lines.size(); i++) {
    if (i) ti.text += L"\r\n";
    ti.text += JoinLine(v, lines[i]);
  }
  for (auto& r : v) u = UnionRect(u, r.o.box);
  const TxtRec& f = v[lines[0][0]];
  ti.family = kFonts[f.fam].key;
  ti.style = f.style;
  ti.size = (int)lroundf(f.size);
  ti.color = f.color;
  ti.box = u;
  ti.lines = (int)lines.size();
  *out = ti;
  return true;
}

bool EdExpandLine(Tab* t, const ESel& s, ESet* out) {
  out->clear();
  out->push_back(s);
  if (!t || !t->doc || s.kind != 3 || s.page < 0 || s.page >= t->doc->npages) return false;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, s.page);
  if (!pg) return false;
  PXf xf = MakeXf(pg, d->pw[s.page], d->ph[s.page]);
  std::vector<TagObj> all;
  EnumAll(pg, xf, &all, false);
  float pw = d->pw[s.page], ph = d->ph[s.page];
  FPDF_TEXTPAGE tp = PA.FPDFText_LoadPage(pg);
  std::vector<TxtRec> v;
  int seed = -1;
  for (auto& o : all) {
    if (o.type != FPDF_PAGEOBJ_TEXT || !RawSelectable(o, pw, ph, false)) continue;
    TxtRec r;
    ReadTextRec(xf, tp, o, &r);
    if (!o.tagged && o.idx == s.obj) seed = (int)v.size();
    v.push_back(r);
  }
  if (tp) PA.FPDFText_ClosePage(tp);
  if (seed < 0) return false;
  double by0 = v[seed].by;
  float sz = v[seed].size;
  std::vector<int> cand;
  for (size_t i = 0; i < v.size(); i++) if ((int)i == seed || fabs(v[i].by - by0) <= 0.45 * max(sz, v[i].size)) cand.push_back((int)i);
  std::sort(cand.begin(), cand.end(), [&](int a, int b) { return v[a].o.box.l < v[b].o.box.l; });
  size_t sp = 0;
  while (sp < cand.size() && cand[sp] != seed) sp++;
  size_t lo = sp, hi = sp;
  float reach = v[seed].o.box.r;
  for (size_t k = sp + 1; k < cand.size(); k++) {
    if (v[cand[k]].o.box.l - reach > sz * 1.6f) break;
    reach = max(reach, v[cand[k]].o.box.r);
    hi = k;
  }
  float left = v[seed].o.box.l;
  for (size_t k = sp; k-- > 0;) {
    if (left - v[cand[k]].o.box.r > sz * 1.6f) break;
    left = min(left, v[cand[k]].o.box.l);
    lo = k;
  }
  out->clear();
  std::set<int> grps;
  for (size_t k = lo; k <= hi; k++) {
    const TagObj& o = v[cand[k]].o;
    ESel e;
    if (o.tagged) { if (grps.count(o.grp)) continue; grps.insert(o.grp); FillSel(&e, s.page, o); }
    else FillSel(&e, s.page, o);
    out->push_back(e);
  }
  return true;
}

// replaces the text objects of the selection by new text in the given face; the first baseline, the left edge and the line pitch stay
bool EdReplaceText(Tab* t, ESet& s, const ws& text, const ws& family, int style, int size, COLORREF c) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page)) return false;
  ESet work = s;
  int grp = NewGroupId();
  bool blank = true;
  for (wchar_t ch : text) if (ch != L' ' && ch != L'\r' && ch != L'\n' && ch != L'\t') blank = false;
  std::vector<ws> lines = SplitLines(text);
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    Resolved R;
    if (!Resolve(d, pg, xf, work, false, &R)) return false;
    std::vector<TxtRec> v;
    FillTextRecs(d, pg, xf, R, &v);
    if (v.empty()) return false;
    std::vector<std::vector<int>> ln;
    LineUp(v, &ln);
    double x0 = 1e9;
    for (auto& r : v) x0 = min(x0, r.bx);
    double base0 = v[ln[0][0]].by, osz = v[ln[0][0]].size;
    double pitch = ln.size() > 1 ? (v[ln.back()[0]].by - base0) / (double)(ln.size() - 1) : osz * 1.2;
    double ratio = osz > 0.1 ? pitch / osz : 1.2;
    if (ratio < 0.8) ratio = 1.2;
    for (int i = (int)v.size() - 1; i >= 0; i--) {
      if (PA.FPDFPage_RemoveObject(pg, v[i].o.o)) PA.FPDFPageObj_Destroy(v[i].o.o);
    }
    // (the removal has to be sorted out: indices above are not needed any more)
    if (!PA.FPDFPage_GenerateContent(pg)) return false;
    if (blank) return true;
    return AdoptObjects(d, page, [&](FPDF_PAGE tmp) {
      return BuildTextLines(d, tmp, xf, (float)x0, (float)base0, (float)(ratio * size), lines, family, style, (float)size, c, grp, 100);
    });
  });
  if (ok) {
    s.clear();
    if (!blank) { ESel e; e.page = page; e.kind = 1; e.grp = grp; e.sub = EK_TEXT; s.push_back(e); }
  }
  return ok;
}

bool EdSetTextStyle(Tab* t, ESet& s, const ws& family, int style) {
  EdTextInfo ti;
  if (!EdGetText(t, s, &ti)) return false;
  return EdReplaceText(t, s, ti.text, family, style, ti.size, ti.color);
}

bool EdCopy(Tab* t, const ESet& s) {
  int page;
  if (!t || !t->doc || !SetPageOf(s, &page) || page >= t->doc->npages) return false;
  Doc* d = t->doc.get();
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  FPDF_PAGE pg = PageOf(d, page);
  if (!pg) return false;
  PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
  std::vector<TagObj> all;
  EnumAll(pg, xf, &all, false);
  FPDF_TEXTPAGE tp = nullptr;
  std::vector<ClipItem> items;
  int unsupported = 0;
  for (size_t ui = 0; ui < s.size(); ui++) {
    const ESel& it = s[ui];
    if (it.kind == 2) {
      FPDF_ANNOTATION an = PA.FPDFPage_GetAnnot(pg, it.annot);
      if (!an) continue;
      int sub = PA.FPDFAnnot_GetSubtype(an);
      FS_RECTF rc;
      unsigned r = 0, g = 0, b = 0, a = 255;
      float hr = 0, vr = 0, bw = 1;
      PA.FPDFAnnot_GetColor(an, FPDFANNOT_COLORTYPE_Color, &r, &g, &b, &a);
      PA.FPDFAnnot_GetBorder(an, &hr, &vr, &bw);
      if (PA.FPDFAnnot_GetRect(an, &rc)) {
        ClipItem ci;
        ci.unit = (int)ui;
        ci.box = TlBoxOfPdf(xf, rc.left, rc.bottom, rc.right, rc.top);
        ci.color = RGB(r, g, b);
        ci.width = max(1, (int)lroundf(bw));
        ci.sub = sub;
        if (sub == FPDF_ANNOT_SQUARE || sub == FPDF_ANNOT_CIRCLE) { ci.type = 3; items.push_back(ci); }
        else if (sub == FPDF_ANNOT_INK) {
          unsigned long np = PA.FPDFAnnot_GetInkListCount(an);
          for (unsigned long pi = 0; pi < np; pi++) {
            unsigned long len = PA.FPDFAnnot_GetInkListPath(an, pi, nullptr, 0);
            std::vector<FS_POINTF> pts(len);
            if (len) PA.FPDFAnnot_GetInkListPath(an, pi, pts.data(), len);
            ClipItem cj = ci;
            cj.type = 4;
            for (auto& p : pts) { double x, y; xf.ToTl(p.x, p.y, &x, &y); cj.pts.push_back(PointF((float)x, (float)y)); }
            if (cj.pts.size() >= 2) items.push_back(cj);
          }
        } else unsupported++;
      }
      PA.FPDFPage_CloseAnnot(an);
      continue;
    }
    for (auto& o : all) {
      bool mine = (it.kind == 1 && o.tagged && o.grp == it.grp) || (it.kind == 3 && !o.tagged && o.idx == it.obj);
      if (!mine) continue;
      ClipItem ci;
      ci.unit = (int)ui;
      ci.box = o.box;
      ci.opacity = o.tagged ? o.op : 100;
      if (o.type == FPDF_PAGEOBJ_TEXT) {
        if (!tp) tp = PA.FPDFText_LoadPage(pg);
        TxtRec r;
        ReadTextRec(xf, tp, o, &r);
        if (r.text.empty()) continue;
        ci.type = 1; ci.text = r.text; ci.family = kFonts[r.fam].key; ci.style = r.style; ci.size = r.size; ci.color = r.color;
        ci.box.l = (float)r.bx; ci.baseline = (float)r.by;
        items.push_back(ci);
      } else if (o.type == FPDF_PAGEOBJ_IMAGE) {
        FPDF_BITMAP bm = PA.FPDFImageObj_GetRenderedBitmap(d->h, pg, o.o);
        if (!bm) { unsupported++; continue; }
        int w = PA.FPDFBitmap_GetWidth(bm), h = PA.FPDFBitmap_GetHeight(bm), stride = PA.FPDFBitmap_GetStride(bm), fmt = PA.FPDFBitmap_GetFormat(bm);
        const unsigned char* src = (const unsigned char*)PA.FPDFBitmap_GetBuffer(bm);
        int bpp = fmt == FPDFBitmap_BGR ? 3 : (fmt == FPDFBitmap_Gray ? 1 : 4);
        if (w > 0 && h > 0 && src) {
          ci.bw = w; ci.bh = h;
          ci.bgra.resize((size_t)w * h * 4);
          for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
              const unsigned char* px = src + (size_t)y * stride + (size_t)x * bpp;
              unsigned char* dp = &ci.bgra[((size_t)y * w + x) * 4];
              dp[0] = px[0]; dp[1] = bpp >= 3 ? px[1] : px[0]; dp[2] = bpp >= 3 ? px[2] : px[0];
              dp[3] = (bpp == 4 && fmt == FPDFBitmap_BGRA) ? px[3] : 255;
            }
          FS_MATRIX m = {1, 0, 0, 1, 0, 0};
          PA.FPDFPageObj_GetMatrix(o.o, &m);
          double x0, y0, x1, y1, x2, y2;
          xf.ToTl(m.e, m.f, &x0, &y0);
          xf.ToTl(m.e + m.a, m.f + m.b, &x1, &y1);
          xf.ToTl(m.e + m.c, m.f + m.d, &x2, &y2);
          double wp = hypot(x1 - x0, y1 - y0), hp = hypot(x2 - x0, y2 - y0);
          ci.angle = atan2(y1 - y0, x1 - x0) * 180.0 / 3.14159265358979323846;
          double cx = (o.box.l + o.box.r) * 0.5, cy = (o.box.t + o.box.b) * 0.5;
          ci.box = {(float)(cx - wp / 2), (float)(cy - hp / 2), (float)(cx + wp / 2), (float)(cy + hp / 2)};
          ci.type = 2;
          items.push_back(ci);
        }
        PA.FPDFBitmap_Destroy(bm);
      } else unsupported++;
    }
  }
  if (tp) PA.FPDFText_ClosePage(tp);
  if (items.empty()) {
    ShowToast(unsupported ? L"Этот вид объектов (линии, векторная графика) копировать нельзя" : L"Нечего копировать");
    return false;
  }
  g_clip = std::move(items);
  if (unsupported) ShowToast(L"Часть объектов (линии, векторная графика) скопировать нельзя");
  return true;
}

bool EdPaste(Tab* t, int page, float dx, float dy, ESet* out) {
  if (!t || !t->doc || page < 0 || page >= t->doc->npages || g_clip.empty()) return false;
  std::vector<ClipItem> items = g_clip;
  std::map<int, int> grpOf;
  for (auto& ci : items) if (ci.type <= 2 && !grpOf.count(ci.unit)) grpOf[ci.unit] = NewGroupId();
  ESet res;
  bool ok = EdEdit(t, false, {page}, [&](Doc* d) {
    FPDF_PAGE pg = PageOf(d, page);
    if (!pg) return false;
    PXf xf = MakeXf(pg, d->pw[page], d->ph[page]);
    float pw = d->pw[page], ph = d->ph[page];
    // keep the copies on the page
    float l = 1e9f, tt = 1e9f, r = -1e9f, b = -1e9f;
    for (auto& ci : items) { l = min(l, ci.box.l); tt = min(tt, ci.box.t); r = max(r, ci.box.r); b = max(b, ci.box.b); }
    float ox = dx, oy = dy;
    if (r + ox > pw) ox -= r + ox - pw;
    if (l + ox < 0) ox = -l;
    if (b + oy > ph) oy -= b + oy - ph;
    if (tt + oy < 0) oy = -tt;
    // annotations first (they stay with the page itself), then the objects through the scratch page
    bool anyObj = false;
    for (auto& ci : items) {
      if (ci.type == 3) {
        FPDF_ANNOTATION an = PA.FPDFPage_CreateAnnot(pg, ci.sub);
        if (!an) return false;
        FRect nb = {ci.box.l + ox, ci.box.t + oy, ci.box.r + ox, ci.box.b + oy};
        float rl, rb, rr, rt;
        PdfRectOfTl(xf, nb, &rl, &rb, &rr, &rt);
        FS_RECTF rc = {rl, rt, rr, rb};
        PA.FPDFAnnot_SetRect(an, &rc);
        unsigned cr, cg, cb;
        ColorOf(ci.color, &cr, &cg, &cb);
        PA.FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, cr, cg, cb, 255);
        PA.FPDFAnnot_SetBorder(an, 0, 0, (float)ci.width);
        PA.FPDFAnnot_SetFlags(an, 4);
        ESel e; e.page = page; e.kind = 2; e.annot = PA.FPDFPage_GetAnnotCount(pg) - 1; e.sub = ci.sub;
        res.push_back(e);
        PA.FPDFPage_CloseAnnot(an);
      } else if (ci.type == 4) {
        FPDF_ANNOTATION an = PA.FPDFPage_CreateAnnot(pg, FPDF_ANNOT_INK);
        if (!an) return false;
        std::vector<FS_POINTF> v;
        float L = 1e9f, B = 1e9f, R = -1e9f, T = -1e9f;
        for (auto& p : ci.pts) {
          double x, y;
          xf.ToPdf(p.X + ox, p.Y + oy, &x, &y);
          v.push_back({(float)x, (float)y});
          L = min(L, (float)x); R = max(R, (float)x); B = min(B, (float)y); T = max(T, (float)y);
        }
        float pad = ci.width * 0.5f + 1;
        FS_RECTF rc = {L - pad, T + pad, R + pad, B - pad};
        PA.FPDFAnnot_SetRect(an, &rc);
        PA.FPDFAnnot_AddInkStroke(an, v.data(), v.size());
        unsigned cr, cg, cb;
        ColorOf(ci.color, &cr, &cg, &cb);
        PA.FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, cr, cg, cb, 255);
        PA.FPDFAnnot_SetBorder(an, 0, 0, (float)ci.width);
        PA.FPDFAnnot_SetFlags(an, 4);
        ESel e; e.page = page; e.kind = 2; e.annot = PA.FPDFPage_GetAnnotCount(pg) - 1; e.sub = FPDF_ANNOT_INK;
        res.push_back(e);
        PA.FPDFPage_CloseAnnot(an);
      } else anyObj = true;
    }
    if (!anyObj) return true;
    bool good = AdoptObjects(d, page, [&](FPDF_PAGE tmp) {
      int made = 0;
      for (auto& ci : items) {
        if (ci.type == 1) {
          if (BuildTextLines(d, tmp, xf, ci.box.l + ox, ci.baseline + oy, ci.size * 1.2f, {ci.text}, ci.family, ci.style, ci.size, ci.color, grpOf[ci.unit], ci.opacity)) made++;
        } else if (ci.type == 2) {
          FPDF_BITMAP bm = PA.FPDFBitmap_Create(ci.bw, ci.bh, 1);
          if (!bm) continue;
          unsigned char* dst = (unsigned char*)PA.FPDFBitmap_GetBuffer(bm);
          int ds = PA.FPDFBitmap_GetStride(bm);
          for (int y = 0; y < ci.bh; y++) memcpy(dst + (size_t)y * ds, &ci.bgra[(size_t)y * ci.bw * 4], (size_t)ci.bw * 4);
          FRect nb = {ci.box.l + ox, ci.box.t + oy, ci.box.r + ox, ci.box.b + oy};
          if (BuildImage(d, tmp, xf, bm, nb, ci.angle, grpOf[ci.unit], ci.opacity)) made++;
          PA.FPDFBitmap_Destroy(bm);
        }
      }
      return made > 0;
    });
    if (!good) return false;
    for (auto& kv : grpOf) { ESel e; e.page = page; e.kind = 1; e.grp = kv.second; res.push_back(e); }
    return true;
  });
  if (ok && out) *out = res;
  return ok;
}

// ------------------------------------------------------------------ every page to one paper size
struct PaperDef { double w, h; };
static const PaperDef kPapers[] = {{595.276, 841.89}, {841.89, 1190.55}, {419.528, 595.276}, {612, 792}, {612, 1008}};

static void FitTarget(const FitOpts& o, double vw, double vh, double* tw, double* th) {
  PaperDef pp = kPapers[min(max(o.paper, 0), 4)];
  bool land = o.orient == 2 || (o.orient == 0 && vw > vh * 1.02);
  *tw = land ? pp.h : pp.w;
  *th = land ? pp.w : pp.h;
}

// Fits one page (as it is seen, with its rotation) onto the paper: scales, centres, clips to the old page, moves annotations. Returns true when it changed the page.
bool FitPageToPaper(FPDF_PAGE pg, const FitOpts& o) {
  double vw = PA.FPDF_GetPageWidthF(pg), vh = PA.FPDF_GetPageHeightF(pg), tw, th;
  FitTarget(o, vw, vh, &tw, &th);
  if (fabs(vw - tw) <= 1.0 && fabs(vh - th) <= 1.0) return false;
  double m = o.marginMm * 72.0 / 25.4;
  int rot = PA.FPDFPage_GetRotation(pg) & 3;
  float ml = 0, mb = 0, mr = 612, mt = 792, cl, cb, cr, ct;
  if (!PA.FPDFPage_GetMediaBox(pg, &ml, &mb, &mr, &mt)) { ml = 0; mb = 0; mr = 612; mt = 792; }
  if (!PA.FPDFPage_GetCropBox(pg, &cl, &cb, &cr, &ct)) { cl = ml; cb = mb; cr = mr; ct = mt; }
  cl = max(cl, min(ml, mr)); cb = max(cb, min(mb, mt)); cr = min(cr, max(ml, mr)); ct = min(ct, max(mb, mt));
  double cw = cr - cl, ch = ct - cb;
  if (cw < 1 || ch < 1) return false;
  double s = min((tw - 2 * m) / vw, (th - 2 * m) / vh);
  if (!o.enlarge) s = min(s, 1.0);
  if (s <= 0) return false;
  double Wm = (rot & 1) ? th : tw, Hm = (rot & 1) ? tw : th;
  double e = (Wm - s * cw) / 2 - s * cl, f = (Hm - s * ch) / 2 - s * cb;
  FS_MATRIX mx = {(float)s, 0, 0, (float)s, (float)e, (float)f};
  FS_RECTF clip = {(float)((Wm - s * cw) / 2), (float)((Hm - s * ch) / 2 + s * ch), (float)((Wm - s * cw) / 2 + s * cw), (float)((Hm - s * ch) / 2)};
  if (!PA.FPDFPage_TransFormWithClip(pg, &mx, &clip)) return false;
  PA.FPDFPage_TransformAnnots(pg, s, 0, 0, s, e, f);
  PA.FPDFPage_SetMediaBox(pg, 0, 0, (float)Wm, (float)Hm);
  PA.FPDFPage_SetCropBox(pg, 0, 0, (float)Wm, (float)Hm);
  PA.FPDFPage_SetTrimBox(pg, 0, 0, (float)Wm, (float)Hm);
  PA.FPDFPage_SetBleedBox(pg, 0, 0, (float)Wm, (float)Hm);
  PA.FPDFPage_SetArtBox(pg, 0, 0, (float)Wm, (float)Hm);
  return true;
}

// for "save a copy": every page of a loaded document onto the paper; the caller holds g_pdf
int FitDocumentPages(FPDF_DOCUMENT doc, const FitOpts& o) {
  int n = PA.FPDF_GetPageCount(doc), changed = 0;
  for (int p = 0; p < n; p++) {
    FPDF_PAGE pg = PA.FPDF_LoadPage(doc, p);
    if (!pg) continue;
    if (FitPageToPaper(pg, o)) { PA.FPDFPage_GenerateContent(pg); changed++; }
    PA.FPDF_ClosePage(pg);
  }
  return changed;
}

bool EdFitPages(Tab* t, const FitOpts& o, int* changedOut) {
  if (changedOut) *changedOut = 0;
  if (!t || !t->doc) return false;
  Doc* d = t->doc.get();
  int need = 0;
  for (int p = 0; p < d->npages; p++) {
    double tw, th;
    FitTarget(o, d->pw[p], d->ph[p], &tw, &th);
    if (fabs(d->pw[p] - tw) > 1.0 || fabs(d->ph[p] - th) > 1.0) need++;
  }
  if (!need) { ShowToast(L"Все страницы уже нужного размера"); return false; }
  int changed = 0;
  bool ok = EdEdit(t, true, {}, [&](Doc* dd) {
    for (int p = 0; p < dd->npages; p++) {
      FPDF_PAGE pg = PageOf(dd, p);
      if (!pg) return false;
      if (FitPageToPaper(pg, o)) changed++;
    }
    return changed > 0;
  });
  if (changedOut) *changedOut = changed;
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

bool EdSaveAs(Tab* t, bool fitA4) {
  if (!t || !t->doc) return false;
  Doc* d = t->doc.get();
  ExportOpts o = g_lastExport;
  if (fitA4) { o.paper = 1; if (o.mode == 0) o.mode = 1; }
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
    if (f.rfind("paper=", 0) == 0) { o.paper = f[6] - '0'; size_t sp = f.find(' '); f = sp == std::string::npos ? "" : f.substr(sp + 1); }
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
