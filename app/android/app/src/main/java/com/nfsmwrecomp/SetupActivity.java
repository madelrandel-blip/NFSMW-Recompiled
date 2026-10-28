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
 *
 * Coming back from that settings screen always goes on to the game, granted or not: asking again right away made a
 * loop (settings, back, the question again). And Android often kills the app when the access is granted; this
 * activity is then created again from its saved state, which remembers it was waiting for the settings screen, so it
 * carries on instead of starting over. If the settings screen cannot be opened at all, the game starts without it.
 */
public class SetupActivity extends Activity {
    private static final String TAG = "NFSMW";
    private static final int REQUEST_STORAGE = 1;
    private static final int REQUEST_SETTINGS = 2;
    private static final String STATE_WAITING = "waitingForSettings";

    private boolean waitingForSettings = false;
    private boolean started = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null && savedInstanceState.getBoolean(STATE_WAITING, false)) {
            // Created again after Android killed the app in the settings screen: the answer is in.
            startGame();
            return;
        }
        continueSetup();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putBoolean(STATE_WAITING, waitingForSettings);
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
            waitingForSettings = true;
            if (!openSettings(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())))
                    && !openSettings(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))) {
                Log.w(TAG, "no all files access screen on this device; starting without it");
                waitingForSettings = false;
                startGame();
            }
        } else {
            requestPermissions(new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE,
                    Manifest.permission.READ_EXTERNAL_STORAGE}, REQUEST_STORAGE);
        }
    }

    private boolean openSettings(Intent intent) {
        try {
            startActivityForResult(intent, REQUEST_SETTINGS);
            return true;
        } catch (Exception e) {
            Log.w(TAG, "cannot open " + intent.getAction(), e);
            return false;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_SETTINGS) {
            // Back from the settings screen: granted or not, on to the game (asked again on the next launch).
            waitingForSettings = false;
            startGame();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (waitingForSettings && hasStorageAccess()) {
            // Some devices do not report a result for that screen: granted is enough to go on.
            waitingForSettings = false;
            startGame();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_STORAGE) {
            // Android 10: granted or declined, on to the game.
            startGame();
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
        if (started) {
            return;
        }
        started = true;
        startActivity(new Intent(this, NfsmwActivity.class));
        finish();
    }
}
