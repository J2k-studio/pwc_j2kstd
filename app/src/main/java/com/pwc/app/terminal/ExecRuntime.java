package com.pwc.app.terminal;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Runtime ที่ช่วยให้ binary ที่ "ดึงมาลง" รันได้บน Android
 *
 * ข้อจำกัดระบบ:
 *   - files/usr/bin  มัก exec ไม่ได้ (EACCES) ตั้งแต่ Android 10+
 *   - nativeLibraryDir รันได้ แต่เขียนเพิ่มหลังติดตั้งแอพไม่ได้ (PM จัดการ)
 *
 * กลยุทธ์ PowerCode:
 *   1) Core ใน APK          → lib/*.so (jsh, pwc, toybox)
 *   2) ของที่ดาวน์โหลดทีหลัง → stage ไปที่ codeCacheDir/bin/ แล้ว chmod +x
 *      (codeCache เป็น candidate ที่ดีที่สุดรองจาก nativeLibraryDir)
 *   3) ชื่อสวยบน PATH       → symlink จาก $HOME/.local/bin/<name>
 *
 * ถ้า stage แล้วยัง exec ไม่ได้บนบาง OEM → แสดงข้อความชัดเจน
 * (ต้องห่อเป็น lib*.so ใน APK หรือรันผ่าน interpreter ที่เป็น lib*.so แล้ว)
 */
public final class ExecRuntime {

    private static final String TAG = "ExecRuntime";

    private ExecRuntime() {}

    /** .../code_cache/bin — ปลายทางหลักของ binary ที่ติดตั้งทีหลัง */
    public static File binDir(Context context) {
        File dir = new File(context.getCodeCacheDir(), "bin");
        if (!dir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
        }
        return dir;
    }

    public static File localBin(Context context) {
        File dir = new File(context.getFilesDir(), "home/.local/bin");
        if (!dir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
        }
        return dir;
    }

    /**
     * คัดลอกไฟล์เข้า codeCache/bin/<name> แล้ว chmod 755
     * จากนั้น symlink ไปที่ ~/.local/bin/<name> เพื่อให้อยู่บน PATH
     *
     * @return path ของไฟล์ที่ stage แล้ว (codeCache) หรือ null ถ้าล้มเหลว
     */
    public static File stageExecutable(Context context, File src, String name) {
        if (src == null || !src.exists() || name == null || name.isEmpty()) {
            return null;
        }
        // กัน path traversal ในชื่อ
        name = new File(name).getName();
        if (name.isEmpty() || name.equals(".") || name.equals("..")) {
            return null;
        }

        File dest = new File(binDir(context), name);
        try {
            copyFile(src, dest);
            chmod755(dest);
        } catch (Exception e) {
            Log.e(TAG, "stage copy failed: " + e.getMessage(), e);
            return null;
        }

        // symlink ชื่อสวยบน PATH
        File link = new File(localBin(context), name);
        try {
            if (link.exists()) {
                //noinspection ResultOfMethodCallIgnored
                link.delete();
            }
            Os.symlink(dest.getAbsolutePath(), link.getAbsolutePath());
            Log.i(TAG, "staged " + name + " → " + dest + " (link " + link + ")");
        } catch (Throwable t) {
            Log.w(TAG, "symlink failed (binary still in codeCache): " + t.getMessage());
        }

        return dest;
    }

    /** stage จาก InputStream (เช่น ดาวน์โหลด / asset) */
    public static File stageExecutable(Context context, InputStream in, String name) {
        if (in == null || name == null) return null;
        name = new File(name).getName();
        File tmp = new File(context.getCacheDir(), "stage-" + name + ".tmp");
        try {
            try (OutputStream out = new FileOutputStream(tmp)) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                out.flush();
            }
            return stageExecutable(context, tmp, name);
        } catch (Exception e) {
            Log.e(TAG, "stage from stream failed: " + e.getMessage(), e);
            return null;
        } finally {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
        }
    }

    /**
     * PATH เสริมสำหรับ session: codeCache/bin + nativeLibraryDir + local/bin
     * (เรียกจาก TerminalSession ตอนประกอบ env)
     */
    public static String pathPrefix(Context context) {
        ApplicationInfo ai = context.getApplicationInfo();
        StringBuilder sb = new StringBuilder();
        sb.append(localBin(context).getAbsolutePath());
        File cbin = binDir(context);
        sb.append(':').append(cbin.getAbsolutePath());
        if (ai.nativeLibraryDir != null) {
            sb.append(':').append(ai.nativeLibraryDir);
        }
        return sb.toString();
    }

    public static void chmod755(File f) {
        //noinspection ResultOfMethodCallIgnored
        f.setReadable(true, false);
        //noinspection ResultOfMethodCallIgnored
        f.setExecutable(true, false);
        try {
            Os.chmod(f.getAbsolutePath(), 0755);
        } catch (Throwable ignored) {
        }
        try {
            Runtime.getRuntime().exec(new String[]{
                    "/system/bin/chmod", "755", f.getAbsolutePath()
            }).waitFor();
        } catch (Throwable ignored) {
        }
    }

    private static void copyFile(File src, File dest) throws Exception {
        File parent = dest.getParentFile();
        if (parent != null && !parent.exists()) {
            //noinspection ResultOfMethodCallIgnored
            parent.mkdirs();
        }
        try (InputStream in = new FileInputStream(src);
             OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
        }
    }
}
