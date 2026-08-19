#!/data/data/com.termux/files/usr/bin/bash
set -e

# ── PowerCode build pipeline (Termux, no Gradle) ──────────────────────

PKG_NAME="com.pwc.app"
APP_NAME="PowerCode"

NDK="${NDK:-$HOME/android-ndk-r29}"
ANDROID_JAR="${ANDROID_JAR:-$HOME/android-jar/android.jar}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-aarch64/bin"
API=26
TRIPLE="aarch64-linux-android"

SRC_JAVA="app/src/main/java"
SRC_CPP="app/src/main/cpp"
SRC_RES="app/src/main/res"
SRC_ASSETS="app/src/main/assets"
MANIFEST="app/src/main/AndroidManifest.xml"

BUILD="build"
OUT_CLASSES="$BUILD/classes"
OUT_JNILIBS="app/src/main/jniLibs/arm64-v8a"
KEYSTORE="debug.keystore"

echo "== [1/7] clean =="
rm -rf "$BUILD"
mkdir -p "$OUT_CLASSES" "$OUT_JNILIBS"

echo "== [2/7] package NDK shared C++ runtime (libc++_shared.so) =="
LIBCXX_SHARED=""
for candidate in \
  "$NDK/toolchains/llvm/prebuilt/linux-aarch64/sysroot/usr/lib/${TRIPLE}/libc++_shared.so" \
  "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/${TRIPLE}/libc++_shared.so" \
  "$NDK/sources/cxx-stl/llvm-libc++/libs/arm64-v8a/libc++_shared.so"
do
  if [ -f "$candidate" ]; then
    LIBCXX_SHARED="$candidate"
    break
  fi
done
if [ -z "$LIBCXX_SHARED" ]; then
  LIBCXX_SHARED=$(find "$NDK" -path "*aarch64*" -name "libc++_shared.so" 2>/dev/null | head -1 || true)
fi
if [ -n "$LIBCXX_SHARED" ] && [ -f "$LIBCXX_SHARED" ]; then
  cp -f "$LIBCXX_SHARED" "$OUT_JNILIBS/libc++_shared.so"
  echo "  copied → $OUT_JNILIBS/libc++_shared.so"
else
  echo "  WARNING: libc++_shared.so not found"
fi

echo "== [3/7] compile native (.so) via NDK =="
if ls "$SRC_CPP"/*.cpp >/dev/null 2>&1; then
  for f in "$SRC_CPP"/*.cpp; do
    name=$(basename "$f" .cpp)
    echo "  compiling $f → lib${name}.so"
    "$TOOLCHAIN/${TRIPLE}${API}-clang++" \
      -shared -fPIC -std=c++17 -O2 \
      -Wl,-soname,lib${name}.so \
      -o "$OUT_JNILIBS/lib${name}.so" \
      "$f" -llog -lc++_shared
  done
else
  echo "  (no .cpp files yet, skipping)"
fi

# ── Compile jsh.c → executable, then ship as libjsh.so ──────────────
# IMPORTANT: do NOT use -shared. jsh is exec'd as a program (main()).
# Packaging as libjsh.so is only so PackageManager extracts it +x-able.
echo "== [3.5/7] compile jsh.c → libjsh.so =="
if [ -f jsh.c ]; then
  echo "  compiling jsh.c (executable, not shared lib)..."
  "$TOOLCHAIN/${TRIPLE}${API}-clang" \
    -O2 -fPIE -pie \
    -o "$OUT_JNILIBS/libjsh.so" \
    jsh.c -llog
  chmod 755 "$OUT_JNILIBS/libjsh.so"
  # also refresh assets/jsh + ./jsh so fallbacks stay in sync
  mkdir -p "$SRC_ASSETS"
  cp -f "$OUT_JNILIBS/libjsh.so" "$SRC_ASSETS/jsh"
  cp -f "$OUT_JNILIBS/libjsh.so" ./jsh
  chmod 755 "$SRC_ASSETS/jsh" ./jsh
  echo "  jsh.c → $OUT_JNILIBS/libjsh.so ($(wc -c < "$OUT_JNILIBS/libjsh.so") bytes)"
else
  # fallback: copy prebuilt binary if no source
  JSH_SRC=""
  if [ -f app/src/main/assets/jsh ]; then
    JSH_SRC="app/src/main/assets/jsh"
  elif [ -f jsh ]; then
    JSH_SRC="jsh"
  fi
  if [ -n "$JSH_SRC" ]; then
    cp -f "$JSH_SRC" "$OUT_JNILIBS/libjsh.so"
    chmod 755 "$OUT_JNILIBS/libjsh.so"
    echo "  (no jsh.c) $JSH_SRC → $OUT_JNILIBS/libjsh.so"
  else
    echo "  WARNING: no jsh.c and no prebuilt jsh binary"
  fi
fi


# ── Compile pwc.c → libpwc.so (exec-able) + assets/pwc fallback ──
# Android blocks exec from files/usr/bin (same EACCES as jsh).
# Package as native lib so PM extracts it under nativeLibraryDir with +x.
echo "== [3.6/7] compile pwc.c → libpwc.so =="
mkdir -p "$SRC_ASSETS" "$OUT_JNILIBS"
if [ -f pwc.c ]; then
  echo "  compiling pwc.c..."
  "$TOOLCHAIN/${TRIPLE}${API}-clang" \
    -O2 -fPIE -pie \
    -o "$OUT_JNILIBS/libpwc.so" \
    pwc.c
  chmod 755 "$OUT_JNILIBS/libpwc.so"
  cp -f "$OUT_JNILIBS/libpwc.so" "$SRC_ASSETS/pwc"
  cp -f "$OUT_JNILIBS/libpwc.so" ./pwc
  echo "  pwc.c → $OUT_JNILIBS/libpwc.so ($(wc -c < "$OUT_JNILIBS/libpwc.so") bytes)"
elif [ -f pwc ]; then
  cp -f pwc "$OUT_JNILIBS/libpwc.so"
  cp -f pwc "$SRC_ASSETS/pwc"
  chmod 755 "$OUT_JNILIBS/libpwc.so" "$SRC_ASSETS/pwc"
  echo "  (prebuilt) pwc → libpwc.so"
else
  echo "  WARNING: no pwc.c / pwc — skip"
fi

echo "== [4/7] compile java =="
JAVA_FILES=$(find "$SRC_JAVA" -name "*.java")
if [ -n "$JAVA_FILES" ]; then
  javac -classpath "$ANDROID_JAR" -d "$OUT_CLASSES" $JAVA_FILES
else
  echo "  (no .java files yet, skipping)"
fi

echo "== [5/7] dex (d8) =="
if [ -d "$OUT_CLASSES" ] && [ "$(find "$OUT_CLASSES" -name '*.class')" ]; then
  d8 --lib "$ANDROID_JAR" --output "$BUILD" $(find "$OUT_CLASSES" -name "*.class")
else
  echo "  (no classes yet, skipping)"
fi

echo "== [6/7] package resources + link (aapt2) =="
if [ -d "$SRC_RES" ] && [ "$(find "$SRC_RES" -type f 2>/dev/null | head -1)" ]; then
  aapt2 compile --dir "$SRC_RES" -o "$BUILD/res.zip"
fi

AAPT2_LINK_ARGS=(-o "$BUILD/app-unsigned.apk" -I "$ANDROID_JAR" --manifest "$MANIFEST")
[ -f "$BUILD/res.zip" ] && AAPT2_LINK_ARGS+=("$BUILD/res.zip")
[ -d "$SRC_ASSETS" ] && AAPT2_LINK_ARGS+=(-A "$SRC_ASSETS")

aapt2 link "${AAPT2_LINK_ARGS[@]}"

echo "== [6.5/7] inject classes.dex + native libs =="
cd "$BUILD"
[ -f classes.dex ] && zip -j app-unsigned.apk classes.dex
cd - >/dev/null

# Android loads native libs ONLY from lib/<abi>/ inside the APK.
if [ -d "$OUT_JNILIBS" ] && [ "$(ls -A "$OUT_JNILIBS" 2>/dev/null)" ]; then
  echo "  jniLibs build dir:"
  ls -la "$OUT_JNILIBS"
  mkdir -p "$BUILD/apk-native/lib/arm64-v8a"
  cp -f "$OUT_JNILIBS"/*.so "$BUILD/apk-native/lib/arm64-v8a/"
  (cd "$BUILD/apk-native" && zip -r "../app-unsigned.apk" lib)
  echo "  packed as lib/arm64-v8a/ (not jniLibs/)"
fi

echo "== [7/7] zipalign + sign =="
zipalign -f 4 "$BUILD/app-unsigned.apk" "$BUILD/app-aligned.apk"

if [ ! -f "$KEYSTORE" ]; then
  echo "  no debug.keystore found, generating one..."
  keytool -genkey -v -keystore "$KEYSTORE" -alias pwcdebug \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass android -keypass android \
    -dname "CN=PowerCode Debug,O=pwc,C=TH"
fi

apksigner sign --ks "$KEYSTORE" --ks-pass pass:android \
  --out "$BUILD/$APP_NAME.apk" "$BUILD/app-aligned.apk"

echo ""
echo "✅ built: $BUILD/$APP_NAME.apk"
echo "   contents (libs + assets):"
unzip -l "$BUILD/$APP_NAME.apk" 2>/dev/null | grep -E 'lib/|assets/|\.so' || true
