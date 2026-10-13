package com.dd.pdfviewer;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Paint;
import android.graphics.pdf.PdfRenderer;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintManager;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.ValueCallback;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * UltraPDF: WebView UI (assets/index.html) + PdfRenderer.
 * Pages are not pushed through the JS bridge: the page loads https://pdf.local/p/N.jpg and PdfClient
 * answers those requests natively on WebView's own threads, so scrolling never waits for rendering.
 * NOTE: only static nested classes here: d8 8.2 crashes (NPE) on non-static inner and anonymous classes.
 */
public class MainActivity extends Activity {
    static final int REQ_PICK = 1;
    static final int REQ_SAVE = 2;
    static final String PREFS = "viewer";
    static final String KEY_LAST = "last";
    static final String KEY_HIST = "hist";
    static final int HIST_MAX = 10;
    static final String REPO = "https://github.com/dd77yy77-wq/UltraPDF";

    final Object histLock = new Object();
    final Object lock = new Object();      // guards renderer (PdfRenderer allows one open page at a time)
    WebView web;
    PdfRenderer renderer;
    ParcelFileDescriptor pfd;
    volatile Uri pending;
    volatile boolean pendingRestore;
    volatile Uri current;
    volatile String name = "";
    volatile int gen;
    volatile int err;
    volatile int pageCount;
    volatile Uri pwdUri;                 // document waiting for a password
    volatile String pendingPassword;
    volatile boolean fullscreen;
    volatile boolean light;
    volatile int brightness;             // 0 = system default, else percent

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        web = new WebView(this);
        web.setBackgroundColor(0xFF1C1D20);
        web.setOverScrollMode(View.OVER_SCROLL_NEVER);
        web.setLongClickable(false);
        web.setHapticFeedbackEnabled(false);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        web.setWebViewClient(new PdfClient(this));
        web.addJavascriptInterface(new Bridge(this), "Android");
        setContentView(web);
        web.loadUrl("https://pdf.local/app/index.html");
        Updater.check(this);
        if (!handle(getIntent())) {
            // plain launch: reopen the last document, if it is still readable
            String last = getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_LAST, null);
            if (last != null) {
                pendingRestore = true; // set first: the bridge thread reads pending, then this flag
                pending = Uri.parse(last);
            }
        }
    }

    /** Back closes menus/dialogs/full screen first (the page decides), then leaves the app. */
    @Override
    public void onBackPressed() {
        if (web == null) {
            super.onBackPressed();
            return;
        }
        web.evaluateJavascript("(function(){return window.onBack?window.onBack():false})()", new BackCallback(this));
    }

    void leave() {
        super.onBackPressed();
    }

    @Override
    protected void onResume() {
        super.onResume();
        Updater.onResume(this);
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        handle(i);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        if (req == REQ_PICK) {
            try {
                getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception ignored) {
            }
            pendingRestore = false;
            pending = u;
        } else if (req == REQ_SAVE && current != null) {
            new Thread(new SaveTask(this, current, u)).start();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (web != null) {
            web.removeJavascriptInterface("Android");
            web.destroy();
        }
        closeDoc();
    }

    private boolean handle(Intent i) {
        if (i == null || i.getData() == null) return false;
        try {
            getContentResolver().takePersistableUriPermission(i.getData(), Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {   // not offered by this provider: the history entry may stop working later
        }
        pendingRestore = false;
        pending = i.getData();
        return true;
    }

    /** Full screen (system bars hidden), light/dark bars and brightness: one place computes them all. */
    void applySystemUi() {
        Window w = getWindow();
        View d = w.getDecorView();
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.screenBrightness = brightness <= 0 ? WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE : Math.max(0.02f, brightness / 100f);
        if (Build.VERSION.SDK_INT >= 28) {
            lp.layoutInDisplayCutoutMode = !fullscreen ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT
                    : Build.VERSION.SDK_INT >= 30 ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        w.setAttributes(lp);
        // Without this the system keeps the page out of the camera cut-out area even when the bars are hidden.
        if (Build.VERSION.SDK_INT >= 30) w.setDecorFitsSystemWindows(!fullscreen);
        w.setStatusBarColor(light ? 0xFFFFFFFF : 0xFF2A2C31);
        w.setNavigationBarColor(light ? 0xFFECEEF2 : 0xFF1C1D20);
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = d.getWindowInsetsController();
            if (c != null) {
                if (fullscreen) {
                    c.hide(WindowInsets.Type.systemBars());
                    c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                } else {
                    c.show(WindowInsets.Type.systemBars());
                }
                int m = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                c.setSystemBarsAppearance(light ? m : 0, m);
            }
        } else {
            int f = View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
            if (fullscreen) {
                f |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;
            } else if (light) {
                if (Build.VERSION.SDK_INT >= 23) f |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
                if (Build.VERSION.SDK_INT >= 26) f |= View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
            }
            d.setSystemUiVisibility(f);
        }
    }

    void toast(String msg) {
        runOnUiThread(new ToastTask(this, msg));
    }

    void closeDoc() {
        synchronized (lock) {
            try {
                if (renderer != null) renderer.close();
            } catch (Exception ignored) {
            }
            try {
                if (pfd != null) pfd.close();
            } catch (Exception ignored) {
            }
            renderer = null;
            pfd = null;
            pageCount = 0;
        }
    }

    private String displayName(Uri u) {
        Cursor c = null;
        try {
            c = getContentResolver().query(u, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null && n.length() > 0) return n;
            }
        } catch (Exception ignored) {
        } finally {
            if (c != null) c.close();
        }
        String n = u.getLastPathSegment();
        return n == null ? "" : n;
    }

    void open(Uri u, boolean restore, String password) {
        synchronized (lock) {
            closeDoc();
            try {
                pfd = getContentResolver().openFileDescriptor(u, "r");
                renderer = password == null ? new PdfRenderer(pfd) : withPassword(pfd, password);
                pageCount = renderer.getPageCount();
                current = u;
                pwdUri = null;
                name = displayName(u);
                err = 0;
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_LAST, u.toString()).apply();
                addHistory(u, name);
            } catch (Throwable e) {
                closeDoc();
                Throwable c = e instanceof InvocationTargetException && e.getCause() != null ? e.getCause() : e;
                if (c instanceof SecurityException) {
                    // password protected (or a wrong password)
                    pwdUri = u;
                    current = u;
                    name = displayName(u);
                    err = password == null ? 2 : 3;
                    gen++;
                    return;
                }
                removeHistory(u.toString());
                if (restore) {
                    // silent: the old file is gone or access was revoked; show the empty start screen
                    getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(KEY_LAST).apply();
                    return;
                }
                err = 1;
            }
            gen++;
        }
    }

    /** Android 15+: new PdfRenderer(pfd, new LoadParams.Builder().setPassword(pw).build()), by reflection. */
    private static PdfRenderer withPassword(ParcelFileDescriptor pfd, String pw) throws Exception {
        Class<?> lp = Class.forName("android.graphics.pdf.LoadParams");
        Class<?> bld = Class.forName("android.graphics.pdf.LoadParams$Builder");
        Object b = bld.getConstructor().newInstance();
        bld.getMethod("setPassword", String.class).invoke(b, pw);
        Object params = bld.getMethod("build").invoke(b);
        return (PdfRenderer) PdfRenderer.class.getConstructor(ParcelFileDescriptor.class, lp).newInstance(pfd, params);
    }

    // ---- history of the last HIST_MAX documents: [{u:uri, n:name, t:millis}], newest first
    private JSONArray loadHist() {
        try {
            return new JSONArray(getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_HIST, "[]"));
        } catch (Exception e) {
            return new JSONArray();
        }
    }

    private void saveHist(JSONArray a) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_HIST, a.toString()).apply();
    }

    private void release(String uri) {
        try {
            getContentResolver().releasePersistableUriPermission(Uri.parse(uri), Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }
    }

    void addHistory(Uri u, String n) {
        synchronized (histLock) {
            try {
                JSONArray old = loadHist();
                JSONArray a = new JSONArray();
                JSONObject me = new JSONObject();
                me.put("u", u.toString());
                me.put("n", n);
                me.put("t", System.currentTimeMillis());
                a.put(me);
                for (int k = 0; k < old.length(); k++) {
                    JSONObject o = old.getJSONObject(k);
                    if (o.getString("u").equals(u.toString())) continue;
                    if (a.length() < HIST_MAX) a.put(o);
                    else release(o.getString("u"));
                }
                saveHist(a);
            } catch (Exception ignored) {
            }
        }
    }

    void removeHistory(String uri) {
        synchronized (histLock) {
            try {
                JSONArray old = loadHist();
                JSONArray a = new JSONArray();
                for (int k = 0; k < old.length(); k++) {
                    JSONObject o = old.getJSONObject(k);
                    if (!o.getString("u").equals(uri)) a.put(o);
                }
                saveHist(a);
            } catch (Exception ignored) {
            }
        }
    }

    String historyJson() {
        synchronized (histLock) {
            try {
                JSONArray old = loadHist();
                JSONArray a = new JSONArray();
                for (int k = 0; k < old.length(); k++) {
                    JSONObject o = old.getJSONObject(k);
                    JSONObject r = new JSONObject();
                    r.put("n", o.optString("n", ""));
                    r.put("t", o.optLong("t", 0));
                    a.put(r);
                }
                return a.toString();
            } catch (Exception e) {
                return "[]";
            }
        }
    }

    void openHistory(int idx) {
        synchronized (histLock) {
            try {
                JSONArray a = loadHist();
                if (idx < 0 || idx >= a.length()) return;
                pendingRestore = false;
                pending = Uri.parse(a.getJSONObject(idx).getString("u"));
            } catch (Exception ignored) {
            }
        }
    }

    void clearHistory() {
        synchronized (histLock) {
            JSONArray a = loadHist();
            for (int k = 0; k < a.length(); k++) {
                try {
                    release(a.getJSONObject(k).getString("u"));
                } catch (Exception ignored) {
                }
            }
            saveHist(new JSONArray());
        }
    }

    // mode 1 = night: inverted like CSS invert(1) hue-rotate(180deg); mode 2 = sepia: grey tinted like warm paper
    static final float[] NIGHT = {
            0.574f, -1.430f, -0.144f, 0, 255,
            -0.426f, -0.430f, -0.144f, 0, 255,
            -0.426f, -1.430f, 0.856f, 0, 255,
            0, 0, 0, 1, 0};
    static final float[] SEPIA = {
            0.299f, 0.587f, 0.114f, 0, 0,
            0.278f, 0.546f, 0.106f, 0, 0,
            0.233f, 0.458f, 0.089f, 0, 0,
            0, 0, 0, 1, 0};

    /** Page i as JPEG (null on failure). */
    byte[] renderJpeg(int i, int widthPx, int mode) {
        Bitmap bmp = null;
        Bitmap out = null;
        try {
            synchronized (lock) {
                if (renderer == null || i < 0 || i >= pageCount) return null;
                PdfRenderer.Page p = renderer.openPage(i);
                try {
                    int w = Math.max(64, Math.min(widthPx, 3000));
                    int h = Math.max(1, (int) ((long) w * p.getHeight() / Math.max(1, p.getWidth())));
                    bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    bmp.eraseColor(0xFFFFFFFF);
                    p.render(bmp, null, null, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY);
                } finally {
                    p.close();
                }
            }
            // everything below runs outside the lock, so the next page can already be rendering
            out = bmp;
            if (mode == 1 || mode == 2) {
                out = Bitmap.createBitmap(bmp.getWidth(), bmp.getHeight(), Bitmap.Config.ARGB_8888);
                Paint paint = new Paint();
                paint.setColorFilter(new ColorMatrixColorFilter(new ColorMatrix(mode == 1 ? NIGHT : SEPIA)));
                new Canvas(out).drawBitmap(bmp, 0, 0, paint);
                bmp.recycle();
                bmp = null;
            }
            ByteArrayOutputStream bo = new ByteArrayOutputStream(out.getWidth() * out.getHeight() / 8);
            out.compress(Bitmap.CompressFormat.JPEG, 85, bo);
            return bo.toByteArray();
        } catch (Exception e) {
            return null;
        } catch (OutOfMemoryError e) {
            return null;
        } finally {
            if (bmp != null) bmp.recycle();
            if (out != null && out != bmp) out.recycle();
        }
    }

    /** Text search on one page (Android 15+ PdfRenderer API, called by reflection). "!" = not supported. */
    String searchPage(int i, String q) {
        synchronized (lock) {
            if (renderer == null || i < 0 || i >= pageCount) return "";
            PdfRenderer.Page p = null;
            try {
                p = renderer.openPage(i);
                Method m;
                try {
                    m = p.getClass().getMethod("searchText", String.class);
                } catch (NoSuchMethodException e) {
                    return "!";
                }
                Object res = m.invoke(p, q);
                if (!(res instanceof List)) return "";
                StringBuilder sb = new StringBuilder();
                for (Object match : (List<?>) res) {
                    Object rects = match.getClass().getMethod("getBounds").invoke(match);
                    StringBuilder one = new StringBuilder();
                    for (Object r : (List<?>) rects) {
                        if (one.length() > 0) one.append(';');
                        one.append(num(r, "left")).append(',').append(num(r, "top")).append(',')
                                .append(num(r, "right")).append(',').append(num(r, "bottom"));
                    }
                    if (one.length() == 0) continue;
                    if (sb.length() > 0) sb.append('|');
                    sb.append(one);
                }
                return sb.toString();
            } catch (Throwable e) {
                return "!";
            } finally {
                if (p != null) p.close();
            }
        }
    }

    private static float num(Object o, String field) throws Exception {
        return ((Number) o.getClass().getField(field).get(o)).floatValue();
    }

    /** "w,h;w,h;..." for n pages starting at from (page size in points). */
    String sizes(int from, int n) {
        synchronized (lock) {
            StringBuilder sb = new StringBuilder();
            for (int i = from; i < from + n && i < pageCount; i++) {
                PdfRenderer.Page p = null;
                try {
                    p = renderer.openPage(i);
                    sb.append(p.getWidth()).append(',').append(p.getHeight());
                } catch (Exception e) {
                    sb.append("0,0");
                } finally {
                    if (p != null) p.close();
                }
                sb.append(';');
            }
            return sb.toString();
        }
    }

    /** Answers https://pdf.local/p/N.jpg?w=..&n=..&g=.. with a rendered page. */
    static class PdfClient extends WebViewClient {
        private final MainActivity a;

        PdfClient(MainActivity a) {
            this.a = a;
        }

        // the page never navigates anywhere: external links go through Bridge.openUrl
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest req) {
            return true;
        }

        @Override
        public boolean shouldOverrideUrlLoading(WebView view, String url) {
            return true;
        }

        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest req) {
            Uri u = req.getUrl();
            if (!"pdf.local".equals(u.getHost())) return null;
            String path = u.getPath() == null ? "" : u.getPath();
            try {
                if (path.startsWith("/app/")) return asset(path.substring(5));
                if (path.startsWith("/doc/")) return doc(req, u);
                if (path.startsWith("/p/")) {
                    String seg = u.getLastPathSegment();
                    int i = Integer.parseInt(seg.substring(0, seg.indexOf('.')));
                    int w = Integer.parseInt(u.getQueryParameter("w"));
                    int mode = Integer.parseInt(u.getQueryParameter("n"));
                    int g = Integer.parseInt(u.getQueryParameter("g"));
                    byte[] jpg = g == a.gen ? a.renderJpeg(i, w, mode) : null;
                    if (jpg != null) return reply(200, "OK", "image/jpeg", jpg);
                }
            } catch (Exception ignored) {
            }
            return reply(404, "Not Found", "text/plain", new byte[0]);
        }

        /** The app itself (index.html, pdf.js) is served from assets under https://pdf.local/app/. */
        private WebResourceResponse asset(String name) throws IOException {
            if (name.length() == 0 || name.contains("..") || !name.matches("[A-Za-z0-9._/-]+")) {
                return reply(404, "Not Found", "text/plain", new byte[0]);
            }
            String type = "application/octet-stream";
            if (name.endsWith(".html")) type = "text/html";
            else if (name.endsWith(".js")) type = "application/javascript";
            else if (name.endsWith(".css")) type = "text/css";
            else if (name.endsWith(".png")) type = "image/png";
            else if (name.endsWith(".json")) type = "application/json";
            InputStream in;
            try {
                in = a.getAssets().open(name);
            } catch (IOException e) {
                return reply(404, "Not Found", "text/plain", new byte[0]);
            }
            Map<String, String> h = new HashMap<String, String>();
            h.put("Cache-Control", "no-cache");
            return new WebResourceResponse(type, type.startsWith("text/") || type.endsWith("javascript") ? "utf-8" : null,
                    200, "OK", h, in);
        }

        /** The open document for pdf.js (outline, links, text): streamed from the provider, with Range support. */
        private WebResourceResponse doc(WebResourceRequest req, Uri u) throws Exception {
            Uri cur = a.current;
            String gq = u.getQueryParameter("g");
            if (cur == null || gq == null || Integer.parseInt(gq) != a.gen) {
                return reply(404, "Not Found", "text/plain", new byte[0]);
            }
            ParcelFileDescriptor p = a.getContentResolver().openFileDescriptor(cur, "r");
            try {
                long size = p.getStatSize();
                FileInputStream fis = new ParcelFileDescriptor.AutoCloseInputStream(p);
                String range = req.getRequestHeaders().get("Range");
                Map<String, String> h = new HashMap<String, String>();
                h.put("Cache-Control", "no-store");
                h.put("Accept-Ranges", "bytes");
                if (size > 0 && range != null && range.startsWith("bytes=")) {
                    String[] se = range.substring(6).split("-", -1);
                    long start;
                    long end;
                    if (se[0].length() == 0 && se.length > 1 && se[1].length() > 0) {   // suffix range: the last n bytes
                        start = Math.max(0, size - Long.parseLong(se[1]));
                        end = size - 1;
                    } else {
                        start = se[0].length() == 0 ? 0 : Long.parseLong(se[0]);
                        end = se.length < 2 || se[1].length() == 0 ? size - 1 : Math.min(Long.parseLong(se[1]), size - 1);
                    }
                    if (start > end || start >= size) {
                        fis.close();
                        h.put("Content-Range", "bytes */" + size);
                        return new WebResourceResponse("application/pdf", null, 416, "Range Not Satisfiable", h,
                                new ByteArrayInputStream(new byte[0]));
                    }
                    fis.getChannel().position(start);
                    h.put("Content-Range", "bytes " + start + "-" + end + "/" + size);
                    h.put("Content-Length", String.valueOf(end - start + 1));
                    return new WebResourceResponse("application/pdf", null, 206, "Partial Content", h,
                            new LimitedStream(fis, end - start + 1));
                }
                if (size > 0) h.put("Content-Length", String.valueOf(size));
                return new WebResourceResponse("application/pdf", null, 200, "OK", h, fis);
            } catch (Exception e) {
                try {
                    p.close();
                } catch (Exception ignored) {
                }
                throw e;
            }
        }

        private static WebResourceResponse reply(int code, String msg, String type, byte[] data) {
            Map<String, String> h = new HashMap<String, String>();
            h.put("Cache-Control", "no-store");
            return new WebResourceResponse(type, null, code, msg, h, new ByteArrayInputStream(data));
        }
    }

    /** Reads at most n bytes of the wrapped stream (a Range response). */
    static class LimitedStream extends FilterInputStream {
        private long left;

        LimitedStream(InputStream in, long n) {
            super(in);
            left = n;
        }

        @Override
        public int read() throws IOException {
            if (left <= 0) return -1;
            int r = super.read();
            if (r >= 0) left--;
            return r;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            if (left <= 0) return -1;
            int r = super.read(b, off, (int) Math.min(len, left));
            if (r > 0) left -= r;
            return r;
        }
    }

    // raw ValueCallback on purpose: a generic one makes javac emit a bridge method that d8 8.2 cannot handle
    @SuppressWarnings({"rawtypes", "unchecked"})
    static class BackCallback implements ValueCallback {
        private final MainActivity a;

        BackCallback(MainActivity a) {
            this.a = a;
        }

        @Override
        public void onReceiveValue(Object v) {
            if (!"true".equals(String.valueOf(v))) a.leave();
        }
    }

    /** window.Android, called from the WebView's JS thread: nothing here may wait for a page render. */
    static class Bridge {
        private final MainActivity a;

        Bridge(MainActivity a) {
            this.a = a;
        }

        @JavascriptInterface
        public int gen() {
            Uri u = a.pending;
            if (u != null) {
                a.pending = null;
                String pw = a.pendingPassword;
                a.pendingPassword = null;
                a.open(u, a.pendingRestore, pw);
                a.pendingRestore = false;
            }
            return a.gen;
        }

        @JavascriptInterface
        public int err() {
            return a.err;
        }

        @JavascriptInterface
        public String name() {
            return a.name;
        }

        @JavascriptInterface
        public int count() {
            return a.pageCount;
        }

        @JavascriptInterface
        public String szs(int from, int n) {
            return a.sizes(from, n);
        }

        @JavascriptInterface
        public String searchPage(int i, String q) {
            return a.searchPage(i, q);
        }

        @JavascriptInterface
        public void pick() {
            a.runOnUiThread(new UiTask(a, 1, 0, null));
        }

        @JavascriptInterface
        public void share() {
            a.runOnUiThread(new UiTask(a, 2, 0, null));
        }

        @JavascriptInterface
        public void saveAs() {
            a.runOnUiThread(new UiTask(a, 3, 0, null));
        }

        @JavascriptInterface
        public void openWithPassword(String pw) {
            Uri u = a.pwdUri;
            if (u == null) return;
            a.pendingRestore = false;
            a.pendingPassword = pw;
            a.pending = u;
        }

        @JavascriptInterface
        public void fullscreen(boolean on) {
            a.fullscreen = on;
            a.runOnUiThread(new UiTask(a, 8, 0, null));
        }

        /** Interface theme: light/dark system bars. */
        @JavascriptInterface
        public void theme(boolean light) {
            a.light = light;
            a.runOnUiThread(new UiTask(a, 8, 0, null));
        }

        /** Screen brightness in percent; 0 = system default. */
        @JavascriptInterface
        public void brightness(int pct) {
            a.brightness = pct;
            a.runOnUiThread(new UiTask(a, 8, 0, null));
        }

        @JavascriptInterface
        public void copy(String text) {
            a.runOnUiThread(new UiTask(a, 9, 0, text));
        }

        @JavascriptInterface
        public void openUrl(String url) {
            a.runOnUiThread(new UiTask(a, 10, 0, url));
        }

        @JavascriptInterface
        public void print() {
            a.runOnUiThread(new UiTask(a, 7, 0, null));
        }

        @JavascriptInterface
        public String history() {
            return a.historyJson();
        }

        @JavascriptInterface
        public void openHistory(int idx) {
            a.openHistory(idx);
        }

        @JavascriptInterface
        public void clearHistory() {
            a.clearHistory();
        }

        @JavascriptInterface
        public String version() {
            try {
                return a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionName;
            } catch (Exception e) {
                return "";
            }
        }

        @JavascriptInterface
        public void checkUpdate() {
            a.runOnUiThread(new UiTask(a, 4, 0, null));
        }

        @JavascriptInterface
        public void shareApp() {
            a.runOnUiThread(new UiTask(a, 5, 0, null));
        }

        @JavascriptInterface
        public void github() {
            a.runOnUiThread(new UiTask(a, 6, 0, null));
        }
    }

    /** Menu actions that need the UI thread. */
    static class UiTask implements Runnable {
        private final MainActivity a;
        private final int what;
        private final int arg;
        private final String str;

        UiTask(MainActivity a, int what, int arg, String str) {
            this.a = a;
            this.what = what;
            this.arg = arg;
            this.str = str;
        }

        @Override
        public void run() {
            try {
                if (what == 1) {
                    Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    it.addCategory(Intent.CATEGORY_OPENABLE);
                    it.setType("application/pdf");
                    it.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    a.startActivityForResult(it, REQ_PICK);
                } else if (what == 2) {
                    Uri u = a.current;
                    if (u == null) return;
                    Intent send = new Intent(Intent.ACTION_SEND);
                    send.setType("application/pdf");
                    send.putExtra(Intent.EXTRA_STREAM, u);
                    send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    a.startActivity(Intent.createChooser(send, "Поделиться"));
                } else if (what == 3) {
                    if (a.current == null) return;
                    String n = a.name == null || a.name.length() == 0 ? "document.pdf" : a.name;
                    if (!n.toLowerCase().endsWith(".pdf")) n = n + ".pdf";
                    Intent it = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    it.addCategory(Intent.CATEGORY_OPENABLE);
                    it.setType("application/pdf");
                    it.putExtra(Intent.EXTRA_TITLE, n);
                    a.startActivityForResult(it, REQ_SAVE);
                } else if (what == 4) {
                    Updater.check(a, true);
                } else if (what == 5) {
                    Intent send = new Intent(Intent.ACTION_SEND);
                    send.setType("text/plain");
                    send.putExtra(Intent.EXTRA_SUBJECT, "UltraPDF");
                    send.putExtra(Intent.EXTRA_TEXT, "UltraPDF: читалка PDF для Android\n" + REPO
                            + "\nAPK: " + REPO + "/raw/main/UltraPDF.apk");
                    a.startActivity(Intent.createChooser(send, "Поделиться приложением"));
                } else if (what == 6) {
                    a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(REPO)));
                } else if (what == 7) {
                    if (a.current == null) return;
                    String n = a.name == null || a.name.length() == 0 ? "document" : a.name;
                    PrintManager pm = (PrintManager) a.getSystemService(Context.PRINT_SERVICE);
                    pm.print(n, new PrintAdapter(a, a.current, n), null);
                } else if (what == 8) {
                    a.applySystemUi();
                } else if (what == 9) {
                    ClipboardManager cm = (ClipboardManager) a.getSystemService(Context.CLIPBOARD_SERVICE);
                    cm.setPrimaryClip(ClipData.newPlainText("UltraPDF", str));
                    Toast.makeText(a, "Скопировано", Toast.LENGTH_SHORT).show();
                } else if (what == 10) {
                    String s = str == null ? "" : str.trim();
                    String low = s.toLowerCase();
                    if (low.startsWith("http://") || low.startsWith("https://") || low.startsWith("mailto:") || low.startsWith("tel:")) {
                        a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(s)));
                    }
                }
            } catch (Exception e) {
                a.toast(what == 2 ? "Не удалось поделиться этим файлом" : "Не удалось выполнить действие");
            }
        }
    }

    /** Print: hands the original PDF to the system print service. */
    static class PrintAdapter extends PrintDocumentAdapter {
        private final MainActivity a;
        private final Uri src;
        private final String title;

        PrintAdapter(MainActivity a, Uri src, String title) {
            this.a = a;
            this.src = src;
            this.title = title;
        }

        @Override
        public void onLayout(PrintAttributes oldAttrs, PrintAttributes newAttrs, CancellationSignal cancel,
                             LayoutResultCallback cb, Bundle extras) {
            if (cancel.isCanceled()) {
                cb.onLayoutCancelled();
                return;
            }
            cb.onLayoutFinished(new PrintDocumentInfo.Builder(title.endsWith(".pdf") ? title : title + ".pdf")
                    .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT)
                    .setPageCount(PrintDocumentInfo.PAGE_COUNT_UNKNOWN).build(), true);
        }

        @Override
        public void onWrite(PageRange[] pages, ParcelFileDescriptor dest, CancellationSignal cancel, WriteResultCallback cb) {
            InputStream in = null;
            OutputStream out = null;
            try {
                in = a.getContentResolver().openInputStream(src);
                out = new FileOutputStream(dest.getFileDescriptor());
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) {
                    if (cancel.isCanceled()) {
                        cb.onWriteCancelled();
                        return;
                    }
                    out.write(buf, 0, n);
                }
                out.flush();
                cb.onWriteFinished(new PageRange[]{PageRange.ALL_PAGES});
            } catch (Exception e) {
                cb.onWriteFailed(e.getMessage());
            } finally {
                try {
                    if (in != null) in.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    /** "Save as": copies the open document into the file the user picked. */
    static class SaveTask implements Runnable {
        private final MainActivity a;
        private final Uri src;
        private final Uri dst;

        SaveTask(MainActivity a, Uri src, Uri dst) {
            this.a = a;
            this.src = src;
            this.dst = dst;
        }

        @Override
        public void run() {
            InputStream in = null;
            OutputStream out = null;
            try {
                in = a.getContentResolver().openInputStream(src);
                out = a.getContentResolver().openOutputStream(dst, "w");
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                out.flush();
                a.toast("Сохранено");
            } catch (Exception e) {
                a.toast("Не удалось сохранить файл");
            } finally {
                try {
                    if (in != null) in.close();
                } catch (Exception ignored) {
                }
                try {
                    if (out != null) out.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    static class ToastTask implements Runnable {
        private final MainActivity a;
        private final String msg;

        ToastTask(MainActivity a, String msg) {
            this.a = a;
            this.msg = msg;
        }

        @Override
        public void run() {
            Toast.makeText(a, msg, Toast.LENGTH_LONG).show();
        }
    }
}
