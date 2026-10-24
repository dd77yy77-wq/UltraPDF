// UltraPDF Editor: editing layer on top of the viewer (only compiled into the editor build).
#pragma once
#include "app.h"
#include <atomic>

#ifdef APP_EDITOR
typedef Gdiplus::PointF PointF;

enum EdCmd {
  ID_ED_SAVE = 600, ID_ED_SAVEAS_E, ID_ED_UNDO, ID_ED_REDO,
  ID_ED_T_SELECT, ID_ED_T_HILITE, ID_ED_T_UNDER, ID_ED_T_STRIKE, ID_ED_T_SQUIG,
  ID_ED_T_TEXT, ID_ED_T_INK, ID_ED_T_RECT, ID_ED_T_ELLIPSE, ID_ED_IMAGE,
  ID_ED_COLOR, ID_ED_SIZE, ID_ED_FONT,
  ID_ED_FRONT, ID_ED_FWD, ID_ED_BWD, ID_ED_BACK, ID_ED_DELETE, ID_ED_PAGES, ID_ED_OBJECT, ID_ED_EDITTEXT,
  // menu items
  ID_ED_MODE0 = 630, ID_ED_MODE1, ID_ED_MODE2,
  ID_ED_PG_ROTCW = 640, ID_ED_PG_ROTCCW, ID_ED_PG_DEL, ID_ED_PG_DELRANGE, ID_ED_PG_UP, ID_ED_PG_DOWN, ID_ED_PG_BLANK,
  ID_ED_PG_INSERT, ID_ED_PG_APPEND, ID_ED_PG_EXTRACT, ID_ED_PG_SPLIT,
  ID_ED_IMG_ADD = 660, ID_ED_IMG_REMOVE,
  // object menu
  ID_ED_OB_COPY = 665, ID_ED_OB_CUT, ID_ED_OB_PASTE, ID_ED_OB_DUP, ID_ED_OB_GROUP, ID_ED_OB_UNGROUP, ID_ED_OB_SELALL, ID_ED_OB_EDITTEXT,
  ID_ED_OB_ROT_CW, ID_ED_OB_ROT_CCW, ID_ED_OB_ROT_180, ID_ED_OB_ROT_ANY,
  ID_ED_OB_AL_LEFT, ID_ED_OB_AL_HC, ID_ED_OB_AL_RIGHT, ID_ED_OB_AL_TOP, ID_ED_OB_AL_VC, ID_ED_OB_AL_BOTTOM, ID_ED_OB_DIST_H, ID_ED_OB_DIST_V,
  ID_ED_OB_OPAC = 690,          // + 0..4: 100, 75, 50, 25, other
  ID_ED_OB_BOLD = 696, ID_ED_OB_ITALIC,
  ID_ED_PG_FIT = 900,           // + 0..15: paper/orientation combinations
  ID_ED_PG_FITOPT = 920, ID_ED_PG_FITMARGIN, ID_ED_PG_FITSAVE,
  ID_ED_MENU_COLOR = 700,       // + 0..15
  ID_ED_MENU_SIZE = 720,        // + 0..15
  ID_ED_MENU_FONT = 740,        // + 0..7
  ID_ED_MENU_IMG = 760,         // + 0..63
  ID_ED_MENU_IMGDEL = 830,      // + 0..63
  ID_ED_CTX_DELETE = 890
};
inline bool IsEdCmd(int id) { return id >= ID_ED_SAVE && id < 940; }

// --- editing engine (editor.cpp). Everything runs on the UI thread; each edit is one undo step.
// Selection of an editor-made object: page objects carry a content mark "UPE" (kind, group); annotations are addressed by index.
struct ESel { int page = -1; int kind = 0; int grp = 0; int annot = -1; int sub = 0; int obj = -1; };
// kind: 0 none, 1 page objects of group `grp` (editor-made or adopted), 2 annotation, 3 an existing page object addressed by its index `obj` (becomes kind 1 on the first change)
typedef std::vector<ESel> ESet;                     // a selection: several items on one page
struct EdHit { ESel sel; FRect box; };
enum { EK_IMAGE = 1, EK_TEXT = 2, EK_PATH = 3, EK_FORM = 4 };

bool EdHitTest(Tab* t, int page, float x, float y, bool markup, EdHit* out, bool deep = false);   // deep: also page-sized objects
bool EdMarquee(Tab* t, int page, FRect r, ESet* out);                    // everything that touches the rectangle
bool EdSelBox(Tab* t, const ESel& s, FRect* box);                        // current bounds, page points, top-left origin
bool EdSetBox(Tab* t, const ESet& s, FRect* box);                        // union of the items
int EdSelObjKind(Tab* t, const ESel& s);                                 // EK_* of a page-object selection, 0 otherwise
int EdSelTextSize(Tab* t, const ESel& s);
bool EdAddMarkup(Tab* t, int which, COLORREF c);                         // 0 highlight, 1 underline, 2 strikeout, 3 squiggly; from the text selection
bool EdAddShape(Tab* t, int page, FRect r, bool ellipse, COLORREF c, int widthPt, ESel* out);
bool EdAddInk(Tab* t, int page, const std::vector<PointF>& pts, COLORREF c, int widthPt, ESel* out);
bool EdAddText(Tab* t, int page, float x, float y, const ws& text, const ws& font, int size, COLORREF c, ESel* out, int style = 0);   // style: 1 bold, 2 italic
bool EdAddImage(Tab* t, int page, FRect r, const ws& file, ESel* out);
HBITMAP EdImagePreview(Tab* t, const ESet& s, int* w, int* h);          // premultiplied BGRA DIB of a single picture object (caller deletes), or null
std::vector<ws> EdAvailableFonts();
bool EdImageSize(const ws& file, int* w, int* h);
bool EdTransformSel(Tab* t, const ESel& s, FRect oldBox, FRect newBox);
bool EdTransformSet(Tab* t, ESet& s, FRect oldBox, FRect newBox);       // moves / scales; `s` is updated (existing objects become editor objects)
bool EdZOrder(Tab* t, const ESel& s, int op);                            // 0 to front, 1 forward, 2 backward, 3 to back
bool EdZOrderSet(Tab* t, ESet& s, int op);
bool EdDeleteSel(Tab* t, const ESel& s);
bool EdDeleteSet(Tab* t, const ESet& s);
bool EdSetColorSel(Tab* t, const ESel& s, COLORREF c);
bool EdSetColorSet(Tab* t, ESet& s, COLORREF c);
bool EdSetTextSize(Tab* t, const ESel& s, int size);
bool EdSetTextSizeSet(Tab* t, ESet& s, int size);
bool EdGroup(Tab* t, ESet& s);
bool EdUngroup(Tab* t, ESet& s);
bool EdAlign(Tab* t, ESet& s, int op);                                   // 0 left, 1 centre, 2 right, 3 top, 4 middle, 5 bottom, 6 spread horizontally, 7 spread vertically
bool EdRotateSet(Tab* t, ESet& s, double deg);                           // clockwise, around the centre of the selection
bool EdSetOpacity(Tab* t, ESet& s, int percent);
int EdSetAngle(Tab* t, const ESet& s);                                   // degrees the objects were turned by the editor (0..359)
int EdSetOpacityOf(Tab* t, const ESet& s);                               // percent
bool EdCopy(Tab* t, const ESet& s);
bool EdClipHas();
bool EdPaste(Tab* t, int page, float dx, float dy, ESet* out);
// text of existing and own text objects
struct EdTextInfo { ws text; ws family; int style = 0; int size = 12; COLORREF color = 0; FRect box; int lines = 0; };
bool EdGetText(Tab* t, const ESet& s, EdTextInfo* out);                  // false when the set holds no text
bool EdExpandLine(Tab* t, const ESel& s, ESet* out);                     // a clicked piece of text -> the whole line it stands in
bool EdReplaceText(Tab* t, ESet& s, const ws& text, const ws& family, int style, int size, COLORREF c);
bool EdSetTextStyle(Tab* t, ESet& s, const ws& family, int style);       // keeps text, size and colour
// pages: every page to a standard paper size (vector, in the document)
struct FitOpts { int paper = 0; int orient = 0; float marginMm = 0; bool enlarge = true; };   // paper 0 A4, 1 A3, 2 A5, 3 Letter, 4 Legal; orient 0 by page, 1 portrait, 2 landscape
int FitDocumentPages(FPDF_DOCUMENT doc, const FitOpts& o);          // caller holds g_pdf
bool EdFitPages(Tab* t, const FitOpts& o, int* changed);
bool EdUndo(Tab* t);
bool EdRedo(Tab* t);
bool EdRotatePage(Tab* t, int page, int delta);
bool EdDeletePages(Tab* t, std::vector<int> pages);
bool EdMovePage(Tab* t, int from, int to);
bool EdBlankPage(Tab* t, int after);
bool EdInsertPages(Tab* t, int at, const std::vector<ws>& files);        // before page `at` (0-based); at == npages appends
bool EdExtractPages(Tab* t, const std::string& range, const ws& outFile);
bool ParsePageRange(const ws& s, int n, std::vector<int>* pages);       // "1-3,5,8-" -> sorted unique 0-based indices

// --- export (export.cpp): "Save as" with options
struct ExportOpts {
  int preset = 1;      // index in the dialog's preset list
  int mode = 1;        // 0 as is, 1 keep text but flatten annotations/forms, 2 turn pages into pictures
  int dpi = 200;
  int color = 1;       // 0 colour, 1 grey, 2 black and white
  int quality = 75;    // JPEG
  int maxMB = 30;      // 0 = no limit
  int paper = 0;       // 0 keep page sizes, 1 A4 by page orientation, 2 A4 portrait, 3 A4 landscape
};
struct ExportJob {
  std::atomic<int> page{0}, total{0}, usedDpi{0}, usedQ{0};
  std::atomic<bool> cancel{false}, done{false};
  std::string out;
  ws err, note;
};
bool ExportBuild(const std::string& src, const std::string& pass, const ExportOpts& o, ExportJob* job);   // worker thread
bool ExportOptionsDialog(HWND owner, ExportOpts* o, int npages);                                           // dialogs.cpp
void ProgressDialog(HWND owner, const wchar_t* title, ExportJob* job);                                      // dialogs.cpp: modal until job->done

// --- session / ui (editor_ui.cpp)
struct EdBtn { int id; RECT r; Icon ic; ws tip; bool enabled; bool checked; int kind; COLORREF col; ws label; };

int EdBarHeight();                                  // 0 when there is nothing to edit
void EdBarLayout(RECT bar);                         // positions the buttons inside `bar`
void EdBarPaint(HDC dc, RECT bar);
bool EdBarMouse(UINT msg, int x, int y);            // true when the bar consumed the event
void EdBarTips(std::vector<std::pair<RECT, ws>>* out);
void EdCommand(int id);                             // toolbar / menu command
bool EdOn();                                        // editing tools are active (mode 2)
void EdSetMode(int mode);                           // 0 viewer, 1 viewer Pro, 2 editor
void EdOnTabChanged();                              // current tab or its document changed: drop selection, cancel gestures
bool EdKey(UINT vk, bool ctrl, bool shift);         // true = handled

// mouse in the document view (client coordinates of the view)
bool EdMouseDown(Tab* t, int mx, int my, bool dbl);
bool EdMouseMove(Tab* t, int mx, int my, WPARAM wp);
bool EdMouseUp(Tab* t, int mx, int my);
bool EdSetCursor(Tab* t, int mx, int my);
void EdPaintOverlay(HDC dc, RECT rc, Tab* t, int first, int last);
void EdDoubleClickEdit();
void EdAfterTextSelection();                        // the user finished selecting text: markup tools apply at once
void EdCommitText();                                // commits the text box that is being edited (if any)
LRESULT EdCtlColor(HDC dc, HWND ctl);
bool EdActive();                                    // an editor gesture is in progress
void EdContextMenu(HMENU m, Tab* t, int mx, int my);   // extra items for the right-click menu of the document view

// document state
bool EdIsEdited(Tab* t);
bool EdConfirmClose(Tab* t);                        // asks about unsaved changes; false = cancel
bool EdConfirmAll();
bool EdSave(Tab* t);
bool EdSaveAs(Tab* t, bool fitA4 = false);
bool EdExportTest(Tab* t, const ExportOpts& o, const ws& out, bool quiet, ws* report);
void EdHandleDocReplaced(Tab* t, bool pagesChanged);  // after undo/redo or page operations: layout, sidebar, caches
bool EdTestCommand(const std::string& line);
int EdUiTest(const std::string& line);               // -1 = not a ui test command        // test scripts

// pdfcore.cpp
bool DocSerialize(Doc* d, std::string* out);
bool DocReload(Doc* d, std::string&& bytes, std::string* oldOut = nullptr);
bool SerializeHandle(FPDF_DOCUMENT h, std::string* out);
void DocClosePageCache(Doc* d);
void DocClearPageData(Doc* d);
void ToTL(FPDF_PAGE page, float w, float h, double x, double y, float* ox, float* oy);

// view.cpp helpers made available to the editor
bool ViewHitPage(Tab* t, int mx, int my, int* page, float* xPt, float* yPt, bool* exact);
RECT ViewRectToScreen(Tab* t, int p, const FRect& r);
void ViewSelOrder(const Sel& s, int* pa, int* ca, int* pb, int* cb);
void ViewPointToScreen(Tab* t, int p, float x, float y, int* sx, int* sy);
void ViewScreenToPage(Tab* t, int p, int mx, int my, float* x, float* y);   // unclamped, page points

bool InputText(HWND owner, const wchar_t* title, const ws& prompt, const ws& init, ws* out);   // dialogs.cpp
#else
inline bool EdIsEdited(Tab*) { return false; }
#endif
