package com.pwc.app;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;

import com.pwc.app.terminal.TerminalService;
import com.pwc.app.terminal.TerminalSession;
import com.pwc.app.terminal.TerminalView;

public class MainActivity extends Activity {

    private static final int BG = Color.parseColor("#0C0C0C");
    private TerminalSession session;
    private TerminalView terminalView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
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
        session.setListener(terminalView);
        tryAttachSession(terminalView, session);

        if (session.start(this)) {
            try {
                Intent fg = new Intent(this, TerminalService.class);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    startForegroundService(fg);
                } else {
                    startService(fg);
                }
            } catch (Throwable t) {
                android.util.Log.w("MainActivity", "TerminalService: " + t.getMessage());
            }
        }

        terminalView.requestFocus();
    }

    private static void tryAttachSession(TerminalView view, TerminalSession session) {
        try {
            view.getClass().getMethod("setSession", TerminalSession.class).invoke(view, session);
        } catch (Throwable ignored) {
            try {
                view.getClass().getMethod("attachSession", TerminalSession.class).invoke(view, session);
            } catch (Throwable ignored2) {
            }
        }
    }

    private void applySystemUi() {
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        w.setStatusBarColor(BG);
        w.setNavigationBarColor(BG);
        w.setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                        | WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(true);
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    protected void onDestroy() {
        try {
            stopService(new Intent(this, TerminalService.class));
        } catch (Throwable ignored) {
        }
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
