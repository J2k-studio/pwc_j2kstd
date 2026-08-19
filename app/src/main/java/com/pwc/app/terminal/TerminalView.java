package com.pwc.app.terminal;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.util.ArrayList;
import java.util.List;

/**
 * Terminal view that understands the ANSI sequences jsh uses for line editing:
 * CR, CSI J (erase below), CSI A/C (cursor), SGR colors including truecolor + dim.
 *
 * Draws a blinking block cursor at (curCol, curRow) so the insert point is visible.
 */
public class TerminalView extends View implements TerminalSession.Listener {

    private static final int MAX_LINES = 500;
    private static final int BG = Color.rgb(12, 12, 16);
    private static final int FG_DEFAULT = Color.rgb(220, 220, 220);
    private static final int FG_DIM = Color.rgb(140, 140, 150);      /* ghost text */
    /* Vertical bar cursor (VS Code / modern terminal style) */
    private static final int CURSOR_BAR = Color.rgb(200, 220, 255);
    private static final int CURSOR_GLOW = Color.argb(60, 120, 180, 255);
    private static final long BLINK_MS = 480;

    private static class Cell {
        char ch;
        int color;
        Cell(char ch, int color) { this.ch = ch; this.color = color; }
    }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint cursorPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final List<List<Cell>> buffer = new ArrayList<>();
    private int curRow = 0;
    private int curCol = 0;
    private int fg = FG_DEFAULT;

    private TerminalSession session;
    private float charWidth;
    private float lineHeight;
    private int cols = 80;
    private int rows = 24;

    /* blinking cursor */
    private boolean cursorVisible = true;
    private boolean cursorBlinkOn = true;
    private final Handler blinkHandler = new Handler(Looper.getMainLooper());
    private final Runnable blinkRunnable = new Runnable() {
        @Override
        public void run() {
            cursorBlinkOn = !cursorBlinkOn;
            invalidate();
            blinkHandler.postDelayed(this, BLINK_MS);
        }
    };

    public TerminalView(Context context) {
        super(context);
        init();
    }

    public TerminalView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        setBackgroundColor(BG);
        setFocusable(true);
        setFocusableInTouchMode(true);
        paint.setTypeface(Typeface.MONOSPACE);
        paint.setTextSize(sp(13));
        cursorPaint.setStyle(Paint.Style.FILL);
        measureFont();
        ensureRow(0);
        setOnClickListener(v -> {
            requestFocus();
            cursorVisible = true;
            cursorBlinkOn = true;
            invalidate();
            InputMethodManager imm = (InputMethodManager)
                    getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        });
    }

    private float sp(float v) {
        return v * getResources().getDisplayMetrics().scaledDensity;
    }

    private void measureFont() {
        charWidth = paint.measureText("M");
        Paint.FontMetrics fm = paint.getFontMetrics();
        lineHeight = fm.descent - fm.ascent + 4;
    }

    public void attachSession(TerminalSession session) {
        this.session = session;
        session.setListener(this);
    }

    /** Optional alias used by some MainActivity builds */
    public void setSession(TerminalSession session) {
        attachSession(session);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        blinkHandler.removeCallbacks(blinkRunnable);
        blinkHandler.postDelayed(blinkRunnable, BLINK_MS);
    }

    @Override
    protected void onDetachedFromWindow() {
        blinkHandler.removeCallbacks(blinkRunnable);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (charWidth > 0 && lineHeight > 0) {
            cols = Math.max(20, (int) ((w - 8) / charWidth));
            rows = Math.max(8, (int) (h / lineHeight));
            if (session != null) session.resize(rows, cols);
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        Paint.FontMetrics fm = paint.getFontMetrics();
        int start = Math.max(0, buffer.size() - rows);
        float y = -fm.ascent + 4;

        for (int r = start; r < buffer.size(); r++) {
            List<Cell> line = buffer.get(r);
            float x = 4;
            for (int c = 0; c < line.size(); c++) {
                Cell cell = line.get(c);
                paint.setColor(cell.color);
                canvas.drawText(String.valueOf(cell.ch), x, y, paint);
                x += charWidth;
            }
            y += lineHeight;
        }

        /* Blinking vertical bar cursor (not a full block) */
        if (cursorVisible && cursorBlinkOn && charWidth > 0) {
            int screenRow = curRow - start;
            if (screenRow >= 0 && screenRow < rows) {
                float cx = 4 + curCol * charWidth;
                float cy = 4 + screenRow * lineHeight;
                float barW = Math.max(2f, charWidth * 0.12f);
                float pad = lineHeight * 0.12f;

                /* soft glow behind bar */
                cursorPaint.setStyle(Paint.Style.FILL);
                cursorPaint.setColor(CURSOR_GLOW);
                canvas.drawRect(cx - 2, cy + pad, cx + barW + 4, cy + lineHeight - pad, cursorPaint);

                /* sharp vertical bar */
                cursorPaint.setColor(CURSOR_BAR);
                canvas.drawRect(cx, cy + pad, cx + barW, cy + lineHeight - pad, cursorPaint);
            }
        }
    }

    @Override
    public void onOutput(byte[] data, int len) {
        String chunk = new String(data, 0, len, java.nio.charset.StandardCharsets.UTF_8);
        process(chunk);
        trimBuffer();
        /* keep cursor lit briefly after output so typing feels responsive */
        cursorBlinkOn = true;
        invalidate();
    }

    @Override
    public void onExit() {
        fg = FG_DEFAULT;
        writePlain("\n[session ended]\n");
        invalidate();
    }

    private void ensureRow(int row) {
        while (buffer.size() <= row) {
            buffer.add(new ArrayList<Cell>());
        }
    }

    private void putChar(char ch) {
        if (ch == '\n') {
            curRow++;
            curCol = 0;
            ensureRow(curRow);
            return;
        }
        if (ch == '\r') {
            curCol = 0;
            return;
        }
        if (ch == '\b') {
            if (curCol > 0) curCol--;
            return;
        }
        if (ch < 32) return;

        ensureRow(curRow);
        List<Cell> line = buffer.get(curRow);
        while (line.size() < curCol) {
            line.add(new Cell(' ', fg));
        }
        if (curCol < line.size()) {
            line.set(curCol, new Cell(ch, fg));
        } else {
            line.add(new Cell(ch, fg));
        }
        curCol++;
        if (cols > 0 && curCol >= cols) {
            curRow++;
            curCol = 0;
            ensureRow(curRow);
        }
    }

    private void writePlain(String s) {
        for (int i = 0; i < s.length(); i++) putChar(s.charAt(i));
    }

    private void eraseBelow() {
        ensureRow(curRow);
        List<Cell> line = buffer.get(curRow);
        while (line.size() > curCol) line.remove(line.size() - 1);
        while (buffer.size() > curRow + 1) {
            buffer.remove(buffer.size() - 1);
        }
    }

    private void eraseLineToEnd() {
        ensureRow(curRow);
        List<Cell> line = buffer.get(curRow);
        while (line.size() > curCol) line.remove(line.size() - 1);
    }

    private void process(String s) {
        int i = 0;
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c == 0x1b && i + 1 < s.length()) {
                char n = s.charAt(i + 1);
                if (n == '[') {
                    i = handleCsi(s, i + 2);
                    continue;
                }
                if (n == ']') {
                    i += 2;
                    while (i < s.length() && s.charAt(i) != 0x07 && s.charAt(i) != 0x1b) i++;
                    if (i < s.length() && s.charAt(i) == 0x07) i++;
                    continue;
                }
                i += 2;
                continue;
            }
            putChar(c);
            i++;
        }
    }

    private int handleCsi(String s, int i) {
        int start = i;
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c >= 0x40 && c <= 0x7e) {
                String params = s.substring(start, i);
                applyCsi(params, c);
                return i + 1;
            }
            i++;
        }
        return i;
    }

    private void applyCsi(String params, char finalByte) {
        int[] p = parseParams(params);
        switch (finalByte) {
            case 'A': {
                int n = p.length > 0 ? Math.max(1, p[0]) : 1;
                curRow = Math.max(0, curRow - n);
                ensureRow(curRow);
                break;
            }
            case 'B': {
                int n = p.length > 0 ? Math.max(1, p[0]) : 1;
                curRow += n;
                ensureRow(curRow);
                break;
            }
            case 'C': {
                int n = p.length > 0 ? Math.max(1, p[0]) : 1;
                curCol += n;
                break;
            }
            case 'D': {
                int n = p.length > 0 ? Math.max(1, p[0]) : 1;
                curCol = Math.max(0, curCol - n);
                break;
            }
            case 'G': {
                int n = p.length > 0 ? Math.max(1, p[0]) : 1;
                curCol = Math.max(0, n - 1);
                break;
            }
            case 'H':
            case 'f': {
                int row = p.length > 0 ? Math.max(1, p[0]) : 1;
                int col = p.length > 1 ? Math.max(1, p[1]) : 1;
                curRow = Math.max(0, row - 1);
                curCol = Math.max(0, col - 1);
                ensureRow(curRow);
                break;
            }
            case 'J': {
                int mode = p.length > 0 ? p[0] : 0;
                if (mode == 0) eraseBelow();
                else if (mode == 2 || mode == 3) {
                    buffer.clear();
                    curRow = 0;
                    curCol = 0;
                    ensureRow(0);
                }
                break;
            }
            case 'K': {
                int mode = p.length > 0 ? p[0] : 0;
                if (mode == 0) eraseLineToEnd();
                else if (mode == 2) {
                    ensureRow(curRow);
                    buffer.get(curRow).clear();
                    curCol = 0;
                }
                break;
            }
            case 'm':
                applySgr(p);
                break;
            case 'q':
                /* DECSCUSR — cursor shape; we always draw a block */
                cursorVisible = true;
                break;
            default:
                break;
        }
    }

    private static int[] parseParams(String params) {
        if (params == null || params.isEmpty()) return new int[0];
        String[] parts = params.split(";");
        int[] out = new int[parts.length];
        for (int i = 0; i < parts.length; i++) {
            try {
                out[i] = parts[i].isEmpty() ? 0 : Integer.parseInt(parts[i]);
            } catch (NumberFormatException e) {
                out[i] = 0;
            }
        }
        return out;
    }

    private void applySgr(int[] p) {
        if (p.length == 0) {
            fg = FG_DEFAULT;
            return;
        }
        for (int i = 0; i < p.length; i++) {
            int code = p[i];
            if (code == 0 || code == 39) {
                fg = FG_DEFAULT;
            } else if (code == 1) {
                /* bold — keep default for MVP */
            } else if (code == 2) {
                fg = FG_DIM; /* dim / ghost text from jsh */
            } else if (code == 22) {
                fg = FG_DEFAULT;
            } else if (code >= 30 && code <= 37) {
                fg = ansiBasic(code - 30, false);
            } else if (code >= 90 && code <= 97) {
                fg = ansiBasic(code - 90, true);
            } else if (code == 38 && i + 1 < p.length) {
                if (p[i + 1] == 2 && i + 4 < p.length) {
                    fg = Color.rgb(clamp(p[i + 2]), clamp(p[i + 3]), clamp(p[i + 4]));
                    i += 4;
                } else if (p[i + 1] == 5 && i + 2 < p.length) {
                    fg = ansi256(p[i + 2]);
                    i += 2;
                }
            }
        }
    }

    private static int clamp(int v) {
        return Math.max(0, Math.min(255, v));
    }

    private static int ansiBasic(int idx, boolean bright) {
        int[] base = {
                Color.rgb(0, 0, 0),
                Color.rgb(205, 49, 49),
                Color.rgb(13, 188, 121),
                Color.rgb(229, 229, 16),
                Color.rgb(36, 114, 200),
                Color.rgb(188, 63, 188),
                Color.rgb(17, 168, 205),
                Color.rgb(229, 229, 229)
        };
        int[] brit = {
                Color.rgb(102, 102, 102),
                Color.rgb(241, 76, 76),
                Color.rgb(35, 209, 139),
                Color.rgb(245, 245, 67),
                Color.rgb(59, 142, 234),
                Color.rgb(214, 112, 214),
                Color.rgb(41, 184, 219),
                Color.rgb(255, 255, 255)
        };
        if (idx < 0 || idx > 7) return FG_DEFAULT;
        return bright ? brit[idx] : base[idx];
    }

    private static int ansi256(int n) {
        if (n < 16) return ansiBasic(n % 8, n >= 8);
        return FG_DEFAULT;
    }

    private void trimBuffer() {
        while (buffer.size() > MAX_LINES) {
            buffer.remove(0);
            if (curRow > 0) curRow--;
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (session == null) return super.onKeyDown(keyCode, event);

        if (keyCode == KeyEvent.KEYCODE_ENTER) {
            session.write("\n");
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DEL) {
            session.write("\u007f");
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DPAD_UP) {
            session.write("\u001b[A");
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DPAD_DOWN) {
            session.write("\u001b[B");
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT) {
            session.write("\u001b[C");
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT) {
            session.write("\u001b[D");
            return true;
        }

        int uc = event.getUnicodeChar();
        if (uc != 0 && (event.getMetaState() & KeyEvent.META_CTRL_ON) != 0) {
            char base = Character.toUpperCase((char) uc);
            if (base >= 'A' && base <= 'Z') {
                session.write(new String(new char[]{(char) (base - '@')}));
                return true;
            }
        }
        if (uc >= 32) {
            session.write(new String(Character.toChars(uc)));
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
        return new BaseInputConnection(this, false) {
            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                if (session != null && text != null) session.write(text.toString());
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                if (session != null && beforeLength > 0) {
                    for (int i = 0; i < beforeLength; i++) session.write("\u007f");
                }
                return true;
            }
        };
    }
}
