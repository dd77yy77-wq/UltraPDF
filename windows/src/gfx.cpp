// GDI+ helpers: vector icons, rounded rectangles, alpha fills, fonts
#include "app.h"

using namespace Gdiplus;

static ULONG_PTR g_gdipToken = 0;
static Bitmap* g_appIcon = nullptr;
static std::map<int, HFONT> g_fonts;

void GfxInit() {
  GdiplusStartupInput in;
  GdiplusStartup(&g_gdipToken, &in, nullptr);
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(300), RT_RCDATA);
  if (r) {
    HGLOBAL g = LoadResource(nullptr, r);
    void* p = LockResource(g);
    DWORD sz = SizeofResource(nullptr, r);
    IStream* st = SHCreateMemStream((const BYTE*)p, sz);
    if (st) {
      g_appIcon = Bitmap::FromStream(st);
      st->Release();
    }
  }
}
void GfxShutdown() {
  delete g_appIcon;
  g_appIcon = nullptr;
  for (auto& f : g_fonts) DeleteObject(f.second);
  g_fonts.clear();
  GdiplusShutdown(g_gdipToken);
}

static Color C(COLORREF c, int a = 255) { return Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void DrawAppIcon(HDC dc, int x, int y, int size) {
  if (!g_appIcon) return;
  Graphics g(dc);
  g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
  g.DrawImage(g_appIcon, x, y, size, size);
}

HFONT UiFont(int px, bool bold) {
  int key = px * 2 + (bold ? 1 : 0);
  auto it = g_fonts.find(key);
  if (it != g_fonts.end()) return it->second;
  LOGFONTW lf = {};
  lf.lfHeight = -px;
  lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcscpy(lf.lfFaceName, L"Segoe UI");
  HFONT f = CreateFontIndirectW(&lf);
  g_fonts[key] = f;
  return f;
}

void DrawTextW2(HDC dc, const ws& s, RECT r, COLORREF col, HFONT f, UINT fmt) {
  HGDIOBJ old = SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, col);
  DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
  SelectObject(dc, old);
}

static void RoundPath(GraphicsPath& p, float x, float y, float w, float h, float r) {
  float d = r * 2;
  if (d > w) d = w;
  if (d > h) d = h;
  p.AddArc(x, y, d, d, 180, 90);
  p.AddArc(x + w - d, y, d, d, 270, 90);
  p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  p.AddArc(x, y + h - d, d, d, 90, 90);
  p.CloseFigure();
}
void FillRoundRect(HDC dc, RECT r, int rad, COLORREF col, int alpha) {
  Graphics g(dc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  GraphicsPath p;
  RoundPath(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)rad);
  SolidBrush b(C(col, alpha));
  g.FillPath(&b, &p);
}
void FrameRoundRect(HDC dc, RECT r, int rad, COLORREF col, float w) {
  Graphics g(dc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  GraphicsPath p;
  RoundPath(p, r.left + 0.5f, r.top + 0.5f, (float)(r.right - r.left - 1), (float)(r.bottom - r.top - 1), (float)rad);
  Pen pen(C(col), w);
  g.DrawPath(&pen, &p);
}
void FillRectA(HDC dc, RECT r, COLORREF col, int alpha) {
  Graphics g(dc);
  SolidBrush b(C(col, alpha));
  g.FillRectangle(&b, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
}
void DrawLineA(HDC dc, float x1, float y1, float x2, float y2, COLORREF col, float w) {
  Graphics g(dc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  Pen p(C(col), w);
  p.SetStartCap(LineCapRound);
  p.SetEndCap(LineCapRound);
  g.DrawLine(&p, x1, y1, x2, y2);
}

void DrawIcon(HDC dc, Icon ic, FRect R, COLORREF col, float sw) {
  Graphics g(dc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(PixelOffsetModeHalf);
  float w = R.r - R.l, h = R.b - R.t;
  float s = min(w, h);
  float x0 = R.l + (w - s) / 2, y0 = R.t + (h - s) / 2;
  float k = s / 20.0f;           // icons are designed on a 20x20 grid
  if (sw > 0) sw = sw * max(1.0f, k * 0.95f);
  auto P = [&](float x, float y) { return PointF(x0 + x * k, y0 + y * k); };
  Pen pen(C(col), sw);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  pen.SetLineJoin(LineJoinRound);
  SolidBrush br(C(col));
  auto L = [&](float a, float b, float c, float d) { g.DrawLine(&pen, P(a, b), P(c, d)); };
  switch (ic) {
    case IC_MENU: L(3, 5, 17, 5); L(3, 10, 17, 10); L(3, 15, 17, 15); break;
    case IC_SIDEBAR: {
      GraphicsPath p; RoundPath(p, x0 + 2.5f * k, y0 + 3.5f * k, 15 * k, 13 * k, 2 * k);
      g.DrawPath(&pen, &p); L(8, 3.5f, 8, 16.5f);
      SolidBrush b2(C(col, 90)); g.FillRectangle(&b2, x0 + 3.2f * k, y0 + 4.2f * k, 4.2f * k, 11.6f * k);
      break;
    }
    case IC_OPEN: {
      PointF pts[] = {P(2.5f, 5), P(7.5f, 5), P(9.5f, 7), P(17, 7), P(17, 15.5f), P(2.5f, 15.5f)};
      g.DrawPolygon(&pen, pts, 6);
      break;
    }
    case IC_UP: { PointF p[] = {P(4.5f, 12.5f), P(10, 7), P(15.5f, 12.5f)}; g.DrawLines(&pen, p, 3); break; }
    case IC_DOWN: { PointF p[] = {P(4.5f, 7.5f), P(10, 13), P(15.5f, 7.5f)}; g.DrawLines(&pen, p, 3); break; }
    case IC_CHEV: { PointF p[] = {P(7.5f, 4.5f), P(13, 10), P(7.5f, 15.5f)}; g.DrawLines(&pen, p, 3); break; }
    case IC_CHEVD: { PointF p[] = {P(4.5f, 7.5f), P(10, 13), P(15.5f, 7.5f)}; g.DrawLines(&pen, p, 3); break; }
    case IC_ZOUT: L(4, 10, 16, 10); break;
    case IC_ZIN: L(4, 10, 16, 10); L(10, 4, 10, 16); break;
    case IC_PLUS: L(4, 10, 16, 10); L(10, 4, 10, 16); break;
    case IC_FITW: L(2.5f, 4, 2.5f, 16); L(17.5f, 4, 17.5f, 16); L(5.5f, 10, 14.5f, 10);
      { PointF a[] = {P(8, 7.2f), P(5.3f, 10), P(8, 12.8f)}; g.DrawLines(&pen, a, 3);
        PointF b[] = {P(12, 7.2f), P(14.7f, 10), P(12, 12.8f)}; g.DrawLines(&pen, b, 3); }
      break;
    case IC_FITP: {
      GraphicsPath p; RoundPath(p, x0 + 5 * k, y0 + 2.5f * k, 10 * k, 15 * k, 1.5f * k);
      g.DrawPath(&pen, &p);
      { PointF a[] = {P(8.2f, 7.5f), P(10, 5.7f), P(11.8f, 7.5f)}; g.DrawLines(&pen, a, 3);
        PointF b[] = {P(8.2f, 12.5f), P(10, 14.3f), P(11.8f, 12.5f)}; g.DrawLines(&pen, b, 3); }
      break;
    }
    case IC_ROT: {
      g.DrawArc(&pen, x0 + 3.5f * k, y0 + 3.5f * k, 13 * k, 13 * k, -60, 300);
      PointF a[] = {P(14.6f, 2.6f), P(16.2f, 5.6f), P(12.8f, 6.4f)};
      g.DrawLines(&pen, a, 3);
      break;
    }
    case IC_SEARCH: g.DrawEllipse(&pen, x0 + 3 * k, y0 + 3 * k, 10 * k, 10 * k); L(12, 12, 17, 17); break;
    case IC_NIGHT: {
      GraphicsPath a, b;
      a.AddEllipse(x0 + 3 * k, y0 + 3 * k, 14 * k, 14 * k);
      b.AddEllipse(x0 + 7.5f * k, y0 + 0.5f * k, 13 * k, 13 * k);
      Region rg(&a); Region rb(&b); rg.Exclude(&rb);
      g.FillRegion(&br, &rg);
      break;
    }
    case IC_BOOK: case IC_BOOKON: {
      PointF p[] = {P(5, 2.5f), P(15, 2.5f), P(15, 17.5f), P(10, 13.5f), P(5, 17.5f)};
      if (ic == IC_BOOKON) g.FillPolygon(&br, p, 5); else g.DrawPolygon(&pen, p, 5);
      break;
    }
    case IC_PRINT: {
      GraphicsPath p; RoundPath(p, x0 + 2.5f * k, y0 + 7 * k, 15 * k, 7 * k, 1.5f * k);
      g.DrawPath(&pen, &p);
      PointF a[] = {P(5.5f, 7), P(5.5f, 2.8f), P(14.5f, 2.8f), P(14.5f, 7)}; g.DrawLines(&pen, a, 4);
      PointF b[] = {P(5.5f, 12), P(5.5f, 17.2f), P(14.5f, 17.2f), P(14.5f, 12), P(5.5f, 12)}; g.DrawLines(&pen, b, 5);
      break;
    }
    case IC_FULL: {
      PointF a[] = {P(3, 7.5f), P(3, 3), P(7.5f, 3)}; g.DrawLines(&pen, a, 3);
      PointF b[] = {P(12.5f, 3), P(17, 3), P(17, 7.5f)}; g.DrawLines(&pen, b, 3);
      PointF c[] = {P(17, 12.5f), P(17, 17), P(12.5f, 17)}; g.DrawLines(&pen, c, 3);
      PointF d[] = {P(7.5f, 17), P(3, 17), P(3, 12.5f)}; g.DrawLines(&pen, d, 3);
      break;
    }
    case IC_PRES: {
      GraphicsPath p; RoundPath(p, x0 + 2.5f * k, y0 + 3.5f * k, 15 * k, 11 * k, 1.5f * k);
      g.DrawPath(&pen, &p);
      PointF t[] = {P(8.6f, 6.4f), P(12.6f, 9), P(8.6f, 11.6f)}; g.FillPolygon(&br, t, 3);
      L(10, 14.5f, 10, 17.5f); L(7, 17.5f, 13, 17.5f);
      break;
    }
    case IC_CLOSE: L(5, 5, 15, 15); L(15, 5, 5, 15); break;
    case IC_CASE: break;
  }
}
