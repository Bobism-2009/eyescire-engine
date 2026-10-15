@echo off
rem -----------------------------------------------------------------------
rem Eyescire Engine - one-shot build for Windows.
rem
rem 1. Build Box2D (third_party\box2d) into bin\box2d.lib (once) — 2D physics.
rem 2. Build Jolt  (third_party\jolt)  into bin\jolt.lib  (once) — 3D physics.
rem 3. Compile the native engine (host\engine.cpp) to engine.dll + engine.lib,
rem    statically linking Box2D and Jolt.
rem 4. Build each .nxa app against engine.lib (the import library).
rem
rem clang++ and NexaC ship as .cmd shims, so we invoke them through `call`
rem to keep control inside this batch file.
rem -----------------------------------------------------------------------

cd /d "%~dp0"
if not exist build  mkdir build
if not exist bin    mkdir bin
if not exist scenes mkdir scenes

set BOX2D_INC=-Ithird_party\box2d\include
set JOLT_INC=-Ithird_party\jolt
rem Match the defines used when compiling engine.cpp against Jolt headers.
set JOLT_DEFS=-DNDEBUG -DJPH_DISABLE_CUSTOM_ALLOCATOR

echo == building Box2D (bin\box2d.lib)
if not exist bin\box2d.lib (
  if not exist third_party\box2d\obj mkdir third_party\box2d\obj
  for /r third_party\box2d\src %%f in (*.cpp) do (
    call clang++ -std=c++17 -O2 -c -Ithird_party\box2d\include -Ithird_party\box2d\src "%%f" -o "third_party\box2d\obj\%%~nf.o"
  )
  call llvm-ar rcs bin\box2d.lib third_party\box2d\obj\*.o
)
if not exist bin\box2d.lib (echo box2d build failed & exit /b 1)

echo == building Jolt (bin\jolt.lib)
if not exist bin\jolt.lib (
  if not exist third_party\jolt\obj mkdir third_party\jolt\obj
  for /r third_party\jolt\Jolt %%f in (*.cpp) do (
    call clang++ -std=c++17 -O2 -c %JOLT_INC% %JOLT_DEFS% "%%f" -o "third_party\jolt\obj\%%~nf.o"
    if errorlevel 1 (echo jolt compile failed: %%f & exit /b 1)
  )
  call llvm-ar rcs bin\jolt.lib third_party\jolt\obj\*.o
)
if not exist bin\jolt.lib (echo jolt build failed & exit /b 1)

echo == compiling engine.dll
call clang++ -std=c++17 -O2 -shared -DEYESCIRE_BUILDING=1 %BOX2D_INC% %JOLT_INC% %JOLT_DEFS% host\engine.cpp bin\box2d.lib bin\jolt.lib -o bin\engine.dll -Wl,--out-implib,bin\engine.lib -lgdi32 -luser32 -lgdiplus -lcomdlg32 -lwinmm -lole32 -luuid -lshell32 -lopengl32 -lmfplat -lmfreadwrite -lmfuuid -ldwmapi
if not exist bin\engine.dll (echo engine.dll build failed & exit /b 1)
copy /y bin\engine.dll build\engine.dll >nul
copy /y bin\engine.lib build\engine.lib >nul

rem llvm-mingw C++ runtime (engine.dll imports these; must sit next to the exes)
echo == copying libc++.dll / libunwind.dll
set RT_DIR=
for /f "delims=" %%i in ('clang++ -print-prog-name^=clang++') do set RT_DIR=%%~dpi
if not exist "%RT_DIR%libc++.dll" (
  echo libc++.dll not found next to the compiler: %RT_DIR%
  exit /b 1
)
if not exist "%RT_DIR%libunwind.dll" (
  echo libunwind.dll not found next to the compiler: %RT_DIR%
  exit /b 1
)
copy /y "%RT_DIR%libc++.dll"    build\ >nul
copy /y "%RT_DIR%libunwind.dll" build\ >nul

rem game scripts compile against the eyescire package (#include ^<eyescire/script^>):
rem keep it in step with engine\ so they see the same API as engine.dll
if exist "%USERPROFILE%\.nexa\packages\eyescire" copy /y engine\*.nxa "%USERPROFILE%\.nexa\packages\eyescire\" >nul

echo == copying editor icons
if not exist build\icons mkdir build\icons
copy /y icons\*.png build\icons\ >nul

echo == application icon resource
if not exist icons\eyescire.ico (
  echo icons\eyescire.ico missing - run: python icons\make_app_icon.py
  exit /b 1
)
call windres -O coff -I icons -o build\eyescire_res.o icons\eyescire.rc
if not exist build\eyescire_res.o (echo windres failed for eyescire.rc & exit /b 1)

echo == compiling editor
if exist build\editor.exe del build\editor.exe
if exist build\editor.exe (echo build\editor.exe is in use - close it and rebuild & exit /b 1)
call NexaC examples\editor.nxa --link build\engine.lib --link build\eyescire_res.o -p --no-console -o build\editor.exe
if not exist build\editor.exe (echo editor build failed & exit /b 1)

echo == compiling EyescirePlayer.dll
rem Not `NexaC --dll`: library builds export every Nexa function as extern "C"
rem with const char* strings, which breaks the engine's internal string calls.
rem Emit the normal C++ instead and link it as a DLL; runtime.nxa exports only
rem player_run (the launcher's entry point).
if exist build\EyescirePlayer.dll del build\EyescirePlayer.dll
if exist build\EyescirePlayer.dll (echo build\EyescirePlayer.dll is in use - close it and rebuild & exit /b 1)
call NexaC examples\runtime.nxa -p --source bin\EyescirePlayer.cpp
call clang++ -std=c++17 -O2 -shared bin\EyescirePlayer.cpp build\engine.lib -o build\EyescirePlayer.dll
if not exist build\EyescirePlayer.dll (echo EyescirePlayer.dll build failed & exit /b 1)

echo == compiling EyescirePlayer3D.exe (std/gfx3d player for 3D scenes)
if exist build\EyescirePlayer3D.exe del build\EyescirePlayer3D.exe
if exist build\EyescirePlayer3D.exe (echo build\EyescirePlayer3D.exe is in use - close it and rebuild & exit /b 1)
call NexaC examples\player3d.nxa --link build\engine.lib --link build\eyescire_res.o -p --no-console -o build\EyescirePlayer3D.exe
if not exist build\EyescirePlayer3D.exe (echo EyescirePlayer3D.exe build failed & exit /b 1)

echo == compiling EyescirePlayer.exe launcher
if exist build\EyescirePlayer.exe del build\EyescirePlayer.exe
if exist build\EyescirePlayer.exe (echo build\EyescirePlayer.exe is in use - close it and rebuild & exit /b 1)
call NexaC examples\player_launcher.nxa --link build\engine.lib --link build\eyescire_res.o -p -o build\EyescirePlayer.exe
if not exist build\EyescirePlayer.exe (echo EyescirePlayer.exe build failed & exit /b 1)

echo.
echo Build complete:
dir /b build\*.exe
dir /b build\libc++.dll build\libunwind.dll 2>nul
echo.
echo Try:
echo   build\editor.exe
echo   build\EyescirePlayer.exe
