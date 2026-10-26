package com.deivid22srk.usm;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.Manifest;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/* Start screen: pick the folder that holds the Ultimate Spider-Man install,
 * check that it is the build this port was recompiled from, set the port
 * options, press Play. The folder is passed to the native side as argv[1];
 * the options are written to port/save/touch.cfg, which the runtime reads. */
public class UsmLauncherActivity extends Activity {
    private static final int REQ_FOLDER = 1, REQ_PERM = 2;

    /* The recompilation uses fixed addresses: it only works with this build.
     * USM.exe 1.0.0.1, Treyarch, 5,926,912 bytes. */
    private static final long EXE_SIZE = 5926912L;
    private static final String EXE_SHA1 = "66c58c6e96ad96a1713f86fb903af1192e8bc8f5";

    private SharedPreferences prefs;
    private TextView folderText, folderStatus, buildStatus;
    private Button playButton;
    private boolean pendingPick, pendingPlay;  /* continue once the storage prompt is answered */
    private boolean askedAllFiles;
    private final Map<String, CheckBox> checks = new LinkedHashMap<>();

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        buildUi();
        refreshFolder();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshFolder();
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView text(LinearLayout parent, String s, int sp, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(Color.rgb(230, 225, 210));
        if (bold) t.setTypeface(null, android.graphics.Typeface.BOLD);
        t.setPadding(0, dp(6), 0, dp(2));
        parent.addView(t);
        return t;
    }

    private CheckBox check(LinearLayout parent, String key, String label, boolean def) {
        CheckBox c = new CheckBox(this);
        c.setText(label);
        c.setTextSize(16);
        c.setTextColor(Color.rgb(230, 225, 210));
        c.setChecked(prefs.getBoolean(key, def));
        parent.addView(c);
        checks.put(key, c);
        return c;
    }

    private void buildUi() {
        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(Color.rgb(24, 22, 20));
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(24), dp(16), dp(24), dp(24));
        sv.addView(root);

        text(root, "Ultimate Spider-Man", 28, true);
        text(root, "PC (2005) static recompilation", 14, false);

        text(root, "Game folder", 20, true);
        folderText = text(root, "", 14, false);
        folderStatus = text(root, "", 14, false);
        buildStatus = text(root, "", 12, false);
        Button pick = new Button(this);
        pick.setText("Choose Ultimate Spider-Man folder");
        pick.setOnClickListener(v -> {
            /* the game reads the folder with plain file calls: ask for file
             * access first (only shown once), then pick */
            if (hasStorage()) openPicker();
            else { pendingPick = true; requestStorage(); }
        });
        root.addView(pick);

        text(root, "Options", 20, true);
        check(root, "touch", "Touch controls", true);
        check(root, "nintendo", "Nintendo button layout (A/B, X/Y swapped)", false);
        check(root, "vsync", "Vertical sync", true);
        check(root, "debuglog", "Debug log (verbose, slower)", false);
        check(root, "logoff", "No log file (turn logging off)", false);
        check(root, "hudctx", "Contextual HUD (fewer buttons in menus)", false);

        playButton = new Button(this);
        playButton.setText("Play");
        playButton.setTextSize(22);
        playButton.setOnClickListener(v -> play());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(64));
        lp.topMargin = dp(16);
        root.addView(playButton, lp);
        setContentView(sv);
    }

    /* Storage: the app targets API 29 with legacy storage, so the normal
     * "allow access to files" prompt gives the game plain file access. Only
     * if this Android ignores legacy storage and the folder still can't be
     * read is all-files access asked for (once, automatically). */
    private boolean hasStorage() {
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                || (Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager());
    }

    private void requestStorage() {
        requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_PERM);
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] res) {
        super.onRequestPermissionsResult(req, perms, res);
        if (req != REQ_PERM) return;
        boolean ok = hasStorage();
        if (ok && pendingPick) openPicker();
        if (ok && pendingPlay) play();
        pendingPick = pendingPlay = false;
        refreshFolder();
    }

    /* folder exists but its files can't be read: legacy storage was refused */
    private boolean unreadable(List<String> bad) {
        for (String b : bad) if (b.startsWith("cannot read")) return true;
        return false;
    }

    private boolean hasAllFiles() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    private void requestAllFiles() {
        try {
            Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(i);
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void openPicker() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_FOLDER);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_FOLDER || res != RESULT_OK || data == null || data.getData() == null) return;
        String path = treeToPath(data.getData());
        if (path == null) {
            folderStatus.setText("That location is not a plain folder on this device; choose one on internal storage or the SD card.");
            return;
        }
        prefs.edit().putString("folder", path).apply();
        refreshFolder();
        if (!askedAllFiles && !hasAllFiles() && unreadable(checkFolder(path))) {
            askedAllFiles = true;
            requestAllFiles();
        }
    }

    /* content://...tree/primary:Games/USM -> /storage/emulated/0/Games/USM */
    private static String treeToPath(Uri tree) {
        String id;
        try { id = DocumentsContract.getTreeDocumentId(tree); } catch (Exception e) { return null; }
        int c = id.indexOf(':');
        if (c < 0) return null;
        String vol = id.substring(0, c), rel = id.substring(c + 1);
        String base = vol.equalsIgnoreCase("primary") ? Environment.getExternalStorageDirectory().getPath()
                : "/storage/" + vol;
        return rel.isEmpty() ? base : base + "/" + rel;
    }

    private static File findCi(File dir, String name) {
        File f = new File(dir, name);
        if (f.exists()) return f;
        String[] l = dir.list();
        if (l != null) for (String s : l) if (s.equalsIgnoreCase(name)) return new File(dir, s);
        return null;
    }

    /* Returns the list of problems with the folder (empty when it is ready). */
    private List<String> checkFolder(String path) {
        List<String> bad = new ArrayList<>();
        if (path == null) { bad.add("no folder chosen"); return bad; }
        File d = new File(path);
        if (!d.isDirectory()) { bad.add("folder not found"); return bad; }
        File data = findCi(d, "data");
        if (data == null) bad.add("data folder missing");
        else if (!data.canRead()) bad.add("cannot read the data folder");
        if (findCi(d, "USM.exe") == null) bad.add("USM.exe missing");
        if (findCi(d, "binkw32.dll") == null) bad.add("binkw32.dll missing");
        return bad;
    }

    private void refreshFolder() {
        String path = prefs.getString("folder", null);
        folderText.setText(path == null ? "No folder chosen yet. Copy the contents of your \"Game Files\" folder "
                + "anywhere outside Android/data, e.g. Internal storage/USM, then choose it here." : path);
        List<String> bad = checkFolder(path);
        folderStatus.setText(bad.isEmpty() ? "Ready" : "Not ready: " + String.join(", ", bad));
        folderStatus.setTextColor(bad.isEmpty() ? Color.rgb(140, 200, 120) : Color.rgb(230, 150, 110));
        if (bad.isEmpty()) buildStatus.setText(describeBuild(findCi(new File(path), "USM.exe")));
        else buildStatus.setText("");
        playButton.setEnabled(bad.isEmpty());
    }

    /* The recompiled code is tied to one executable. Say so before Play. */
    private String describeBuild(File exe) {
        if (exe == null) return "";
        if (exe.length() != EXE_SIZE) {
            return "Warning: USM.exe is " + exe.length() + " bytes; this port was built for "
                    + EXE_SIZE + " bytes (1.0.0.1). It will probably not run.";
        }
        String sha = sha1(exe);
        if (sha == null) return "";
        if (!sha.equals(EXE_SHA1)) {
            return "Warning: USM.exe SHA-1 " + sha.substring(0, 12) + "… is not the build this port was "
                    + "recompiled from (" + EXE_SHA1.substring(0, 12) + "…). It will probably not run.";
        }
        return "USM.exe 1.0.0.1, SHA-1 " + EXE_SHA1.substring(0, 12) + "… — this is the build the port expects.";
    }

    private static String sha1(File f) {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest()) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return null;
        }
    }

    private void saveChoices() {
        SharedPreferences.Editor e = prefs.edit();
        for (Map.Entry<String, CheckBox> c : checks.entrySet()) e.putBoolean(c.getKey(), c.getValue().isChecked());
        e.apply();
    }

    private void play() {
        if (!hasStorage()) { pendingPlay = true; requestStorage(); return; }
        saveChoices();
        String folder = prefs.getString("folder", null);
        File save = new File(folder, "port/save");
        save.mkdirs();
        /* Read the skip-cutscenes value the game saved BEFORE opening the file
         * for writing: FileOutputStream truncates it. */
        int skip = 0;
        File cfg = new File(save, "touch.cfg");
        if (cfg.exists()) {
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(cfg))) {
                String line = r.readLine();
                if (line != null) {
                    String[] p = line.trim().split(" ");
                    if (p.length >= 3) skip = Integer.parseInt(p[2]);
                }
            } catch (Exception ignored) {
                /* unreadable or old format: keep skip at 0 */
            }
        }
        try (Writer w = new OutputStreamWriter(new FileOutputStream(cfg))) {
            /* touch nintendo skip-cutscenes log-level contextual-hud
             * (log: 0 off, 1 normal, 2 debug) */
            int logLevel = checks.get("logoff").isChecked() ? 0 : checks.get("debuglog").isChecked() ? 2 : 1;
            w.write((checks.get("touch").isChecked() ? "1" : "0") + " "
                    + (checks.get("nintendo").isChecked() ? "1" : "0") + " "
                    + skip + " "
                    + logLevel + " "
                    + (checks.get("hudctx").isChecked() ? "1" : "0") + "\n");
            try (Writer v = new OutputStreamWriter(new FileOutputStream(new File(save, "vsync.cfg")))) {
                v.write(checks.get("vsync").isChecked() ? "1\n" : "0\n");
            }
        } catch (Exception ex) {
            folderStatus.setText("Could not write the settings: " + ex);
            return;
        }
        Intent i = new Intent(this, UsmActivity.class);
        i.putExtra("game", folder);
        startActivity(i);
        finish();
    }
}
