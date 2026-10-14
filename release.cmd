@echo off
rem -----------------------------------------------------------------------
rem Eyescire Engine - assemble release\ from build\ (run build.cmd first).
rem
rem Ships the editor and exactly the DLLs it loads:
rem   editor.exe     the program
rem   EyescirePlayer3D.exe  started by the editor's Play for 3D scenes
rem   engine.dll     window, drawing, scenes, physics (imported by both exes)
rem   engine.lib     not loaded at run time: Play/Build link project scripts
rem                  against it (NexaC --dll ... --link engine.lib)
rem   libc++.dll     llvm-mingw C++ runtime  (imported by engine.dll)
rem   libunwind.dll  llvm-mingw unwinder     (imported by libc++.dll / engine.dll)
rem Everything else they import (UCRT api-ms-win-crt-*, gdiplus, winmm,
rem comdlg32, user32, gdi32, kernel32) is part of Windows 10/11.
rem
rem The runtime DLLs are taken from the toolchain that compiled engine.dll,
rem so their versions always match it.
rem -----------------------------------------------------------------------

cd /d "%~dp0"

if not exist build\editor.exe (echo build\editor.exe missing - run build.cmd first & exit /b 1)
if not exist build\engine.dll (echo build\engine.dll missing - run build.cmd first & exit /b 1)
if not exist build\EyescirePlayer3D.exe (echo build\EyescirePlayer3D.exe missing - run build.cmd first & exit /b 1)

rem Prefer the copies build.cmd already staged; fall back to the compiler bin.
set CXX_DLL=build\libc++.dll
set UNW_DLL=build\libunwind.dll
if not exist "%CXX_DLL%" (
  set RT_DIR=
  for /f "delims=" %%i in ('clang++ -print-prog-name^=clang++') do set RT_DIR=%%~dpi
  set CXX_DLL=%RT_DIR%libc++.dll
  set UNW_DLL=%RT_DIR%libunwind.dll
)
if not exist "%CXX_DLL%" (echo libc++.dll not found - run build.cmd first & exit /b 1)
if not exist "%UNW_DLL%" (echo libunwind.dll not found - run build.cmd first & exit /b 1)

rem rename first: it fails as a whole if anything inside is open, so a running
rem release\editor.exe can't leave the folder half-deleted
if exist release (
  ren release release_old || (echo release\ is in use - close the editor running from it and retry & exit /b 1)
  rmdir /s /q release_old
)
mkdir release

copy /y build\editor.exe           release\ >nul
copy /y build\EyescirePlayer3D.exe release\ >nul
copy /y build\engine.dll           release\ >nul
copy /y build\engine.lib           release\ >nul
copy /y "%CXX_DLL%"                release\libc++.dll >nul
copy /y "%UNW_DLL%"                release\libunwind.dll >nul
if exist build\icons (
  mkdir release\icons
  copy /y build\icons\*.png release\icons\ >nul
)

echo release\:
dir /b release

echo.
echo Optional: run installer.cmd to build release\EyescireSetup.exe
echo ^(GUI installer: engine + eyescire package, optional NexaC / Nexium^)
