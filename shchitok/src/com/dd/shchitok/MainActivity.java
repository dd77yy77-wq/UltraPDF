package com.dd.shchitok;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import android.os.Bundle;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;

public class MainActivity extends Activity {
    private WebView web;

    /** Мост для страницы: версия приложения и ручная проверка обновлений. */
    static class Bridge {
        private final Activity a;

        Bridge(Activity a) {
            this.a = a;
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
        public void shareApk() {
            new Thread(new Runnable() {
                public void run() {
                    try {
                        File src = new File(a.getApplicationInfo().sourceDir);
                        File dst = new File(a.getCacheDir(), "Shchitok.apk");
                        InputStream in = new FileInputStream(src);
                        OutputStream out = new FileOutputStream(dst);
                        byte[] buf = new byte[65536];
                        int n;
                        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                        out.close();
                        in.close();
                        final Uri u = Uri.parse("content://" + ApkProvider.AUTH + "/Shchitok.apk");
                        a.runOnUiThread(new Runnable() {
                            public void run() {
                                try {
                                    Intent it = new Intent("android.intent.action.SEND");
                                    it.setType("application/vnd.android.package-archive");
                                    it.putExtra("android.intent.extra.STREAM", u);
                                    it.setClipData(ClipData.newRawUri("Shchitok.apk", u));
                                    it.addFlags(1); // FLAG_GRANT_READ_URI_PERMISSION
                                    a.startActivity(Intent.createChooser(it, "Поделиться приложением Щиток"));
                                } catch (Throwable e) {
                                    Toast.makeText(a, "Не удалось поделиться: " + e.getMessage(), 1).show();
                                }
                            }
                        });
                    } catch (final Throwable e) {
                        a.runOnUiThread(new Runnable() {
                            public void run() {
                                Toast.makeText(a, "Не удалось подготовить файл: " + e.getMessage(), 1).show();
                            }
                        });
                    }
                }
            }).start();
        }

        @JavascriptInterface
        public void checkUpdate() {
            Updater.check(a, true);
        }
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().setStatusBarColor(0xFF101216);
        getWindow().setNavigationBarColor(0xFF101216);
        web = new WebView(this);
        web.setBackgroundColor(0xFF101216);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setBuiltInZoomControls(false);
        s.setSupportZoom(false);
        web.addJavascriptInterface(new Bridge(this), "AndroidApp");
        setContentView(web);
        web.loadUrl("file:///android_asset/index.html");
        Updater.check(this, false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Updater.onResume(this);
    }

    @Override
    public void onBackPressed() {
        if (web != null && web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }
}
