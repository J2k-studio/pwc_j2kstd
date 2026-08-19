package com.pwc.app;

import android.app.Activity;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;

import com.pwc.app.terminal.TerminalSession;
import com.pwc.app.terminal.TerminalView;

/**
 * Full-screen terminal host.
 * - No ActionBar / no title strip "PowerCode"
 * - Dark terminal background (#0C0C0C)
 * - Soft keyboard adjusts layout
 *
 * Wires TerminalView ↔ TerminalSession.
 * If your TerminalView uses different method names (e.g. attachSession),
 * change the two lines marked ADJUST below.
 */
public class MainActivity extends Activity {

    private static final int BG = Color.parseColor("#0C0C0C");

    private TerminalSession session;
    private TerminalView terminalView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Must be before super.onCreate — removes title bar labeled "PowerCode"
        requestWindowFeature(Window.FEATURE_NO_TITLE);

        super.onCreate(savedInstanceState);
        applySystemUi();

        terminalView = new TerminalView(this);
        terminalView.setBackgroundColor(BG);
        terminalView.setFocusable(true);
        terminalView.setFocusableInTouchMode(true);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(BG);
        root.addView(terminalView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);

        session = new TerminalSession();

        // ADJUST: if TerminalView implements TerminalSession.Listener directly:
        session.setListener(terminalView);
        // ADJUST: if TerminalView needs a session reference for input → PTY:
        // Prefer setSession / attachSession — try both names that projects often use.
        tryAttachSession(terminalView, session);

        if (!session.start(this)) {
            // Session failed (native load / missing libjsh.so)
            // TerminalView may still show whatever onExit/onOutput it gets.
        }

        terminalView.requestFocus();
    }

    /** Best-effort: call setSession or attachSession if present. */
    private static void tryAttachSession(TerminalView view, TerminalSession session) {
        try {
            view.getClass().getMethod("setSession", TerminalSession.class)
                    .invoke(view, session);
            return;
        } catch (Throwable ignored) {
        }
        try {
            view.getClass().getMethod("attachSession", TerminalSession.class)
                    .invoke(view, session);
        } catch (Throwable ignored) {
        }
    }

    private void applySystemUi() {
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        w.setStatusBarColor(BG);
        w.setNavigationBarColor(BG);
        // Soft keyboard resizes content instead of covering the prompt
        w.setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                        | WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);

        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(true);
        } else {
            View decor = w.getDecorView();
            decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    protected void onDestroy() {
        if (session != null) {
            session.stop();
            session = null;
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && terminalView != null) {
            terminalView.requestFocus();
        }
    }
}
