// Modal dialogs: password, about; file association
#include "app.h"
#include <uxtheme.h>

struct DlgState {
  bool done = false;
  int result = 0;
  ws text;
  ws file;
  bool retry = false;
  HWND edit = nullptr;
  int kind = 0;     // 0 password, 1 about
};

enum { IDB_OK = 1, IDB_CANCEL = 2, IDB_UPD = 10, IDB_WEB = 11, IDB_ASSOC = 12 };

static HBRUSH g_dlgBrush = nullptr;

static LRESULT CALLBACK DlgProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  DlgState* st = (DlgState*)GetWindowLongPtrW(hw, GWLP_USERDATA);
  switch (msg) {
    case WM_NCCREATE: {
      CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
      SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
      return DefWindowProcW(hw, msg, wp, lp);
    }
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(hw, &rc);
      HBRUSH b = CreateSolidBrush(T.chrome2);
      FillRect((HDC)wp, &rc, b);
      DeleteObject(b);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hw, &ps);
      if (st && st->kind == 1) {
        RECT rc;
        GetClientRect(hw, &rc);
        int cx = rc.right / 2;
        DrawAppIcon(dc, cx - S(48), S(22), S(96));
        RECT r = {0, S(128), rc.right, S(164)};
        DrawTextW2(dc, L"UltraPDF", r, T.text, UiFont(S(24), true), DT_CENTER | DT_SINGLELINE);
        r = {0, S(166), rc.right, S(190)};
        DrawTextW2(dc, FmtT(L"Версия для Windows %s (сборка %d)", APP_VERSION_NAME, APP_VERSION_CODE), r, T.dim, UiFont(S(13)), DT_CENTER | DT_SINGLELINE);
        r = {S(20), S(196), rc.right - S(20), S(250)};
        DrawTextW2(dc, L"Просмотр PDF без лишнего. Движок отрисовки PDFium (Apache-2.0).\nПароли, оглавление, поиск, закладки, печать — в режиме Pro.", r,
                   T.dim, UiFont(S(13)), DT_CENTER | DT_WORDBREAK);
      } else if (st) {
        RECT r = {S(20), S(16), S(400), S(70)};
        ws t = st->retry ? L"Пароль не подошёл. Введите ещё раз." : L"Документ защищён паролем.";
        DrawTextW2(dc, t + L"\n" + st->file, r, T.text, UiFont(S(13)), DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
      }
      EndPaint(hw, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT: {
      SetTextColor((HDC)wp, T.text);
      SetBkColor((HDC)wp, T.field);
      static HBRUSH eb = nullptr;
      if (eb) DeleteObject(eb);
      eb = CreateSolidBrush(T.field);
      return (LRESULT)eb;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
      SetTextColor((HDC)wp, T.text);
      SetBkColor((HDC)wp, T.chrome2);
      if (g_dlgBrush) DeleteObject(g_dlgBrush);
      g_dlgBrush = CreateSolidBrush(T.chrome2);
      return (LRESULT)g_dlgBrush;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (!st) return 0;
      if (id == IDB_OK) {
        if (st->kind == 0) {
          wchar_t b[512];
          GetWindowTextW(st->edit, b, 512);
          st->text = b;
        }
        st->result = 1; st->done = true;
      } else if (id == IDB_CANCEL) { st->result = 0; st->done = true; }
      else if (id == IDB_UPD) { st->done = true; st->result = 2; }
      else if (id == IDB_WEB) ShellExecuteW(hw, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL);
      else if (id == IDB_ASSOC) { st->done = true; st->result = 3; }
      return 0;
    }
    case WM_CLOSE: if (st) { st->done = true; st->result = 0; } return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

static void EnsureClass() {
  static bool done = false;
  if (done) return;
  done = true;
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = DlgProc;
  wc.hInstance = A_.inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"UltraPDF.Dlg";
  RegisterClassExW(&wc);
}

static HWND MkButton(HWND p, const wchar_t* t, int id, int x, int y, int w, int h, bool def) {
  HWND b = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y, w, h, p,
                           (HMENU)(INT_PTR)id, A_.inst, nullptr);
  SendMessageW(b, WM_SETFONT, (WPARAM)UiFont(S(13)), TRUE);
  SetWindowTheme(b, T.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  return b;
}

static void RunModal(HWND owner, HWND dlg, DlgState* st) {
  EnableWindow(owner, FALSE);
  ShowWindow(dlg, SW_SHOW);
  UpdateWindow(dlg);
  MSG m;
  while (!st->done && GetMessageW(&m, nullptr, 0, 0)) {
    if (m.message == WM_KEYDOWN && (m.hwnd == dlg || IsChild(dlg, m.hwnd))) {
      if (m.wParam == VK_RETURN) { SendMessageW(dlg, WM_COMMAND, IDB_OK, 0); continue; }
      if (m.wParam == VK_ESCAPE) { SendMessageW(dlg, WM_COMMAND, IDB_CANCEL, 0); continue; }
    }
    if (!IsDialogMessageW(dlg, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  EnableWindow(owner, TRUE);
  DestroyWindow(dlg);
  SetForegroundWindow(owner);
}

static HWND MakeDlg(HWND owner, const wchar_t* title, int cw, int ch, DlgState* st) {
  EnsureClass();
  RECT r = {0, 0, cw, ch};
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
  RECT orc;
  GetWindowRect(owner, &orc);
  int w = r.right - r.left, h = r.bottom - r.top;
  int x = orc.left + ((orc.right - orc.left) - w) / 2, y = orc.top + ((orc.bottom - orc.top) - h) / 2;
  HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME, L"UltraPDF.Dlg", title, style, x, y, w, h, owner, nullptr, A_.inst, st);
  BOOL dark = T.dark;
  HMODULE dw = LoadLibraryW(L"dwmapi.dll");
  if (dw) {
    typedef HRESULT(WINAPI * F)(HWND, DWORD, LPCVOID, DWORD);
    F f = (F)GetProcAddress(dw, "DwmSetWindowAttribute");
    if (f) f(d, 20, &dark, sizeof(dark));
    FreeLibrary(dw);
  }
  return d;
}

bool InputPassword(HWND owner, const ws& file, bool retry, ws* out) {
  DlgState st;
  st.kind = 0;
  st.file = file;
  st.retry = retry;
  HWND d = MakeDlg(owner, L"Пароль", S(420), S(170), &st);
  st.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, S(20), S(78), S(380), S(28), d,
                            nullptr, A_.inst, nullptr);
  SendMessageW(st.edit, WM_SETFONT, (WPARAM)UiFont(S(14)), TRUE);
  MkButton(d, L"Открыть", IDB_OK, S(214), S(122), S(90), S(32), true);
  MkButton(d, L"Отмена", IDB_CANCEL, S(310), S(122), S(90), S(32), false);
  SetFocus(st.edit);
  RunModal(owner, d, &st);
  if (st.result == 1) { *out = st.text; return true; }
  return false;
}

void ShowAbout(HWND owner) {
  DlgState st;
  st.kind = 1;
  HWND d = MakeDlg(owner, L"О программе", S(460), S(330), &st);
  MkButton(d, L"Проверить обновления", IDB_UPD, S(20), S(262), S(200), S(34), false);
  MkButton(d, L"Страница на GitHub", IDB_WEB, S(236), S(262), S(204), S(34), false);
  MkButton(d, L"Назначить для PDF…", IDB_ASSOC, S(20), S(216), S(200), S(34), false);
  MkButton(d, L"Закрыть", IDB_CANCEL, S(236), S(216), S(204), S(34), true);
  RunModal(owner, d, &st);
  if (st.result == 2) CheckUpdates(true);
  else if (st.result == 3) RegisterAssoc();
}

// ---------------------------------------------------------------- file association
static void SetKey(HKEY root, const ws& path, const wchar_t* name, const ws& value) {
  HKEY k;
  if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
  }
}

void RegisterAssoc() {
  ws exe = ExePath();
  ws cmd = L"\"" + exe + L"\" \"%1\"";
  HKEY r = HKEY_CURRENT_USER;
  SetKey(r, L"Software\\Classes\\UltraPDF.Document", nullptr, L"Документ PDF");
  SetKey(r, L"Software\\Classes\\UltraPDF.Document\\DefaultIcon", nullptr, exe + L",0");
  SetKey(r, L"Software\\Classes\\UltraPDF.Document\\shell\\open\\command", nullptr, cmd);
  SetKey(r, L"Software\\Classes\\.pdf\\OpenWithProgids", L"UltraPDF.Document", L"");
  SetKey(r, L"Software\\Classes\\Applications\\UltraPDF.exe", L"FriendlyAppName", L"UltraPDF");
  SetKey(r, L"Software\\Classes\\Applications\\UltraPDF.exe\\shell\\open\\command", nullptr, cmd);
  SetKey(r, L"Software\\Classes\\Applications\\UltraPDF.exe\\SupportedTypes", L".pdf", L"");
  SetKey(r, L"Software\\UltraPDF\\Capabilities", L"ApplicationName", L"UltraPDF");
  SetKey(r, L"Software\\UltraPDF\\Capabilities", L"ApplicationDescription", L"Просмотр PDF");
  SetKey(r, L"Software\\UltraPDF\\Capabilities\\FileAssociations", L".pdf", L"UltraPDF.Document");
  SetKey(r, L"Software\\RegisteredApplications", L"UltraPDF", L"Software\\UltraPDF\\Capabilities");
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  MessageBoxW(A_.main,
              L"UltraPDF добавлен в список программ для PDF.\n\nWindows не разрешает программам менять выбор самостоятельно, поэтому "
              L"сейчас откроются параметры: выберите там UltraPDF для файлов .pdf. Либо правый клик по любому PDF → «Открыть с помощью» → UltraPDF → «Всегда».",
              L"UltraPDF", MB_OK | MB_ICONINFORMATION);
  ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}
