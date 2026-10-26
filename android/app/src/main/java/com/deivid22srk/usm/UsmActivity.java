package com.deivid22srk.usm;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.os.Build;
import android.os.Bundle;
import java.io.File;
import java.io.FileWriter;
import java.util.Date;
import java.util.List;
import org.libsdl.app.SDLActivity;

public class UsmActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "dxvk_d3d9", "main" };
    }

    /* argv[1] = game folder, argv[2] = save folder, from the launcher. */
    @Override
    protected String[] getArguments() {
        String game = getIntent().getStringExtra("game");
        return game == null ? new String[0] : new String[] { game };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        saveLastExit();
        super.onCreate(savedInstanceState);
        applyEdgeToEdge();
    }

    /* True edge-to-edge: without it the window settles below the status
     * bar/cutout (e.g. 2285x1080 on a 2400x1080 panel) and the compositor
     * letterboxes the frame with black bars on the sides. */
    private void applyEdgeToEdge() {
        android.view.Window w = getWindow();
        if (Build.VERSION.SDK_INT >= 28) {
            android.view.WindowManager.LayoutParams lp = w.getAttributes();
            lp.layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams
                    .LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            w.setAttributes(lp);
        }
        android.view.View d = w.getDecorView();
        d.setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyEdgeToEdge();
    }

    /* Why the previous run ended (crash, low memory kill, ...), kept in
     * files/last_exit.txt since log.txt is replaced on every launch. */
    private void saveLastExit() {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            List<ApplicationExitInfo> l = am.getHistoricalProcessExitReasons(null, 0, 3);
            String game = getIntent().getStringExtra("game");
            File dir = game != null ? new File(game, "port") : null;
            if (dir != null) dir.mkdirs();
            if (l.isEmpty() || dir == null) return;
            try (FileWriter w = new FileWriter(new File(dir, "last_exit.txt"))) {
                for (ApplicationExitInfo e : l) {
                    w.write(new Date(e.getTimestamp()) + ": reason " + reasonName(e.getReason())
                            + " status " + e.getStatus() + " importance " + e.getImportance()
                            + " pss " + e.getPss() / 1024 + "MB rss " + e.getRss() / 1024 + "MB"
                            + " desc " + e.getDescription() + "\n");
                }
                ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
                am.getMemoryInfo(mi);
                w.write("device RAM " + mi.totalMem / (1 << 20) + "MB, available now " + mi.availMem / (1 << 20)
                        + "MB, low-memory threshold " + mi.threshold / (1 << 20) + "MB\n");
            }
        } catch (Exception ignored) {
        }
    }

    private static String reasonName(int r) {
        switch (r) {
        case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY";
        case ApplicationExitInfo.REASON_CRASH: return "CRASH_JAVA";
        case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
        case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
        case ApplicationExitInfo.REASON_ANR: return "ANR";
        case ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
        case ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
        case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
        case ApplicationExitInfo.REASON_OTHER: return "OTHER";
        default: return "code " + r;
        }
    }
}
