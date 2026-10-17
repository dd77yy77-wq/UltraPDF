package com.dd.shchitok;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/** Автообновление: при запуске читает version.json в репозитории и ставит новую версию APK. */
final class Updater {
    static final String BASE = "https://raw.githubusercontent.com/dd77yy77-wq/UltraPDF/main/shchitok/";
    private static boolean checked, pending;

    private Updater() {}

    /** Вернулись из настроек разрешений: если ждали разрешения на установку, продолжаем загрузку. */
    static void onResume(Activity a) {
        if (pending && (Build.VERSION.SDK_INT < 26 || a.getPackageManager().canRequestPackageInstalls())) {
            pending = false;
            download(a);
        }
    }

    static void toast(final Activity a, final String msg) {
        a.runOnUiThread(new Runnable() {
            public void run() {
                Toast.makeText(a, msg, 1).show();
            }
        });
    }

    /** manual: вызвано кнопкой, тогда сообщаем результат даже если обновления нет. */
    static void check(final Activity a, final boolean manual) {
        if (!manual) {
            if (checked) return;
            checked = true;
        } else {
            toast(a, "Проверяю обновления…");
        }
        new Thread(new Runnable() {
            public void run() {
                try {
                    byte[] raw = read(BASE + "version.json?t=" + System.currentTimeMillis());
                    org.json.JSONObject o = new org.json.JSONObject(new String(raw, "UTF-8"));
                    final int code = o.getInt("code");
                    final String name = o.optString("name", String.valueOf(code));
                    final String notes = o.optString("notes", "");
                    int cur = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionCode;
                    if (code > cur) {
                        a.runOnUiThread(new Runnable() {
                            public void run() {
                                ask(a, name, notes);
                            }
                        });
                    } else if (manual) {
                        toast(a, "Установлена последняя версия");
                    }
                } catch (Throwable e) {
                    if (manual) toast(a, "Не удалось проверить: нет связи с сервером");
                    checked = false;
                }
            }
        }).start();
    }

    private static byte[] read(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(8000);
        c.setReadTimeout(10000);
        InputStream in = c.getInputStream();
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        byte[] b = new byte[4096];
        int n;
        while ((n = in.read(b)) > 0) bo.write(b, 0, n);
        in.close();
        return bo.toByteArray();
    }

    private static void ask(final Activity a, String name, String notes) {
        if (a.isFinishing()) return;
        new AlertDialog.Builder(a)
                .setTitle("Доступна версия " + name)
                .setMessage((notes.length() > 0 ? notes + "\n\n" : "") + "Скачать и установить обновление? Ваши подписи сохранятся.")
                .setPositiveButton("Обновить", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) {
                        download(a);
                    }
                })
                .setNegativeButton("Позже", null)
                .show();
    }

    private static void download(final Activity a) {
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) {
            Toast.makeText(a, "Разрешите установку из этого источника и вернитесь назад: обновление скачается само", 1).show();
            pending = true;
            a.startActivity(new Intent("android.settings.MANAGE_UNKNOWN_APP_SOURCES", Uri.parse("package:" + a.getPackageName())));
            return;
        }
        pending = false;
        final AlertDialog dlg = new AlertDialog.Builder(a).setTitle("Скачивание обновления").setMessage("0%").setCancelable(false).show();
        new Thread(new Runnable() {
            public void run() {
                try {
                    File f = new File(a.getCacheDir(), "update.apk");
                    HttpURLConnection c = (HttpURLConnection) new URL(BASE + "Shchitok.apk?t=" + System.currentTimeMillis()).openConnection();
                    c.setConnectTimeout(10000);
                    c.setReadTimeout(20000);
                    final int total = c.getContentLength();
                    InputStream in = c.getInputStream();
                    OutputStream out = new FileOutputStream(f);
                    byte[] b = new byte[16384];
                    int n, got = 0, last = -1;
                    while ((n = in.read(b)) > 0) {
                        out.write(b, 0, n);
                        got += n;
                        final int pct = total > 0 ? got * 100 / total : 0;
                        if (pct != last) {
                            last = pct;
                            a.runOnUiThread(new Runnable() {
                                public void run() {
                                    dlg.setMessage(pct + "%");
                                }
                            });
                        }
                    }
                    out.close();
                    in.close();
                    if (total > 0 && got != total) throw new Exception("incomplete");
                    a.runOnUiThread(new Runnable() {
                        public void run() {
                            dlg.dismiss();
                            install(a);
                        }
                    });
                } catch (final Throwable e) {
                    a.runOnUiThread(new Runnable() {
                        public void run() {
                            dlg.dismiss();
                            Toast.makeText(a, "Не удалось скачать обновление: " + e, 1).show();
                        }
                    });
                    checked = false;
                }
            }
        }).start();
    }

    /** Передаёт скачанный APK системному установщику. */
    private static void install(Activity a) {
        Uri u = Uri.parse("content://" + ApkProvider.AUTH + "/update.apk");
        Intent it = new Intent("android.intent.action.VIEW");
        it.setDataAndType(u, "application/vnd.android.package-archive");
        it.setClipData(android.content.ClipData.newRawUri("update.apk", u));
        it.addFlags(1 | 0x10000000); // GRANT_READ_URI_PERMISSION | ACTIVITY_NEW_TASK
        try {
            a.startActivity(it);
        } catch (Throwable e) {
            Toast.makeText(a, "Не удалось открыть установщик: " + e.getMessage(), 1).show();
            checked = false;
        }
    }
}
