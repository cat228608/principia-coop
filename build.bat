@echo off
setlocal
chcp 65001 >nul
title Principia - build

set "MSYS=C:\msys64"

if not exist "%MSYS%\mingw64\bin\gcc.exe" (
    echo [!] Не нашёл MSYS2 MINGW64 в "%MSYS%".
    pause
    exit /b 1
)

cd /d "%~dp0"

if not exist "CMakeLists.txt" (
    echo [!] В этой папке нет CMakeLists.txt.
    pause
    exit /b 1
)

set "PATH=%MSYS%\mingw64\bin;%MSYS%\usr\bin;%PATH%"
set "CC=%MSYS%/mingw64/bin/gcc.exe"
set "CXX=%MSYS%/mingw64/bin/g++.exe"

if /i "%~1"=="clean" (
    echo === Удаляю старую папку build ===
    rmdir /s /q build 2>nul
)

echo === Конфигурация ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DCMAKE_C_COMPILER="%CC%" -DCMAKE_CXX_COMPILER="%CXX%"
if errorlevel 1 goto fail

echo.
echo === Сборка ===
cmake --build build -j
if errorlevel 1 goto fail

echo.
if exist "deploy_win.sh" (
    echo === deploy_win.sh ===
    set "MSYSTEM=MINGW64"
    "%MSYS%\usr\bin\bash.exe" -lc "cd \"$(cygpath -u '%CD%')\" && bash deploy_win.sh"
)

echo.
echo ==========================================
echo   ГОТОВО. Портативная сборка: principia-portable
echo ==========================================
pause
exit /b 0

:fail
echo.
echo ==========================================
echo   ОШИБКА СБОРКИ - смотри сообщения выше.
echo ==========================================
pause
exit /b 1