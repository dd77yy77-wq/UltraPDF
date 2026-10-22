// UltraPDF for Windows: shared declarations.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <type_traits>
#include <shlobj.h>
#include <winhttp.h>
#include <functional>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
template <class A, class B> inline typename std::common_type<A, B>::type max(A a, B b) { return a > b ? a : b; }
template <class A, class B> inline typename std::common_type<A, B>::type min(A a, B b) { return a < b ? a : b; }
#include <gdiplus.h>

#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_doc.h"

#define APP_VERSION_CODE 10
#define APP_VERSION_NAME L"1.0"
#define APP_VERSION_NAME_A "1.0"
#define GITHUB_URL L"https://github.com/dd77yy77-wq/UltraPDF"
#define UPDATE_JSON_HOST L"raw.githubusercontent.com"
#define UPDATE_JSON_PATH L"/dd77yy77-wq/UltraPDF/main/version-win.json"
#define UPDATE_EXE_PATH L"/dd77yy77-wq/UltraPDF/main/UltraPDF.exe"

typedef std::wstring ws;

// ------------------------------------------------------------------ pdfium dynamic API
struct PdfApi {
#define PDF_FUNCS(X) \
  X(FPDF_InitLibrary) X(FPDF_DestroyLibrary) X(FPDF_GetLastError) \
  X(FPDF_LoadCustomDocument) X(FPDF_CloseDocument) X(FPDF_GetPageCount) \
  X(FPDF_LoadPage) X(FPDF_ClosePage) X(FPDF_GetPageSizeByIndexF) \
  X(FPDF_GetPageWidthF) X(FPDF_GetPageHeightF) \
  X(FPDF_RenderPageBitmap) X(FPDF_RenderPage) X(FPDF_PageToDevice) \
  X(FPDFBitmap_CreateEx) X(FPDFBitmap_FillRect) X(FPDFBitmap_Destroy) \
  X(FPDFText_LoadPage) X(FPDFText_ClosePage) X(FPDFText_CountChars) \
  X(FPDFText_GetUnicode) X(FPDFText_GetCharBox) X(FPDFText_CountRects) X(FPDFText_GetRect) \
  X(FPDFText_FindStart) X(FPDFText_FindNext) X(FPDFText_FindPrev) X(FPDFText_GetSchResultIndex) \
  X(FPDFText_GetSchCount) X(FPDFText_FindClose) \
  X(FPDFLink_Enumerate) X(FPDFLink_GetAnnotRect) X(FPDFLink_GetDest) X(FPDFLink_GetAction) \
  X(FPDFAction_GetType) X(FPDFAction_GetDest) X(FPDFAction_GetURIPath) \
  X(FPDFDest_GetDestPageIndex) X(FPDFDest_GetLocationInPage) \
  X(FPDFBookmark_GetFirstChild) X(FPDFBookmark_GetNextSibling) X(FPDFBookmark_GetTitle) \
  X(FPDFBookmark_GetDest) X(FPDFBookmark_GetAction) \
  X(FPDFLink_LoadWebLinks) X(FPDFLink_CountWebLinks) X(FPDFLink_GetURL) X(FPDFLink_CountRects) \
  X(FPDFLink_GetRect) X(FPDFLink_CloseWebLinks) X(FPDF_GetMetaText)
#define X(n) decltype(&::n) n;
  PDF_FUNCS(X)
#undef X
};
extern PdfApi PA;
bool PdfLoad(ws* err);

// ------------------------------------------------------------------ small helpers
struct FRect { float l = 0, t = 0, r = 0, b = 0; };
ws Utf8ToW(const std::string& s);
std::string WToUtf8(const ws& s);
ws BaseName(const ws& p);
ws DirName(const ws& p);
ws StripExt(const ws& p);
ws FmtT(const wchar_t* f, ...);
bool FileExists(const ws& p);
uint64_t NowMs();
int Dpi();                       // current DPI of the main window
int S(int v);                    // scale a 96-dpi number to the current dpi
ws LowerW(const ws& s);
bool ReadFileAll(const ws& path, std::string* out);
bool WriteFileAtomic(const ws& path, const std::string& data);
ws AppDataDir();
ws ExePath();

// ------------------------------------------------------------------ theme
struct Theme {
  bool dark = false;
  COLORREF chrome, chrome2, text, dim, accent, hover, press, line, viewbg, tabActive, tabIdle, field, fieldLine;
};
extern Theme T;
void ApplyTheme(int mode);       // 0 system, 1 light, 2 dark
bool SystemIsDark();

// ------------------------------------------------------------------ settings & per-document state
struct DocState {
  ws path;
  uint64_t time = 0;
  int page = 0;
  float off = 0;                 // view top offset from the top of `page`, in points
  double zoom = 1.0;
  int fit = 1;                   // 0 none, 1 width, 2 page
  int rot = 0;
  std::vector<int> bookmarks;
};
struct Settings {
  bool pro = true;
  int themeMode = 0;
  int readMode = 0;              // 0 day, 1 sepia, 2 night
  bool sidebar = true;
  int sideW = 250;
  int sideTab = 0;               // 0 pages, 1 outline, 2 bookmarks
  int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT, winW = 1180, winH = 820;
  bool maximized = false;
  bool matchCase = false;
  uint64_t lastUpdateCheck = 0;
  std::vector<ws> session;
  int sessionActive = 0;
  std::vector<DocState> docs;    // most recent first
};
extern Settings G;
void LoadSettings();
void SaveSettings();
DocState* FindState(const ws& path, bool create);
void TouchState(const DocState& st);

// ------------------------------------------------------------------ GDI+ drawing
void GfxInit();
void GfxShutdown();
enum Icon {
  IC_MENU, IC_SIDEBAR, IC_OPEN, IC_UP, IC_DOWN, IC_ZOUT, IC_ZIN, IC_FITW, IC_FITP, IC_ROT, IC_SEARCH,
  IC_NIGHT, IC_BOOK, IC_BOOKON, IC_PRINT, IC_FULL, IC_CLOSE, IC_PLUS, IC_CASE, IC_CHEV, IC_CHEVD, IC_PRES
};
void DrawIcon(HDC dc, Icon ic, FRect r, COLORREF col, float stroke = 1.6f);
void FillRoundRect(HDC dc, RECT r, int rad, COLORREF col, int alpha = 255);
void FillRectA(HDC dc, RECT r, COLORREF col, int alpha);    // alpha blended fill
void FrameRoundRect(HDC dc, RECT r, int rad, COLORREF col, float w = 1.0f);
void DrawLineA(HDC dc, float x1, float y1, float x2, float y2, COLORREF col, float w);
HFONT UiFont(int px, bool bold = false);
void DrawTextW2(HDC dc, const ws& s, RECT r, COLORREF col, HFONT f, UINT fmt);
void DrawAppIcon(HDC dc, int x, int y, int size);

// ------------------------------------------------------------------ document, cache, worker
struct Link {
  FRect r;                       // in page points, top-left origin, page orientation (rot 0)
  int page = -1;                 // internal target page
  bool hasY = false;
  double yPdf = 0;
  ws url;
};
struct PageData {
  std::vector<wchar_t> ch;
  std::vector<FRect> box;        // per char; zero-size for generated chars
  std::vector<Link> links;
};
struct OutlineItem {
  ws title;
  int page = -1;
  bool hasY = false;
  double yPdf = 0;
  int level = 0;
  bool hasKids = false;
  bool open = false;
};
struct SearchHit {
  int page = 0, start = 0, count = 0;
  std::vector<FRect> rects;
};

struct Doc {
  int id = 0;
  ws path;
  HANDLE hf = INVALID_HANDLE_VALUE;
  FPDF_DOCUMENT h = nullptr;
  int npages = 0;
  std::vector<float> pw, ph;
  std::vector<OutlineItem> outline;
  std::map<int, std::shared_ptr<PageData>> pdata;
  std::atomic<bool> closed{false};
  FPDF_FILEACCESS fa;
  ~Doc();
};
extern std::recursive_mutex g_pdf;     // guards every PDFium call

struct OpenResult {
  int tabId = 0;
  std::shared_ptr<Doc> doc;
  int error = 0;                 // 0 ok, 1 cannot read, 2 password needed, 3 bad password, 4 broken
  ws path;
};

struct TileKey {
  int doc = 0, page = 0, tx = 0, ty = 0, zq = 0, rot = 0, mode = 0, kind = 0;   // kind 0 tile, 1 preview, 2 thumb
  bool operator==(const TileKey& o) const {
    return doc == o.doc && page == o.page && tx == o.tx && ty == o.ty && zq == o.zq && rot == o.rot && mode == o.mode &&
           kind == o.kind;
  }
};
struct TileKeyHash {
  size_t operator()(const TileKey& k) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int v) { h ^= (uint64_t)(uint32_t)v; h *= 1099511628211ull; };
    mix(k.doc); mix(k.page); mix(k.tx); mix(k.ty); mix(k.zq); mix(k.rot); mix(k.mode); mix(k.kind);
    return (size_t)h;
  }
};
struct Tile { HBITMAP bmp = nullptr; int w = 0, h = 0; };

// consumers of rendering results
enum Consumer { CONS_VIEW = 0, CONS_SIDE = 1, CONS_COUNT };

void WorkerAckRefresh();
void WorkerStart(HWND mainWnd);
void WorkerStop();
void WorkerOpen(int tabId, const ws& path, const std::string& password);
void CacheLock();
void CacheUnlock();
bool CacheGetLocked(const TileKey& k, Tile* t);    // call between CacheLock/Unlock; HBITMAP valid until unlock
void CachePurgeDoc(int docId);
void ReqBegin(Consumer c);                          // start collecting the requests of one paint
void ReqTile(Consumer c, const std::shared_ptr<Doc>& d, const TileKey& k, int pageW, int pageH, int ox, int oy, int tileW, int tileH, int prio);
void ReqPageData(Consumer c, const std::shared_ptr<Doc>& d, int page, int prio);
void ReqEnd(Consumer c);                            // publish them atomically; anything not requested again is dropped
std::shared_ptr<PageData> GetPageDataNow(const std::shared_ptr<Doc>& d, int page, bool load);
void CloseDoc(std::shared_ptr<Doc>& d);
bool ResolveDestY(const std::shared_ptr<Doc>& d, int page, double yPdf, float* yTop);   // page points, top-left
void SearchStart(const std::shared_ptr<Doc>& d, const ws& q, bool matchCase, int tabId);
void SearchCancel();
// results of search are fetched under lock
void SearchSnapshot(int tabId, std::vector<SearchHit>* out, bool* done, int* scanned);

#define WM_APP_OPENED (WM_APP + 1)
#define WM_APP_REFRESH (WM_APP + 2)
#define WM_APP_SEARCH (WM_APP + 3)
#define WM_APP_UPDATE (WM_APP + 4)
#define WM_APP_PRINTED (WM_APP + 5)
#define WM_APP_TESTCMD (WM_APP + 6)

// ------------------------------------------------------------------ UI
struct Sel {
  bool active = false;
  int p0 = 0, c0 = 0, p1 = 0, c1 = 0;   // anchor and focus carets
};

struct Tab {
  int id = 0;
  ws path, title;
  std::shared_ptr<Doc> doc;
  bool loading = true;
  ws error;
  // view state
  double zoom = 1.0;
  int fit = 1;
  int rot = 0;
  double sx = 0, sy = 0, tx = 0, ty = 0;
  bool restored = false;
  int fitRef = 0;                  // page used as the reference for fit width / fit page
  bool needPw = false;             // password-protected document restored in a background tab
  bool jumpPending = false;        // jump to the first search hit when results arrive
  DocState saved;
  // layout cache
  double scale = 1;                // px per point
  int viewW = 0, viewH = 0;
  std::vector<int> ytop, pwpx, phpx;
  int docW = 0, docH = 0;
  bool layoutOk = false;
  Sel sel;
  // search
  ws query;
  std::vector<SearchHit> hits;
  int curHit = -1;
  bool searchDone = true;
  int searchScanned = 0;
  // outline UI
  std::vector<int> outlineVisible;
  // navigation back stack (page, offset)
  std::vector<std::pair<int, float>> back;
  bool dirty = false;
};

struct App {
  HINSTANCE inst = nullptr;
  HWND main = nullptr, view = nullptr, side = nullptr, pageEdit = nullptr, findEdit = nullptr;
  std::vector<std::unique_ptr<Tab>> tabs;
  int cur = -1;
  int nextTabId = 1;
  bool fullscreen = false, presentation = false;
  bool findOpen = false;
  WINDOWPLACEMENT wpPrev{};
  LONG stylePrev = 0;
  int dpi = 96;
};
extern App A_;
Tab* CurTab();
Tab* TabById(int id);

enum Cmd {
  ID_OPEN = 100, ID_CLOSE_TAB, ID_NEXT_TAB, ID_PREV_TAB, ID_PRO, ID_SIDEBAR, ID_FIND, ID_FIND_NEXT, ID_FIND_PREV,
  ID_GOTO, ID_ZOOM_IN, ID_ZOOM_OUT, ID_ZOOM_100, ID_FIT_W, ID_FIT_P, ID_ROT_CW, ID_ROT_CCW, ID_NIGHT_TOGGLE,
  ID_BOOKMARK, ID_PRINT, ID_FULLSCREEN, ID_PRESENT, ID_SAVEAS, ID_SHOWFOLDER, ID_ABOUT, ID_UPDATE, ID_COPY,
  ID_SELALL, ID_BACK, ID_ASSOC, ID_EXIT, ID_MODE_DAY, ID_MODE_SEPIA, ID_MODE_NIGHT, ID_THEME_SYS,
  ID_THEME_LIGHT, ID_THEME_DARK, ID_SIDE_PAGES, ID_SIDE_OUTLINE, ID_SIDE_BOOKS, ID_ZOOM_PRESET = 300,
  ID_HISTORY = 400, ID_CLEAR_HISTORY = 420, ID_NEWTAB_DLG, ID_PAGE_FIRST, ID_PAGE_LAST, ID_PAGE_NEXT, ID_PAGE_PREV
};
void DoCmd(int id);

// main window / chrome
bool MainCreate(HINSTANCE inst, int show, const ws& cmdline);
void MainLayout();
void MainRelayoutAll();
void MainRedrawChrome();
void MainUpdatePageBox();
void MainShowFind(bool show);
void UpdateTitle();
void OpenFiles(const std::vector<ws>& paths, bool activate);
void OpenPath(const ws& path);
void CloseTab(int idx);
void SwitchTab(int idx);
void SaveAllState();
void ApplyOpened(OpenResult* r);
void MainSetFullscreen(bool on, bool presentation);

// view
bool ViewRegister(HINSTANCE inst);
HWND ViewCreate(HWND parent);
void ViewInvalidate();
void ViewSync();                         // rebuild layout/scrollbars for the current tab
void ViewGoPage(int page, float yPt, bool animate, bool pushBack);
int ViewCurPage();
void ViewZoom(double z, int anchorX, int anchorY);
void ViewZoomStep(int dir, int anchorX = -1, int anchorY = -1);
void ViewSetFit(int fit);
void ViewRotate(int dir);
void ViewScrollBy(int dx, int dy, bool animate);
void ViewPageStep(int dir);
void ViewShowHit(int idx);
void ViewCopy();
void ViewSelectAll();
bool ViewHasSel();
void ViewClearSel();
void ViewSaveState(Tab* t);
void ViewPrepareTab(Tab* t);            // apply restored state once doc is ready
void ViewEnsureLayout(Tab* t);
void ViewFollowLink(const Link& l);
void ViewFindStep(int dir);
void ViewSearchChanged();
int PageAtDocY(Tab* t, double y);
void ViewPresentStep(int dir);

// sidebar
bool SideRegister(HINSTANCE inst);
HWND SideCreate(HWND parent);
void SideSync();
void SideInvalidate();
void SideScrollToCurrent();
void SideRebuildOutline(Tab* t);

// misc
void PrintDoc(HWND owner);
void CheckUpdates(bool manual);
void OnUpdateMsg(WPARAM wp, LPARAM lp);
extern HANDLE g_mutex;
void ShowAbout(HWND owner);
bool InputPassword(HWND owner, const ws& file, bool retry, ws* out);
void RegisterAssoc();
void TestRun(const ws& script);
void ShowToast(const ws& msg);
void DrawToast(HDC dc, RECT rc);
bool IsBookmarked(Tab* t, int page);
void ToggleBookmark(Tab* t, int page);
