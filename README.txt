PowerCode fix v2 — prompt "~" + FHS + build.sh compile jsh.c
=============================================================

สาเหตุที่ prompt ยังเป็น "home"
--------------------------------
build.sh เดิมไม่ได้ compile jsh.c!
มัน copy binary เก่าจาก:
  1) app/src/main/assets/jsh
  2) หรือ ./jsh
ไปเป็น libjsh.so ทุกครั้งที่ bash build.sh

ดังนั้นแม้คุณ compile libjsh.so เอง รอบถัดไปที่รัน build.sh
ก็ถูกทับด้วย binary เก่า → prompt ไม่เปลี่ยน

สิ่งที่แก้
---------
1. build.sh
   - ขั้น [3.5/7] compile jsh.c เป็น executable (-fPIE -pie)
     แล้ววางเป็น libjsh.so
   - sync ไป assets/jsh และ ./jsh ด้วย
   - ห้ามใช้ -shared (jsh ต้องมี main() เพื่อ exec)

2. TerminalSession.java
   - สร้าง FHS ย่อครบ:
       files/usr/bin/
       files/home/          ← $HOME → prompt ~
       files/home/.local/bin/
       files/bin/
       files/tmp/
   - set HOME, PWD, PATH (local/bin ก่อน), TMPDIR, TERM, LANG, USER, SHELL

3. jsh.c
   - short_prompt_path รองรับ $HOME + heuristic /files/home → ~

วิธีใช้
-------
cd ~/PowerCode

# ทับไฟล์
cp /path/to/pwc-fix-v2/build.sh .
cp /path/to/pwc-fix-v2/jsh.c .
cp /path/to/pwc-fix-v2/TerminalSession.java \
   app/src/main/java/com/pwc/app/terminal/

# บิลด์ (จะ compile jsh.c ให้อัตโนมัติ)
bash build.sh

# ติดตั้งใหม่ — แนะนำ uninstall เก่าก่อน
cp build/PowerCode.apk /sdcard/

ผลที่ควรได้
-----------
~ !>>
~/kk !>>

และโฟลเดอร์:
  /data/data/com.pwc.app/files/home/
  /data/data/com.pwc.app/files/home/.local/bin/
  /data/data/com.pwc.app/files/usr/bin/
  /data/data/com.pwc.app/files/tmp/

