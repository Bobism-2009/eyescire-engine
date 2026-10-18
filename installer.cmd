@echo off
rem -----------------------------------------------------------------------
rem Build EyescireSetup.exe (GUI installer) from release\ + engine\*.nxa.
rem Run build.cmd and release.cmd first so release\ has the binaries.
rem -----------------------------------------------------------------------
cd /d "%~dp0"

if not exist release\editor.exe (echo release\editor.exe missing - run build.cmd then release.cmd & exit /b 1)
if not exist release\engine.dll (echo release\engine.dll missing - run build.cmd then release.cmd & exit /b 1)

where NexaC >nul 2>&1 || (echo NexaC not on PATH & exit /b 1)

if exist installer\payload rmdir /s /q installer\payload
mkdir installer\payload
mkdir installer\payload\icons
mkdir installer\payload\package

copy /y "release\editor.exe"           "installer\payload\" >nul
copy /y "release\engine.dll"           "installer\payload\" >nul
copy /y "release\engine.lib"           "installer\payload\" >nul
if exist "release\filament_bridge.dll" copy /y "release\filament_bridge.dll" "installer\payload\" >nul
copy /y "release\EyescirePlayer3D.exe" "installer\payload\" >nul
copy /y "release\EyescirePlayer.exe"   "installer\payload\" >nul
copy /y "release\EyescirePlayer.dll"   "installer\payload\" >nul
copy /y "release\libc++.dll"           "installer\payload\" >nul
if errorlevel 1 (echo failed to copy libc++.dll & exit /b 1)
copy /y "release\libunwind.dll"        "installer\payload\" >nul
if exist "release\icons\*.png" copy /y "release\icons\*.png" "installer\payload\icons\" >nul
copy /y "engine\*.nxa"                 "installer\payload\package\" >nul
copy /y "installer\VERSION"            "installer\payload\VERSION" >nul
if not exist "installer\payload\libc++.dll" (echo payload missing libc++.dll & exit /b 1)
if not exist "installer\payload\package\engine.nxa" (echo payload missing package\engine.nxa & exit /b 1)

if not exist installer\out mkdir installer\out

if not exist build\eyescire_res.o (
  if not exist icons\eyescire.ico (echo icons\eyescire.ico missing - run: python icons\make_app_icon.py & exit /b 1)
  if not exist build mkdir build
  call windres -O coff -I icons -o build\eyescire_res.o icons\eyescire.rc
  if not exist build\eyescire_res.o (echo windres failed for eyescire.rc & exit /b 1)
)

echo Compiling setup stub...
call NexaC installer\setup.nxa --link build\eyescire_res.o -p --no-console -o installer\out\setup_stub.exe
if errorlevel 1 exit /b 1

echo Compiling packer...
call NexaC installer\pack.nxa -p -o installer\out\pack.exe
if errorlevel 1 exit /b 1

echo Packing EyescireSetup.exe...
installer\out\pack.exe release\EyescireSetup.exe installer\out\setup_stub.exe installer\payload
if errorlevel 1 exit /b 1

echo.
echo Built release\EyescireSetup.exe
dir release\EyescireSetup.exe
