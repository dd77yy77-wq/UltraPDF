// Modal dialogs: password, about; file association
#include "app.h"
#ifdef APP_EDITOR
#include "editor.h"
#endif
#include <uxtheme.h>

struct DlgState {
  bool done = false;
  int result = 0;
  ws text;
  ws file;
  bool retry = false;
  HWND edit = nullptr;
  int kind = 0;     // 0 password, 1 about, 2 text input, 3 export options, 4 export progress
#ifdef APP_EDITOR
  HWND c[8] = {};
  ExportOpts* eo = nullptr;
  ExportJob* job = nullptr;
  int npages = 0;
#endif
};

enum { IDB_OK = 1, IDB_CANCEL = 2, IDB_UPD = 10, IDB_WEB = 11, IDB_ASSOC = 12 };

static HBRUSH g_dlgBrush = nullptr;

#ifdef APP_EDITOR
enum { IDC_PRESET = 101, IDC_MODE, IDC_DPI, IDC_COLOR, IDC_QUAL, IDC_LIMIT, IDC_PAPER };
struct ExpPreset { const wchar_t* name; int mode, dpi, color, q, mb; const wchar_t* hint; };
static const ExpPreset kExpPresets[] = {
    {L"Как есть (без изменений)", 0, 200, 1, 75, 0, L"Файл сохраняется таким, какой он сейчас. Текст остаётся текстом."},
    {L"Для суда: документ (Word → PDF, подпись и печать)", 1, 200, 1, 75, 30,
     L"«Мой Арбитр»: до 30 МБ на файл, без пароля и интерактива. Текст остаётся чётким и выделяемым; ссылки, поля форм и пометки встраиваются в страницу. Электронная подпись (отсоединённая .sig) делается отдельно, после сохранения — файл не менять."},
    {L"Для суда: скан, серый, 200 dpi", 2, 200, 1, 75, 30,
     L"«Мой Арбитр»: скан 200–300 dpi, чёрно-белый или серый, до 30 МБ. Страницы станут картинками (текст нельзя выделить). Размер подгоняется под лимит автоматически."},
    {L"Для суда: скан, чёрно-белый, 200 dpi", 2, 200, 2, 75, 30,
     L"Страницы станут чёрно-белыми картинками 200 dpi — самый малый размер для текстовых документов. Фотографии и цветные печати лучше сохранять серыми или цветными."},
    {L"Для суда: скан, цветной, 200 dpi", 2, 200, 0, 75, 30,
     L"«Мой Арбитр» допускает цвет там, где он важен (печати, схемы, фото). Страницы станут цветными картинками 200 dpi, размер подгоняется под 30 МБ."},
    {L"Для почты: меньше размер (до 10 МБ)", 2, 150, 0, 60, 10, L"Цветные страницы-картинки 150 dpi, размер подгоняется под 10 МБ."},
    {L"Свои настройки", 2, 200, 1, 75, 0, L"Выберите параметры ниже."},
};
static const int kExpCustom = (int)(sizeof(kExpPresets) / sizeof(kExpPresets[0])) - 1;
static const int kDpiList[] = {100, 150, 200, 250, 300, 400, 600};
static const int kQualList[] = {90, 75, 60, 45};

static void ExpWrite(DlgState* st) {            // opts -> controls
  ExportOpts& o = *st->eo;
  SendMessageW(st->c[0], CB_SETCURSEL, o.preset, 0);
  SendMessageW(st->c[1], CB_SETCURSEL, o.mode, 0);
  int di = 2, qi = 1;
  for (int i = 0; i < 7; i++) if (kDpiList[i] == o.dpi) di = i;
  for (int i = 0; i < 4; i++) if (kQualList[i] <= o.quality) { qi = i; break; }
  SendMessageW(st->c[2], CB_SETCURSEL, di, 0);
  SendMessageW(st->c[3], CB_SETCURSEL, o.color, 0);
  SendMessageW(st->c[4], CB_SETCURSEL, qi, 0);
  SetWindowTextW(st->c[5], std::to_wstring(o.maxMB).c_str());
  SendMessageW(st->c[6], CB_SETCURSEL, min(max(o.paper, 0), 3), 0);
}
static void ExpRead(DlgState* st) {             // controls -> opts
  ExportOpts& o = *st->eo;
  o.preset = (int)SendMessageW(st->c[0], CB_GETCURSEL, 0, 0);
  o.mode = (int)SendMessageW(st->c[1], CB_GETCURSEL, 0, 0);
  int di = (int)SendMessageW(st->c[2], CB_GETCURSEL, 0, 0), qi = (int)SendMessageW(st->c[4], CB_GETCURSEL, 0, 0);
  o.dpi = kDpiList[min(max(di, 0), 6)];
  o.color = (int)SendMessageW(st->c[3], CB_GETCURSEL, 0, 0);
  o.quality = kQualList[min(max(qi, 0), 3)];
  wchar_t b[32];
  GetWindowTextW(st->c[5], b, 32);
  o.maxMB = min(max(_wtoi(b), 0), 2000);
  o.paper = o.mode == 0 ? 0 : (int)min(max((int)SendMessageW(st->c[6], CB_GETCURSEL, 0, 0), 0), 3);
}
static void ExpRefresh(DlgState* st) {
  int mode = (int)SendMessageW(st->c[1], CB_GETCURSEL, 0, 0), color = (int)SendMessageW(st->c[3], CB_GETCURSEL, 0, 0);
  EnableWindow(st->c[2], mode == 2);
  EnableWindow(st->c[3], mode == 2);
  EnableWindow(st->c[4], mode == 2 && color != 2);
  EnableWindow(st->c[5], mode != 0);
  EnableWindow(st->c[6], mode != 0);
  int pr = (int)SendMessageW(st->c[0], CB_GETCURSEL, 0, 0);
  st->file = kExpPresets[min(max(pr, 0), kExpCustom)].hint;
  RECT r = {S(20), S(272), S(500), S(370)};
  InvalidateRect(GetParent(st->c[0]), &r, TRUE);
}
#endif

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
        DrawWordmark(dc, cx, S(130), S(30), T.dark);
        RECT r = {0, S(166), rc.right, S(190)};
        DrawTextW2(dc, FmtT(L"Версия для Windows %s (сборка %d)", APP_VERSION_NAME, APP_VERSION_CODE), r, T.dim, UiFont(S(13)), DT_CENTER | DT_SINGLELINE);
        r = {S(20), S(194), rc.right - S(20), S(236)};
        DrawTextW2(dc, 
#ifdef APP_EDITOR
                 L"Просмотр и редактирование PDF: разметка, текст, подпись и печать, страницы. Движок PDFium (Apache-2.0)."
#else
                 L"Просмотр PDF без лишнего. Движок отрисовки PDFium (Apache-2.0).\nПароли, оглавление, поиск, закладки, печать — в режиме Pro."
#endif
, r,
                   T.dim, UiFont(S(13)), DT_CENTER | DT_WORDBREAK);
#ifdef APP_EDITOR
      } else if (st && st->kind == 3) {
        auto lab = [&](const wchar_t* t, int x, int y) { RECT r = {S(x), S(y), S(x + 200), S(y + 20)}; DrawTextW2(dc, t, r, T.dim, UiFont(S(12)), DT_LEFT | DT_SINGLELINE); };
        lab(L"Набор параметров", 20, 14);
        lab(L"Что сделать со страницами", 20, 72);
        lab(L"Разрешение, dpi", 20, 130);
        lab(L"Цветность", 190, 130);
        lab(L"Качество JPEG", 360, 130);
        lab(L"Лимит размера файла, МБ", 20, 192);
        lab(L"Размер страниц", 320, 192);
        RECT lr = {S(150), S(218), S(400), S(244)};
        DrawTextW2(dc, L"0 — без ограничения", lr, T.dim, UiFont(S(12)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT hr = {S(20), S(272), S(500), S(370)};
        DrawTextW2(dc, st->file, hr, T.text, UiFont(S(13)), DT_LEFT | DT_WORDBREAK);
      } else if (st && st->kind == 4 && st->job) {
        int pg = st->job->page, tot = st->job->total;
        RECT r = {S(20), S(16), S(400), S(44)};
        { HBRUSH eb = CreateSolidBrush(T.chrome2); RECT cl = {0, 0, S(420), S(90)}; FillRect(dc, &cl, eb); DeleteObject(eb); }
        DrawTextW2(dc, tot > 0 ? FmtT(L"Страница %d из %d…", pg, tot) : ws(L"Подготовка…"), r, T.text, UiFont(S(14)), DT_LEFT | DT_SINGLELINE);
        RECT bar = {S(20), S(56), S(400), S(70)};
        FillRoundRect(dc, bar, S(5), T.hover);
        if (tot > 0) {
          RECT fill = bar;
          fill.right = bar.left + (int)((bar.right - bar.left) * (double)pg / tot);
          if (fill.right > fill.left + S(6)) FillRoundRect(dc, fill, S(5), T.accent);
        }
#endif
      } else if (st && st->kind == 2) {
        RECT r = {S(20), S(16), S(400), S(78)};
        DrawTextW2(dc, st->file, r, T.text, UiFont(S(13)), DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
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
#ifdef APP_EDITOR
    case WM_TIMER:
      if (st && st->kind == 4 && st->job) {
        if (st->job->done) { KillTimer(hw, 1); st->done = true; }
        else InvalidateRect(hw, nullptr, FALSE);
      }
      return 0;
#endif
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (!st) return 0;
#ifdef APP_EDITOR
      if (st->kind == 4) { if (st->job && (id == IDB_CANCEL || id == IDB_OK)) st->job->cancel = true; return 0; }
      if (st->kind == 3 && HIWORD(wp) == CBN_SELCHANGE) {
        if (id == IDC_PRESET) {
          int pr = (int)SendMessageW(st->c[0], CB_GETCURSEL, 0, 0);
          if (pr >= 0 && pr < kExpCustom) {
            const ExpPreset& p = kExpPresets[pr];
            st->eo->preset = pr; st->eo->mode = p.mode; st->eo->dpi = p.dpi; st->eo->color = p.color; st->eo->quality = p.q; st->eo->maxMB = p.mb;
            ExpWrite(st);
          }
        } else if (id >= IDC_MODE && id <= IDC_QUAL) SendMessageW(st->c[0], CB_SETCURSEL, kExpCustom, 0);
        ExpRefresh(st);
        return 0;
      }
      if (st->kind == 3 && id == IDC_LIMIT && HIWORD(wp) == EN_CHANGE) { if (SendMessageW(st->c[0], CB_GETCURSEL, 0, 0) != kExpCustom && GetFocus() == st->c[5]) { SendMessageW(st->c[0], CB_SETCURSEL, kExpCustom, 0); ExpRefresh(st); } return 0; }
      if (st->kind == 3 && id == IDB_OK) { ExpRead(st); st->result = 1; st->done = true; return 0; }
#endif
      if (id == IDB_OK) {
        if (st->kind == 0 || st->kind == 2) {
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
    case WM_CLOSE:
#ifdef APP_EDITOR
      if (st && st->kind == 4) { if (st->job) st->job->cancel = true; return 0; }
#endif
      if (st) { st->done = true; st->result = 0; }
      return 0;
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

#ifdef APP_EDITOR
bool ExportOptionsDialog(HWND owner, ExportOpts* o, int npages) {
  DlgState st;
  st.kind = 3;
  st.eo = o;
  st.npages = npages;
  HWND d = MakeDlg(owner, L"Сохранить как…", S(520), S(440), &st);
  auto combo = [&](int id, int x, int y, int w) {
    HWND h = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, S(x), S(y), S(w), S(260), d, (HMENU)(INT_PTR)id,
                             A_.inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)UiFont(S(13)), TRUE);
    SendMessageW(h, CB_SETITEMHEIGHT, (WPARAM)-1, S(22));
    SetWindowTheme(h, T.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    return h;
  };
  st.c[0] = combo(IDC_PRESET, 20, 36, 480);
  for (auto& p : kExpPresets) SendMessageW(st.c[0], CB_ADDSTRING, 0, (LPARAM)p.name);
  st.c[1] = combo(IDC_MODE, 20, 94, 480);
  SendMessageW(st.c[1], CB_ADDSTRING, 0, (LPARAM)L"Оставить без изменений");
  SendMessageW(st.c[1], CB_ADDSTRING, 0, (LPARAM)L"Оставить текст, встроить пометки и убрать интерактив");
  SendMessageW(st.c[1], CB_ADDSTRING, 0, (LPARAM)L"Превратить страницы в картинки (как скан)");
  st.c[2] = combo(IDC_DPI, 20, 152, 150);
  for (int v : kDpiList) SendMessageW(st.c[2], CB_ADDSTRING, 0, (LPARAM)std::to_wstring(v).c_str());
  st.c[3] = combo(IDC_COLOR, 190, 152, 150);
  SendMessageW(st.c[3], CB_ADDSTRING, 0, (LPARAM)L"Цветное");
  SendMessageW(st.c[3], CB_ADDSTRING, 0, (LPARAM)L"Серое");
  SendMessageW(st.c[3], CB_ADDSTRING, 0, (LPARAM)L"Чёрно-белое");
  st.c[4] = combo(IDC_QUAL, 360, 152, 140);
  SendMessageW(st.c[4], CB_ADDSTRING, 0, (LPARAM)L"Высокое");
  SendMessageW(st.c[4], CB_ADDSTRING, 0, (LPARAM)L"Хорошее");
  SendMessageW(st.c[4], CB_ADDSTRING, 0, (LPARAM)L"Среднее");
  SendMessageW(st.c[4], CB_ADDSTRING, 0, (LPARAM)L"Низкое");
  st.c[5] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, S(20), S(216), S(110), S(28), d, (HMENU)(INT_PTR)IDC_LIMIT, A_.inst, nullptr);
  SendMessageW(st.c[5], WM_SETFONT, (WPARAM)UiFont(S(14)), TRUE);
  st.c[6] = combo(IDC_PAPER, 320, 216, 180);
  SendMessageW(st.c[6], CB_ADDSTRING, 0, (LPARAM)L"Не менять");
  SendMessageW(st.c[6], CB_ADDSTRING, 0, (LPARAM)L"A4 (как у страницы)");
  SendMessageW(st.c[6], CB_ADDSTRING, 0, (LPARAM)L"A4 книжная");
  SendMessageW(st.c[6], CB_ADDSTRING, 0, (LPARAM)L"A4 альбомная");
  ExpWrite(&st);
  ExpRefresh(&st);
  MkButton(d, L"Продолжить", IDB_OK, S(306), S(386), S(100), S(32), true);
  MkButton(d, L"Отмена", IDB_CANCEL, S(412), S(386), S(90), S(32), false);
  RunModal(owner, d, &st);
  return st.result == 1;
}

void ProgressDialog(HWND owner, const wchar_t* title, ExportJob* job) {
  DlgState st;
  st.kind = 4;
  st.job = job;
  HWND d = MakeDlg(owner, title, S(420), S(150), &st);
  MkButton(d, L"Отмена", IDB_CANCEL, S(300), S(96), S(100), S(32), false);
  SetTimer(d, 1, 100, nullptr);
  RunModal(owner, d, &st);
}

bool InputText(HWND owner, const wchar_t* title, const ws& prompt, const ws& init, ws* out) {
  DlgState st;
  st.kind = 2;
  st.file = prompt;
  HWND d = MakeDlg(owner, title, S(420), S(170), &st);
  st.edit = CreateWindowExW(0, L"EDIT", init.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, S(20), S(86), S(380), S(28), d, nullptr, A_.inst, nullptr);
  SendMessageW(st.edit, WM_SETFONT, (WPARAM)UiFont(S(14)), TRUE);
  MkButton(d, L"OK", IDB_OK, S(214), S(126), S(90), S(32), true);
  MkButton(d, L"Отмена", IDB_CANCEL, S(310), S(126), S(90), S(32), false);
  SetFocus(st.edit);
  SendMessageW(st.edit, EM_SETSEL, 0, -1);
  RunModal(owner, d, &st);
  if (st.result == 1) { *out = st.text; return true; }
  return false;
}
#endif

void ShowAbout(HWND owner) {
  DlgState st;
  st.kind = 1;
  HWND d = MakeDlg(owner, L"О программе", S(460), S(350), &st);
  MkButton(d, L"Назначить для PDF…", IDB_ASSOC, S(20), S(248), S(200), S(34), false);
  MkButton(d, L"Закрыть", IDB_CANCEL, S(236), S(248), S(204), S(34), true);
  MkButton(d, L"Проверить обновления", IDB_UPD, S(20), S(292), S(200), S(34), false);
  MkButton(d, L"Страница на GitHub", IDB_WEB, S(236), S(292), S(204), S(34), false);
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
  SetKey(r, L"Software\\Classes\\" APP_ID L".Document", nullptr, L"Документ PDF");
  SetKey(r, L"Software\\Classes\\" APP_ID L".Document\\DefaultIcon", nullptr, exe + L",0");
  SetKey(r, L"Software\\Classes\\" APP_ID L".Document\\shell\\open\\command", nullptr, cmd);
  SetKey(r, L"Software\\Classes\\.pdf\\OpenWithProgids", APP_ID L".Document", L"");
  SetKey(r, L"Software\\Classes\\Applications\\" APP_EXE L"", L"FriendlyAppName", APP_TITLE);
  SetKey(r, L"Software\\Classes\\Applications\\" APP_EXE L"\\shell\\open\\command", nullptr, cmd);
  SetKey(r, L"Software\\Classes\\Applications\\" APP_EXE L"\\SupportedTypes", L".pdf", L"");
  SetKey(r, L"Software\\" APP_ID L"\\Capabilities", L"ApplicationName", APP_TITLE);
  SetKey(r, L"Software\\" APP_ID L"\\Capabilities", L"ApplicationDescription", L"Просмотр PDF");
  SetKey(r, L"Software\\" APP_ID L"\\Capabilities\\FileAssociations", L".pdf", APP_ID L".Document");
  SetKey(r, L"Software\\RegisteredApplications", APP_ID, L"Software\\" APP_ID L"\\Capabilities");
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  MessageBoxW(A_.main,
              APP_TITLE L" добавлен в список программ для PDF.\n\nWindows не разрешает программам менять выбор самостоятельно, поэтому "
              L"сейчас откроются параметры: выберите там " APP_TITLE L" для файлов .pdf. Либо правый клик по любому PDF → «Открыть с помощью» → " APP_TITLE L" → «Всегда».",
              APP_TITLE, MB_OK | MB_ICONINFORMATION);
  ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}
