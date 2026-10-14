@echo off
rem -----------------------------------------------------------------------
rem Build the modern-UI experiment editor only.
rem Does not touch examples\editor.nxa or the production theme.
rem Output: build\editor_modern.exe
rem -----------------------------------------------------------------------

cd /d "%~dp0\..\.."
if not exist build mkdir build

if not exist build\engine.lib (
  echo build\engine.lib missing - run build.cmd from the repo root first
  exit /b 1
)
if not exist build\eyescire_res.o (
  echo build\eyescire_res.o missing - run build.cmd from the repo root first
  exit /b 1
)

echo == copying editor icons
if not exist build\icons mkdir build\icons
copy /y icons\*.png build\icons\ >nul

echo == compiling editor_modern.exe
if exist build\editor_modern.exe del build\editor_modern.exe
if exist build\editor_modern.exe (
  echo build\editor_modern.exe is in use - close it and rebuild
  exit /b 1
)
call NexaC test\modern-ui\editor_modern.nxa --link build\engine.lib --link build\eyescire_res.o -p --no-console -o build\editor_modern.exe
if not exist build\editor_modern.exe (
  echo editor_modern build failed
  exit /b 1
)

echo.
echo Done.
echo   build\editor_modern.exe
echo Production editor is unchanged: build\editor.exe
exit /b 0
