// Self-update from GitHub (version-win.json + UltraPDF.exe) over HTTPS
#include "app.h"

struct UpdInfo { int code = 0; ws name, notes; std::string sha; };
static std::atomic<bool> g_busy{false};
static bool g_manual = false;

static bool HttpGet(const ws& host, const ws& path, std::string* body, const ws& saveTo, DWORD* status) {
  HINTERNET s = WinHttpOpen(APP_TITLE L"/" APP_VERSION_NAME, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!s) return false;
  WinHttpSetTimeouts(s, 8000, 8000, 15000, 30000);
  bool ok = false;
  HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET rq = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  if (rq && WinHttpSendRequest(rq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(rq, nullptr)) {
    DWORD st = 0, sz = sizeof(st);
    WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &st, &sz, nullptr);
    if (status) *status = st;
    if (st == 200) {
      HANDLE f = INVALID_HANDLE_VALUE;
      if (!saveTo.empty()) f = CreateFileW(saveTo.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      ok = saveTo.empty() || f != INVALID_HANDLE_VALUE;
      std::vector<char> buf(64 * 1024);
      DWORD got = 0;
      while (ok && WinHttpReadData(rq, buf.data(), (DWORD)buf.size(), &got) && got) {
        if (f != INVALID_HANDLE_VALUE) { DWORD w; if (!WriteFile(f, buf.data(), got, &w, nullptr) || w != got) ok = false; }
        else { body->append(buf.data(), got); if (body->size() > (1 << 20)) break; }
      }
      if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
  }
  if (rq) WinHttpCloseHandle(rq);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok;
}

static std::string JsonStr(const std::string& j, const char* key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = j.find(k);
  if (p == std::string::npos) return "";
  p = j.find(':', p);
  if (p == std::string::npos) return "";
  p++;
  while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
  if (p < j.size() && j[p] == '"') {
    std::string o;
    p++;
    while (p < j.size() && j[p] != '"') {
      if (j[p] == '\\' && p + 1 < j.size()) {
        p++;
        char c = j[p];
        if (c == 'n') o += '\n';
        else if (c == 'u' && p + 4 < j.size()) {
          unsigned v = (unsigned)strtoul(j.substr(p + 1, 4).c_str(), nullptr, 16);
          wchar_t w = (wchar_t)v;
          o += WToUtf8(ws(1, w));
          p += 4;
        } else o += c;
      } else o += j[p];
      p++;
    }
    return o;
  }
  size_t e = p;
  while (e < j.size() && (isdigit((unsigned char)j[e]) || j[e] == '.' || j[e] == '-')) e++;
  return j.substr(p, e - p);
}

static std::string Sha256File(const ws& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  std::string out;
  if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
    std::vector<BYTE> buf(1 << 16);
    DWORD got;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) CryptHashData(hash, buf.data(), got, 0);
    BYTE d[32];
    DWORD dl = 32;
    if (CryptGetHashParam(hash, HP_HASHVAL, d, &dl, 0)) {
      char hx[3];
      for (DWORD i = 0; i < dl; i++) { sprintf(hx, "%02x", d[i]); out += hx; }
    }
  }
  if (hash) CryptDestroyHash(hash);
  if (prov) CryptReleaseContext(prov, 0);
  CloseHandle(f);
  return out;
}

struct UpdMsg { int kind; UpdInfo info; ws text; };

static void Post(int kind, const UpdInfo& i, const ws& text) {
  auto* m = new UpdMsg{kind, i, text};
  PostMessageW(A_.main, WM_APP_UPDATE, kind, (LPARAM)m);
}

void CheckUpdates(bool manual) {
  if (g_busy.exchange(true)) return;
  g_manual = manual;
  G.lastUpdateCheck = (uint64_t)time(nullptr);
  std::thread([manual]() {
    std::string body;
    DWORD st = 0;
    UpdInfo info;
    bool ok = HttpGet(UPDATE_JSON_HOST, UPDATE_JSON_PATH, &body, L"", &st);
    if (!ok) {
      g_busy = false;
      if (manual) Post(3, info, L"Не удалось связаться с GitHub. Проверьте интернет.");
      return;
    }
    info.code = atoi(JsonStr(body, "code").c_str());
    info.name = Utf8ToW(JsonStr(body, "name"));
    info.notes = Utf8ToW(JsonStr(body, "notes"));
    info.sha = JsonStr(body, "sha256");
    g_busy = false;
    if (info.code > APP_VERSION_CODE) Post(1, info, L"");
    else if (manual) Post(2, info, L"");
  }).detach();
}

void ApplyDownloaded(const ws& neu) {
  ws self = ExePath();
  ws old = self + L".old";
  DeleteFileW(old.c_str());
  if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    MessageBoxW(A_.main,
                L"Не удалось заменить файл программы (нет прав на папку). Скачайте новую версию вручную со страницы GitHub — открою её.",
                APP_TITLE, MB_OK | MB_ICONWARNING);
    ShellExecuteW(nullptr, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL);
    DeleteFileW(neu.c_str());
    return;
  }
  if (!MoveFileExW(neu.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING);
    MessageBoxW(A_.main, L"Не удалось установить обновление.", APP_TITLE, MB_OK | MB_ICONWARNING);
    return;
  }
  SaveAllState();
  if (g_mutex) { CloseHandle(g_mutex); g_mutex = nullptr; }
  ShellExecuteW(nullptr, L"open", self.c_str(), L"--updated", nullptr, SW_SHOWNORMAL);
  ExitProcess(0);
}

void OnUpdateMsg(WPARAM wp, LPARAM lp) {
  UpdMsg* m = (UpdMsg*)lp;
  if (!m) return;
  std::unique_ptr<UpdMsg> guard(m);
  if (wp == 1) {
    ws t = FmtT(L"Доступна версия %s (у вас %s).", m->info.name.c_str(), APP_VERSION_NAME);
    if (!m->info.notes.empty()) t += L"\n\n" + m->info.notes;
    t += L"\n\nОбновить сейчас? Программа перезапустится, открытые вкладки вернутся.";
    if (MessageBoxW(A_.main, t.c_str(), L"Обновление " APP_TITLE, MB_YESNO | MB_ICONINFORMATION) != IDYES) return;
    ShowToast(L"Скачиваю обновление…");
    UpdInfo info = m->info;
    g_busy = true;
    std::thread([info]() {
      ws neu = ExePath() + L".new";
      DWORD st = 0;
      std::string dummy;
      bool ok = HttpGet(UPDATE_JSON_HOST, UPDATE_EXE_PATH, &dummy, neu, &st);
      if (ok) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        ok = GetFileAttributesExW(neu.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (1u << 20);
        if (ok) {
          char mz[2] = {0, 0};
          HANDLE f = CreateFileW(neu.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
          DWORD got = 0;
          if (f != INVALID_HANDLE_VALUE) { ReadFile(f, mz, 2, &got, nullptr); CloseHandle(f); }
          ok = got == 2 && mz[0] == 'M' && mz[1] == 'Z';
        }
        if (ok && !info.sha.empty()) ok = Sha256File(neu) == info.sha;
      }
      g_busy = false;
      if (ok) Post(4, info, neu);
      else { DeleteFileW(neu.c_str()); Post(5, info, L"Не удалось скачать обновление или файл не прошёл проверку."); }
    }).detach();
  } else if (wp == 2) {
    MessageBoxW(A_.main, L"Установлена последняя версия.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
  } else if (wp == 3 || wp == 5) {
    MessageBoxW(A_.main, m->text.c_str(), APP_TITLE, MB_OK | MB_ICONWARNING);
  } else if (wp == 4) {
    ApplyDownloaded(m->text);
  }
}
