package com.nfsmwrecomp;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.PowerManager;
import android.os.Build;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * What the game left behind, for a test build without a debugger, exported from the launcher
 * (NfsmwActivity.exportLogs): the game's own logs, and a report with Android's view of the last runs. The report
 * has the exit reasons (a crash, the app being removed from the recent apps...), the backtrace of the last native
 * crash (from its tombstone), the uncaught Java exception if there was one, and the app's logcat (logd keeps the
 * lines of dead processes of the same app, and the game's log goes to logcat too).
 */
final class CrashReport {
    private static final String JAVA_CRASH = "crash_java.txt";
    private static final int LOGCAT_LINES = 3000;
    private static final int TOMBSTONE_FRAMES = 64;

    private CrashReport() {}

    /** Called when the game starts: catches Java exceptions into a file the next report includes. */
    static void sessionStarted(Context context) {
        final File javaCrash = new File(context.getFilesDir(), JAVA_CRASH);
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try (PrintWriter out = new PrintWriter(new FileWriter(javaCrash, false))) {
                out.println(timestamp(System.currentTimeMillis()) + " uncaught exception in thread "
                        + thread.getName());
                error.printStackTrace(out);
            } catch (Exception ignored) {
            }
            if (previous != null) {
                previous.uncaughtException(thread, error);
            }
        });
    }

    /** The report as text. */
    static String build(Context context) {
        StringBuilder report = new StringBuilder();
        String version = "?";
        try {
            version = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName;
        } catch (Exception ignored) {
        }
        report.append("NFSMW Recompiled ").append(version).append(" - report of ")
                .append(timestamp(System.currentTimeMillis())).append('\n');
        report.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(", Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT)
                .append("), ").append(Build.HARDWARE).append("\n\n");

        appendDeviceDetails(context, report);
        appendExitReasons(context, report);

        File javaCrash = new File(context.getFilesDir(), JAVA_CRASH);
        if (javaCrash.isFile()) {
            try {
                report.append("== Java exception ==\n")
                        .append(new String(Files.readAllBytes(javaCrash.toPath()), StandardCharsets.UTF_8))
                        .append('\n');
            } catch (Exception ignored) {
            }
        }

        report.append("== logcat (last ").append(LOGCAT_LINES).append(" lines) ==\n");
        appendLogcat(report);
        return report.toString();
    }

    /** The newest game logs (logs/nfsmw_NNN.log, in the visible folder or the app's own), newest first. */
    /** The game's continuous dump of its audio output (audio_volcado_continuo_s), or null. */
    static File audioDump(Context context) {
        File newest = null;
        for (File folder : new File[] {NfsmwActivity.dataFolder(context, false), context.getExternalFilesDir(null)}) {
            File file = folder != null ? new File(new File(folder, "logs"), AUDIO_DUMP) : null;
            if (file != null && file.isFile() && (newest == null || file.lastModified() > newest.lastModified())) {
                newest = file;
            }
        }
        return newest;
    }

    static final String AUDIO_DUMP = "audio_salida.wav";

    /**
     * What the game logs do not say about the phone, for performance reports: SoC, cores and their top clocks,
     * memory, heat and battery, at the time of the export. The game logs the clocks, heat and CPU use while it runs
     * (nfsmw_monitor_sistema).
     */
    private static void appendDeviceDetails(Context context, StringBuilder report) {
        report.append("== Device ==\n");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            report.append("SoC: ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        }
        report.append("Board: ").append(Build.BOARD).append(", ABIs: ").append(String.join(" ", Build.SUPPORTED_ABIS))
                .append('\n');
        int cores = Runtime.getRuntime().availableProcessors();
        report.append("CPU cores: ").append(cores).append(", top MHz per core:");
        for (int i = 0; i < cores; ++i) {
            report.append(' ').append(readLong("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq") / 1000);
        }
        report.append('\n');
        ActivityManager activities = context.getSystemService(ActivityManager.class);
        if (activities != null) {
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            activities.getMemoryInfo(memory);
            report.append("RAM: ").append(memory.totalMem >> 20).append(" MB, ").append(memory.availMem >> 20)
                    .append(" MB free").append(memory.lowMemory ? " (low memory)" : "").append('\n');
        }
        PowerManager power = context.getSystemService(PowerManager.class);
        if (power != null) {
            report.append("Thermal status: ").append(power.getCurrentThermalStatus())
                    .append(" (0 none, 1 light, 2 moderate, 3 severe, 4 critical); power saving ")
                    .append(power.isPowerSaveMode()).append('\n');
        }
        Intent battery = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery != null) {
            report.append("Battery: ").append(battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)).append(" %, ")
                    .append(battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0).append(" C, charging ")
                    .append(battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0).append('\n');
        }
        report.append('\n');
    }

    private static long readLong(String path) {
        try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
            return Long.parseLong(reader.readLine().trim());
        } catch (Exception e) {
            return 0;
        }
    }

    /** nfsmw_012 for nfsmw_012.log and for its rotated parts nfsmw_012.1.log, nfsmw_012.2.log... */
    private static String logSession(String name) {
        String base = name.substring(0, name.length() - ".log".length());
        int dot = base.lastIndexOf('.');
        return dot > 0 && base.substring(dot + 1).matches("\\d+") ? base.substring(0, dot) : base;
    }

    /**
     * The logs of the newest `count` sessions, each with all its parts: a long session rotates every 5 MB into
     * nfsmw_012.log, nfsmw_012.1.log..., and sending only the newest files lost its beginning.
     */
    static List<File> newestLogs(Context context, int count) {
        List<File> logs = new ArrayList<>();
        for (File folder : new File[] {NfsmwActivity.dataFolder(context, false), context.getExternalFilesDir(null)}) {
            File[] files = folder != null ? new File(folder, "logs").listFiles() : null;
            if (files != null) {
                for (File file : files) {
                    if (file.getName().endsWith(".log") && !logs.contains(file)) {
                        logs.add(file);
                    }
                }
            }
        }
        logs.sort((a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        List<String> sessions = new ArrayList<>();
        List<File> chosen = new ArrayList<>();
        for (File log : logs) {
            String session = logSession(log.getName());
            if (!sessions.contains(session)) {
                if (sessions.size() == count) {
                    continue;
                }
                sessions.add(session);
            }
            chosen.add(log);
        }
        return chosen;
    }

    private static void appendExitReasons(Context context, StringBuilder report) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return;
        }
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        List<ApplicationExitInfo> exits =
                manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 5);
        report.append("== Android exit reasons (newest first) ==\n");
        ApplicationExitInfo nativeCrash = null;
        for (ApplicationExitInfo exit : exits) {
            report.append(timestamp(exit.getTimestamp())).append(": reason ").append(exitReasonName(exit.getReason()))
                    .append(", status ").append(exit.getStatus()).append(", ").append(exit.getDescription())
                    .append('\n');
            if (nativeCrash == null && exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                nativeCrash = exit;
            }
        }
        report.append('\n');
        if (nativeCrash != null) {
            appendTombstone(nativeCrash, report);
        }
    }

    /** The native crash: its tombstone, which API 31+ hands over as a protobuf (see Tombstone). */
    private static void appendTombstone(ApplicationExitInfo exit, StringBuilder report) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return;
        }
        byte[] data;
        try (InputStream in = exit.getTraceInputStream()) {
            if (in == null) {
                return;
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            int read;
            while ((read = in.read(buffer)) > 0 && out.size() < 4 * 1024 * 1024) {
                out.write(buffer, 0, read);
            }
            data = out.toByteArray();
        } catch (Exception e) {
            report.append("could not read the tombstone: ").append(e).append("\n\n");
            return;
        }
        report.append("== last native crash (").append(timestamp(exit.getTimestamp())).append(") ==\n");
        try {
            Tombstone.append(data, report, TOMBSTONE_FRAMES);
        } catch (Exception e) {
            report.append("could not parse the tombstone: ").append(e).append('\n');
        }
        report.append('\n');
    }

    private static String exitReasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
            case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY";
            case ApplicationExitInfo.REASON_CRASH: return "CRASH (Java)";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
            case ApplicationExitInfo.REASON_ANR: return "ANR";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INITIALIZATION_FAILURE";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY_DIED";
            case ApplicationExitInfo.REASON_OTHER: return "OTHER";
            default: return String.valueOf(reason);
        }
    }

    private static void appendLogcat(StringBuilder report) {
        ArrayDeque<String> tail = new ArrayDeque<>();
        try {
            // The game, SDL, the loaders and crashes in full; from everything else (system UI noise), only errors.
            List<String> command = new ArrayList<>(Arrays.asList("logcat", "-d", "-v", "threadtime", "-b", "main",
                    "-b", "system", "-b", "crash", "NFSMW:V", "SDL:V", "SDL/APP:V", "SDL/ERROR:V", "SDL/SYSTEM:V",
                    "SDL/VIDEO:V", "SDL/AUDIO:V", "SDL/RENDER:V", "SDL/INPUT:V", "SDL/ASSERT:V", "libc:V", "DEBUG:V",
                    "AndroidRuntime:V", "vulkan:V", "linker:V", "nativeloader:V", "libc++abi:V", "*:E"));
            Process process = new ProcessBuilder(command).redirectErrorStream(true).start();
            try (BufferedReader in = new BufferedReader(
                    new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = in.readLine()) != null) {
                    tail.addLast(line);
                    if (tail.size() > LOGCAT_LINES) {
                        tail.removeFirst();
                    }
                }
            }
            process.waitFor();
        } catch (Exception e) {
            report.append("could not read logcat: ").append(e).append('\n');
        }
        for (String line : tail) {
            report.append(line).append('\n');
        }
    }

    private static String timestamp(long ms) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(ms));
    }
}
