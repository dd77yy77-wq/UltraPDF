package com.dd.pdfviewer;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/** Hands the installed APK of this very app to the share sheet (read-only, only through a temporary grant). */
public class ApkProvider extends ContentProvider {
    static final String AUTH = "com.dd.pdfviewer.apk";

    private File apk(Uri uri) {
        if (uri.getPathSegments().size() == 2 && "doc".equals(uri.getPathSegments().get(0))) {
            String n = uri.getLastPathSegment();
            if (n.contains("/") || n.contains("..")) n = "x";
            return new File(MainActivity.recentDir(getContext()), n);
        }
        if ("update.apk".equals(uri.getLastPathSegment())) return new File(getContext().getCacheDir(), "update.apk");
        return new File(getContext().getApplicationInfo().sourceDir);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read only");
        return ParcelFileDescriptor.open(apk(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        String[] cols = projection != null ? projection : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(cols);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = uri.getLastPathSegment().replaceFirst("^[0-9a-f]{1,8}_", "");
            else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = apk(uri).length();
        }
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return uri.getPathSegments().size() == 2 ? "application/pdf" : "application/vnd.android.package-archive";
    }

    @Override
    public Uri insert(Uri uri, ContentValues v) {
        return null;
    }

    @Override
    public int delete(Uri uri, String s, String[] a) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues v, String s, String[] a) {
        return 0;
    }
}
