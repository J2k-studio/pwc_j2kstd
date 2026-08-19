package com.pwc.app.terminal;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.Handler;
import android.os.Looper;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Owns the native PTY + reader thread. Feeds bytes to a listener (TerminalView).
 *
 * jsh is packaged as lib/arm64-v8a/libjsh.so so the package manager extracts it
 * with execute permission. Executing from files/ is blocked on many Android builds
 * (EACCES / Permission denied).
 *
 * FHS-lite layout under getFilesDir():
 *   files/
 *   ├── usr/bin/          ← external commands (after pwc setup / stage)
 *   ├── home/             ← $HOME  → prompt shows \~
 *   │   └── .local/bin/   ← pwc + toybox symlinks (first on PATH)
 *   ├── bin/              ← fallback only
 *   └── tmp/
 *
 * Env for pwc setup:
 *   PWC_NATIVE_LIB = nativeLibraryDir  (libjsh.so / libpwc.so / libtoybox.so)
 *   PWC_BIN        = codeCacheDir/bin  (runtime stage target)
 */
public class TerminalSession {

    public interface Listener {
        void onOutput(byte[] data, int len);
        void onExit();
    }

    private static final String TAG = "TerminalSession";

    private static String loadError = null;

    static {
        try {
            System.loadLibrary("c++_shared");
            System.loadLibrary("pty_jni");
        } catch (UnsatisfiedLinkError e) {
            loadError = e.getMessage();
            Log.e(TAG, "native load failed: " + loadError, e);
        } catch (Throwable t) {
            loadError = t.getClass().getSimpleName() + ": " + t.getMessage();
            Log.e(TAG, "native load failed: " + loadError, t);
        }
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private Listener listener;
    private Thread readerThread;
    private volatile boolean running;

    private native int nativeStart(String shellPath, String cwd, String[] envp);
    private native int nativeRead(byte[] buf);
    private native int nativeWrite(byte[] buf, int off, int len);
    private native void nativeResize(int rows, int cols);
    private native void nativeStop();
    private native int nativeIsRunning();

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    public boolean start(Context context) {
        if (loadError != null) {
            Log.e(TAG, "cannot start — native libs failed: " + loadError);
            return false;
        }

        String jshPath = findJshExecutable(context);
        if (jshPath == null) {
            Log.e(TAG, "no jsh executable found (libjsh.so / assets fallback failed)");
            return false;
        }
        Log.i(TAG, "using shell: " + jshPath);

        /* ── FHS-lite sandbox ─────────────────────────────────────── */
        File filesDir = context.getFilesDir();
        File home     = mkdir(new File(filesDir, "home"));
        File localBin = mkdir(new File(home, ".local/bin"));
        File usrBin   = mkdir(new File(filesDir, "usr/bin"));
        File binDir   = mkdir(new File(filesDir, "bin"));
        File tmp      = mkdir(new File(filesDir, "tmp"));

        ApplicationInfo ai = context.getApplicationInfo();
        String nativeLibDir = ai.nativeLibraryDir;

        /* pwc: libpwc.so → \~/.local/bin/pwc (exec from files/ is blocked) */
        File libPwc = nativeLibDir != null
                ? new File(nativeLibDir, "libpwc.so") : null;
        File pwcLink = new File(localBin, "pwc");
        if (libPwc != null && libPwc.exists() && libPwc.length() > 0) {
            makeExecutable(libPwc);
            try {
                if (pwcLink.exists()) {
                    //noinspection ResultOfMethodCallIgnored
                    pwcLink.delete();
                }
                Os.symlink(libPwc.getAbsolutePath(), pwcLink.getAbsolutePath());
                Log.i(TAG, "pwc symlink " + pwcLink + " → " + libPwc);
            } catch (Throwable t) {
                Log.w(TAG, "pwc symlink failed: " + t.getMessage());
                copyAsset(context, "pwc", new File(usrBin, "pwc"));
            }
        } else {
            File pwcDest = new File(usrBin, "pwc");
            if (copyAsset(context, "pwc", pwcDest)) {
                makeExecutable(pwcDest);
                Log.w(TAG, "libpwc.so missing — extracted assets/pwc (may fail exec)");
            }
        }

        /* PATH: \~/.local/bin → codeCache/bin → nativeLibraryDir → usr/bin → … */
        String homePath = home.getAbsolutePath();
        File runtimeBin = ExecRuntime.binDir(context);
        String path = ExecRuntime.pathPrefix(context)
                + ":" + usrBin.getAbsolutePath()
                + ":" + binDir.getAbsolutePath()
                + ":/system/bin:/system/xbin";

        List<String> env = new ArrayList<>();
        env.add("HOME=" + homePath);
        env.add("PWD=" + homePath);
        env.add("TMPDIR=" + tmp.getAbsolutePath());
        env.add("PWC_BIN=" + runtimeBin.getAbsolutePath());
        env.add("PATH=" + path);

        /* pwc setup หา libtoybox.so จาก $PWC_NATIVE_LIB ก่อน */
        if (nativeLibDir != null && !nativeLibDir.isEmpty()) {
            env.add("PWC_NATIVE_LIB=" + nativeLibDir);
            Log.i(TAG, "PWC_NATIVE_LIB=" + nativeLibDir);
        }

        env.add("TERM=xterm-256color");
        env.add("LANG=en_US.UTF-8");
        env.add("USER=pwc");
        env.add("SHELL=jsh");

        Log.i(TAG, "HOME=" + homePath + " PATH=" + path);

        int rc = nativeStart(jshPath, homePath, env.toArray(new String[0]));
        if (rc != 0) {
            Log.e(TAG, "nativeStart failed for " + jshPath);
            return false;
        }

        running = true;
        readerThread = new Thread(this::readerLoop, "pwc-pty-reader");
        readerThread.setDaemon(true);
        readerThread.start();
        return true;
    }

    private static File mkdir(File dir) {
        if (!dir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
        }
        return dir;
    }

    /**
     * 1) libjsh.so from nativeLibraryDir (best — extracted by PackageManager)
     * 2) fallback: assets/jsh → files/bin/jsh + chmod
     */
    private static String findJshExecutable(Context context) {
        ApplicationInfo ai = context.getApplicationInfo();
        if (ai.nativeLibraryDir != null) {
            File libJsh = new File(ai.nativeLibraryDir, "libjsh.so");
            if (libJsh.exists() && libJsh.length() > 0) {
                makeExecutable(libJsh);
                Log.i(TAG, "libjsh.so exists canExecute=" + libJsh.canExecute()
                        + " path=" + libJsh.getAbsolutePath()
                        + " size=" + libJsh.length());
                return libJsh.getAbsolutePath();
            }
            Log.w(TAG, "libjsh.so missing in " + ai.nativeLibraryDir);
        }

        File binDir = new File(context.getFilesDir(), "bin");
        if (!binDir.exists()) binDir.mkdirs();
        File jsh = new File(binDir, "jsh");
        if (!copyAsset(context, "jsh", jsh)) {
            return null;
        }
        makeExecutable(jsh);
        return jsh.getAbsolutePath();
    }

    private static void makeExecutable(File f) {
        //noinspection ResultOfMethodCallIgnored
        f.setReadable(true, false);
        //noinspection ResultOfMethodCallIgnored
        f.setExecutable(true, false);
        try {
            Os.chmod(f.getAbsolutePath(), 0755);
        } catch (Throwable t) {
            Log.w(TAG, "Os.chmod: " + t.getMessage());
        }
        try {
            Process p = Runtime.getRuntime().exec(new String[]{
                    "/system/bin/chmod", "755", f.getAbsolutePath()
            });
            p.waitFor();
        } catch (Throwable ignored) {
        }
    }

    private static boolean copyAsset(Context context, String assetName, File dest) {
        try {
            if (dest.exists()) {
                //noinspection ResultOfMethodCallIgnored
                dest.delete();
            }
            try (InputStream in = context.getAssets().open(assetName);
                 FileOutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                out.flush();
                out.getFD().sync();
            }
            return dest.exists() && dest.length() > 0;
        } catch (Exception e) {
            Log.e(TAG, "copyAsset " + assetName, e);
            return false;
        }
    }

    private void readerLoop() {
        byte[] buf = new byte[4096];
        while (running) {
            int n = nativeRead(buf);
            if (n > 0) {
                final int len = n;
                final byte[] copy = new byte[len];
                System.arraycopy(buf, 0, copy, 0, len);
                mainHandler.post(() -> {
                    if (listener != null) listener.onOutput(copy, len);
                });
            } else if (n < 0) {
                break;
            } else {
                try {
                    Thread.sleep(10);
                } catch (InterruptedException e) {
                    break;
                }
            }
            if (nativeIsRunning() == 0) break;
        }
        running = false;
        mainHandler.post(() -> {
            if (listener != null) listener.onExit();
        });
    }

    public void write(byte[] data, int off, int len) {
        if (!running || len <= 0) return;
        nativeWrite(data, off, len);
    }

    public void write(String s) {
        byte[] b = s.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        write(b, 0, b.length);
    }

    public void resize(int rows, int cols) {
        nativeResize(rows, cols);
    }

    public void stop() {
        running = false;
        nativeStop();
        if (readerThread != null) {
            try {
                readerThread.join(500);
            } catch (InterruptedException ignored) {
            }
            readerThread = null;
        }
    }

    public boolean isRunning() {
        return running && nativeIsRunning() != 0;
    }
}
