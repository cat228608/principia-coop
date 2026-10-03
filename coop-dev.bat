@echo off
setlocal
chcp 65001 >nul

rem =====================================================================
rem  Principia co-op: build + launch two instances
rem
rem    coop-dev.bat          - build RelWithDebInfo + run 2 instances
rem    coop-dev.bat run      - no build, just run 2 instances
rem    coop-dev.bat build    - build only
rem    coop-dev.bat debug    - CMAKE_BUILD_TYPE=Debug + run 2 instances
rem    coop-dev.bat gdb      - build, instance 1 under gdb, instance 2 normal
rem    coop-dev.bat clean    - wipe build dir, reconfigure, build
rem
rem  Live output in both windows + logs\host.log / logs\client.log
rem =====================================================================

set "MSYS=C:\msys64"

set "PROJ=%~dp0"
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"

set "BUILD_TYPE=RelWithDebInfo"
if /i "%MODE%"=="debug" set "BUILD_TYPE=Debug"

set "BASH=%MSYS%\usr\bin\bash.exe"
set "MINTTY=%MSYS%\usr\bin\mintty.exe"

if not exist "%BASH%" goto no_msys

set MSYSTEM=MINGW64
set CHERE_INVOKING=1

if not exist "%PROJ%\logs" mkdir "%PROJ%\logs"

if /i "%MODE%"=="clean" goto do_clean
if /i "%MODE%"=="run" goto pick_exe
goto do_build

:do_clean
echo [*] removing build directory...
rmdir /s /q "%PROJ%\build" 2>nul
set "MODE=build"

:do_build
echo [*] configure + build %BUILD_TYPE% ...
"%BASH%" -lc "set -e; cd \"$(cygpath -u '%PROJ%')\"; cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_COMPILER=/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=/mingw64/bin/g++.exe; cmake --build build -j"
if errorlevel 1 goto build_failed

if not exist "%PROJ%\deploy_win.sh" goto after_deploy
echo [*] deploy_win.sh -^> principia-portable ...
"%BASH%" -lc "cd \"$(cygpath -u '%PROJ%')\"; ./deploy_win.sh"

:after_deploy
if /i "%MODE%"=="build" goto done_build_only

:pick_exe
set "RUNDIR=%PROJ%\principia-portable"
if not exist "%RUNDIR%\principia.exe" set "RUNDIR=%PROJ%\build"
if not exist "%RUNDIR%\principia.exe" goto no_exe
echo [*] running from: %RUNDIR%

rem two instances on one machine: the second one needs --multi, see
rem tms/backend/pipe.cc ; PRINCIPIA_MULTI=1 does the same

set "DBG=export TMS_LOG_LEVEL=0 SDL_LOGGING=app=debug PRINCIPIA_LOG=-;"
set "HOLD=echo; read -n1 -p 'press any key to close'"

if /i "%MODE%"=="gdb" goto run_gdb

echo [*] instance 1 - HOST ...
start "" "%MINTTY%" -t "Principia HOST" -s 140,45 -e /bin/bash -lc "cd \"$(cygpath -u '%RUNDIR%')\"; %DBG% ./principia.exe 2>&1 | tee \"$(cygpath -u '%PROJ%')/logs/host.log\"; %HOLD%"
goto run_second

:run_gdb
echo [*] instance 1 - HOST under gdb ...
start "" "%MINTTY%" -t "Principia HOST gdb" -s 140,45 -e /bin/bash -lc "cd \"$(cygpath -u '%RUNDIR%')\"; %DBG% gdb -ex run -ex bt --args ./principia.exe 2>&1 | tee \"$(cygpath -u '%PROJ%')/logs/host.log\"; %HOLD%"

:run_second
timeout /t 4 /nobreak >nul
echo [*] instance 2 - CLIENT --multi ...
start "" "%MINTTY%" -t "Principia CLIENT" -s 140,45 -e /bin/bash -lc "cd \"$(cygpath -u '%RUNDIR%')\"; %DBG% export PRINCIPIA_MULTI=1; ./principia.exe --multi 2>&1 | tee \"$(cygpath -u '%PROJ%')/logs/client.log\"; %HOLD%"

echo.
echo [*] HOST   log: %PROJ%\logs\host.log
echo [*] CLIENT log: %PROJ%\logs\client.log
echo.
echo     window 1: Multiplayer -^> Host, port 7777
echo     window 2: Multiplayer -^> Join, 127.0.0.1:7777
echo.
exit /b 0

:done_build_only
echo [*] build done.
exit /b 0

:build_failed
echo.
echo [!] BUILD FAILED
pause
exit /b 1

:no_exe
echo [!] principia.exe not found in principia-portable or build
pause
exit /b 1

:no_msys
echo [!] MSYS2 not found at %MSYS%
echo     edit the MSYS variable at the top of this file
pause
exit /b 1
