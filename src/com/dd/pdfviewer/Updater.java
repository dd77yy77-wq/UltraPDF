package com.dd.pdfviewer;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/** Автообновление: при запуске смотрит version.json в репозитории UltraPDF и ставит новую версию APK. */
final class Updater {
    private static final String BASE = "https://raw.githubusercontent.com/dd77yy77-wq/UltraPDF/main/";
    private static boolean checked, pending;
    private static int target;

    /** Вызывается при возврате в игру: если перед этим ждали разрешения на установку — продолжаем загрузку сами. */
    static void onResume(Activity a) {
        if (pending && (Build.VERSION.SDK_INT < 26 || a.getPackageManager().canRequestPackageInstalls())) { pending = false; download(a); }
    }

    private Updater() {}

    static void check(final Activity a) { check(a, false); }

    /** manual — вызвано кнопкой в настройках: говорит о результате даже если обновления нет. */
    static void check(final Activity a, final boolean manual) {
        if (!manual) { if (checked) return; checked = true; afterUpdate(a); }
        else a.runOnUiThread(() -> Toast.makeText(a, "Проверяю обновления…", Toast.LENGTH_SHORT).show());
        new Thread(() -> {
            try {
                byte[] raw = read(BASE + "version.json?t=" + System.currentTimeMillis());
                org.json.JSONObject o = new org.json.JSONObject(new String(raw, "UTF-8"));
                final int code = o.getInt("code");
                final String name = o.optString("name", String.valueOf(code)), notes = o.optString("notes", "");
                int cur = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionCode;
                if (code > cur) {
                    target = code;
                    android.content.SharedPreferences sp = a.getSharedPreferences("viewer", Context.MODE_PRIVATE);
                    final String warn = sp.getInt("upd_try", 0) == code
                            ? "\n\nПрошлая установка этой версии не завершилась. В окне установщика нажмите «Обновить» и дождитесь конца."
                            : "";
                    a.runOnUiThread(() -> ask(a, name, notes + warn));
                }
                else if (manual) a.runOnUiThread(() -> Toast.makeText(a, "Установлена последняя версия", Toast.LENGTH_LONG).show());
            } catch (Throwable e) {
                if (manual) a.runOnUiThread(() -> Toast.makeText(a, "Не удалось проверить: нет связи с сервером", Toast.LENGTH_LONG).show());
            }
        }).start();
    }

    /** First start after an install: confirm it, drop the downloaded file and forget the pending attempt. */
    private static void afterUpdate(Activity a) {
        try {
            android.content.pm.PackageInfo pi = a.getPackageManager().getPackageInfo(a.getPackageName(), 0);
            android.content.SharedPreferences sp = a.getSharedPreferences("viewer", Context.MODE_PRIVATE);
            int t = sp.getInt("upd_try", 0);
            File f = new File(a.getCacheDir(), "update.apk");
            if (f.exists() && t <= pi.versionCode) f.delete();
            if (t != 0 && t <= pi.versionCode) {
                sp.edit().remove("upd_try").apply();
                if (t == pi.versionCode) Toast.makeText(a, "Обновлено до версии " + pi.versionName, Toast.LENGTH_LONG).show();
            }
        } catch (Throwable ignored) {
        }
    }

    private static byte[] read(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(8000); c.setReadTimeout(10000);
        InputStream in = c.getInputStream();
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        byte[] b = new byte[4096]; int n;
        while ((n = in.read(b)) > 0) bo.write(b, 0, n);
        in.close();
        return bo.toByteArray();
    }

    private static void ask(final Activity a, String name, String notes) {
        if (a.isFinishing()) return;
        new AlertDialog.Builder(a, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle("Доступна версия " + name)
                .setMessage((notes.length() > 0 ? notes + "\n\n" : "") + "Скачать и установить обновление? Данные сохранятся.")
                .setPositiveButton("Обновить", (d, w) -> download(a))
                .setNegativeButton("Позже", null)
                .show();
    }

    private static void download(final Activity a) {
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) {
            Toast.makeText(a, "Разрешите установку из этого источника и вернитесь назад: обновление скачается само", Toast.LENGTH_LONG).show();
            pending = true;
            a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName())));
            return;
        }
        pending = false;
        a.getSharedPreferences("viewer", Context.MODE_PRIVATE).edit().putInt("upd_try", target).apply();
        final AlertDialog dlg = new AlertDialog.Builder(a, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle("Скачивание обновления").setMessage("0%").setCancelable(false).show();
        new Thread(() -> {
            try {
                File f = new File(a.getCacheDir(), "update.apk");
                HttpURLConnection c = (HttpURLConnection) new URL(BASE + "UltraPDF.apk?t=" + System.currentTimeMillis()).openConnection();
                c.setConnectTimeout(10000); c.setReadTimeout(20000);
                final int total = c.getContentLength();
                InputStream in = c.getInputStream();
                OutputStream out = new FileOutputStream(f);
                byte[] b = new byte[16384]; int n, got = 0, last = -1;
                while ((n = in.read(b)) > 0) {
                    out.write(b, 0, n); got += n;
                    final int pct = total > 0 ? got * 100 / total : 0;
                    if (pct != last) { last = pct; a.runOnUiThread(() -> dlg.setMessage(pct + "%")); }
                }
                out.close(); in.close();
                if (total > 0 && got != total) throw new Exception("incomplete");
                a.runOnUiThread(() -> { dlg.dismiss(); install(a); });
            } catch (Throwable e) {
                a.runOnUiThread(() -> { dlg.dismiss(); Toast.makeText(a, "Не удалось скачать обновление: " + e, Toast.LENGTH_LONG).show(); });
                checked = false;
            }
        }).start();
    }

    /** Hands the downloaded APK to the system installer (the same route file managers use). */
    private static void install(final Activity a) {
        Uri u = Uri.parse("content://" + ApkProvider.AUTH + "/update.apk");
        Intent it = new Intent(Intent.ACTION_VIEW);
        it.setDataAndType(u, "application/vnd.android.package-archive");
        it.setClipData(android.content.ClipData.newRawUri("update.apk", u));
        it.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            a.startActivity(it);
        } catch (Throwable e) {
            Toast.makeText(a, "Не удалось открыть установщик: " + e.getMessage(), Toast.LENGTH_LONG).show();
            checked = false;
        }
    }
}
