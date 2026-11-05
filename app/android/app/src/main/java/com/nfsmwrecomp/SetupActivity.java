package com.nfsmwrecomp;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;

/**
 * The launcher entry: starts the game.
 *
 * It no longer asks for storage access. The .iso is opened through Android's picker, which grants reading that one
 * file with no permission (NfsmwActivity.pickGamePath and openGameDocument). Asking at launch for "all files
 * access" (MANAGE_EXTERNAL_STORAGE) made Android kill the app on some phones when it was granted, and the player was
 * left outside it.
 *
 * Who already granted it keeps the data folder visible as "NFSMW Recompiled" at the storage root (see
 * NfsmwActivity.dataFolder); everyone else gets the app's own external folder.
 */
public class SetupActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startActivity(new Intent(this, NfsmwActivity.class));
        finish();
    }

    /** All files access (Android 11+) or the legacy storage permission (Android 10) is granted. */
    static boolean hasStorageAccess(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }
}
