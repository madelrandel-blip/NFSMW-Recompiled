package com.nfsmwrecomp;

import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;

import org.libsdl.app.SDLActivity;

/**
 * The game's activity: SDL3's SDLActivity, which owns the window, input, audio and lifecycle, and loads the native
 * code (libmain.so) and calls its SDL_main.
 *
 * Before that, the native code is told where its data lives: Android has no executable folder, so a data folder
 * takes that role. With storage access (SetupActivity) it is "NFSMW Recompiled" at the storage root, visible from a
 * file manager or a PC; without it, the app's external files folder (Android/data/com.nfsmwrecomp/files). The
 * settings, logs, caches and the game copy go there.
 *
 * The launcher's "choose the ISO / folder" buttons call pickGamePath from the native thread (see
 * app/src/lanzador/sistema.cpp): it opens Android's file picker and turns the chosen document into a filesystem path
 * the game can open (storage access is asked for at launch, by SetupActivity).
 *
 * The launcher's "export the log" button calls exportLogs: the game's logs and a CrashReport, through Android's
 * share sheet.
 */
public class NfsmwActivity extends SDLActivity {
    private static final String TAG = "NFSMW";
    private static final String FOLDER_NAME = "NFSMW Recompiled";
    private static final int REQUEST_PICK = 0x4E46;

    private static File dataFolder;

    // The pick in progress: the native thread waits on the latch until the picker (or the settings screen) returns.
    private CountDownLatch pickDone;
    private String pickResult;
    private boolean pickFolder;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (dataFolder == null) {
            dataFolder = dataFolder(this, true);
            CrashReport.sessionStarted(this);
        }
        File folder = new File(dataFolder, "nfsmw");
        if (!folder.isDirectory() && !folder.mkdirs()) {
            Log.e(TAG, "could not create " + folder);
        }
        try {
            Os.setenv("REX_ANDROID_DATA_DIR", dataFolder.getAbsolutePath(), true);
            Os.setenv("HOME", folder.getAbsolutePath(), true);
        } catch (ErrnoException e) {
            Log.e(TAG, "could not set the data folder", e);
        }
        Log.i(TAG, "data folder: " + dataFolder);
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        // The shared C++ library the others link against, SDL3 (its JNI entry points), the runtime and the game.
        return new String[] {"c++_shared", "SDL3", "rexruntime", "main"};
    }

    /** SDL picks the orientation from the window size; the launcher and the game are landscape only. */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    /** The data folder: the visible one with storage access (created if create is set), else the app's own. */
    static File dataFolder(Context context, boolean create) {
        if (SetupActivity.hasStorageAccess(context)) {
            File visible = new File(Environment.getExternalStorageDirectory(), FOLDER_NAME);
            if (visible.isDirectory() || (create && visible.mkdirs())) {
                return visible;
            }
            if (create) {
                Log.e(TAG, "could not create " + visible + ", using the app folder");
            }
        }
        File data = context.getExternalFilesDir(null);
        return data != null ? data : context.getFilesDir();
    }

    // ---- Exporting the logs -----------------------------------------------------------------------------------------

    /**
     * Called from the native thread: writes the report (informe.txt) and opens the share sheet with it and the two
     * newest game logs (this session's and the previous one), complete, as files, plus the last minute of the game's
     * audio output (audio_salida.wav) if there is one.
     */
    public void exportLogs() {
        final ArrayList<Uri> uris = new ArrayList<>();
        try {
            File folder = new File(getCacheDir(), "report");
            if (!folder.isDirectory() && !folder.mkdirs()) {
                Log.e(TAG, "could not create " + folder);
            }
            File report = new File(folder, "informe.txt");
            Files.write(report.toPath(), CrashReport.build(this).getBytes(StandardCharsets.UTF_8));
            uris.add(LogProvider.uriFor("report", report.getName()));
        } catch (Exception e) {
            Log.e(TAG, "could not write the report", e);
        }
        for (File log : CrashReport.newestLogs(this, 2)) {
            uris.add(LogProvider.uriFor("logs", log.getName()));
        }
        // The last minute of what the game played (a report of bad sound comes with the sound).
        if (CrashReport.audioDump(this) != null) {
            uris.add(LogProvider.uriFor("logs", CrashReport.AUDIO_DUMP));
        }
        runOnUiThread(() -> {
            if (uris.isEmpty()) {
                Toast.makeText(this, R.string.export_nothing, Toast.LENGTH_LONG).show();
                return;
            }
            Intent send = new Intent(Intent.ACTION_SEND_MULTIPLE);
            send.setType("*/*");  // text and the audio dump
            send.putExtra(Intent.EXTRA_SUBJECT, "NFSMW Recompiled - logs");
            send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
            // The permission to read every file goes with the ClipData.
            ClipData clip = ClipData.newRawUri("NFSMW Recompiled", uris.get(0));
            for (int i = 1; i < uris.size(); ++i) {
                clip.addItem(new ClipData.Item(uris.get(i)));
            }
            send.setClipData(clip);
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(send, getString(R.string.export_title)));
        });
    }

    // ---- Picking the game ------------------------------------------------------------------------------------------

    /**
     * Called from the native thread: blocks until the user picks an .iso (folder = false) or the extracted game's
     * folder (folder = true). Returns its path, or an empty string if cancelled or not reachable as a path.
     */
    public String pickGamePath(boolean folder) {
        final CountDownLatch done = new CountDownLatch(1);
        runOnUiThread(() -> {
            pickDone = done;
            pickResult = "";
            pickFolder = folder;
            if (SetupActivity.hasStorageAccess(this)) {
                openPicker();
            } else {
                // The game reads the ISO by path, which needs all files access. It is asked for at launch
                // (SetupActivity), not here: granting it makes Android kill the app.
                Toast.makeText(this, R.string.pick_need_access, Toast.LENGTH_LONG).show();
                finishPick("");
            }
        });
        try {
            done.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return pickResult != null ? pickResult : "";
    }

    private void openPicker() {
        Intent intent;
        if (pickFolder) {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");  // no MIME type is registered for .iso everywhere
        }
        try {
            startActivityForResult(intent, REQUEST_PICK);
        } catch (ActivityNotFoundException e) {
            Log.e(TAG, "no document picker", e);
            finishPick("");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_PICK) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        String path = "";
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            path = pathOf(data.getData(), pickFolder);
            if (path.isEmpty()) {
                Toast.makeText(this, R.string.pick_not_a_path, Toast.LENGTH_LONG).show();
            }
        }
        finishPick(path);
    }

    private void finishPick(String path) {
        pickResult = path;
        if (pickDone != null) {
            pickDone.countDown();
            pickDone = null;
        }
    }

    /**
     * The filesystem path behind a document the picker returned, if there is one: shared storage documents are
     * "primary:<path>" (internal storage) or "<volume id>:<path>" (an SD card or USB drive); the downloads provider
     * may give "raw:<path>", or only an id, in which case the file is looked for in Download by its name.
     */
    private String pathOf(Uri uri, boolean tree) {
        try {
            String id = tree ? DocumentsContract.getTreeDocumentId(uri) : DocumentsContract.getDocumentId(uri);
            String authority = uri.getAuthority();
            if ("com.android.externalstorage.documents".equals(authority)) {
                int colon = id.indexOf(':');
                String volume = colon >= 0 ? id.substring(0, colon) : id;
                String relative = colon >= 0 ? id.substring(colon + 1) : "";
                File root = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory()
                        : new File("/storage/" + volume);
                File file = relative.isEmpty() ? root : new File(root, relative);
                return file.exists() ? file.getAbsolutePath() : "";
            }
            if ("com.android.providers.downloads.documents".equals(authority)) {
                if (id.startsWith("raw:")) {
                    return id.substring(4);
                }
                String name = displayName(uri);
                if (name != null) {
                    File file = new File(Environment.getExternalStoragePublicDirectory(
                            Environment.DIRECTORY_DOWNLOADS), name);
                    if (file.isFile()) {
                        return file.getAbsolutePath();
                    }
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "could not resolve " + uri, e);
        }
        Log.w(TAG, "not a filesystem document: " + uri);
        return "";
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null,
                null)) {
            if (cursor != null && cursor.moveToFirst()) {
                return cursor.getString(0);
            }
        } catch (Exception ignored) {
        }
        return null;
    }
}
