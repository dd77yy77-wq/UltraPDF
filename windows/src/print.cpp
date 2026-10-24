// Printing through the Windows print dialog; pages go to the printer via PDFium
#include "app.h"

struct PrintJob {
  std::shared_ptr<Doc> doc;
  HDC dc = nullptr;
  int from = 0, to = 0, rot = 0;
  ws name;
};

static void PrintThread(PrintJob job) {
  DOCINFOW di = {sizeof(di)};
  di.lpszDocName = job.name.c_str();
  bool ok = StartDocW(job.dc, &di) > 0;
  int pw = GetDeviceCaps(job.dc, HORZRES), ph = GetDeviceCaps(job.dc, VERTRES);
  for (int p = job.from; ok && p <= job.to; p++) {
    if (StartPage(job.dc) <= 0) { ok = false; break; }
    {
      std::lock_guard<std::recursive_mutex> lk(g_pdf);
      if (job.doc->closed) { ok = false; }
      else {
        FPDF_PAGE pg = PA.FPDF_LoadPage(job.doc->h, p);
        if (pg) {
          double w = PA.FPDF_GetPageWidthF(pg), h = PA.FPDF_GetPageHeightF(pg);
          int rot = job.rot & 3;
          double rw = (rot & 1) ? h : w, rh = (rot & 1) ? w : h;
          // turn landscape pages on portrait paper (and the other way round)
          if ((rw > rh) != (pw > ph)) { rot = (rot + 1) & 3; std::swap(rw, rh); }
          double sc = min(pw / rw, ph / rh);
          int dw = (int)(rw * sc), dh = (int)(rh * sc);
          PA.FPDF_RenderPage(job.dc, pg, (pw - dw) / 2, (ph - dh) / 2, dw, dh, rot, FPDF_ANNOT | FPDF_PRINTING);
          PA.FPDF_ClosePage(pg);
        }
      }
    }
    EndPage(job.dc);
  }
  if (ok) EndDoc(job.dc); else AbortDoc(job.dc);
  DeleteDC(job.dc);
  PostMessageW(A_.main, WM_APP_PRINTED, ok ? 1 : 0, 0);
}

void PrintDoc(HWND owner) {
  Tab* t = CurTab();
  if (!t || !t->doc) return;
  PRINTDLGW pd = {sizeof(pd)};
  pd.hwndOwner = owner;
  pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE | PD_ALLPAGES;
  pd.nMinPage = 1;
  pd.nMaxPage = (WORD)min(t->doc->npages, 65535);
  pd.nFromPage = 1;
  pd.nToPage = pd.nMaxPage;
  pd.nCopies = 1;
  if (!PrintDlgW(&pd)) {
    DWORD e = CommDlgExtendedError();
    if (e) MessageBoxW(owner, L"Не удалось открыть окно печати. Возможно, в системе нет принтеров.", APP_TITLE, MB_OK | MB_ICONWARNING);
    return;
  }
  PrintJob j;
  j.doc = t->doc;
  j.dc = pd.hDC;
  j.rot = t->rot;
  j.name = t->title;
  j.from = 0;
  j.to = t->doc->npages - 1;
  if (pd.Flags & PD_PAGENUMS) {
    j.from = max(0, (int)pd.nFromPage - 1);
    j.to = min(t->doc->npages - 1, (int)pd.nToPage - 1);
  }
  if (pd.hDevMode) GlobalFree(pd.hDevMode);
  if (pd.hDevNames) GlobalFree(pd.hDevNames);
  ShowToast(L"Отправляю на печать…");
  std::thread(PrintThread, j).detach();
}
