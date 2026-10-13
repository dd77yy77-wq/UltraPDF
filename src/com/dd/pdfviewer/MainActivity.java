package com.dd.pdfviewer;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Paint;
import android.graphics.pdf.PdfRenderer;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.view.View;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
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
    static final String REPO = "https://github.com/dd77yy77-wq/UltraPDF";

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
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        web.setWebViewClient(new PdfClient(this));
        web.addJavascriptInterface(new Bridge(this), "Android");
        setContentView(web);
        web.loadUrl("file:///android_asset/index.html");
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
        pendingRestore = false;
        pending = i.getData();
        return true;
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

    void open(Uri u, boolean restore) {
        synchronized (lock) {
            closeDoc();
            try {
                pfd = getContentResolver().openFileDescriptor(u, "r");
                renderer = new PdfRenderer(pfd);
                pageCount = renderer.getPageCount();
                current = u;
                name = displayName(u);
                err = 0;
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_LAST, u.toString()).apply();
            } catch (Exception e) {
                closeDoc();
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

    /** Page i as JPEG (null on failure); night = inverted like CSS invert(1) hue-rotate(180deg). */
    byte[] renderJpeg(int i, int widthPx, boolean night) {
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
            if (night) {
                out = Bitmap.createBitmap(bmp.getWidth(), bmp.getHeight(), Bitmap.Config.ARGB_8888);
                Paint paint = new Paint();
                paint.setColorFilter(new ColorMatrixColorFilter(new ColorMatrix(new float[]{
                        0.574f, -1.430f, -0.144f, 0, 255,
                        -0.426f, -0.430f, -0.144f, 0, 255,
                        -0.426f, -1.430f, 0.856f, 0, 255,
                        0, 0, 0, 1, 0})));
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

        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest req) {
            Uri u = req.getUrl();
            if (!"pdf.local".equals(u.getHost())) return null;
            try {
                String seg = u.getLastPathSegment();
                int i = Integer.parseInt(seg.substring(0, seg.indexOf('.')));
                int w = Integer.parseInt(u.getQueryParameter("w"));
                boolean night = "1".equals(u.getQueryParameter("n"));
                int g = Integer.parseInt(u.getQueryParameter("g"));
                byte[] jpg = g == a.gen ? a.renderJpeg(i, w, night) : null;
                if (jpg != null) return reply(200, "OK", "image/jpeg", jpg);
            } catch (Exception ignored) {
            }
            return reply(404, "Not Found", "text/plain", new byte[0]);
        }

        private static WebResourceResponse reply(int code, String msg, String type, byte[] data) {
            Map<String, String> h = new HashMap<String, String>();
            h.put("Cache-Control", "no-store");
            h.put("Access-Control-Allow-Origin", "*");
            return new WebResourceResponse(type, null, code, msg, h, new ByteArrayInputStream(data));
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
                a.open(u, a.pendingRestore);
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
            a.runOnUiThread(new UiTask(a, 1));
        }

        @JavascriptInterface
        public void share() {
            a.runOnUiThread(new UiTask(a, 2));
        }

        @JavascriptInterface
        public void saveAs() {
            a.runOnUiThread(new UiTask(a, 3));
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
            a.runOnUiThread(new UiTask(a, 4));
        }

        @JavascriptInterface
        public void shareApp() {
            a.runOnUiThread(new UiTask(a, 5));
        }

        @JavascriptInterface
        public void github() {
            a.runOnUiThread(new UiTask(a, 6));
        }
    }

    /** Menu actions that need the UI thread. */
    static class UiTask implements Runnable {
        private final MainActivity a;
        private final int what;

        UiTask(MainActivity a, int what) {
            this.a = a;
            this.what = what;
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
                }
            } catch (Exception e) {
                a.toast(what == 2 ? "Не удалось поделиться этим файлом" : "Не удалось выполнить действие");
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
