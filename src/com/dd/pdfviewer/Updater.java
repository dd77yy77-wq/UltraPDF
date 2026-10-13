package com.dd.pdfviewer;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/** Автообновление: при запуске смотрит version.json в репозитории UltraPDF и ставит новую версию APK. */
final class Updater {
    private static final String BASE = "https://raw.githubusercontent.com/dd77yy77-wq/UltraPDF/main/";
    private static boolean checked, pending;

    /** Вызывается при возврате в игру: если перед этим ждали разрешения на установку — продолжаем загрузку сами. */
    static void onResume(Activity a) {
        if (pending && (Build.VERSION.SDK_INT < 26 || a.getPackageManager().canRequestPackageInstalls())) { pending = false; download(a); }
    }

    private Updater() {}

    static void check(final Activity a) { check(a, false); }

    /** manual — вызвано кнопкой в настройках: говорит о результате даже если обновления нет. */
    static void check(final Activity a, final boolean manual) {
        if (!manual) { if (checked) return; checked = true; }
        else a.runOnUiThread(() -> Toast.makeText(a, "Проверяю обновления…", Toast.LENGTH_SHORT).show());
        new Thread(() -> {
            try {
                byte[] raw = read(BASE + "version.json?t=" + System.currentTimeMillis());
                org.json.JSONObject o = new org.json.JSONObject(new String(raw, "UTF-8"));
                final int code = o.getInt("code");
                final String name = o.optString("name", String.valueOf(code)), notes = o.optString("notes", "");
                int cur = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionCode;
                if (code > cur) a.runOnUiThread(() -> ask(a, name, notes));
                else if (manual) a.runOnUiThread(() -> Toast.makeText(a, "Установлена последняя версия", Toast.LENGTH_LONG).show());
            } catch (Throwable e) {
                if (manual) a.runOnUiThread(() -> Toast.makeText(a, "Не удалось проверить: нет связи с сервером", Toast.LENGTH_LONG).show());
            }
        }).start();
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
                a.runOnUiThread(() -> { dlg.dismiss(); });
                install(a, f);
            } catch (Throwable e) {
                a.runOnUiThread(() -> { dlg.dismiss(); Toast.makeText(a, "Не удалось скачать обновление", Toast.LENGTH_LONG).show(); });
                checked = false;
            }
        }).start();
    }

    private static void install(final Activity a, File f) throws Exception {
        final Context app = a.getApplicationContext();
        PackageInstaller pi = a.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        int id = pi.createSession(sp);
        PackageInstaller.Session s = pi.openSession(id);
        OutputStream os = s.openWrite("app", 0, f.length());
        InputStream in = new FileInputStream(f);
        byte[] b = new byte[16384]; int n;
        while ((n = in.read(b)) > 0) os.write(b, 0, n);
        in.close();
        s.fsync(os);
        os.close();
        final String action = app.getPackageName() + ".UPDATE_DONE";
        BroadcastReceiver r = new BroadcastReceiver() {
            @Override public void onReceive(Context ctx, Intent i) {
                int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, -1);
                if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent ui = (Intent) i.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (ui != null) { ui.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); ctx.startActivity(ui); }
                } else if (st != PackageInstaller.STATUS_SUCCESS) {
                    Toast.makeText(ctx, "Установка не удалась: " + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE), Toast.LENGTH_LONG).show();
                    checked = false;
                }
            }
        };
        IntentFilter fl = new IntentFilter(action);
        if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(r, fl, 2); else app.registerReceiver(r, fl);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
        PendingIntent pend = PendingIntent.getBroadcast(app, id, new Intent(action).setPackage(app.getPackageName()), flags);
        s.commit(pend.getIntentSender());
    }
}
