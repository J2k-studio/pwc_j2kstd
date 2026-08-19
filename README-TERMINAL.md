# PowerCode — Terminal MVP slice

## สิ่งที่เพิ่มแล้ว
- `app/src/main/cpp/pty_jni.cpp` — openpty + fork/exec jsh + read/write/resize
- `app/src/main/java/com/pwc/app/terminal/TerminalSession.java`
- `app/src/main/java/com/pwc/app/terminal/TerminalView.java`
- `MainActivity` เปิด TerminalView แทน TextView
- `jsh.c` sync เป็นเวอร์ชันล่าสุด (~4500 บรรทัด)

## สิ่งที่คุณต้องมี / ต้องทำ

### 1) เครื่อง build (Termux บน ARM64 ตามเดิม)
- Android NDK r29 ที่ path `$HOME/android-ndk-r29` (ตาม `build.sh`)
- `android.jar`, `aapt2`, `d8`, `zipalign`, `apksigner` ตามที่เคยใช้ได้แล้ว

### 2) binary `jsh` แบบ **aarch64 Android** ใส่ assets
```bash
cd ~/PowerCode   # หรือโฟลเดอร์ที่แตก zip นี้

# compile jsh ด้วย NDK (สำคัญ: ต้องเป็น Android target ไม่ใช่ binary ของ Termux ตรงๆ ถ้า linker คนละตัว)
$NDK/toolchains/llvm/prebuilt/linux-aarch64/bin/aarch64-linux-android26-clang \
  -O2 -o app/src/main/assets/jsh jsh.c

chmod +x app/src/main/assets/jsh
```
ถ้า compile ด้วย clang ของ Termux แล้วรันในแอพไม่ได้ ให้ใช้ NDK clang ตามด้านบน

### 3) บิลด์ APK
```bash
./build.sh
# ได้ build/PowerCode.apk
```

### 4) ติดตั้ง + ทดสอบ
```bash
adb install -r build/PowerCode.apk
# หรือคัดลอก APK ไปติดตั้งบนเครื่อง
```
เปิดแอพ → แตะจอให้แป้นพิมพ์ขึ้น → ควรเห็น prompt jsh

### 5) ถ้าไม่ขึ้น shell
- ดู `adb logcat -s pwc-pty TerminalSession`
- ตรวจว่า `assets/jsh` อยู่ใน APK: `unzip -l build/PowerCode.apk | grep jsh`
- ตรวจ `libpty_jni.so`: `unzip -l build/PowerCode.apk | grep pty`

## ยังไม่ทำในรอบนี้
- AnsiParser เต็ม (ตอนนี้แค่ strip CSI)
- Modifier key bar / multi-session
- pwc install / toybox ในแอพ
- Foreground Service
