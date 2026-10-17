package com.dd.shchitok;

import android.app.Activity;
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
