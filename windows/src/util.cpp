// helpers, theme, settings storage
#include "app.h"
#include <cstdarg>

PdfApi PA;
Theme T;
Settings G;

ws Utf8ToW(const std::string& s) {
  if (s.empty()) return ws();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  ws w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
std::string WToUtf8(const ws& s) {
  if (s.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string o(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n, nullptr, nullptr);
  return o;
}
ws BaseName(const ws& p) {
  size_t i = p.find_last_of(L"\\/");
  return i == ws::npos ? p : p.substr(i + 1);
}
ws DirName(const ws& p) {
  size_t i = p.find_last_of(L"\\/");
  return i == ws::npos ? ws() : p.substr(0, i);
}
ws StripExt(const ws& p) {
  size_t i = p.find_last_of(L'.');
  size_t s = p.find_last_of(L"\\/");
  if (i == ws::npos || (s != ws::npos && i < s)) return p;
  return p.substr(0, i);
}
ws FmtT(const wchar_t* f, ...) {
  wchar_t buf[1024];
  va_list ap;
  va_start(ap, f);
  _vsnwprintf(buf, 1023, f, ap);
  va_end(ap);
  buf[1023] = 0;
  return buf;
}
bool FileExists(const ws& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
uint64_t NowMs() { return GetTickCount64(); }
ws LowerW(const ws& s) {
  ws r = s;
  CharLowerBuffW(&r[0], (DWORD)r.size());
  return r;
}
int Dpi() { return A_.dpi; }
int S(int v) { return MulDiv(v, A_.dpi, 96); }

bool ReadFileAll(const ws& path, std::string* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  GetFileSizeEx(h, &sz);
  if (sz.QuadPart > (1 << 26)) { CloseHandle(h); return false; }
  out->resize((size_t)sz.QuadPart);
  DWORD got = 0;
  bool ok = sz.QuadPart == 0 || ReadFile(h, &(*out)[0], (DWORD)sz.QuadPart, &got, nullptr);
  CloseHandle(h);
  return ok;
}
bool WriteFileAtomic(const ws& path, const std::string& data) {
  ws tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD w = 0;
  bool ok = data.empty() || WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
  CloseHandle(h);
  if (!ok) { DeleteFileW(tmp.c_str()); return false; }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
ws AppDataDir() {
  wchar_t b[MAX_PATH];
  ws d;
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, b))) d = b; else d = L".";
  d += L"\\UltraPDF";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}
ws ExePath() {
  wchar_t b[1024];
  DWORD n = GetModuleFileNameW(nullptr, b, 1024);
  return ws(b, n);
}

// ---------------------------------------------------------------- theme
bool SystemIsDark() {
  DWORD v = 1, sz = 4;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                   RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
    return false;
  return v == 0;
}
void ApplyTheme(int mode) {
  bool dark = mode == 2 || (mode == 0 && SystemIsDark());
  T.dark = dark;
  COLORREF acc = RGB(0, 120, 212);
  DWORD col = 0; BOOL opaque = FALSE;
  HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
  if (dwm) {
    typedef HRESULT(WINAPI * GC)(DWORD*, BOOL*);
    GC gc = (GC)GetProcAddress(dwm, "DwmGetColorizationColor");
    if (gc && SUCCEEDED(gc(&col, &opaque))) acc = RGB((col >> 16) & 255, (col >> 8) & 255, col & 255);
    FreeLibrary(dwm);
  }
  // keep the accent readable
  int lum = (GetRValue(acc) * 3 + GetGValue(acc) * 6 + GetBValue(acc)) / 10;
  if (dark && lum < 90) acc = RGB(76, 160, 255);
  if (!dark && lum > 170) acc = RGB(0, 103, 192);
  T.accent = acc;
  if (dark) {
    T.chrome = RGB(32, 32, 32);   T.chrome2 = RGB(43, 43, 43);  T.text = RGB(240, 240, 240); T.dim = RGB(160, 160, 160);
    T.hover = RGB(62, 62, 62);    T.press = RGB(78, 78, 78);    T.line = RGB(64, 64, 64);     T.viewbg = RGB(40, 40, 40);
    T.tabActive = RGB(43, 43, 43); T.tabIdle = RGB(24, 24, 24); T.field = RGB(55, 55, 55);    T.fieldLine = RGB(90, 90, 90);
  } else {
    T.chrome = RGB(243, 243, 243); T.chrome2 = RGB(251, 251, 251); T.text = RGB(27, 27, 27);  T.dim = RGB(104, 104, 104);
    T.hover = RGB(226, 226, 226);  T.press = RGB(210, 210, 210);   T.line = RGB(218, 218, 218); T.viewbg = RGB(225, 225, 225);
    T.tabActive = RGB(251, 251, 251); T.tabIdle = RGB(232, 232, 232); T.field = RGB(255, 255, 255); T.fieldLine = RGB(190, 190, 190);
  }
}

// ---------------------------------------------------------------- settings
static ws SettingsPath() { return AppDataDir() + L"\\state.txt"; }

static std::vector<std::string> Split(const std::string& s, char c) {
  std::vector<std::string> o;
  size_t i = 0;
  while (true) {
    size_t j = s.find(c, i);
    if (j == std::string::npos) { o.push_back(s.substr(i)); break; }
    o.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return o;
}

void LoadSettings() {
  std::string data;
  if (!ReadFileAll(SettingsPath(), &data)) return;
  size_t i = 0;
  while (i < data.size()) {
    size_t j = data.find('\n', i);
    if (j == std::string::npos) j = data.size();
    std::string line = data.substr(i, j - i);
    i = j + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto f = Split(line, '\t');
    if (f.empty()) continue;
    if (f[0] == "S" && f.size() >= 3) {
      const std::string& k = f[1];
      long v = atol(f[2].c_str());
      if (k == "pro") G.pro = v != 0;
      else if (k == "theme") G.themeMode = (int)v;
      else if (k == "flip") G.flipMode = (int)v;
      else if (k == "mode") G.readMode = (int)v;
      else if (k == "sidebar") G.sidebar = v != 0;
      else if (k == "sideW") G.sideW = (int)v;
      else if (k == "sideTab") G.sideTab = (int)v;
      else if (k == "x") G.winX = (int)v;
      else if (k == "y") G.winY = (int)v;
      else if (k == "w") G.winW = (int)v;
      else if (k == "h") G.winH = (int)v;
      else if (k == "max") G.maximized = v != 0;
      else if (k == "case") G.matchCase = v != 0;
      else if (k == "upd") G.lastUpdateCheck = (uint64_t)_strtoui64(f[2].c_str(), nullptr, 10);
      else if (k == "active") G.sessionActive = (int)v;
    } else if (f[0] == "T" && f.size() >= 2) {
      G.session.push_back(Utf8ToW(f[1]));
    } else if (f[0] == "D" && f.size() >= 9) {
      DocState d;
      d.path = Utf8ToW(f[1]);
      d.time = (uint64_t)_strtoui64(f[2].c_str(), nullptr, 10);
      d.page = atoi(f[3].c_str());
      d.off = (float)atof(f[4].c_str());
      d.zoom = atof(f[5].c_str());
      d.fit = atoi(f[6].c_str());
      d.rot = atoi(f[7].c_str()) & 3;
      for (auto& b : Split(f[8], ',')) if (!b.empty()) d.bookmarks.push_back(atoi(b.c_str()));
      if (d.zoom < 0.05 || d.zoom > 32) d.zoom = 1.0;
      G.docs.push_back(d);
    }
  }
  std::stable_sort(G.docs.begin(), G.docs.end(), [](const DocState& a, const DocState& b) { return a.time > b.time; });
  if (G.sideW < 140 || G.sideW > 600) G.sideW = 250;
  if (G.winW < 400 || G.winH < 300) { G.winW = 1180; G.winH = 820; }
}

void SaveSettings() {
  std::string o;
  auto S_ = [&](const char* k, long long v) { o += "S\t"; o += k; o += "\t"; o += std::to_string(v); o += "\n"; };
  S_("pro", G.pro); S_("theme", G.themeMode); S_("flip", G.flipMode); S_("mode", G.readMode); S_("sidebar", G.sidebar); S_("sideW", G.sideW);
  S_("sideTab", G.sideTab); S_("x", G.winX); S_("y", G.winY); S_("w", G.winW); S_("h", G.winH); S_("max", G.maximized);
  S_("case", G.matchCase); S_("upd", (long long)G.lastUpdateCheck); S_("active", G.sessionActive);
  for (auto& s : G.session) { o += "T\t" + WToUtf8(s) + "\n"; }
  int n = 0;
  for (auto& d : G.docs) {
    if (n++ >= 200) break;
    o += "D\t" + WToUtf8(d.path) + "\t" + std::to_string(d.time) + "\t" + std::to_string(d.page) + "\t" + std::to_string(d.off) +
         "\t" + std::to_string(d.zoom) + "\t" + std::to_string(d.fit) + "\t" + std::to_string(d.rot) + "\t";
    for (size_t i = 0; i < d.bookmarks.size(); i++) { if (i) o += ","; o += std::to_string(d.bookmarks[i]); }
    o += "\n";
  }
  WriteFileAtomic(SettingsPath(), o);
}

DocState* FindState(const ws& path, bool create) {
  ws lp = LowerW(path);
  for (auto& d : G.docs) if (LowerW(d.path) == lp) return &d;
  if (!create) return nullptr;
  DocState d;
  d.path = path;
  G.docs.insert(G.docs.begin(), d);
  return &G.docs.front();
}
void TouchState(const DocState& st) {
  DocState* d = FindState(st.path, true);
  *d = st;
  d->time = (uint64_t)time(nullptr);
  std::stable_sort(G.docs.begin(), G.docs.end(), [](const DocState& a, const DocState& b) { return a.time > b.time; });
}
