// Scripted self-test hook: runs only when ULTRAPDF_TEST points to a script. Used for screenshots/regression in CI (Wine).
#include "app.h"
#include <fstream>
#include <sstream>

static void OnUI(std::function<void()> f) { SendMessageW(A_.main, WM_APP_TESTCMD, 0, (LPARAM)&f); }

static void SaveShot(const std::string& file, bool full = false) {
  RECT rc;
  GetClientRect(A_.main, &rc);
  POINT pt = {0, 0};
  ClientToScreen(A_.main, &pt);
  if (full) { rc.right = GetSystemMetrics(SM_CXSCREEN); rc.bottom = GetSystemMetrics(SM_CYSCREEN); pt = {0, 0}; }
  HDC sdc = GetDC(nullptr);
  HDC mdc = CreateCompatibleDC(sdc);
  HBITMAP bm = CreateCompatibleBitmap(sdc, rc.right, rc.bottom);
  SelectObject(mdc, bm);
  BitBlt(mdc, 0, 0, rc.right, rc.bottom, sdc, pt.x, pt.y, SRCCOPY);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = rc.right;
  bi.bmiHeader.biHeight = -rc.bottom;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  int stride = (rc.right * 3 + 3) & ~3;
  std::vector<BYTE> px((size_t)stride * rc.bottom);
  GetDIBits(mdc, bm, 0, rc.bottom, px.data(), &bi, DIB_RGB_COLORS);
  BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof(fh) + sizeof(BITMAPINFOHEADER) + px.size()), 0, 0, sizeof(fh) + sizeof(BITMAPINFOHEADER)};
  std::ofstream o(file, std::ios::binary);
  o.write((char*)&fh, sizeof(fh));
  o.write((char*)&bi.bmiHeader, sizeof(BITMAPINFOHEADER));
  o.write((char*)px.data(), px.size());
  DeleteObject(bm);
  DeleteDC(mdc);
  ReleaseDC(nullptr, sdc);
}

static int NameToCmd(const std::string& n) {
  static const std::map<std::string, int> m = {
      {"find", ID_FIND}, {"zoomin", ID_ZOOM_IN}, {"zoomout", ID_ZOOM_OUT}, {"zoom100", ID_ZOOM_100}, {"fitw", ID_FIT_W}, {"fitp", ID_FIT_P},
      {"rotcw", ID_ROT_CW}, {"rotccw", ID_ROT_CCW}, {"night", ID_NIGHT_TOGGLE}, {"sepia", ID_MODE_SEPIA}, {"day", ID_MODE_DAY},
      {"sidebar", ID_SIDEBAR}, {"bookmark", ID_BOOKMARK}, {"pro", ID_PRO}, {"fullscreen", ID_FULLSCREEN}, {"present", ID_PRESENT},
      {"closetab", ID_CLOSE_TAB}, {"nexttab", ID_NEXT_TAB}, {"copy", ID_COPY}, {"selall", ID_SELALL}, {"back", ID_BACK},
      {"next", ID_PAGE_NEXT}, {"prev", ID_PAGE_PREV}, {"themedark", ID_THEME_DARK}, {"themelight", ID_THEME_LIGHT}, {"themesys", ID_THEME_SYS},
      {"side_pages", ID_SIDE_PAGES}, {"side_outline", ID_SIDE_OUTLINE}, {"side_books", ID_SIDE_BOOKS}, {"findnext", ID_FIND_NEXT},
      {"findprev", ID_FIND_PREV}, {"first", ID_PAGE_FIRST}, {"last", ID_PAGE_LAST}};
  auto it = m.find(n);
  return it == m.end() ? 0 : it->second;
}

static void ScriptThread(ws script) {
  std::ifstream in(WToUtf8(script));
  std::string line;
  Sleep(800);
  OnUI([]() { SetForegroundWindow(A_.main); SetActiveWindow(A_.main); SetFocus(A_.view); });
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string c;
    ss >> c;
    std::string rest;
    std::getline(ss, rest);
    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    if (c == "wait") Sleep(atoi(rest.c_str()));
    else if (c == "open") { ws p = Utf8ToW(rest); OnUI([p]() { OpenPath(p); }); }
    else if (c == "cmd") { int id = NameToCmd(rest); OnUI([id]() { if (id) DoCmd(id); }); }
    else if (c == "shot") SaveShot(rest);
    else if (c == "shotscreen") SaveShot(rest, true);
    else if (c == "postcmd") { int id = NameToCmd(rest); if (rest == "menu") id = ID_OPEN + 1000; if (rest == "about") id = ID_ABOUT; PostMessageW(A_.main, WM_COMMAND, 0, 0); auto* f = new std::function<void()>([id]() { DoCmd(id); }); PostMessageW(A_.main, WM_APP_TESTCMD, 1, (LPARAM)f); }
    else if (c == "endmenu") { OnUI([]() { EndMenu(); }); }
    else if (c == "closedlg") { HWND d = FindWindowW(L"UltraPDF.Dlg", nullptr); if (d) SendMessageW(d, WM_COMMAND, 2, 0); }
    else if (c == "size") {
      int w, h;
      sscanf(rest.c_str(), "%d %d", &w, &h);
      OnUI([w, h]() { SetWindowPos(A_.main, nullptr, 20, 20, w, h, SWP_NOZORDER); });
    } else if (c == "text") {
      ws t = Utf8ToW(rest);
      OnUI([t]() { SetWindowTextW(A_.findEdit, t.c_str()); });
    } else if (c == "wheel") {
      int d = 0, ctrl = 0, x = -1, y = -1;
      sscanf(rest.c_str(), "%d %d %d %d", &d, &ctrl, &x, &y);
      OnUI([=]() {
        RECT rc;
        GetClientRect(A_.view, &rc);
        POINT pt = {x < 0 ? rc.right / 2 : x, y < 0 ? rc.bottom / 2 : y};
        ClientToScreen(A_.view, &pt);
        SendMessageW(A_.view, WM_MOUSEWHEEL, MAKEWPARAM(ctrl ? MK_CONTROL : 0, (short)d), MAKELPARAM(pt.x, pt.y));
      });
    } else if (c == "drag") {
      int x1, y1, x2, y2;
      sscanf(rest.c_str(), "%d %d %d %d", &x1, &y1, &x2, &y2);
      OnUI([=]() {
        SendMessageW(A_.view, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
        for (int i = 1; i <= 8; i++)
          SendMessageW(A_.view, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8));
        SendMessageW(A_.view, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
      });
    } else if (c == "click" || c == "dblclick") {
      int x, y;
      sscanf(rest.c_str(), "%d %d", &x, &y);
      bool dbl = c == "dblclick";
      OnUI([=]() {
        SendMessageW(A_.view, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(A_.view, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        if (dbl) {
          SendMessageW(A_.view, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
          SendMessageW(A_.view, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        }
      });
    } else if (c == "clickside") {
      int x, y;
      sscanf(rest.c_str(), "%d %d", &x, &y);
      OnUI([=]() {
        SendMessageW(A_.side, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(A_.side, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
      });
    } else if (c == "clickmain") {
      int x, y;
      sscanf(rest.c_str(), "%d %d", &x, &y);
      OnUI([=]() {
        SendMessageW(A_.main, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(A_.main, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
      });
    } else if (c == "key") {
      int vk = atoi(rest.c_str());
      OnUI([=]() { PostMessageW(A_.view, WM_KEYDOWN, vk, 0); });
    } else if (c == "goto") {
      int p = atoi(rest.c_str());
      OnUI([=]() { ViewGoPage(p - 1, 0, false, false); });
    } else if (c == "zoom") {
      double z = atof(rest.c_str());
      OnUI([=]() { ViewZoom(z, -1, -1); });
    } else if (c == "startfind") {
      OnUI([]() { SendMessageW(A_.main, WM_TIMER, 11, 0); });
    } else if (c == "info") {
      std::string f = rest;
      OnUI([f]() {
        Tab* t = CurTab();
        std::ofstream o(f);
        if (!t) { o << "no tab\n"; return; }
        if (t->doc) {
          auto pd = GetPageDataNow(t->doc, 0, false);
          extern double g_paintAvg; extern int g_ticks; extern int g_timerRet; extern int g_paintTotal;
        o << "paints=" << g_paintTotal << "\n";
        o << "timerRet=" << g_timerRet << " err=" << GetLastError() << "\n";
        o << "ticks=" << g_ticks << "\n";
        o << "paintAvgMs=" << g_paintAvg << "\n";
        o << "p0chars=" << (pd ? pd->ch.size() : -1) << " p0links=" << (pd ? pd->links.size() : -1) << "\n";
          if (pd) for (auto& l : pd->links) o << "link " << l.r.l << "," << l.r.t << "," << l.r.r << "," << l.r.b << " page=" << l.page << " url=" << WToUtf8(l.url) << "\n";
        }
        o << "title=" << WToUtf8(t->title) << "\nloading=" << t->loading << "\nerror=" << WToUtf8(t->error) << "\n";
        if (t->doc) {
          o << "pages=" << t->doc->npages << "\ncur=" << ViewCurPage() + 1 << "\nzoom=" << t->zoom << "\nfit=" << t->fit << "\nrot=" << t->rot
            << "\nviewW=" << t->viewW << "\nviewH=" << t->viewH << "\ndocW=" << t->docW << "\npw0=" << (t->pwpx.empty() ? 0 : t->pwpx[0]) << "\nsx=" << t->sx << "\nscale=" << t->scale << "\nty=" << t->ty << "\nsy=" << t->sy << "\ndocH=" << t->docH << "\nscanned=" << t->searchScanned << "\ndone=" << t->searchDone << "\nquery=" << WToUtf8(t->query) << "\nhits=" << t->hits.size() << "\ncurHit=" << t->curHit << "\nsel=" << ViewHasSel()
            << "\noutline=" << t->doc->outline.size() << "\nbookmarks=" << t->saved.bookmarks.size() << "\nmode=" << G.readMode << "\ntabs=" << A_.tabs.size() << "\n";
        }
      });
    } else if (c == "dumpclip") {
      std::string f = rest;
      OnUI([f]() {
        std::ofstream o(f, std::ios::binary);
        if (OpenClipboard(A_.main)) {
          HANDLE h = GetClipboardData(CF_UNICODETEXT);
          if (h) { const wchar_t* p = (const wchar_t*)GlobalLock(h); o << WToUtf8(p); GlobalUnlock(h); }
          CloseClipboard();
        }
      });
    } else if (c == "dlgpw" || c == "dlgcancel") {
      std::string pw = rest;
      bool cancel = c == "dlgcancel";
      for (int i = 0; i < 40; i++) {
        HWND d = FindWindowW(L"UltraPDF.Dlg", nullptr);
        if (d) {
          HWND e = FindWindowExW(d, nullptr, L"EDIT", nullptr);
          ws w = Utf8ToW(pw);
          if (e && !cancel) SendMessageW(e, WM_SETTEXT, 0, (LPARAM)w.c_str());
          SendMessageW(d, WM_COMMAND, cancel ? 2 : 1, 0);
          break;
        }
        Sleep(100);
      }
    } else if (c == "applyupdate") {
      ws p = Utf8ToW(rest);
      SetEnvironmentVariableW(L"ULTRAPDF_TEST", nullptr);
      OnUI([p]() { ApplyDownloaded(p); });
    } else if (c == "quit") {
      PostMessageW(A_.main, WM_CLOSE, 0, 0);
      return;
    }
  }
}

void TestRun(const ws& script) { std::thread(ScriptThread, script).detach(); }
