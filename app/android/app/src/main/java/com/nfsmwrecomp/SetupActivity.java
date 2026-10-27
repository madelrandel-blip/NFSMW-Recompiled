package com.nfsmwrecomp;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

/**
 * The launcher entry: asks for access to shared storage, so the data folder can be a visible
 * "NFSMW Recompiled" folder at the storage root (where the game copy goes, reachable from a file manager or a PC),
 * then starts the game. If the user declines, the game still starts, in the app's own external folder, and the
 * question comes back on the next launch.
 *
 * Android 11+ needs "all files access" (MANAGE_EXTERNAL_STORAGE, granted in a settings screen); Android 10 uses the
 * WRITE_EXTERNAL_STORAGE prompt with the legacy storage model.
 */
public class SetupActivity extends Activity {
    private static final String TAG = "NFSMW";
    private static final int REQUEST_STORAGE = 1;

    private boolean waitingForSettings = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        continueSetup();
    }

    // Asked here, before the game starts, on every launch until granted: Android kills the app when all files
    // access is granted, so asking later (when picking the ISO) lost whatever the game was doing.
    private void continueSetup() {
        if (hasStorageAccess()) {
            startGame();
            return;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.access_title)
                .setMessage(R.string.access_message)
                .setCancelable(false)
                .setPositiveButton(R.string.access_grant, (d, which) -> requestStorageAccess())
                .setNegativeButton(R.string.access_later, (d, which) -> startGame())
                .show();
    }

    private void requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            } catch (Exception e) {
                Log.w(TAG, "no per-app all files access screen, opening the general one", e);
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
            waitingForSettings = true;
        } else {
            requestPermissions(new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE,
                    Manifest.permission.READ_EXTERNAL_STORAGE}, REQUEST_STORAGE);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (waitingForSettings) {
            // Back from the settings screen without Android restarting the app: granted or not, ask again or go on.
            waitingForSettings = false;
            continueSetup();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_STORAGE) {
            continueSetup();
        }
    }

    static boolean hasStorageAccess(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    private boolean hasStorageAccess() {
        return hasStorageAccess(this);
    }

    private void startGame() {
        startActivity(new Intent(this, NfsmwActivity.class));
        finish();
    }
}
