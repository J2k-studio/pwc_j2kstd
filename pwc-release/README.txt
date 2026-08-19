PowerCode v0.1.4 — Roadmap ใหม่ + MainActivity เต็มจอ
=====================================================

ไฟล์ในแพ็กนี้
-------------
Roadmap.md                          ← เอกสารรวมแผน + สิ่งที่ทำแล้วทั้งหมด
app/src/main/java/.../MainActivity.java
app/src/main/AndroidManifest.xml
app/src/main/res/values/styles.xml
app/src/main/res/values/colors.xml

วิธีวาง
-------
cd ~/PowerCode

cp Roadmap.md .

mkdir -p app/src/main/res/values
cp app/src/main/res/values/styles.xml   ~/PowerCode/app/src/main/res/values/
cp app/src/main/res/values/colors.xml   ~/PowerCode/app/src/main/res/values/
cp app/src/main/AndroidManifest.xml     ~/PowerCode/app/src/main/

# MainActivity — ทับได้ถ้าของเดิมเป็น stub
# ถ้าของเดิม wire TerminalView ซับซ้อน ส่งไฟล์มาให้ช่วย merge
cp app/src/main/java/com/pwc/app/MainActivity.java \
   ~/PowerCode/app/src/main/java/com/pwc/app/

bash build.sh
cp build/PowerCode.apk /sdcard/
# uninstall เก่า → ติดตั้งใหม่

หมายเหตุ MainActivity
---------------------
เรียก session.setListener(terminalView)
และพยายามเรียก setSession / attachSession ถ้ามีใน TerminalView ของคุณ
ถ้า compile error เรื่อง method → ส่ง TerminalView.java มา จะปรับให้ตรง

