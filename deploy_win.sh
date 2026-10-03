#!/usr/bin/env bash
# Принципия: собрать переносимую папку с principia.exe и всеми DLL.
# Запускать в MSYS2 MINGW64 из корня проекта:
#   bash deploy_win.sh

set -e

ROOT="$(pwd)"
BUILD="$ROOT/build"
OUT="$ROOT/principia-portable"
SYSDIR="${MINGW_PREFIX:-/mingw64}/bin"

if [ ! -f "$BUILD/principia.exe" ]; then
    echo "Не найден $BUILD/principia.exe — сначала собери: cmake --build build -j"
    exit 1
fi

if [ ! -d "$SYSDIR" ]; then
    echo "Не найден $SYSDIR — запусти скрипт из оболочки MINGW64"
    exit 1
fi

export PATH="$SYSDIR:$PATH"

echo "== создаю $OUT"
rm -rf "$OUT"
mkdir -p "$OUT"

cp "$BUILD/principia.exe" "$OUT/"

if [ -d "$ROOT/data" ]; then
    echo "== копирую data/"
    cp -r "$ROOT/data" "$OUT/data"
else
    echo "!! папка data/ не найдена, игра может не запуститься"
fi

# portable-режим: сохранения рядом с exe, а не в AppData
touch "$OUT/portable.txt"

# запуск для коопа на одном ПК: снимает одноэкземплярность
printf '@echo off\r\nstart "" "%%~dp0principia.exe" --multi\r\n' > "$OUT/coop-instance.bat"

echo "== ищу DLL"
pass=1
while : ; do
    before=$(ls -1 "$OUT"/*.dll 2>/dev/null | wc -l)

    for f in "$OUT/principia.exe" "$OUT"/*.dll; do
        [ -e "$f" ] || continue
        ldd "$f" 2>/dev/null \
          | awk '{print $3}' \
          | grep -iE '^(/mingw64|'"$SYSDIR"')/' \
          | while read -r dll; do
                [ -f "$dll" ] && cp -n "$dll" "$OUT/" || true
            done
    done

    after=$(ls -1 "$OUT"/*.dll 2>/dev/null | wc -l)
    echo "   проход $pass: $after DLL"
    [ "$before" = "$after" ] && break
    pass=$((pass+1))
done

for extra in libpng16-16.dll libjpeg-8.dll zlib1.dll libfreetype-6.dll \
             libcurl-4.dll SDL3.dll libwinpthread-1.dll \
             libbz2-1.dll libbrotlidec.dll libbrotlicommon.dll \
             libharfbuzz-0.dll libgraphite2.dll libglib-2.0-0.dll \
             libintl-8.dll libiconv-2.dll libpcre2-8-0.dll libzstd.dll \
             libssl-3-x64.dll libcrypto-3-x64.dll libssh2-1.dll \
             libnghttp2-14.dll libnghttp3-9.dll libngtcp2-16.dll \
             libngtcp2_crypto_ossl-0.dll libidn2-0.dll \
             libunistring-5.dll libpsl-5.dll libssp-0.dll \
             libgcc_s_seh-1.dll libstdc++-6.dll; do
    [ -f "$SYSDIR/$extra" ] && cp -n "$SYSDIR/$extra" "$OUT/" || true
done

echo
echo "== готово: $OUT"
echo "Всего файлов: $(ls -1 "$OUT" | wc -l)"
echo "Первый экземпляр: principia.exe, второй: coop-instance.bat"