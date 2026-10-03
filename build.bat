@echo off
rem ============================================================
rem  LAN AI Chat Server - build script
rem
rem  Compiles server.cpp with MinGW-w64 g++ (fully static), then
rem  assembles a ready-to-run product folder NEXT TO this source
rem  folder:
rem
rem      ..\AI-Chat-Server\
rem          server.exe        <- just compiled
rem          config.ini  start.bat  web\  llama\  models\
rem
rem  This folder is self-contained: copy it to any Windows x64
rem  machine that has MinGW-w64 g++ installed, run build.bat,
rem  and you get the finished AI-Chat-Server next to it.
rem
rem  Usage:
rem      build.bat            compile + assemble (normal use)
rem      build.bat compile    compile only, skip assembling
rem
rem  Requirement: MinGW-w64 g++ (see the build notes .txt in
rem  this folder). llama.cpp engine and json.hpp are already
rem  bundled here - nothing else to download.
rem ============================================================
setlocal enabledelayedexpansion

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"

set "PRODUCT=%SRC%\..\AI-Chat-Server"
for %%i in ("%PRODUCT%") do set "PRODUCT=%%~fi"

rem ---------------- locate g++ ----------------
set "GXX="

if defined GXX_OVERRIDE (
  if exist "%GXX_OVERRIDE%" set "GXX=%GXX_OVERRIDE%"
)

if not defined GXX (
  for /f "delims=" %%i in ('where g++ 2^>nul') do (
    if not defined GXX set "GXX=%%i"
  )
)

if not defined GXX (
  for %%p in (
    "C:\Users\momo\Desktop\MOMO\toolchain\mingw64\bin\g++.exe"
    "C:\mingw64\bin\g++.exe"
    "C:\winlibs\mingw64\bin\g++.exe"
    "C:\msys64\mingw64\bin\g++.exe"
    "C:\msys64\ucrt64\bin\g++.exe"
    "%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe"
    "D:\mingw64\bin\g++.exe"
  ) do (
    if not defined GXX if exist %%p set "GXX=%%~p"
  )
)

if not defined GXX (
  echo [ERROR] g++ not found.
  echo         Install MinGW-w64 / WinLibs, add its bin folder to PATH,
  echo         or set GXX_OVERRIDE to the full path of g++.exe .
  echo         See the build notes in this folder for step-by-step help.
  pause
  exit /b 1
)

echo [INFO] compiler : %GXX%
echo [INFO] source   : %SRC%
echo [INFO] product  : %PRODUCT%

rem ---------------- refuse to overwrite a running server ----------------
tasklist /fi "imagename eq server.exe" 2>nul | find /i "server.exe" >nul
if not errorlevel 1 (
  echo.
  echo [ERROR] server.exe is currently running.
  echo         Close the server window first, then run this script again.
  pause
  exit /b 1
)

if not exist "%PRODUCT%" mkdir "%PRODUCT%"

rem ---------------- compile ----------------
echo.
echo [1/2] Compiling server.cpp ...
"%GXX%" -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -s ^
  -o "%PRODUCT%\server.exe" "%SRC%\server.cpp" ^
  -lws2_32 -liphlpapi -lwinpthread

if errorlevel 1 (
  echo.
  echo [FAILED] compile error - see messages above.
  pause
  exit /b 1
)
echo [OK] %PRODUCT%\server.exe

if /i "%~1"=="compile" (
  echo.
  echo Compile only - assembling skipped.
  goto :done
)

rem ---------------- assemble ----------------
echo.
echo [2/2] Assembling product folder ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%\pack.ps1" -Source "%SRC%" -Product "%PRODUCT%"
if errorlevel 1 (
  echo.
  echo [FAILED] assemble error - see messages above.
  pause
  exit /b 1
)

:done
echo.
echo Build finished.
echo Run:  "%PRODUCT%\start.bat"
echo.
pause
