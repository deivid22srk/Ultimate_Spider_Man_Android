package com.deivid22srk.usm;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
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
import android.view.Window;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
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
 * the options are written to port/save/touch.cfg, which the runtime reads.
 *
 * The whole screen is built in code on purpose: the APK carries no AndroidX
 * and no layout resources, so the UI is framework views plus a handful of
 * GradientDrawable/RippleDrawable shapes, and nothing here can drift out of
 * sync with an XML file. */
public class UsmLauncherActivity extends Activity {
    private static final int REQ_FOLDER = 1, REQ_PERM = 2, REQ_DRIVER = 3;

    /* The recompilation uses fixed addresses: it only works with this build.
     * USM.exe 1.0.0.1, Treyarch, 5,926,912 bytes. */
    private static final long EXE_SIZE = 5926912L;
    private static final String EXE_SHA1 = "66c58c6e96ad96a1713f86fb903af1192e8bc8f5";

    private static final String CHANNEL = "https://youtube.com/@hail-games1?si=MgrsWaIA8fw7UJQf";

    /* Palette: near-black with a red core (the game's colour) and the channel's
     * lime for the credit chip, so the two never fight. */
    private static final int BG_TOP = 0xFF0B0E16, BG_BOT = 0xFF04050A;
    private static final int CARD = 0xF0131620, CARD_LINE = 0x16FFFFFF;
    private static final int TEXT = 0xFFF4F6FA, MUTED = 0xFF98A1B2, DIM = 0xFF6C7486;
    private static final int RED = 0xFFEE4B4B, RED_DK = 0xFFAE1B1B;
    private static final int LIME = 0xFFDFFF3B;
    private static final int GREEN = 0xFF57D98A, AMBER = 0xFFFFB86B;

    private SharedPreferences prefs;
    private TextView folderText, folderStatus, buildStatus, statusLine;
    private View playButton;
    private FrameLayout homeScreen, settingsScreen;
    private final Map<String, CompoundButton> toggles = new LinkedHashMap<>();
    private final TextView[] logChips = new TextView[3];
    private int logLevel = 1;
    private boolean pendingPick, pendingPlay;  /* continue once the storage prompt is answered */
    private boolean sawUnreadable;             /* last checkFolder() hit a permission wall */
    private boolean askedAllFiles;
    private LinearLayout driverList;            /* GPU driver rows, rebuilt on change */

    private Typeface black, medium, regular;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        black = Typeface.create("sans-serif-black", Typeface.NORMAL);
        medium = Typeface.create("sans-serif-medium", Typeface.NORMAL);
        regular = Typeface.create("sans-serif", Typeface.NORMAL);
        showInTheDark();
        /* "loglevel" replaced the two old checkboxes; keep whatever a previous
         * install had chosen. */
        if (prefs.contains("loglevel")) logLevel = prefs.getInt("loglevel", 1);
        else logLevel = prefs.getBoolean("logoff", false) ? 0 : prefs.getBoolean("debuglog", false) ? 2 : 1;
        buildUi();
        refreshFolder();
        checkDriverCrash();
    }

    /* Full screen, but the system bars stay usable: the content is padded by
     * the insets instead of being hidden under a notch. */
    private void showInTheDark() {
        Window w = getWindow();
        w.setStatusBarColor(BG_TOP);
        w.setNavigationBarColor(BG_BOT);
        w.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshFolder();
    }

    @Override
    public void onBackPressed() {
        if (settingsScreen != null && settingsScreen.getVisibility() == View.VISIBLE) {
            showScreen(false);
            return;
        }
        super.onBackPressed();
    }

    // ---- small helpers ------------------------------------------------------

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private TextView tv(String s, float sp, int color, Typeface tf) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setTypeface(tf);
        t.setIncludeFontPadding(false);
        return t;
    }

    /* rounded rectangle, optionally with a hairline border */
    private GradientDrawable shape(int fill, float radius, int strokeColor, float strokeDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(radius));
        if (strokeColor != 0) g.setStroke(Math.max(1, dp(strokeDp)), strokeColor);
        return g;
    }

    private GradientDrawable gradient(int from, int to, float radius) {
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[]{from, to});
        g.setCornerRadius(dp(radius));
        return g;
    }

    /* every tappable surface gets the same soft ripple */
    private Drawable press(Drawable base) {
        return new RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), base, null);
    }

    private void tap(View v, Runnable r) {
        v.setClickable(true);
        v.setFocusable(true);
        v.setOnClickListener(x -> r.run());
    }

    private TextView button(String label, boolean primary) {
        TextView t = tv(label, 15, TEXT, medium);
        t.setGravity(Gravity.CENTER);
        t.setLetterSpacing(0.06f);
        if (primary) {
            t.setBackground(press(gradient(0xFFF15A5A, RED_DK, 100)));
            t.setElevation(dp(8));
            t.setTextSize(17);
        } else {
            t.setBackground(press(shape(0x0DFFFFFF, 100, 0x33FFFFFF, 1.2f)));
        }
        return t;
    }

    private LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(shape(CARD, 20, CARD_LINE, 1f));
        int p = dp(18);
        c.setPadding(p, dp(16), p, dp(16));
        return c;
    }

    private void sectionTitle(LinearLayout parent, String s) {
        TextView t = tv(s, 10, DIM, medium);
        t.setLetterSpacing(0.22f);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(10);
        parent.addView(t, lp);
    }

    private void divider(LinearLayout parent) {
        View v = new View(this);
        v.setBackgroundColor(0x12FFFFFF);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1);
        lp.topMargin = dp(12);
        lp.bottomMargin = dp(12);
        parent.addView(v, lp);
    }

    private CompoundButton toggle(LinearLayout parent, String key, String title, String sub, boolean def) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.addView(tv(title, 15, TEXT, medium));
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.topMargin = dp(3);
        TextView s = tv(sub, 11, MUTED, regular);
        s.setLineSpacing(dp(1), 1f);
        col.addView(s, slp);
        row.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Switch sw = new Switch(this);
        sw.setChecked(prefs.getBoolean(key, def));
        sw.setThumbTintList(new ColorStateList(new int[][]{
                new int[]{android.R.attr.state_checked}, new int[]{}}, new int[]{LIME, 0xFFB6BDCB}));
        sw.setTrackTintList(new ColorStateList(new int[][]{
                new int[]{android.R.attr.state_checked}, new int[]{}}, new int[]{0x66DFFF3B, 0x33FFFFFF}));
        sw.setOnCheckedChangeListener((b, on) -> prefs.edit().putBoolean(key, on).apply());
        row.addView(sw, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp.topMargin = dp(4);
        rlp.bottomMargin = dp(4);
        parent.addView(row, rlp);
        toggles.put(key, sw);
        return sw;
    }

    private boolean on(String key) {
        CompoundButton b = toggles.get(key);
        return b != null && b.isChecked();
    }

    // ---- screen -------------------------------------------------------------

    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[]{BG_TOP, BG_BOT}));
        glow(root, RED, 520, 520, Gravity.TOP | Gravity.END, -140, -180);
        glow(root, 0xFF2C6BD8, 460, 460, Gravity.BOTTOM | Gravity.START, -160, 120);

        homeScreen = new FrameLayout(this);
        homeScreen.addView(buildHome());
        root.addView(homeScreen, match());

        settingsScreen = new FrameLayout(this);
        settingsScreen.addView(buildSettings());
        settingsScreen.setVisibility(View.GONE);
        root.addView(settingsScreen, match());

        View credit = credits();
        FrameLayout.LayoutParams clp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.END);
        clp.setMargins(0, 0, dp(18), dp(14));
        root.addView(credit, clp);

        root.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                    insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });

        setContentView(root);
    }

    /* a soft colour wash in a corner, so the black is not flat */
    private void glow(FrameLayout root, int color, int w, int h, int gravity, int dx, int dy) {
        View v = new View(this);
        GradientDrawable g = new GradientDrawable();
        g.setShape(GradientDrawable.OVAL);
        g.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        g.setGradientRadius(dp(w) / 2f);
        g.setColors(new int[]{(color & 0x00FFFFFF) | 0x2E000000, color & 0x00FFFFFF});
        v.setBackground(g);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(w), dp(h), gravity);
        lp.leftMargin = dp(dx);
        lp.topMargin = dp(dy);
        root.addView(v, lp);
    }

    private FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private View buildHome() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(30), dp(20), dp(30), dp(84));   /* room for the credit chip */

        /* left: identity + actions */
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);

        TextView kicker = tv("PLAYSTATION 2 · PC 2005 · ANDROID", 10, RED, medium);
        kicker.setLetterSpacing(0.24f);
        left.addView(kicker);

        TextView t1 = tv("ULTIMATE", 34, TEXT, black);
        t1.setLetterSpacing(0.02f);
        LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        l1.topMargin = dp(6);
        left.addView(t1, l1);

        TextView t2 = tv("SPIDER-MAN", 34, RED, black);
        t2.setLetterSpacing(0.02f);
        left.addView(t2);

        TextView sub = tv("Recompilação estática — o executável original rodando nativo, sem emulador.", 11, MUTED, regular);
        sub.setLineSpacing(dp(2), 1f);
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(dp(330), ViewGroup.LayoutParams.WRAP_CONTENT);
        sl.topMargin = dp(10);
        left.addView(sub, sl);

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        alp.topMargin = dp(22);
        left.addView(actions, alp);

        playButton = button("JOGAR", true);
        tap(playButton, this::play);
        actions.addView(playButton, new LinearLayout.LayoutParams(dp(190), dp(56)));

        TextView cfg = button("CONFIGURAÇÕES", false);
        tap(cfg, () -> showScreen(true));
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(dp(190), dp(56));
        clp.leftMargin = dp(12);
        actions.addView(cfg, clp);

        statusLine = tv("", 11, MUTED, regular);
        LinearLayout.LayoutParams stl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        stl.topMargin = dp(16);
        left.addView(statusLine, stl);

        row.addView(left, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.25f));

        /* right: the install folder */
        LinearLayout right = card();
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        rlp.leftMargin = dp(26);
        row.addView(right, rlp);

        sectionTitle(right, "PASTA DO JOGO");

        folderText = tv("", 13, TEXT, medium);
        folderText.setMaxLines(2);
        folderText.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        folderText.setLineSpacing(dp(2), 1f);
        right.addView(folderText);

        folderStatus = tv("", 12, MUTED, medium);
        LinearLayout.LayoutParams fsl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        fsl.topMargin = dp(8);
        right.addView(folderStatus, fsl);

        buildStatus = tv("", 10, DIM, regular);
        buildStatus.setLineSpacing(dp(2), 1f);
        LinearLayout.LayoutParams bsl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        bsl.topMargin = dp(8);
        right.addView(buildStatus, bsl);

        TextView pick = button("ESCOLHER PASTA", false);
        tap(pick, () -> {
            /* the game reads the folder with plain file calls: ask for file
             * access first (only shown once), then pick */
            if (hasStorage()) openPicker();
            else { pendingPick = true; requestStorage(); }
        });
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48));
        plp.topMargin = dp(16);
        right.addView(pick, plp);

        return row;
    }

    private View buildSettings() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(30), dp(18), dp(30), dp(74));

        /* header */
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);

        TextView back = tv("‹", 26, TEXT, medium);
        back.setGravity(Gravity.CENTER);
        back.setBackground(press(shape(0x0DFFFFFF, 100, 0x33FFFFFF, 1.2f)));
        tap(back, () -> showScreen(false));
        head.addView(back, new LinearLayout.LayoutParams(dp(44), dp(44)));

        TextView title = tv("CONFIGURAÇÕES", 20, TEXT, black);
        title.setLetterSpacing(0.10f);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        tlp.leftMargin = dp(14);
        head.addView(title, tlp);

        TextView reset = tv("RESTAURAR PADRÕES", 10, MUTED, medium);
        reset.setLetterSpacing(0.14f);
        reset.setPadding(dp(12), dp(10), dp(12), dp(10));
        reset.setBackground(press(shape(0x0AFFFFFF, 100, 0x1FFFFFFF, 1f)));
        tap(reset, this::resetDefaults);
        head.addView(reset);

        col.addView(head);

        LinearLayout cards = new LinearLayout(this);
        cards.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams cclp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        cclp.topMargin = dp(18);
        col.addView(cards, cclp);

        /* left card: controls */
        LinearLayout left = card();
        sectionTitle(left, "CONTROLES");
        toggle(left, "touch", "Controles de toque", "Camada de toque sobre o jogo: direcional, botões e câmera.", true);
        divider(left);
        toggle(left, "hudctx", "HUD contextual", "Nos menus mostra só A e Start; no jogo, todos os botões.", false);
        divider(left);
        toggle(left, "nintendo", "Layout Nintendo", "Troca A/B e X/Y (para quem vem do Switch).", false);

        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        llp.rightMargin = dp(12);
        cards.addView(left, llp);

        /* right card: video + diagnostics */
        LinearLayout right = card();
        sectionTitle(right, "VÍDEO E DIAGNÓSTICO");
        toggle(right, "vsync", "Sincronia vertical", "Limita o framerate ao do display. Desligue para medir o teto.", true);
        divider(right);

        TextView l1 = tv("Nível de log", 15, TEXT, medium);
        right.addView(l1);
        TextView l2 = tv("Debug grava muito e pesa no framerate — use só para diagnosticar.", 11, MUTED, regular);
        l2.setLineSpacing(dp(2), 1f);
        LinearLayout.LayoutParams l2p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        l2p.topMargin = dp(3);
        right.addView(l2, l2p);

        LinearLayout seg = new LinearLayout(this);
        seg.setOrientation(LinearLayout.HORIZONTAL);
        seg.setBackground(shape(0x0AFFFFFF, 100, 0x1FFFFFFF, 1f));
        seg.setPadding(dp(4), dp(4), dp(4), dp(4));
        String[] names = {"DESLIGADO", "NORMAL", "DEBUG"};
        for (int i = 0; i < names.length; i++) {
            final int level = i;
            TextView chip = tv(names[i], 10, MUTED, medium);
            chip.setGravity(Gravity.CENTER);
            chip.setLetterSpacing(0.10f);
            tap(chip, () -> setLogLevel(level));
            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(0, dp(34), 1f);
            seg.addView(chip, clp);
            logChips[i] = chip;
        }
        LinearLayout.LayoutParams segp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        segp.topMargin = dp(12);
        right.addView(seg, segp);
        paintLogChips();

        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        rlp.leftMargin = dp(12);
        cards.addView(right, rlp);

        /* full width card: which Vulkan driver the game runs on */
        LinearLayout drv = card();
        sectionTitle(drv, "DRIVER DE GPU");
        drv.addView(tv("Vulkan", 15, TEXT, medium));
        TextView dsub = tv("Sistema usa o driver da Qualcomm. Um driver importado (Turnip) é usado "
                + "no lugar dele — resolve bugs e às vezes destrava o jogo em Adreno. arm64.", 11, MUTED, regular);
        dsub.setLineSpacing(dp(2), 1f);
        LinearLayout.LayoutParams dsubp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        dsubp.topMargin = dp(3);
        drv.addView(dsub, dsubp);

        driverList = new LinearLayout(this);
        driverList.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        dlp.topMargin = dp(8);
        drv.addView(driverList, dlp);

        TextView imp = tv("IMPORTAR DRIVER (.ZIP)", 11, TEXT, medium);
        imp.setLetterSpacing(0.10f);
        imp.setGravity(Gravity.CENTER);
        imp.setBackground(press(shape(0x0DFFFFFF, 100, 0x33FFFFFF, 1.2f)));
        tap(imp, this::importDriver);
        LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(42));
        ilp.topMargin = dp(10);
        drv.addView(imp, ilp);

        LinearLayout.LayoutParams drvp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        drvp.topMargin = dp(12);
        col.addView(drv, drvp);

        ScrollView sv = new ScrollView(this);
        sv.setVerticalScrollBarEnabled(false);
        sv.setFillViewport(true);
        /* wrap_content height, or the column would be pinned to the viewport and
         * the cards would be clipped instead of scrolling on a short screen */
        sv.addView(col, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        refreshDrivers();
        return sv;
    }

    private void setLogLevel(int level) {
        logLevel = level;
        prefs.edit().putInt("loglevel", level).apply();
        paintLogChips();
    }

    private void paintLogChips() {
        for (int i = 0; i < logChips.length; i++) {
            if (logChips[i] == null) continue;
            boolean on = i == logLevel;
            logChips[i].setBackground(on ? gradient(0xFFF15A5A, RED_DK, 100) : shape(0x00000000, 100, 0, 0f));
            logChips[i].setTextColor(on ? 0xFFFFFFFF : MUTED);
        }
    }

    private void resetDefaults() {
        prefs.edit().putBoolean("touch", true).putBoolean("hudctx", false)
                .putBoolean("nintendo", false).putBoolean("vsync", true)
                .putInt("loglevel", 1).apply();
        for (Map.Entry<String, CompoundButton> e : toggles.entrySet()) {
            String k = e.getKey();
            boolean def = k.equals("touch") || k.equals("vsync");
            e.getValue().setChecked(def);
        }
        logLevel = 1;
        paintLogChips();
        Toast.makeText(this, "Padrões restaurados", Toast.LENGTH_SHORT).show();
    }

    private void showScreen(boolean settings) {
        View in = settings ? settingsScreen : homeScreen;
        View out = settings ? homeScreen : settingsScreen;
        if (in.getVisibility() == View.VISIBLE) return;
        in.setAlpha(0f);
        in.setTranslationY(dp(10));
        in.setVisibility(View.VISIBLE);
        in.animate().alpha(1f).translationY(0f).setDuration(180).start();
        out.animate().alpha(0f).setDuration(120).withEndAction(() -> {
            out.setVisibility(View.GONE);
            out.setAlpha(1f);
        }).start();
    }

    /* credit chip, bottom right, opens the channel */
    private View credits() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.HORIZONTAL);
        box.setGravity(Gravity.CENTER_VERTICAL);
        box.setPadding(dp(8), dp(6), dp(14), dp(6));
        box.setBackground(press(shape(0x14FFFFFF, 100, 0x1FFFFFFF, 1f)));
        box.setElevation(dp(4));

        int size = dp(34);
        ImageView av = new ImageView(this);
        Bitmap bm = avatar(size);
        if (bm != null) av.setImageBitmap(bm);
        else av.setBackground(shape(0xFF161A22, 100, 0x33FFFFFF, 1f));
        box.addView(av, new LinearLayout.LayoutParams(size, size));

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        clp.leftMargin = dp(9);

        TextView a = tv("FEITO POR", 8, DIM, medium);
        a.setLetterSpacing(0.20f);
        col.addView(a);
        TextView n = tv("Hailgames", 14, TEXT, medium);
        col.addView(n);
        TextView y = tv("youtube.com/@hail-games1", 9, LIME, regular);
        y.setAlpha(0.75f);
        col.addView(y);

        box.addView(col, clp);
        tap(box, this::openChannel);
        return box;
    }

    private void openChannel() {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(CHANNEL)));
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, "Nenhum app abriu o link", Toast.LENGTH_SHORT).show();
        }
    }

    /* the channel art ships in the APK and is cut into a circle here, so the
     * launcher never needs a network request or a storage permission */
    private Bitmap avatar(int size) {
        try (InputStream in = getAssets().open("hailgames.jpg")) {
            Bitmap src = BitmapFactory.decodeStream(in);
            if (src == null) return null;
            Bitmap out = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(out);
            Paint p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
            c.drawBitmap(Bitmap.createScaledBitmap(src, size, size, true), 0, 0, p);
            Paint mask = new Paint(Paint.ANTI_ALIAS_FLAG);
            mask.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_IN));
            mask.setColor(0xFF000000);
            c.drawCircle(size / 2f, size / 2f, size / 2f, mask);
            mask.setXfermode(null);
            mask.setStyle(Paint.Style.STROKE);
            mask.setStrokeWidth(Math.max(1f, size / 28f));
            mask.setColor(0x40FFFFFF);
            c.drawCircle(size / 2f, size / 2f, size / 2f - mask.getStrokeWidth() / 2f, mask);
            return out;
        } catch (Exception e) {
            return null;
        }
    }

    // ---- storage ------------------------------------------------------------

    /* Storage: the app targets API 29 with legacy storage, so the normal
     * "allow access to files" prompt gives the game plain file access. Only
     * if this Android ignores legacy storage and the folder still can't be
     * read is all-files access asked for (once, automatically). */
    private boolean hasStorage() {
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                || (Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager());
    }

    private void requestStorage() {
        requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE,
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
    private boolean unreadable() {
        return sawUnreadable;
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
        if (req == REQ_DRIVER) {
            if (res == RESULT_OK && data != null && data.getData() != null) unpackDriver(data.getData());
            return;
        }
        if (req != REQ_FOLDER || res != RESULT_OK || data == null || data.getData() == null) return;
        String path = treeToPath(data.getData());
        if (path == null) {
            folderStatus.setText("Local não é uma pasta comum; escolha no armazenamento interno ou no cartão SD.");
            folderStatus.setTextColor(AMBER);
            return;
        }
        prefs.edit().putString("folder", path).apply();
        refreshFolder();
        checkFolder(path);
        if (!askedAllFiles && !hasAllFiles() && unreadable()) {
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
        sawUnreadable = false;
        if (path == null) { bad.add("nenhuma pasta escolhida"); return bad; }
        File d = new File(path);
        if (!d.isDirectory()) { bad.add("pasta não encontrada"); return bad; }
        File data = findCi(d, "data");
        if (data == null) bad.add("falta a pasta data");
        else if (!data.canRead()) { bad.add("sem permissão para ler a pasta data"); sawUnreadable = true; }
        if (findCi(d, "USM.exe") == null) bad.add("falta USM.exe");
        if (findCi(d, "binkw32.dll") == null) bad.add("falta binkw32.dll");
        return bad;
    }

    private void refreshFolder() {
        if (folderText == null) return;
        String path = prefs.getString("folder", null);
        folderText.setText(path == null
                ? "Copie o conteúdo da sua pasta \"Game Files\" para qualquer lugar fora de Android/data "
                  + "(ex.: Armazenamento interno/USM) e escolha aqui."
                : path);
        List<String> bad = checkFolder(path);
        boolean ready = bad.isEmpty();
        folderStatus.setText(ready ? "●  Pronto para jogar" : "●  " + String.join(" · ", bad));
        folderStatus.setTextColor(ready ? GREEN : AMBER);
        buildStatus.setText(ready ? describeBuild(findCi(new File(path), "USM.exe")) : "");
        playButton.setEnabled(ready);
        playButton.setAlpha(ready ? 1f : 0.45f);
        if (statusLine != null) {
            statusLine.setText(ready ? "Tudo certo. Toque em JOGAR." : "Selecione a pasta do jogo para liberar o JOGAR.");
            statusLine.setTextColor(ready ? GREEN : MUTED);
        }
    }

    /* The recompiled code is tied to one executable. Say so before Play. */
    private String describeBuild(File exe) {
        if (exe == null) return "";
        if (exe.length() != EXE_SIZE) {
            return "Aviso: USM.exe tem " + exe.length() + " bytes; o port foi compilado para "
                    + EXE_SIZE + " bytes (1.0.0.1). Provavelmente não vai rodar.";
        }
        String sha = sha1(exe);
        if (sha == null) return "";
        if (!sha.equals(EXE_SHA1)) {
            return "Aviso: USM.exe SHA-1 " + sha.substring(0, 12) + "… não é o build de origem ("
                    + EXE_SHA1.substring(0, 12) + "…). Provavelmente não vai rodar.";
        }
        return "USM.exe 1.0.0.1 · SHA-1 " + EXE_SHA1.substring(0, 12) + "… — este é o build esperado.";
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

    /* The HUD icon pack ships inside the APK (assets/icons). The runtime opens
     * it with plain file calls from <game>/port/icons, which the APK is not,
     * so copy it out once per version. */
    private void installIcons(File game) {
        File dst = new File(game, "port/icons");
        dst.mkdirs();
        try {
            String[] names = getAssets().list("icons");
            if (names == null) return;
            for (String n : names) copyAsset("icons/" + n, new File(dst, n));
        } catch (Exception ignored) {
            /* no pack: the overlay falls back to plain discs */
        }
    }

    private void copyAsset(String src, File dst) {
        try {
            String[] kids = getAssets().list(src);
            if (kids != null && kids.length > 0) {   /* a folder (on/) */
                dst.mkdirs();
                for (String k : kids) copyAsset(src + "/" + k, new File(dst, k));
                return;
            }
        } catch (Exception ignored) {
            return;
        }
        try (InputStream in = getAssets().open(src); OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
        } catch (Exception ignored) {
            /* one missing icon is not fatal */
        }
    }

    /* ---- GPU driver ------------------------------------------------- */

    /* Drivers live in the app's private storage: dlopen() from internal data is
     * allowed, dlopen() from shared storage is not, and libadrenotools needs to
     * dlopen the user's libvulkan.so. */
    private File driversDir() {
        File d = new File(getFilesDir(), "drivers");
        d.mkdirs();
        return d;
    }

    private String driverChoice() {
        return prefs.getString("driver", "system");
    }

    private String driverPath() {
        String c = driverChoice();
        if (c.isEmpty() || c.equals("system")) return "";
        File d = new File(driversDir(), c);
        return new File(d, "libvulkan.so").exists() ? d.getAbsolutePath() : "";
    }

    private java.util.List<String> installedDrivers() {
        java.util.List<String> out = new java.util.ArrayList<>();
        File[] fs = driversDir().listFiles();
        if (fs == null) return out;
        for (File f : fs)
            if (f.isDirectory() && new File(f, "libvulkan.so").exists()) out.add(f.getName());
        java.util.Collections.sort(out);
        return out;
    }

    private void refreshDrivers() {
        if (driverList == null) return;
        driverList.removeAllViews();
        driverList.addView(driverRow("system", "Sistema (Qualcomm)", driverChoice().equals("system"), false));
        for (String n : installedDrivers()) {
            divider(driverList);
            driverList.addView(driverRow(n, n, driverChoice().equals(n), true));
        }
    }

    private View driverRow(String key, String label, boolean selected, boolean removable) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));

        View dot = new View(this);
        dot.setBackground(selected ? dot(0xFFF15A5A, 0xFFF15A5A) : dot(0x00000000, 0xFF3E4452));
        row.addView(dot, new LinearLayout.LayoutParams(dp(14), dp(14)));

        TextView t = tv(label, 14, selected ? TEXT : 0xFFC6CBD6, medium);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        tlp.leftMargin = dp(12);
        row.addView(t, tlp);

        if (removable) {
            TextView del = tv("REMOVER", 9, MUTED, medium);
            del.setLetterSpacing(0.12f);
            del.setPadding(dp(10), dp(6), dp(10), dp(6));
            tap(del, () -> deleteDriver(key));
            row.addView(del);
        }
        tap(row, () -> {
            prefs.edit().putString("driver", key).apply();
            refreshDrivers();
        });
        return row;
    }

    private android.graphics.drawable.GradientDrawable dot(int fill, int stroke) {
        android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable();
        g.setShape(android.graphics.drawable.GradientDrawable.OVAL);
        g.setColor(fill);
        g.setStroke(dp(1.4f), stroke);
        return g;
    }

    private void importDriver() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        /* zip packs and a bare libvulkan.so are both accepted here; some file
         * managers hand .so over as application/octet-stream, hence the list. */
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                "application/zip", "application/x-zip-compressed", "application/octet-stream" });
        try {
            startActivityForResult(i, REQ_DRIVER);
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, "Nenhum gerenciador de arquivos encontrado", Toast.LENGTH_LONG).show();
        }
    }

    private String displayName(Uri uri) {
        try (android.database.Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                if (i >= 0) return c.getString(i);
            }
        } catch (Exception ignored) {
        }
        String p = uri.getLastPathSegment();
        return p == null ? "driver" : p;
    }

    private static String slug(String name) {
        String n = name == null ? "driver" : name;
        String low = n.toLowerCase();
        if (low.endsWith(".zip")) n = n.substring(0, n.length() - 4);
        else if (low.contains(".so")) n = n.substring(0, low.indexOf(".so"));
        n = n.replaceAll("[^A-Za-z0-9._-]", "-").replaceAll("-{2,}", "-");
        if (n.isEmpty() || n.equals(".") || n.equals("..")) n = "driver";
        return n.length() > 40 ? n.substring(0, 40) : n;
    }

    /* A driver pack is one of:
     *   - a zip with the library somewhere inside (top level or a folder),
     *   - the library itself, picked straight out of the file manager.
     * AdrenoTools packs name it libvulkan.so; Mesa's Turnip builds ship
     * libvulkan_freedreno.so. Both are accepted and the result is exposed as
     * libvulkan.so, because that is the name libadrenotools opens in
     * runtime/driver_android.cpp.
     * Extraction runs off the UI thread, so a huge or broken archive cannot
     * lock the launcher. */
    private void unpackDriver(final Uri uri) {
        final String label = displayName(uri);
        new Thread(() -> {
            File tmp = new File(getCacheDir(), "driver-import");
            deleteRecursive(tmp);
            tmp.mkdirs();
            String err = null;
            int files = 0;
            try (java.io.InputStream rawIn = getContentResolver().openInputStream(uri)) {
                if (rawIn == null) throw new java.io.IOException("não foi possível abrir o arquivo");
                java.io.BufferedInputStream in = new java.io.BufferedInputStream(rawIn, 1 << 16);
                in.mark(8);
                int b0 = in.read(), b1 = in.read();
                in.reset();
                if (b0 == 'P' && b1 == 'K') {
                    try (java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(in)) {
                        byte[] buf = new byte[1 << 16];
                        java.util.zip.ZipEntry e;
                        while ((e = z.getNextEntry()) != null) {
                            String n = e.getName().replace('\\', '/');
                            if (n.startsWith("/") || n.contains("..") || n.startsWith("__MACOSX/")) continue;
                            File out = new File(tmp, n);
                            if (e.isDirectory()) { out.mkdirs(); continue; }
                            File parent = out.getParentFile();
                            if (parent != null) parent.mkdirs();
                            try (java.io.FileOutputStream fo = new java.io.FileOutputStream(out)) {
                                int r;
                                while ((r = z.read(buf)) > 0) fo.write(buf, 0, r);
                            }
                            files++;
                        }
                    }
                } else {
                    /* Not a zip: treat the picked file as the library itself. */
                    File out = new File(tmp, "libvulkan.so");
                    try (java.io.FileOutputStream fo = new java.io.FileOutputStream(out)) {
                        byte[] buf = new byte[1 << 16];
                        int r;
                        while ((r = in.read(buf)) > 0) fo.write(buf, 0, r);
                    }
                    files = 1;
                }
            } catch (Exception ex) {
                err = ex.toString();
            }

            File lib = err == null ? findDriverLib(tmp, 4) : null;
            if (err == null && lib == null) {
                String found = listLibs(tmp, 4);
                err = found.isEmpty()
                        ? "o pacote não tem libvulkan.so nem libvulkan*.so"
                        : "o pacote não tem libvulkan.so; achei " + found;
            }

            final String ferr = err;
            final File flib = lib;
            final int nfiles = files;
            runOnUiThread(() -> {
                if (ferr != null) {
                    Toast.makeText(this, "Importação falhou: " + ferr, Toast.LENGTH_LONG).show();
                    return;
                }
                File src = flib.getParentFile();
                /* The loader opens this exact name, so a pack that shipped
                 * libvulkan_freedreno.so gets it here too. */
                File named = new File(src, "libvulkan.so");
                if (!flib.equals(named) && !named.exists() && !copyFile(flib, named)) {
                    Toast.makeText(this, "Não foi possível preparar o driver", Toast.LENGTH_LONG).show();
                    return;
                }
                boolean fixedStack = normalizeExecStack(named);
                String name = slug(label);
                File dst = new File(driversDir(), name);
                for (int i = 2; dst.exists(); i++) dst = new File(driversDir(), name + "-" + i);
                if (!src.renameTo(dst) && !copyTree(src, dst)) {
                    Toast.makeText(this, "Não foi possível gravar em " + dst, Toast.LENGTH_LONG).show();
                    return;
                }
                deleteRecursive(tmp);
                prefs.edit().putString("driver", dst.getName()).apply();
                refreshDrivers();
                Toast.makeText(this, "Driver " + dst.getName() + " importado (" + nfiles
                        + " arquivos" + (fixedStack ? ", pilha executável corrigida" : "")
                        + "). Vale no próximo JOGAR.", Toast.LENGTH_LONG).show();
            });
        }).start();
    }

    /* Android refuses an executable stack for apps targeting API 29+, and the
     * refusal shows up as "avc: denied { execstack }" while the driver is being
     * loaded. Some Adreno/Turnip builds still carry PF_X on PT_GNU_STACK even
     * though nothing runs from the stack, so the flag is cleared here. Returns
     * true when the file was changed. */
    private static boolean normalizeExecStack(File f) {
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(f, "rw")) {
            byte[] h = new byte[64];
            raf.readFully(h);
            if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2) return false;
            java.nio.ByteBuffer hb = java.nio.ByteBuffer.wrap(h).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            long phoff = hb.getLong(32);
            int phentsize = hb.getShort(54) & 0xffff;
            int phnum = hb.getShort(56) & 0xffff;
            byte[] ph = new byte[phentsize];
            for (int i = 0; i < phnum; i++) {
                raf.seek(phoff + (long) i * phentsize);
                raf.readFully(ph);
                java.nio.ByteBuffer pb = java.nio.ByteBuffer.wrap(ph).order(java.nio.ByteOrder.LITTLE_ENDIAN);
                if (pb.getInt(0) != 0x6474e551) continue;          // PT_GNU_STACK
                int flags = pb.getInt(4);
                if ((flags & 1) == 0) return false;                // already RW
                raf.seek(phoff + (long) i * phentsize + 4);
                raf.write(java.nio.ByteBuffer.allocate(4)
                        .order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(flags & ~1).array());
                return true;
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    /* Exact name wins; otherwise any libvulkan*.so, then any *vulkan*.so, so a
     * Turnip build named libvulkan_freedreno.so still qualifies. */
    private static File findDriverLib(File dir, int depth) {
        if (dir == null || depth < 0) return null;
        File[] fs = dir.listFiles();
        if (fs == null) return null;
        File loose = null;
        for (File f : fs) {
            if (!f.isFile()) continue;
            String n = f.getName();
            if (n.equals("libvulkan.so")) return f;
            if (loose == null && n.startsWith("lib") && n.contains("vulkan") && n.contains(".so")) loose = f;
        }
        if (loose != null) return loose;
        for (File f : fs) {
            if (!f.isDirectory()) continue;
            File r = findDriverLib(f, depth - 1);
            if (r != null) return r;
        }
        return null;
    }

    /* What the user actually packed, for the failure message. */
    private static String listLibs(File dir, int depth) {
        StringBuilder sb = new StringBuilder();
        collectLibs(dir, depth, sb);
        String s = sb.toString();
        return s.length() > 120 ? s.substring(0, 117) + "..." : s;
    }

    private static void collectLibs(File dir, int depth, StringBuilder sb) {
        if (dir == null || depth < 0 || sb.length() > 120) return;
        File[] fs = dir.listFiles();
        if (fs == null) return;
        for (File f : fs) {
            if (f.isFile() && f.getName().contains(".so")) {
                if (sb.length() > 0) sb.append(", ");
                sb.append(f.getName());
            }
        }
        for (File f : fs) {
            if (!f.isDirectory()) continue;
            collectLibs(f, depth - 1, sb);
        }
    }

    private static boolean copyFile(File from, File to) {
        try (java.io.InputStream in = new java.io.FileInputStream(from);
             java.io.OutputStream out = new java.io.FileOutputStream(to)) {
            byte[] b = new byte[1 << 16];
            int n;
            while ((n = in.read(b)) > 0) out.write(b, 0, n);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean copyTree(File src, File dst) {
        try {
            if (src.isDirectory()) {
                dst.mkdirs();
                File[] fs = src.listFiles();
                if (fs != null) for (File f : fs) if (!copyTree(f, new File(dst, f.getName()))) return false;
                return true;
            }
            try (java.io.InputStream in = new java.io.FileInputStream(src);
                 java.io.OutputStream out = new java.io.FileOutputStream(dst)) {
                byte[] b = new byte[1 << 16];
                int n;
                while ((n = in.read(b)) > 0) out.write(b, 0, n);
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static void deleteRecursive(File f) {
        File[] fs = f.listFiles();
        if (fs != null) for (File c : fs) deleteRecursive(c);
        f.delete();
    }

    private void deleteDriver(String name) {
        deleteRecursive(new File(driversDir(), name));
        if (driverChoice().equals(name)) prefs.edit().putString("driver", "system").apply();
        refreshDrivers();
    }

    /* A driver that crashes the game must not leave the app unusable: the run
     * that used one is timestamped, and if the process died of a native crash
     * after that, the launcher offers to drop the driver. The exit history is
     * asked of the framework here -- ActivityManager only reports this
     * package's own processes, and only from API 30 on. */
    private void checkDriverCrash() {
        final String pending = prefs.getString("pendingDriver", "");
        final long since = prefs.getLong("pendingDriverAt", 0L);
        prefs.edit().remove("pendingDriver").remove("pendingDriverAt").apply();
        if (pending == null || pending.isEmpty() || Build.VERSION.SDK_INT < 30) return;

        String reason = null;
        try {
            android.app.ActivityManager am =
                    (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            java.util.List<android.app.ApplicationExitInfo> l =
                    am.getHistoricalProcessExitReasons(getPackageName(), 0, 5);
            for (android.app.ApplicationExitInfo e : l) {
                if (e.getTimestamp() < since) continue;
                int r = e.getReason();
                if (r == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE) reason = "falha nativa";
                else if (r == android.app.ApplicationExitInfo.REASON_SIGNALED) reason = "sinal recebido";
                else if (r == android.app.ApplicationExitInfo.REASON_LOW_MEMORY) reason = "memória insuficiente";
                if (reason != null) break;
            }
        } catch (Exception ignored) {
        }
        if (reason == null) return;

        final String name = new File(pending).getName();
        new AlertDialog.Builder(this)
                .setTitle("O jogo fechou com o driver " + name)
                .setMessage("A execução anterior terminou em " + reason + " logo depois de abrir "
                        + "com esse driver.\n\nVoltar para o driver da GPU do aparelho? O jogo "
                        + "abre normalmente, só sem o driver importado.")
                .setPositiveButton("Usar o do sistema", (d, w) -> {
                    prefs.edit().putString("driver", "system").apply();
                    refreshDrivers();
                })
                .setNegativeButton("Manter", null)
                .show();
    }

    private void play() {
        if (!hasStorage()) { pendingPlay = true; requestStorage(); return; }
        if (!checkFolder(prefs.getString("folder", null)).isEmpty()) { showScreen(false); refreshFolder(); return; }
        String folder = prefs.getString("folder", null);
        File save = new File(folder, "port/save");
        save.mkdirs();
        installIcons(new File(folder));
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
            w.write((on("touch") ? "1" : "0") + " "
                    + (on("nintendo") ? "1" : "0") + " "
                    + skip + " "
                    + logLevel + " "
                    + (on("hudctx") ? "1" : "0") + "\n");
            try (Writer v = new OutputStreamWriter(new FileOutputStream(new File(save, "vsync.cfg")))) {
                v.write(on("vsync") ? "1\n" : "0\n");
            }
        } catch (Exception ex) {
            folderStatus.setText("Não foi possível gravar as configurações: " + ex);
            folderStatus.setTextColor(AMBER);
            return;
        }
        String drv = driverPath();
        /* Recorded so the next launch can tell the user that the run which
         * never came back was started with a custom driver, and offer to drop
         * it. Cleared by checkDriverCrash() as soon as it is used. */
        prefs.edit().putString("pendingDriver", drv)
                .putLong("pendingDriverAt", System.currentTimeMillis()).apply();
        Intent i = new Intent(this, UsmActivity.class);
        i.putExtra("game", folder);
        i.putExtra("driver", drv);
        startActivity(i);
        finish();
    }
}
