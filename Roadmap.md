# PowerCode (pwc) — Roadmap เต็ม v0.1.4

> สถานะปัจจุบัน: **Beta 0.1.4 — jsh รันในแอพจริงแล้ว, prompt `~`, FHS ย่อ, PTY+TerminalView ทำงาน**
>
> เอกสารนี้รวมแผนจาก v0.1.1.5 + งานจริงที่ทำระหว่างผูก Android (v0.1.3–0.1.4)

---

## สรุปสิ่งที่ทำไปแล้ว (2026-08-19)

| งาน | รายละเอียด | ผล |
|-----|------------|-----|
| jsh ขยายใหญ่ | control flow, job control, functions, history, tab, pipe/redirect, … | ✅ ทดสอบใน Termux แล้ว |
| `pty_jni.cpp` | openpty / fork / exec / read / write / resize | ✅ |
| `TerminalSession` + `TerminalView` | ผูก PTY ↔ UI | ✅ เปิด shell ในแอพได้ |
| แก้ exec Permission denied | แพ็ก jsh เป็น `libjsh.so` ใน `lib/arm64-v8a/` | ✅ |
| `build.sh` compile jsh.c | ขั้น 3.5 compile เป็น executable แล้ว pack เป็น libjsh.so (ไม่ copy binary เก่า) | ✅ |
| prompt `home` → `~` | `short_prompt_path` + set `HOME` จาก TerminalSession | ✅ `~ !>>` |
| FHS ย่อ | `files/{home, home/.local/bin, usr/bin, bin, tmp}` + env | ✅ |
| pack native ถูก path | `lib/arm64-v8a/` ไม่ใช่ `jniLibs/` ใน APK | ✅ |
| ปิดแถบ title "PowerCode" | theme NoActionBar + `FEATURE_NO_TITLE` | 🚧 กำลังใส่ |

### ปัญหาที่เจอและแก้แล้ว

| # | อาการ | สาเหตุ | วิธีแก้ |
|---|--------|--------|---------|
| 1 | แอพเด้งตอนเปิด | ไม่มี `libc++_shared.so` | copy จาก NDK + `loadLibrary` |
| 2 | duplicate class MainActivity | ไฟล์สอง path | เหลือ `com.pwc.app` อย่างเดียว |
| 3 | Shell failed to start | ไม่มี jsh ใน APK | compile + pack |
| 4 | โหลด `.so` ไม่ได้ | pack เป็น `jniLibs/` | เปลี่ยนเป็น `lib/arm64-v8a/` |
| 5 | `exec failed: Permission denied` | Android บล็อก exec จาก `files/bin/` | แพ็กเป็น `libjsh.so` |
| 6 | prompt ค้างที่ `home` | `build.sh` copy binary เก่าทับทุกครั้ง | compile `jsh.c` ในขั้น 3.5 |

---

## MVP — ขอบเขตก่อนเรียกว่า “ใช้พัฒนาบนมือถือได้”

| ชั้น | สิ่งที่ต้องมี | สถานะ |
|------|----------------|--------|
| **Shell (jsh)** | รันคำสั่ง, pipe/redirect, ตัวแปร, script, job control | ✅ |
| **PTY + Terminal UI** | `pty_jni` + `TerminalView` + session | ✅ |
| **Filesystem (FHS ย่อ)** | `usr/bin`, `home/`, `$PATH` | ✅ สร้างแล้ว (ยังว่าง — ไม่มี toybox) |
| **คำสั่งพื้นฐาน** | toybox ผ่าน `pwc setup` / `pwc install` | ❌ |
| **pwc CLI ขั้นต่ำ** | install / uninstall / search / get / link | 🚧 `pwc link` ใช้ได้; ที่เหลือ stub |
| **Reliability** | Foreground Service | ❌ |
| **UI เต็มจอ** | ไม่มี title bar "PowerCode" | 🚧 |

### ลำดับปิด MVP (จากจุดนี้)

1. ~~exec jsh ในแอพ~~ ✅
2. ~~prompt `~` + HOME + FHS~~ ✅
3. **ปิด title bar + ปรับ theme เทอร์มินัล** ← กำลังทำ
4. AnsiParser subset (สี / clear / cursor)
5. `pwc setup` + toybox
6. `pwc install` ขั้นต่ำ
7. Foreground Service

---

## 0. เครื่องมือที่พร้อมแล้ว (Environment)

- [x] Android NDK r29 (aarch64)
- [x] android.jar API 34
- [x] aapt2, apksigner, d8, zipalign
- [x] `build.sh` — compile jsh.c + native + pack `lib/arm64-v8a` อัตโนมัติ
- [x] minSdk 26 / targetSdk 34
- [x] `AndroidManifest.xml` + `MainActivity` เปิด TerminalView ได้

---

## 1. Terminal core

- [x] `pty_jni.cpp` — openpty / fork / exec / read / write / resize
- [x] exec jsh สำเร็จบนเครื่องจริง (`libjsh.so`)
- [x] prompt `~` + gradient `!>>`
- [ ] `AnsiParser.java` — subset: cursor, สี, clear
- [x] `TerminalView.java` — MVP: mono text + strip CSI เบื้องต้น
- [x] `TerminalSession.java` — ผูก PTY ↔ View + FHS + env
- [ ] `SessionManager.java` — หลายแท็บ (หลัง MVP)
- [ ] custom escape `ESC ] pwc-img ; …` (ทีหลัง)
- [ ] ปิด ActionBar / title "PowerCode" (theme NoActionBar)

## 1.5 Terminal essentials (UI แอพ)

- [x] History / line-edit ฝั่ง jsh (↑↓, shortcuts, ghost text, Tab)
- [ ] Touch cursor positioning
- [ ] ปุ่ม Ctrl+C / Ctrl+D จาก UI
- [x] Continuation prompt ฝั่ง jsh (`>`)
- [x] Bracketed paste ฝั่ง jsh
- [ ] Copy/paste จาก output (UI)

## 1.6 pwcode — in-app editor

- [ ] `pwc code` / `pwcode [file]`
- [ ] ธีมคล้าย VSCode, autocomplete ร่วมหมวด 8

## 1.7 jsh — สถานะจริง

- ภาษา: **C** · นามสกุล **`.jsh`** (แยกจาก `.sh` → ใช้ bash ที่ติดตั้งแยก)
- สถาปัตยกรรม: `Java → JNI → jsh (native) → PTY → child`

### ✅ ทำเสร็จแล้ว (ในแอพ + Termux)

**Core:** fork/exec, `cd`/`pwd`/`exit`/`clear`/`history`, ตัวแปร, `export`, `$?` `$$`, chaining `&&` `||` `;`, pipe, redirect, glob, quote, escape, `#`, `$(...)`, `$((...))`, `~`, command-not-found

**Interactive:** history ถาวร `~/.jsh_history`, Tab + ghost text, Ctrl+R, line-edit shortcuts, line-wrap, bracketed paste

**Job control:** `&`, jobs, fg, bg, Ctrl+Z, Ctrl+C แยก process group

**Scripting:** if/while/for, break/continue, functions + `$1`…, source/., wait, here-doc, subshell, set -e/-x, read, type/which/command/builtin/hash, alias

**Prompt:** path ย่อเป็น `~` / `~/...`, สัญลักษณ์ `!>>` ไล่เฉดสี

### ⚠️ ข้อจำกัดที่ตั้งใจไว้

- `$(...)` ไม่ re-split เป็นหลาย argument
- Builtin ใน pipeline ไม่ได้
- ไม่ใช่ POSIX/bash เต็ม — `.sh` ใช้ bash แยก

### 🔮 อนาคต (ไม่บล็อก MVP)

1. พฤติกรรม background ตอน `exit`
2. `~user` tilde
3. Arrays เบาๆ
4. `[[ ... ]]`
5. IFS / field splitting
6. Full POSIX mode — ใช้ `pwc install bash` แทน

## 1.8–1.11

- Multi-session UI, `pwc get-a`, Foreground Service, Localization — ตามแผนเดิม (หลังแกน terminal นิ่ง)

---

## 3. Filesystem (FHS ย่อใน sandbox แอพ)

```
/data/data/com.pwc.app/files/
├── usr/bin/           ← โปรแกรมจาก pwc install
├── home/              ← $HOME → prompt ~
│   └── .local/bin/    ← pwc link
├── bin/               ← fallback (เลี่ยง exec ที่นี่บน Android ใหม่)
└── tmp/
```

- [x] สร้าง `home/`, `tmp/`, `usr/bin`, `bin`, `home/.local/bin` ตอน start session
- [x] ตั้ง `$HOME` `$PWD` `$PATH` `$TMPDIR` `$TERM` `$LANG` `$USER` `$SHELL`
- [ ] มี toybox ใน `usr/bin` หลัง `pwc setup`
- [ ] Path convention: มี `/` → exec ตรง, ไม่มี → ค้น `$PATH` (jsh ทำอยู่แล้วบางส่วน)

**หมายเหตุ:** จาก `~` โฟลเดอร์ `bin`/`usr` อยู่ระดับบน → `ls ../bin`, `ls ../usr/bin`

---

## 4. pwc CLI

| คำสั่ง | สถานะ |
|--------|--------|
| `pwc link <file>` | ✅ |
| install / uninstall / search / update / get / get-a / set graphic | ❌ stub |
| `pwcode [file]` | ❌ ยังไม่เริ่ม |

- [ ] ExtensionManager / Registry / `.pwck`

---

## 5–11. Coreutils, Graphics, AI, Editor, Input, UI, GitHub

(แผนเดิมจาก v0.1.1.5 — ทำหลัง MVP terminal นิ่ง)

- toybox bundle + symlink ใน `usr/bin`
- OpenGL ES default; Vulkan / NNAPI ทางเลือก
- llama.cpp เป็น extension
- EditorView + autocomplete แบบ word ก่อน; LSP ท้ายสุด
- ModifierKeyBar, Settings drawer, GitHub OAuth

---

## 12. Performance / `.pwck` / setup

- dirty-region + SurfaceView เมื่อ TerminalView โต
- `.pwck` atomic install
- `pwc setup` bootstrap ครั้งแรก (toybox + essentials)

---

## 13. Brand / Funding / Community

- **PowerCode by J2K-studio**
- โดเนทสมัครใจ ไม่มีโฆษณา / pro lock
- `pwc catnip` — เริ่มจากลิงก์นอกได้

---

## 14. J2k (AI assist extension, offline)

- ทำหลัง `.pwck` + python เสถียร; consent ก่อน action

---

## ลำดับแนะนำให้เริ่มทำจริง (อัปเดต v0.1.4)

**ตอนนี้**
1. ปิด title bar "PowerCode" (theme + MainActivity)
2. ปรับ UI โทน PowerShell/CMD/Termux (สีพื้น, ฟอนต์) โดยคง `~ !>>`

**ปิด MVP**
3. AnsiParser subset
4. `pwc setup` + toybox
5. `pwc install` ขั้นต่ำ
6. Foreground Service

**หลัง MVP**
7. Multi-session, modifier bar, copy-paste UI
8. pwcode editor
9. `.pwck` + registry
10. Graphics / web / GitHub / J2k

---

*อัปเดตล่าสุด: 2026-08-19 — v0.1.4 รวม dev log ผูกแอพ + prompt ~ + FHS + build.sh compile jsh.c*
