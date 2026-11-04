package com.nfsmwrecomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Starts the game again in a new process (NfsmwActivity.restartApp). A native library cannot be swapped inside a
 * process, so playing a copy that needs another game module (libmain_usa.so instead of libmain.so) takes a fresh
 * one. This activity runs in its own process (":reinicio" in the manifest): the game's process ends right after
 * starting it, and from here, in the foreground, the game is opened again with the arguments it was given.
 */
public class RestartActivity extends Activity {
    static final String EXTRA_TARGET = "com.nfsmwrecomp.target";
    static final String EXTRA_PID = "com.nfsmwrecomp.pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0) {
            Process.killProcess(pid);  // in case it has not ended yet
        }
        Intent target = getIntent().getParcelableExtra(EXTRA_TARGET);
        if (target != null) {
            startActivity(target);
        }
        finish();
        Runtime.getRuntime().exit(0);
    }
}
