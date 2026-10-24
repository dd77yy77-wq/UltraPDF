// PDFium loading, document opening, tile cache, worker thread, text/links/outline, search
#include "app.h"

std::recursive_mutex g_pdf;
static HWND g_notify = nullptr;
static std::atomic<int> g_nextDocId{1};

// ------------------------------------------------------------------ loading the library
bool PdfLoad(ws* err) {
  ws path;
  wchar_t env[1024];
  if (GetEnvironmentVariableW(L"ULTRAPDF_PDFIUM", env, 1024) > 0) {
    path = env;
  } else {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(301), RT_RCDATA);
    if (r) {
      HGLOBAL g = LoadResource(nullptr, r);
      const void* p = LockResource(g);
      DWORD sz = SizeofResource(nullptr, r);
      wchar_t base[MAX_PATH];
      std::vector<ws> dirs;
      if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) dirs.push_back(ws(base) + L"\\UltraPDF");
      if (GetTempPathW(MAX_PATH, base)) dirs.push_back(ws(base) + L"UltraPDF");
      for (auto& d : dirs) {
        CreateDirectoryW(d.c_str(), nullptr);
        ws f = d + L"\\pdfium-" + L"157.0.8100.0" + L".dll";
        WIN32_FILE_ATTRIBUTE_DATA fa;
        bool have = GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == sz && fa.nFileSizeHigh == 0;
        if (!have) {
          ws tmp = f + L"." + std::to_wstring(GetCurrentProcessId());
          HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
          if (h == INVALID_HANDLE_VALUE) continue;
          DWORD w = 0;
          bool ok = WriteFile(h, p, sz, &w, nullptr) && w == sz;
          CloseHandle(h);
          if (!ok || !MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            if (!FileExists(f)) continue;
          }
        }
        path = f;
        break;
      }
    }
    if (path.empty()) {
      ws local = DirName(ExePath()) + L"\\pdfium.dll";
      if (FileExists(local)) path = local;
    }
  }
  if (path.empty()) { *err = L"Не удалось распаковать библиотеку PDFium."; return false; }
  HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!m) { *err = L"Не удалось загрузить PDFium: " + path; return false; }
#define WIDE2(x) L##x
#define WIDE1(x) WIDE2(x)
#define X(n) PA.n = (decltype(PA.n))GetProcAddress(m, #n); if (!PA.n) { *err = ws(L"В PDFium нет функции ") + WIDE1(#n); return false; }
  PDF_FUNCS(X)
#undef X
  PA.FPDF_InitLibrary();
  return true;
}

// ------------------------------------------------------------------ document
FPDF_PAGE Doc::CachedPage(int idx) {
  for (size_t i = 0; i < pcache.size(); i++)
    if (pcache[i].first == idx) {
      auto e = pcache[i];
      pcache.erase(pcache.begin() + i);
      pcache.push_back(e);
      return e.second;
    }
  FPDF_PAGE pg = PA.FPDF_LoadPage(h, idx);
  if (!pg) return nullptr;
  if (pcache.size() >= 8) { PA.FPDF_ClosePage(pcache.front().second); pcache.erase(pcache.begin()); }
  pcache.push_back({idx, pg});
  return pg;
}

Doc::~Doc() {
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  for (auto& e : pcache) PA.FPDF_ClosePage(e.second);
  pcache.clear();
#ifdef APP_EDITOR
  for (auto& f : fonts) if (f.second) PA.FPDFFont_Close((FPDF_FONT)f.second);
  fonts.clear();
#endif
  if (h) PA.FPDF_CloseDocument(h);
  h = nullptr;
  if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
}

static int GetBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
  HANDLE h = (HANDLE)param;
  LARGE_INTEGER li;
  li.QuadPart = pos;
  if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return 0;
  DWORD got = 0;
  if (!ReadFile(h, buf, size, &got, nullptr) || got != size) return 0;
  return 1;
}

static void BuildOutlineRec(Doc* d, FPDF_BOOKMARK parent, int level, std::vector<OutlineItem>& out) {
  if (level > 10 || out.size() > 30000) return;
  FPDF_BOOKMARK bm = PA.FPDFBookmark_GetFirstChild(d->h, parent);
  int guard = 0;
  while (bm && guard++ < 20000) {
    OutlineItem it;
    it.level = level;
    unsigned long need = PA.FPDFBookmark_GetTitle(bm, nullptr, 0);
    if (need >= 2 && need < 4000) {
      std::vector<unsigned short> buf(need / 2 + 1, 0);
      PA.FPDFBookmark_GetTitle(bm, buf.data(), need);
      it.title.assign((wchar_t*)buf.data());
    }
    FPDF_DEST dest = PA.FPDFBookmark_GetDest(d->h, bm);
    if (!dest) {
      FPDF_ACTION ac = PA.FPDFBookmark_GetAction(bm);
      if (ac && PA.FPDFAction_GetType(ac) == PDFACTION_GOTO) dest = PA.FPDFAction_GetDest(d->h, ac);
    }
    if (dest) {
      it.page = PA.FPDFDest_GetDestPageIndex(d->h, dest);
      FPDF_BOOL hx = 0, hy = 0, hz = 0;
      float x = 0, y = 0, z = 0;
      if (PA.FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &y, &z) && hy) { it.hasY = true; it.yPdf = y; }
    }
    size_t idx = out.size();
    out.push_back(it);
    size_t before = out.size();
    BuildOutlineRec(d, bm, level + 1, out);
    if (out.size() > before) out[idx].hasKids = true;
    bm = PA.FPDFBookmark_GetNextSibling(d->h, bm);
  }
}

// page sizes and outline of a freshly loaded PDFium document (under g_pdf)
static void ReadDocInfo(Doc* d, OpenResult* res) {
  d->npages = PA.FPDF_GetPageCount(d->h);
  d->pw.assign(d->npages, 595);
  d->ph.assign(d->npages, 842);
  for (int i = 0; i < d->npages; i++) {
    FS_SIZEF sz2 = {595, 842};
    if (!PA.FPDF_GetPageSizeByIndexF(d->h, i, &sz2) || sz2.width < 1 || sz2.height < 1) { sz2.width = 595; sz2.height = 842; }
    d->pw[i] = sz2.width;
    d->ph[i] = sz2.height;
  }
  d->outline.clear();
  BuildOutlineRec(d, nullptr, 0, d->outline);
  int roots = 0;
  for (auto& o : d->outline) if (o.level == 0) roots++;
  if (roots == 1 && !d->outline.empty()) d->outline[0].open = true;
  if (d->npages <= 0 && res) res->error = 4;
}

static void OpenJob(int tabId, const ws& path, const std::string& password) {
  auto* res = new OpenResult();
  res->tabId = tabId;
  res->path = path;
  auto d = std::make_shared<Doc>();
  d->id = g_nextDocId++;
  d->path = path;
#ifdef APP_EDITOR
  // the editor keeps the whole document in memory: edits are serialized there, undo needs no file, saving cannot collide with a read handle
  if (!ReadFileAll(path, &d->src)) {
    res->error = 1;
  } else {
    d->pass = password;
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    d->h = PA.FPDF_LoadMemDocument(d->src.data(), (int)d->src.size(), password.empty() ? nullptr : password.c_str());
    if (!d->h) {
      unsigned long e = PA.FPDF_GetLastError();
      res->error = e == 4 ? (password.empty() ? 2 : 3) : 4;
    } else ReadDocInfo(d.get(), res);
  }
#else
  d->hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (d->hf == INVALID_HANDLE_VALUE) {
    res->error = 1;
  } else {
    LARGE_INTEGER sz;
    GetFileSizeEx(d->hf, &sz);
    d->fa.m_FileLen = (unsigned long)sz.QuadPart;
    d->fa.m_GetBlock = GetBlock;
    d->fa.m_Param = d->hf;
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    d->h = PA.FPDF_LoadCustomDocument(&d->fa, password.empty() ? nullptr : password.c_str());
    if (!d->h) {
      unsigned long e = PA.FPDF_GetLastError();
      res->error = e == 4 ? (password.empty() ? 2 : 3) : 4;
    } else ReadDocInfo(d.get(), res);
  }
#endif
  if (res->error == 0) res->doc = d;
  PostMessageW(g_notify, WM_APP_OPENED, 0, (LPARAM)res);
}

#ifdef APP_EDITOR
// ------------------------------------------------------------------ editing support
struct MemWriter : FPDF_FILEWRITE {
  std::string* out;
};
static int WriteBlockMem(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
  ((MemWriter*)self)->out->append((const char*)data, size);
  return 1;
}
// serializes a live document (caller holds g_pdf); every edited page must have had its content generated
bool SerializeHandle(FPDF_DOCUMENT h, std::string* out) {
  out->clear();
  MemWriter w;
  w.version = 1;
  w.WriteBlock = WriteBlockMem;
  w.out = out;
  if (PA.FPDF_SaveAsCopy(h, &w, FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS)) return true;
  out->clear();
  return PA.FPDF_SaveAsCopy(h, &w, FPDF_NO_INCREMENTAL) != 0;
}
bool DocSerialize(Doc* d, std::string* out) { return SerializeHandle(d->h, out); }
void DocClosePageCache(Doc* d) {
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  for (auto& e : d->pcache) PA.FPDF_ClosePage(e.second);
  d->pcache.clear();
}
// replaces the document by the one in `bytes` (same Doc object, same id); the previous bytes go to *oldOut. Tiles are not purged here.
bool DocReload(Doc* d, std::string&& bytes, std::string* oldOut) {
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  std::string nsrc = std::move(bytes);
  FPDF_DOCUMENT nh = PA.FPDF_LoadMemDocument(nsrc.data(), (int)nsrc.size(), d->pass.empty() ? nullptr : d->pass.c_str());
  if (!nh) return false;
  for (auto& e : d->pcache) PA.FPDF_ClosePage(e.second);
  d->pcache.clear();
  for (auto& f : d->fonts) if (f.second) PA.FPDFFont_Close((FPDF_FONT)f.second);
  d->fonts.clear();
  PA.FPDF_CloseDocument(d->h);
  d->h = nh;
  if (oldOut) *oldOut = std::move(d->src);
  d->src = std::move(nsrc);
  ReadDocInfo(d, nullptr);
  return true;
}
void DocClearPageData(Doc* d) {
  extern std::mutex g_pdm;
  std::lock_guard<std::mutex> lk(g_pdm);
  d->pdata.clear();
}
#endif

// ------------------------------------------------------------------ cache
static std::mutex g_cm;
struct CEnt { Tile t; std::list<TileKey>::iterator it; size_t bytes; };
static std::unordered_map<TileKey, CEnt, TileKeyHash> g_cache;
static std::list<TileKey> g_lru;
static size_t g_cacheBytes = 0;
static const size_t kCacheMax = 420u << 20;

void CacheLock() { g_cm.lock(); }
void CacheUnlock() { g_cm.unlock(); }
bool CacheGetLocked(const TileKey& k, Tile* t) {
  auto it = g_cache.find(k);
  if (it == g_cache.end()) return false;
  g_lru.splice(g_lru.begin(), g_lru, it->second.it);
  *t = it->second.t;
  return true;
}
// eviction keeps what is near the view: tiles of other documents go first, then tiles of the pages farthest from the visible range;
// pages on screen are only evicted when nothing else is left (plain LRU made previews of far pages push out the visible tiles)
static int g_focDoc = -1, g_focFirst = 0, g_focLast = 0, g_focZq = 0;
void CacheFocus(int docId, int first, int last, int zq) { g_focDoc = docId; g_focFirst = first; g_focLast = last; g_focZq = zq; }   // call with the cache locked
static int CacheDist(const TileKey& k) {
  if (k.doc != g_focDoc) return 1 << 20;
  if (k.kind == 0 && k.zq != g_focZq) return 1 << 19;             // tiles of another zoom level (left over from a pinch) are the first to go
  if (k.page < g_focFirst) return g_focFirst - k.page;
  if (k.page > g_focLast) return k.page - g_focLast;
  return 0;
}
static void CachePut(const TileKey& k, const Tile& t) {
  std::lock_guard<std::mutex> lk(g_cm);
  auto it = g_cache.find(k);
  if (it != g_cache.end()) {
    g_cacheBytes -= it->second.bytes;
    DeleteObject(it->second.t.bmp);
    g_lru.erase(it->second.it);
    g_cache.erase(it);
  }
  g_lru.push_front(k);
  CEnt e;
  e.t = t;
  e.it = g_lru.begin();
  e.bytes = (size_t)t.w * t.h * 4;
  g_cacheBytes += e.bytes;
  g_cache[k] = e;
  while (g_cacheBytes > kCacheMax && g_lru.size() > 8) {
    auto victim = g_lru.end();
    int bestD = -1;
    for (auto li = g_lru.end(); li != g_lru.begin();) {         // from the least recently used
      --li;
      if (*li == k) continue;                                    // never the tile that was just rendered
      int d = CacheDist(*li);
      if (d > bestD) { bestD = d; victim = li; }
    }
    if (victim == g_lru.end()) break;
    auto f = g_cache.find(*victim);
    g_cacheBytes -= f->second.bytes;
    DeleteObject(f->second.t.bmp);
    g_cache.erase(f);
    g_lru.erase(victim);
  }
}
void CachePurgeDoc(int docId) {
  std::lock_guard<std::mutex> lk(g_cm);
  for (auto it = g_cache.begin(); it != g_cache.end();) {
    if (it->first.doc == docId) {
      g_cacheBytes -= it->second.bytes;
      DeleteObject(it->second.t.bmp);
      g_lru.erase(it->second.it);
      it = g_cache.erase(it);
    } else ++it;
  }
}

void CachePurgePage(int docId, int page) {
  std::lock_guard<std::mutex> lk(g_cm);
  for (auto it = g_cache.begin(); it != g_cache.end();) {
    if (it->first.doc == docId && it->first.page == page) {
      g_cacheBytes -= it->second.bytes;
      DeleteObject(it->second.t.bmp);
      g_lru.erase(it->second.it);
      it = g_cache.erase(it);
    } else ++it;
  }
}

// ------------------------------------------------------------------ rendering
static HBITMAP MakeDib(int w, int h, void** bits) {
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void TintBits(uint32_t* px, size_t n, int mode) {
  if (mode == 1) {            // sepia: multiply by a warm paper colour
    for (size_t i = 0; i < n; i++) {
      uint32_t c = px[i];
      uint32_t b = c & 255, g = (c >> 8) & 255, r = (c >> 16) & 255;
      r = r * 244 / 255; g = g * 232 / 255; b = b * 200 / 255;
      px[i] = (r << 16) | (g << 8) | b;
    }
  } else if (mode == 2) {     // night: invert, softened
    for (size_t i = 0; i < n; i++) {
      uint32_t c = px[i];
      uint32_t b = 255 - (c & 255), g = 255 - ((c >> 8) & 255), r = 255 - ((c >> 16) & 255);
      r = 16 + r * 210 / 255; g = 16 + g * 210 / 255; b = 16 + b * 210 / 255;
      px[i] = (r << 16) | (g << 8) | b;
    }
  }
}

struct Job {
  int type = 0;                  // 1 open, 2 tile, 3 pagedata
  int cons = 0;
  uint64_t stamp = 0;
  std::shared_ptr<Doc> doc;
  TileKey key;
  int pageW = 0, pageH = 0, ox = 0, oy = 0, tw = 0, th = 0;
  int prio = 0;
  int page = 0;
  int tabId = 0;
  ws path;
  std::string pw;
};

static std::mutex g_qm;
static std::condition_variable g_qcv;
static std::vector<Job> g_queue;
static uint64_t g_stamp[CONS_COUNT] = {1, 1};
static std::vector<Job> g_collect[CONS_COUNT];
static std::unordered_map<TileKey, int, TileKeyHash> g_pendingTiles;     // queued or in flight
static std::set<std::pair<int, int>> g_pendingData;
static bool g_stop = false;
static std::thread g_thread;
static std::atomic<bool> g_refreshPosted{false};

double g_rndMs = 0; int g_rndN = 0; int g_rndFail = 0, g_rndStale = 0;
size_t CacheStatBytes(size_t* n) { std::lock_guard<std::mutex> lk(g_cm); *n = g_cache.size(); return g_cacheBytes; }
static void RenderJob(const Job& j) {
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  uint64_t rt0 = GetTickCount64();
  if (j.doc->closed) return;
  FPDF_PAGE pg = j.doc->CachedPage(j.key.page);
  if (!pg) return;
  void* bits = nullptr;
  HBITMAP hb = MakeDib(j.tw, j.th, &bits);
  if (!hb) { g_rndFail++; return; }
  FPDF_BITMAP bm = PA.FPDFBitmap_CreateEx(j.tw, j.th, 3 /*BGRx*/, bits, j.tw * 4);
  PA.FPDFBitmap_FillRect(bm, 0, 0, j.tw, j.th, 0xFFFFFFFF);
  int flags = FPDF_ANNOT | (j.key.mode == 0 && j.key.kind == 0 ? FPDF_LCD_TEXT : 0);
  PA.FPDF_RenderPageBitmap(bm, pg, -j.ox, -j.oy, j.pageW, j.pageH, j.key.rot & 3, flags);
  PA.FPDFBitmap_Destroy(bm);
  if (j.key.mode) TintBits((uint32_t*)bits, (size_t)j.tw * j.th, j.key.mode);
  Tile t;
  t.bmp = hb;
  t.w = j.tw;
  t.h = j.th;
  CachePut(j.key, t);
  g_rndMs += (double)(GetTickCount64() - rt0); g_rndN++;
}

static void Notify() {
  bool exp = false;
  if (g_refreshPosted.compare_exchange_strong(exp, true)) PostMessageW(g_notify, WM_APP_REFRESH, 0, 0);
}
void WorkerAckRefresh() { g_refreshPosted = false; }

static void WorkerMain() {
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> lk(g_qm);
      for (;;) {
        if (g_stop) return;
        // drop stale jobs
        for (size_t i = 0; i < g_queue.size();) {
          Job& q = g_queue[i];
          if ((q.type == 2 || q.type == 3) && q.stamp != g_stamp[q.cons]) {
            if (q.type == 2) g_pendingTiles.erase(q.key); else g_pendingData.erase({q.doc->id, q.page});
            g_queue.erase(g_queue.begin() + i);
          } else i++;
        }
        if (!g_queue.empty()) break;
        g_qcv.wait(lk);
      }
      size_t best = 0;
      for (size_t i = 1; i < g_queue.size(); i++)
        if (g_queue[i].prio < g_queue[best].prio) best = i;
      j = g_queue[best];
      g_queue.erase(g_queue.begin() + best);
    }
    if (j.type == 1) {
      OpenJob(j.tabId, j.path, j.pw);
    } else if (j.type == 2) {
      RenderJob(j);
      { std::lock_guard<std::mutex> lk(g_qm); g_pendingTiles.erase(j.key); }
      Notify();
    } else if (j.type == 3) {
      bool had = GetPageDataNow(j.doc, j.page, false) != nullptr;
      if (!had) GetPageDataNow(j.doc, j.page, true);
      { std::lock_guard<std::mutex> lk(g_qm); g_pendingData.erase({j.doc->id, j.page}); }
      if (!had) Notify();
    }
  }
}

void WorkerStart(HWND mainWnd) {
  g_notify = mainWnd;
  g_stop = false;
  g_thread = std::thread(WorkerMain);
}
void WorkerStop() {
  { std::lock_guard<std::mutex> lk(g_qm); g_stop = true; }
  g_qcv.notify_all();
  if (g_thread.joinable()) g_thread.join();
}
void WorkerOpen(int tabId, const ws& path, const std::string& password) {
  Job j;
  j.type = 1;
  j.tabId = tabId;
  j.path = path;
  j.pw = password;
  j.prio = -1;
  { std::lock_guard<std::mutex> lk(g_qm); g_queue.push_back(j); }
  g_qcv.notify_one();
}

void ReqBegin(Consumer c) { g_collect[c].clear(); }
void ReqTile(Consumer c, const std::shared_ptr<Doc>& d, const TileKey& k, int pageW, int pageH, int ox, int oy, int tw, int th, int prio) {
  Job j;
  j.type = 2; j.cons = c; j.doc = d; j.key = k; j.pageW = pageW; j.pageH = pageH; j.ox = ox; j.oy = oy; j.tw = tw; j.th = th; j.prio = prio;
  g_collect[c].push_back(j);
}
void ReqPageData(Consumer c, const std::shared_ptr<Doc>& d, int page, int prio) {
  Job j;
  j.type = 3; j.cons = c; j.doc = d; j.page = page; j.prio = prio;
  g_collect[c].push_back(j);
}
void ReqEnd(Consumer c) {
  {
    std::lock_guard<std::mutex> lk(g_qm);
    uint64_t st = ++g_stamp[c];
    for (Job& n : g_collect[c]) {
      bool found = false;
      for (Job& q : g_queue) {
        if (q.type == n.type && q.cons == c && ((n.type == 2 && q.key == n.key) || (n.type == 3 && q.doc->id == n.doc->id && q.page == n.page))) {
          q.stamp = st; q.prio = n.prio; found = true; break;
        }
      }
      if (found) continue;
      if (n.type == 2) {
        if (g_pendingTiles.count(n.key)) continue;          // in flight
        g_pendingTiles[n.key] = 1;
      } else {
        auto key = std::make_pair(n.doc->id, n.page);
        if (g_pendingData.count(key)) continue;
        g_pendingData.insert(key);
      }
      n.stamp = st;
      g_queue.push_back(n);
    }
    g_collect[c].clear();
  }
  g_qcv.notify_one();
}

// ------------------------------------------------------------------ text, links
std::mutex g_pdm;

void ToTL(FPDF_PAGE page, float w, float h, double x, double y, float* ox, float* oy) {
  const int Q = 16;
  int sw = (int)(w * Q), sh = (int)(h * Q);
  if (sw < 1) sw = 1;
  if (sh < 1) sh = 1;
  int dx = 0, dy = 0;
  PA.FPDF_PageToDevice(page, 0, 0, sw, sh, 0, x, y, &dx, &dy);
  *ox = (float)(dx * (double)w / sw);
  *oy = (float)(dy * (double)h / sh);
}

static std::shared_ptr<PageData> BuildPageData(Doc* d, int pgIdx, bool light = false) {
  auto pd = std::make_shared<PageData>();
  FPDF_PAGE page = PA.FPDF_LoadPage(d->h, pgIdx);
  if (!page) return pd;
  float w = PA.FPDF_GetPageWidthF(page), h = PA.FPDF_GetPageHeightF(page);
  FPDF_TEXTPAGE tp = PA.FPDFText_LoadPage(page);
  if (tp) {
    int n = PA.FPDFText_CountChars(tp);
    if (n < 0) n = 0;
    if (n > 400000) n = 400000;
    pd->ch.resize(n);
    pd->box.resize(n);
    for (int i = 0; i < n; i++) {
      pd->ch[i] = (wchar_t)PA.FPDFText_GetUnicode(tp, i);
      double l, r, b, t;
      if (PA.FPDFText_GetCharBox(tp, i, &l, &r, &b, &t) && (r > l) && (t > b)) {
        float x1, y1, x2, y2;
        ToTL(page, w, h, l, b, &x1, &y1);
        ToTL(page, w, h, r, t, &x2, &y2);
        FRect rc;
        rc.l = min(x1, x2); rc.r = max(x1, x2); rc.t = min(y1, y2); rc.b = max(y1, y2);
        pd->box[i] = rc;
      }
    }
    // web links in plain text
    FPDF_PAGELINK wl = light ? nullptr : PA.FPDFLink_LoadWebLinks(tp);
    if (wl) {
      int cnt = PA.FPDFLink_CountWebLinks(wl);
      for (int i = 0; i < cnt && i < 2000; i++) {
        int len = PA.FPDFLink_GetURL(wl, i, nullptr, 0);
        if (len <= 1) continue;
        std::vector<unsigned short> buf(len + 1, 0);
        PA.FPDFLink_GetURL(wl, i, buf.data(), len);
        ws url((wchar_t*)buf.data());
        int rc = PA.FPDFLink_CountRects(wl, i);
        for (int k = 0; k < rc; k++) {
          double l, t, r, b;
          if (!PA.FPDFLink_GetRect(wl, i, k, &l, &t, &r, &b)) continue;
          float x1, y1, x2, y2;
          ToTL(page, w, h, l, b, &x1, &y1);
          ToTL(page, w, h, r, t, &x2, &y2);
          Link lk;
          lk.url = url;
          lk.r.l = min(x1, x2); lk.r.r = max(x1, x2); lk.r.t = min(y1, y2); lk.r.b = max(y1, y2);
          pd->links.push_back(lk);
        }
      }
      PA.FPDFLink_CloseWebLinks(wl);
    }
    PA.FPDFText_ClosePage(tp);
  }
  if (light) { PA.FPDF_ClosePage(page); return pd; }
  // annotated links
  int pos = 0;
  FPDF_LINK lnk = nullptr;
  int guard = 0;
  while (PA.FPDFLink_Enumerate(page, &pos, &lnk) && guard++ < 5000) {
    FS_RECTF rc;
    if (!PA.FPDFLink_GetAnnotRect(lnk, &rc)) continue;
    Link L;
    float x1, y1, x2, y2;
    ToTL(page, w, h, rc.left, rc.bottom, &x1, &y1);
    ToTL(page, w, h, rc.right, rc.top, &x2, &y2);
    L.r.l = min(x1, x2); L.r.r = max(x1, x2); L.r.t = min(y1, y2); L.r.b = max(y1, y2);
    FPDF_DEST dest = PA.FPDFLink_GetDest(d->h, lnk);
    FPDF_ACTION ac = PA.FPDFLink_GetAction(lnk);
    if (!dest && ac) {
      unsigned long ty = PA.FPDFAction_GetType(ac);
      if (ty == PDFACTION_GOTO) dest = PA.FPDFAction_GetDest(d->h, ac);
      else if (ty == PDFACTION_URI) {
        unsigned long need = PA.FPDFAction_GetURIPath(d->h, ac, nullptr, 0);
        if (need > 1 && need < 8000) {
          std::string u(need, 0);
          PA.FPDFAction_GetURIPath(d->h, ac, &u[0], need);
          u.resize(strlen(u.c_str()));
          L.url = Utf8ToW(u);
        }
      }
    }
    if (dest) {
      L.page = PA.FPDFDest_GetDestPageIndex(d->h, dest);
      FPDF_BOOL hx = 0, hy = 0, hz = 0;
      float x = 0, y = 0, z = 0;
      if (PA.FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &y, &z) && hy) { L.hasY = true; L.yPdf = y; }
    }
    if (L.page >= 0 || !L.url.empty()) pd->links.push_back(L);
  }
  PA.FPDF_ClosePage(page);
  return pd;
}

std::shared_ptr<PageData> GetPageDataNow(const std::shared_ptr<Doc>& d, int page, bool load) {
  {
    std::lock_guard<std::mutex> lk(g_pdm);
    auto it = d->pdata.find(page);
    if (it != d->pdata.end()) return it->second;
  }
  if (!load || page < 0 || page >= d->npages) return nullptr;
  std::shared_ptr<PageData> pd;
  {
    std::lock_guard<std::recursive_mutex> lk(g_pdf);
    if (d->closed) return nullptr;
    pd = BuildPageData(d.get(), page);
  }
  std::lock_guard<std::mutex> lk(g_pdm);
  if (d->pdata.size() > 90) {
    auto farIt = d->pdata.begin();
    for (auto it = d->pdata.begin(); it != d->pdata.end(); ++it)
      if (std::abs(it->first - page) > std::abs(farIt->first - page)) farIt = it;
    d->pdata.erase(farIt);
  }
  d->pdata[page] = pd;
  return pd;
}

bool ResolveDestY(const std::shared_ptr<Doc>& d, int page, double yPdf, float* yTop) {
  std::lock_guard<std::recursive_mutex> lk(g_pdf);
  if (d->closed || page < 0 || page >= d->npages) return false;
  FPDF_PAGE pg = PA.FPDF_LoadPage(d->h, page);
  if (!pg) return false;
  float w = PA.FPDF_GetPageWidthF(pg), h = PA.FPDF_GetPageHeightF(pg);
  float x, y;
  ToTL(pg, w, h, 0, yPdf, &x, &y);
  PA.FPDF_ClosePage(pg);
  *yTop = y;
  return true;
}

void CloseDoc(std::shared_ptr<Doc>& d) {
  if (!d) return;
  d->closed = true;
  CachePurgeDoc(d->id);
  d.reset();
}

// ------------------------------------------------------------------ search
static std::mutex g_sm;
static std::atomic<int> g_searchGen{0};
static std::vector<SearchHit> g_hits;
static bool g_searchDone = true;
static int g_searchScanned = 0;
static int g_searchTab = 0;

void SearchCancel() { g_searchGen++; }

void SearchSnapshot(int tabId, std::vector<SearchHit>* out, bool* done, int* scanned) {
  std::lock_guard<std::mutex> lk(g_sm);
  if (g_searchTab != tabId) { out->clear(); *done = true; *scanned = 0; return; }
  *out = g_hits;
  *done = g_searchDone;
  *scanned = g_searchScanned;
}

static void FoldQuery(ws& s, bool matchCase) {
  if (!matchCase) CharLowerBuffW(&s[0], (DWORD)s.size());
}

// builds highlight rectangles (one per text line) for chars [s, e) of a page
static void RunsFromBoxes(const PageData& pd, int s, int e, std::vector<FRect>* out) {
  FRect run;
  bool have = false;
  for (int i = max(s, 0); i < e && i < (int)pd.box.size(); i++) {
    const FRect& b = pd.box[i];
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
    run = b;
    have = true;
  }
  if (have) out->push_back(run);
}

// own text search: Unicode case folding (Cyrillic too), whitespace and line breaks are interchangeable
static void FindInPage(const PageData& pd, int page, const ws& q, bool matchCase, std::vector<SearchHit>* out) {
  ws norm;
  std::vector<int> map;
  norm.reserve(pd.ch.size());
  map.reserve(pd.ch.size());
  bool lastSpace = true;
  for (int i = 0; i < (int)pd.ch.size(); i++) {
    wchar_t c = pd.ch[i];
    if (c == 0xAD) continue;                               // soft hyphen
    bool sp = c == L' ' || c == L'\r' || c == L'\n' || c == L'\t' || c == 0xA0 || c == 0;
    if (sp) {
      if (lastSpace) continue;
      norm.push_back(L' ');
      map.push_back(i);
      lastSpace = true;
    } else {
      norm.push_back(c);
      map.push_back(i);
      lastSpace = false;
    }
  }
  if (norm.empty()) return;
  FoldQuery(norm, matchCase);
  size_t pos = 0;
  int guard = 0;
  while ((pos = norm.find(q, pos)) != ws::npos && guard++ < 3000) {
    int s = map[pos], e = map[pos + q.size() - 1] + 1;
    SearchHit h;
    h.page = page;
    h.start = s;
    h.count = e - s;
    RunsFromBoxes(pd, s, e, &h.rects);
    out->push_back(h);
    pos += q.size();
  }
}

void SearchStart(const std::shared_ptr<Doc>& d, const ws& q0, bool matchCase, int tabId) {
  int gen = ++g_searchGen;
  // normalise the query the same way as the text
  ws q;
  {
    bool lastSpace = true;
    for (wchar_t c : q0) {
      bool sp = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0xA0;
      if (sp) { if (!lastSpace) q.push_back(L' '); lastSpace = true; }
      else { q.push_back(c); lastSpace = false; }
    }
    while (!q.empty() && q.back() == L' ') q.pop_back();
    FoldQuery(q, matchCase);
  }
  {
    std::lock_guard<std::mutex> lk(g_sm);
    g_hits.clear();
    g_searchDone = q.empty();
    g_searchScanned = 0;
    g_searchTab = tabId;
  }
  if (q.empty()) return;
  std::thread([d, q, matchCase, gen, tabId]() {
    DWORD lastPost = 0;
    for (int p = 0; p < d->npages; p++) {
      if (gen != g_searchGen || d->closed) return;
      std::vector<SearchHit> found;
      std::shared_ptr<PageData> pd = GetPageDataNow(d, p, false);
      if (!pd) {
        std::lock_guard<std::recursive_mutex> lk(g_pdf);
        if (d->closed) return;
        pd = BuildPageData(d.get(), p, true);
      }
      if (pd) FindInPage(*pd, p, q, matchCase, &found);
      {
        std::lock_guard<std::mutex> lk(g_sm);
        if (gen != g_searchGen) return;
        for (auto& f : found) if (g_hits.size() < 20000) g_hits.push_back(f);
        g_searchScanned = p + 1;
        if (p == d->npages - 1) g_searchDone = true;
      }
      DWORD now = GetTickCount();
      if (now - lastPost > 120 || p == d->npages - 1) { lastPost = now; PostMessageW(g_notify, WM_APP_SEARCH, tabId, gen); }
      std::this_thread::yield();
      Sleep(0);
    }
  }).detach();
}
