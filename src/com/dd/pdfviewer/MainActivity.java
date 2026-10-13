package com.dd.pdfviewer;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.pdf.PdfRenderer;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;

import java.io.ByteArrayOutputStream;

/** PDF viewer: WebView UI (assets/index.html) + PdfRenderer behind the JS bridge "Android". */
public class MainActivity extends Activity {
    private static final int REQ_PICK = 1;
    private static final String PREFS = "viewer";
    private static final String KEY_LAST = "last";

    final Object lock = new Object();
    private WebView web;
    PdfRenderer renderer;
    private ParcelFileDescriptor pfd;
    volatile Uri pending;
    volatile boolean pendingRestore;
    String name = "";
    int gen;
    int err;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        web = new WebView(this);
        web.setBackgroundColor(0xFF1C1D20);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(false);
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
        if (req == REQ_PICK && res == RESULT_OK && data != null && data.getData() != null) {
            Uri u = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception ignored) {
            }
            pendingRestore = false;
            pending = u;
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

    private void closeDoc() {
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

    /** window.Android: called from the WebView's JS thread. */
    private static class Bridge {
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
            synchronized (a.lock) {
                return a.gen;
            }
        }

        @JavascriptInterface
        public int err() {
            synchronized (a.lock) {
                return a.err;
            }
        }

        @JavascriptInterface
        public String name() {
            synchronized (a.lock) {
                return a.name;
            }
        }

        @JavascriptInterface
        public int count() {
            synchronized (a.lock) {
                return a.renderer == null ? 0 : a.renderer.getPageCount();
            }
        }

        /** (widthPoints << 16) | heightPoints */
        @JavascriptInterface
        public int sz(int i) {
            synchronized (a.lock) {
                if (a.renderer == null) return 0;
                PdfRenderer.Page p = null;
                try {
                    p = a.renderer.openPage(i);
                    return (p.getWidth() << 16) | (p.getHeight() & 0xFFFF);
                } catch (Exception e) {
                    return 0;
                } finally {
                    if (p != null) p.close();
                }
            }
        }

        /** Page i rendered on white at the given pixel width, JPEG q90, base64 without wraps; null on failure. */
        @JavascriptInterface
        public String render(int i, int widthPx) {
            synchronized (a.lock) {
                if (a.renderer == null) return null;
                PdfRenderer.Page p = null;
                Bitmap bmp = null;
                try {
                    p = a.renderer.openPage(i);
                    int w = Math.max(64, Math.min(widthPx, 3000));
                    int h = Math.max(1, (int) ((long) w * p.getHeight() / Math.max(1, p.getWidth())));
                    bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    bmp.eraseColor(0xFFFFFFFF);
                    p.render(bmp, null, null, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY);
                    ByteArrayOutputStream bo = new ByteArrayOutputStream(w * h / 6);
                    bmp.compress(Bitmap.CompressFormat.JPEG, 90, bo);
                    return Base64.encodeToString(bo.toByteArray(), Base64.NO_WRAP);
                } catch (Exception | OutOfMemoryError e) {
                    return null;
                } finally {
                    if (p != null) p.close();
                    if (bmp != null) bmp.recycle();
                }
            }
        }

        @JavascriptInterface
        public void pick() {
            a.runOnUiThread(new PickRunner(a));
        }
    }

    /** Static nested classes only: d8 8.2 crashes (NPE) on non-static inner and anonymous classes. */
    private static class PickRunner implements Runnable {
        private final MainActivity a;

        PickRunner(MainActivity a) {
            this.a = a;
        }

        @Override
        public void run() {
            Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            it.addCategory(Intent.CATEGORY_OPENABLE);
            it.setType("application/pdf");
            it.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                a.startActivityForResult(it, REQ_PICK);
            } catch (Exception ignored) {
            }
        }
    }
}
