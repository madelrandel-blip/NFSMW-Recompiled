package com.nfsmwrecomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.List;

/**
 * Read-only access to the files the launcher exports (NfsmwActivity.exportLogs), for the app they are shared
 * with: Android does not let a file path cross apps, so they travel as
 * content://com.nfsmwrecomp.logs/<kind>/<name> URIs, readable only by the app given the permission in the share
 * intent.
 *
 *   logs/<name>    a game log, in the data folder's logs (visible folder or the app's own), or the audio dump
 *   report/<name>  the report written into the app's cache
 */
public class LogProvider extends ContentProvider {
    static final String AUTHORITY = "com.nfsmwrecomp.logs";

    static Uri uriFor(String kind, String name) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(kind).appendPath(name).build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    private File fileFor(Uri uri) throws FileNotFoundException {
        List<String> segments = uri.getPathSegments();
        if (segments.size() != 2 || segments.get(1).contains("/") || segments.get(1).startsWith(".")) {
            throw new FileNotFoundException(uri.toString());
        }
        String kind = segments.get(0);
        String name = segments.get(1);
        if ("report".equals(kind)) {
            File file = new File(new File(getContext().getCacheDir(), "report"), name);
            if (file.isFile()) {
                return file;
            }
        } else if ("logs".equals(kind) && CrashReport.AUDIO_DUMP.equals(name)) {
            File dump = CrashReport.audioDump(getContext());
            if (dump != null) {
                return dump;
            }
        } else if ("logs".equals(kind) && name.endsWith(".log")) {
            for (File log : CrashReport.newestLogs(getContext(), Integer.MAX_VALUE)) {
                if (log.getName().equals(name)) {
                    return log;
                }
            }
        }
        throw new FileNotFoundException(uri.toString());
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new FileNotFoundException("read only");
        }
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File file;
        try {
            file = fileFor(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] columns = projection != null ? projection
                : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; ++i) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) {
                row[i] = file.getName();
            } else if (OpenableColumns.SIZE.equals(columns[i])) {
                row[i] = file.length();
            }
        }
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name != null && name.endsWith(".wav")) {
            return "audio/wav";
        }
        return "text/plain";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read only");
    }
}
