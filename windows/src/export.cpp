// UltraPDF Editor: "Save as" with options (court-ready files): flatten, or turn pages into pictures at a chosen resolution / colour / size limit.
#include "editor.h"
#ifdef APP_EDITOR
#include <objbase.h>
#include <algorithm>
#include <cstdio>

namespace {

struct PageOut {
  float w = 0, h = 0;       // points
  int px = 0, py = 0;
  int kind = 0;             // 0 JPEG (RGB), 1 bilevel (RunLength)
  std::string data;
};

bool JpegClsid(CLSID* out) {
  UINT n = 0, sz = 0;
  Gdiplus::GetImageEncodersSize(&n, &sz);
  if (!n || !sz) return false;
  std::vector<BYTE> buf(sz);
  Gdiplus::ImageCodecInfo* ci = (Gdiplus::ImageCodecInfo*)buf.data();
  if (Gdiplus::GetImageEncoders(n, sz, ci) != Gdiplus::Ok) return false;
  for (UINT i = 0; i < n; i++)
    if (wcscmp(ci[i].MimeType, L"image/jpeg") == 0) { *out = ci[i].Clsid; return true; }
  return false;
}

bool EncodeJpeg(uint8_t* bgrx, int w, int h, int quality, std::string* out) {
  static CLSID clsid;
  static bool have = JpegClsid(&clsid);
  if (!have) return false;
  Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppRGB, bgrx);
  IStream* st = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) return false;
  ULONG q = (ULONG)min(max(quality, 10), 100);
  Gdiplus::EncoderParameters ep;
  ep.Count = 1;
  ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
  ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
  ep.Parameter[0].NumberOfValues = 1;
  ep.Parameter[0].Value = &q;
  bool ok = bmp.Save(st, &clsid, &ep) == Gdiplus::Ok;
  if (ok) {
    HGLOBAL hg = nullptr;
    GetHGlobalFromStream(st, &hg);
    SIZE_T n = GlobalSize(hg);
    // the stream is larger than the data when it was grown: the JPEG ends at the last EOI marker
    const uint8_t* p = (const uint8_t*)GlobalLock(hg);
    SIZE_T len = n;
    STATSTG ss;
    if (SUCCEEDED(st->Stat(&ss, STATFLAG_NONAME)) && ss.cbSize.QuadPart > 0 && ss.cbSize.QuadPart <= n) len = (SIZE_T)ss.cbSize.QuadPart;
    out->assign((const char*)p, len);
    GlobalUnlock(hg);
  }
  st->Release();
  return ok && out->size() > 4;
}

void ToGray(uint8_t* p, size_t npx) {
  for (size_t i = 0; i < npx; i++, p += 4) {
    uint8_t g = (uint8_t)((p[2] * 77 + p[1] * 151 + p[0] * 28) >> 8);
    p[0] = p[1] = p[2] = g;
  }
}

// Otsu threshold on the grey levels, then pack 1 bit per pixel (1 = white) and run-length encode every row
void ToBilevel(const uint8_t* p, int w, int h, std::string* out) {
  std::vector<uint32_t> hist(256, 0);
  size_t npx = (size_t)w * h;
  for (size_t i = 0; i < npx; i++) hist[(p[i * 4 + 2] * 77 + p[i * 4 + 1] * 151 + p[i * 4] * 28) >> 8]++;
  double sum = 0;
  for (int i = 0; i < 256; i++) sum += (double)i * hist[i];
  double sumB = 0, wB = 0, best = -1;
  int thr = 128;
  for (int t = 0; t < 256; t++) {
    wB += hist[t];
    if (wB == 0) continue;
    double wF = (double)npx - wB;
    if (wF == 0) break;
    sumB += (double)t * hist[t];
    double mB = sumB / wB, mF = (sum - sumB) / wF;
    double between = wB * wF * (mB - mF) * (mB - mF);
    if (between > best) { best = between; thr = t; }
  }
  thr = min(max(thr, 90), 200);      // never push a clean page to an extreme
  int rowBytes = (w + 7) / 8;
  std::string raw((size_t)rowBytes * h, '\0');
  for (int y = 0; y < h; y++) {
    uint8_t* row = (uint8_t*)&raw[(size_t)y * rowBytes];
    const uint8_t* s = p + (size_t)y * w * 4;
    for (int x = 0; x < w; x++) {
      int g = (s[x * 4 + 2] * 77 + s[x * 4 + 1] * 151 + s[x * 4] * 28) >> 8;
      if (g > thr) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
    int pad = rowBytes * 8 - w;
    if (pad) row[rowBytes - 1] |= (uint8_t)((1 << pad) - 1);      // padding bits white
  }
  // PackBits-style run length (the stream is one long sequence; runs of 128 max)
  out->clear();
  out->reserve(raw.size() / 4 + 64);
  const uint8_t* d = (const uint8_t*)raw.data();
  size_t n = raw.size(), i = 0;
  while (i < n) {
    size_t run = 1;
    while (i + run < n && d[i + run] == d[i] && run < 128) run++;
    if (run >= 2) {
      out->push_back((char)(257 - run));
      out->push_back((char)d[i]);
      i += run;
    } else {
      size_t j = i;
      while (j < n && j - i < 128 && !(j + 1 < n && d[j] == d[j + 1])) j++;
      if (j == i) j = i + 1;
      out->push_back((char)(j - i - 1));
      out->append((const char*)d + i, j - i);
      i = j;
    }
  }
  out->push_back((char)128);      // EOD
}

struct Att { int dpi; int q; };

enum PassResult { PASS_OK, PASS_BIG, PASS_FAIL, PASS_CANCEL };

PassResult RasterPass(const std::string& src, const std::string& pass, int dpi, int color, int quality, size_t limit, bool allowEarly, ExportJob* job,
                      std::vector<PageOut>* pages, size_t* total) {
  pages->clear();
  *total = 0;
  FPDF_DOCUMENT doc;
  int n;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    doc = PA.FPDF_LoadMemDocument(src.data(), (int)src.size(), pass.empty() ? nullptr : pass.c_str());
    if (!doc) return PASS_FAIL;
    n = PA.FPDF_GetPageCount(doc);
  }
  job->total = n;
  PassResult res = PASS_OK;
  for (int i = 0; i < n; i++) {
    if (job->cancel) { res = PASS_CANCEL; break; }
    job->page = i + 1;
    PageOut po;
    std::vector<uint8_t> buf;
    {
      std::lock_guard<std::recursive_mutex> lk(g_pdf);
      FPDF_PAGE pg = PA.FPDF_LoadPage(doc, i);
      if (!pg) { res = PASS_FAIL; break; }
      po.w = PA.FPDF_GetPageWidthF(pg);
      po.h = PA.FPDF_GetPageHeightF(pg);
      po.px = max(1, (int)lround(po.w / 72.0 * dpi));
      po.py = max(1, (int)lround(po.h / 72.0 * dpi));
      if ((double)po.px * po.py > 90e6) { PA.FPDF_ClosePage(pg); res = PASS_FAIL; job->err = L"Страница слишком большая для такого разрешения"; break; }
      buf.assign((size_t)po.px * po.py * 4, 0xFF);
      FPDF_BITMAP bm = PA.FPDFBitmap_CreateEx(po.px, po.py, 3 /*BGRx*/, buf.data(), po.px * 4);
      if (!bm) { PA.FPDF_ClosePage(pg); res = PASS_FAIL; break; }
      PA.FPDFBitmap_FillRect(bm, 0, 0, po.px, po.py, 0xFFFFFFFF);
      PA.FPDFPage_GetAnnotCount(pg);        // makes PDFium build the appearance of annotations without one (markup, shapes)
      PA.FPDF_RenderPageBitmap(bm, pg, 0, 0, po.px, po.py, 0, FPDF_ANNOT);
      PA.FPDFBitmap_Destroy(bm);
      PA.FPDF_ClosePage(pg);
    }
    if (color == 2) {
      po.kind = 1;
      ToBilevel(buf.data(), po.px, po.py, &po.data);
    } else {
      if (color == 1) ToGray(buf.data(), (size_t)po.px * po.py);
      po.kind = 0;
      if (!EncodeJpeg(buf.data(), po.px, po.py, quality, &po.data)) { res = PASS_FAIL; job->err = L"Не удалось сжать страницу"; break; }
    }
    *total += po.data.size();
    pages->push_back(std::move(po));
    if (allowEarly && limit && i >= 2 && i + 1 < n) {
      double proj = (double)*total / (i + 1) * n;
      if (proj > (double)limit * 1.3) { res = PASS_BIG; break; }
    }
  }
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    PA.FPDF_CloseDocument(doc);
  }
  if (res == PASS_OK && limit && *total > limit) res = PASS_BIG;
  return res;
}

std::string AssemblePdf(const std::vector<PageOut>& pages) {
  std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  int n = (int)pages.size();
  std::vector<size_t> off(3 + 3 * n + 2, 0);
  auto begin = [&](int num) { off[num] = pdf.size(); pdf += std::to_string(num) + " 0 obj\n"; };
  int info = 3 + 3 * n;
  begin(1);
  pdf += "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
  begin(2);
  pdf += "<< /Type /Pages /Count " + std::to_string(n) + " /Kids [";
  for (int i = 0; i < n; i++) pdf += std::to_string(3 + 3 * i) + " 0 R ";
  pdf += "] >>\nendobj\n";
  char tmp[256];
  for (int i = 0; i < n; i++) {
    const PageOut& p = pages[i];
    int po = 3 + 3 * i, co = po + 1, io = po + 2;
    begin(po);
    snprintf(tmp, sizeof tmp, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>\nendobj\n", p.w, p.h, io, co);
    pdf += tmp;
    snprintf(tmp, sizeof tmp, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", p.w, p.h);
    std::string content = tmp;
    begin(co);
    pdf += "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream\nendobj\n";
    begin(io);
    if (p.kind == 0)
      snprintf(tmp, sizeof tmp, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %zu >>\nstream\n", p.px, p.py, p.data.size());
    else
      snprintf(tmp, sizeof tmp, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /RunLengthDecode /Length %zu >>\nstream\n", p.px, p.py, p.data.size());
    pdf += tmp;
    pdf += p.data;
    pdf += "\nendstream\nendobj\n";
  }
  begin(info);
  pdf += "<< /Producer (UltraPDF Editor) >>\nendobj\n";
  size_t xref = pdf.size();
  int count = info + 1;
  pdf += "xref\n0 " + std::to_string(count) + "\n0000000000 65535 f \n";
  for (int i = 1; i < count; i++) {
    snprintf(tmp, sizeof tmp, "%010zu 00000 n \n", off[i]);
    pdf += tmp;
  }
  pdf += "trailer\n<< /Size " + std::to_string(count) + " /Root 1 0 R /Info " + std::to_string(info) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

}  // namespace

// Worker: builds the output file in job->out. Never touches the UI.
bool ExportBuild(const std::string& src0, const std::string& pass, const ExportOpts& o, ExportJob* job) {
  job->out.clear();
  if (o.mode == 0) { job->out = src0; return true; }
  std::string srcFit;
  if (o.paper >= 1 && o.paper <= 3) {          // every page onto A4 first (vector), then flatten / rasterise at the chosen resolution
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    FPDF_DOCUMENT fd = PA.FPDF_LoadMemDocument(src0.data(), (int)src0.size(), pass.empty() ? nullptr : pass.c_str());
    if (!fd) { job->err = L"Не удалось открыть документ"; return false; }
    FitOpts fo;
    fo.paper = 0; fo.orient = o.paper == 1 ? 0 : o.paper == 2 ? 1 : 2; fo.marginMm = 0; fo.enlarge = true;
    FitDocumentPages(fd, fo);
    bool ok = SerializeHandle(fd, &srcFit);
    PA.FPDF_CloseDocument(fd);
    if (!ok) { job->err = L"Не удалось подогнать страницы под A4"; return false; }
  }
  const std::string& src = srcFit.empty() ? src0 : srcFit;
  size_t limit = o.maxMB > 0 ? (size_t)o.maxMB * 1024 * 1024 : 0;
  if (o.mode == 1) {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    FPDF_DOCUMENT doc = PA.FPDF_LoadMemDocument(src.data(), (int)src.size(), pass.empty() ? nullptr : pass.c_str());
    if (!doc) { job->err = L"Не удалось открыть документ"; return false; }
    int n = PA.FPDF_GetPageCount(doc);
    job->total = n;
    for (int i = 0; i < n; i++) {
      job->page = i + 1;
      FPDF_PAGE pg = PA.FPDF_LoadPage(doc, i);
      if (!pg) continue;
      {
        // PDFium builds the appearance of markup / shape annotations while it draws the page: draw it once (tiny) so that flattening has something to bake in
        uint32_t px[16] = {};
        FPDF_BITMAP bm = PA.FPDFBitmap_CreateEx(4, 4, 3, px, 16);
        if (bm) {
          PA.FPDF_RenderPageBitmap(bm, pg, 0, 0, 4, 4, 0, FPDF_ANNOT);
          PA.FPDFBitmap_Destroy(bm);
        }
      }
      PA.FPDFPage_Flatten(pg, 1 /*FLAT_PRINT*/);
      PA.FPDF_ClosePage(pg);
    }
    bool ok = SerializeHandle(doc, &job->out);
    PA.FPDF_CloseDocument(doc);
    if (!ok) { job->err = L"Не удалось записать документ"; return false; }
    if (limit && job->out.size() > limit) job->note = L"Файл больше лимита. Для уменьшения выберите «Превратить страницы в картинки».";
    return true;
  }
  // rasterise
  std::vector<Att> at;
  at.push_back({o.dpi, o.quality});
  if (limit) {
    if (o.color != 2) for (int q : {60, 45, 35}) if (q < o.quality) at.push_back({o.dpi, q});
    for (int d : {250, 200, 150}) if (d < o.dpi) {
      at.push_back({d, o.color == 2 ? o.quality : min(o.quality, 60)});
      if (o.color != 2) at.push_back({d, 45});
    }
  }
  std::vector<PageOut> pages, bestPages;
  size_t total = 0, bestTotal = 0;
  int bestIdx = -1;
  for (size_t k = 0; k < at.size(); k++) {
    bool last = k + 1 == at.size();
    PassResult r = RasterPass(src, pass, at[k].dpi, o.color, at[k].q, limit, !last, job, &pages, &total);
    if (r == PASS_CANCEL) { job->err = L"Отменено"; return false; }
    if (r == PASS_FAIL) { if (job->err.empty()) job->err = L"Не удалось подготовить страницы"; return false; }
    if (r == PASS_OK || (r == PASS_BIG && last)) {
      bestPages = std::move(pages); bestTotal = total; bestIdx = (int)k;
      if (r == PASS_OK) break;
    }
  }
  if (bestIdx < 0) { job->err = L"Не удалось подготовить файл"; return false; }
  job->usedDpi = at[bestIdx].dpi;
  job->usedQ = at[bestIdx].q;
  job->out = AssemblePdf(bestPages);
  if (limit && job->out.size() > limit) job->note = L"Не удалось уложиться в лимит размера. Уменьшите разрешение или разбейте документ на части.";
  else if (bestIdx > 0) job->note = L"Чтобы уложиться в лимит, разрешение/качество уменьшены автоматически.";
  (void)bestTotal;
  return true;
}
#endif
