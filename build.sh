#!/data/data/com.termux/files/usr/bin/bash
set -e

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
PREBUILT="prebuilt"

BUILD="build"
OUT_CLASSES="$BUILD/classes"
OUT_JNILIBS="app/src/main/jniLibs/arm64-v8a"
KEYSTORE="debug.keystore"

echo "== [1/8] clean =="
rm -rf "$BUILD"
mkdir -p "$OUT_CLASSES" "$OUT_JNILIBS"

echo "== [2/8] package NDK shared C++ runtime =="
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
  echo "  copied -> $OUT_JNILIBS/libc++_shared.so"
else
  echo "  WARNING: libc++_shared.so not found"
fi

echo "== [3/8] compile native (.so) via NDK =="
if ls "$SRC_CPP"/*.cpp >/dev/null 2>&1; then
  for f in "$SRC_CPP"/*.cpp; do
    name=$(basename "$f" .cpp)
    echo "  compiling $f -> lib${name}.so"
    "$TOOLCHAIN/${TRIPLE}${API}-clang++" \
      -shared -fPIC -std=c++17 -O2 \
      -Wl,-soname,lib${name}.so \
      -o "$OUT_JNILIBS/lib${name}.so" \
      "$f" -llog -lc++_shared
  done
else
  echo "  (no .cpp files yet, skipping)"
fi

echo "== [3.5/8] compile jsh.c -> libjsh.so =="
if [ -f jsh.c ]; then
  echo "  compiling jsh.c..."
  "$TOOLCHAIN/${TRIPLE}${API}-clang" \
    -O2 -fPIE -pie \
    -o "$OUT_JNILIBS/libjsh.so" \
    jsh.c -llog
  chmod 755 "$OUT_JNILIBS/libjsh.so"
  mkdir -p "$SRC_ASSETS"
  cp -f "$OUT_JNILIBS/libjsh.so" "$SRC_ASSETS/jsh"
  cp -f "$OUT_JNILIBS/libjsh.so" ./jsh
  chmod 755 "$SRC_ASSETS/jsh" ./jsh
  echo "  jsh OK"
else
  echo "  WARNING: no jsh.c"
fi

echo "== [3.6/8] compile pwc.c -> libpwc.so =="
mkdir -p "$SRC_ASSETS" "$OUT_JNILIBS"
if [ -f pwc.c ]; then
  "$TOOLCHAIN/${TRIPLE}${API}-clang" \
    -O2 -fPIE -pie \
    -o "$OUT_JNILIBS/libpwc.so" \
    pwc.c
  chmod 755 "$OUT_JNILIBS/libpwc.so"
  cp -f "$OUT_JNILIBS/libpwc.so" "$SRC_ASSETS/pwc"
  cp -f "$OUT_JNILIBS/libpwc.so" ./pwc
  echo "  pwc OK"
else
  echo "  WARNING: no pwc.c"
fi

echo "== [3.7/8] pack toybox -> libtoybox.so =="
if [ -f "$PREBUILT/toybox-aarch64" ]; then
  cp -f "$PREBUILT/toybox-aarch64" "$OUT_JNILIBS/libtoybox.so"
  chmod 755 "$OUT_JNILIBS/libtoybox.so"
  echo "  toybox OK"
else
  echo "  WARNING: no prebuilt/toybox-aarch64"
fi

echo "== [4/8] compile java =="
JAVA_FILES=$(find "$SRC_JAVA" -name "*.java")
if [ -n "$JAVA_FILES" ]; then
  javac -classpath "$ANDROID_JAR" -d "$OUT_CLASSES" $JAVA_FILES
else
  echo "  (no java)"
fi

echo "== [5/8] dex (d8) =="
if [ -d "$OUT_CLASSES" ] && [ "$(find "$OUT_CLASSES" -name '*.class')" ]; then
  d8 --lib "$ANDROID_JAR" --output "$BUILD" $(find "$OUT_CLASSES" -name "*.class")
fi

echo "== [6/8] aapt2 =="
if [ -d "$SRC_RES" ] && [ "$(find "$SRC_RES" -type f 2>/dev/null | head -1)" ]; then
  aapt2 compile --dir "$SRC_RES" -o "$BUILD/res.zip"
fi
AAPT2_LINK_ARGS=(-o "$BUILD/app-unsigned.apk" -I "$ANDROID_JAR" --manifest "$MANIFEST")
[ -f "$BUILD/res.zip" ] && AAPT2_LINK_ARGS+=("$BUILD/res.zip")
[ -d "$SRC_ASSETS" ] && AAPT2_LINK_ARGS+=(-A "$SRC_ASSETS")
aapt2 link "${AAPT2_LINK_ARGS[@]}"

echo "== [6.5/8] inject dex + libs =="
cd "$BUILD"
[ -f classes.dex ] && zip -j app-unsigned.apk classes.dex
cd - >/dev/null
if [ -d "$OUT_JNILIBS" ] && [ "$(ls -A "$OUT_JNILIBS" 2>/dev/null)" ]; then
  ls -la "$OUT_JNILIBS"
  mkdir -p "$BUILD/apk-native/lib/arm64-v8a"
  cp -f "$OUT_JNILIBS"/*.so "$BUILD/apk-native/lib/arm64-v8a/"
  (cd "$BUILD/apk-native" && zip -r "../app-unsigned.apk" lib)
fi

echo "== [7/8] zipalign + sign =="
zipalign -f 4 "$BUILD/app-unsigned.apk" "$BUILD/app-aligned.apk"
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkey -v -keystore "$KEYSTORE" -alias pwcdebug \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass android -keypass android \
    -dname "CN=PowerCode Debug,O=pwc,C=TH"
fi
apksigner sign --ks "$KEYSTORE" --ks-pass pass:android \
  --out "$BUILD/$APP_NAME.apk" "$BUILD/app-aligned.apk"

echo "== [8/8] verify =="
echo "built: $BUILD/$APP_NAME.apk"
for need in libjsh.so libpwc.so libpty_jni.so libtoybox.so libc++_shared.so; do
  if unzip -l "$BUILD/$APP_NAME.apk" 2>/dev/null | grep -q "lib/arm64-v8a/$need"; then
    echo "  OK  $need"
  else
    echo "  MISSING  $need"
  fi
done
