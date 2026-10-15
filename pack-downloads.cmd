@echo off
rem -----------------------------------------------------------------------
rem Pack website download archives:
rem   website\downloads\EyescireSetup-<ver>-windows.7z
rem   website\downloads\eyescire-engine-<ver>-source.7z
rem
rem Run build.cmd, release.cmd, and installer.cmd first so
rem release\EyescireSetup.exe exists.
rem -----------------------------------------------------------------------
cd /d "%~dp0"

set "SEVEN="
if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVEN=%ProgramFiles%\7-Zip\7z.exe"
if exist "%ProgramFiles(x86)%\7-Zip\7z.exe" set "SEVEN=%ProgramFiles(x86)%\7-Zip\7z.exe"
if not defined SEVEN (
  where 7z >nul 2>&1 && for /f "delims=" %%i in ('where 7z') do set "SEVEN=%%i"
)
if not defined SEVEN (echo 7-Zip not found. Install 7-Zip or add 7z to PATH. & exit /b 1)

if not exist release\EyescireSetup.exe (
  echo release\EyescireSetup.exe missing - run build.cmd, release.cmd, installer.cmd
  exit /b 1
)

set "VER=0.1"
if exist installer\VERSION set /p VER=<installer\VERSION
for /f "tokens=* delims= " %%v in ("%VER%") do set "VER=%%v"

if not exist website\downloads mkdir website\downloads

set "SETUP_7Z=website\downloads\EyescireSetup-%VER%-windows.7z"
set "SRC_7Z=website\downloads\eyescire-engine-%VER%-source.7z"
set "LIST=%TEMP%\eyescire-src-exclude.txt"

echo == packing installer %SETUP_7Z%
if exist "%SETUP_7Z%" del /f /q "%SETUP_7Z%"
"%SEVEN%" a -t7z -mx=9 "%SETUP_7Z%" ".\release\EyescireSetup.exe"
if errorlevel 1 (echo failed to pack installer & exit /b 1)

echo == packing source %SRC_7Z%
> "%LIST%" (
  echo .git\
  echo build\
  echo release\
  echo release_old\
  echo installer\payload\
  echo installer\out\
  echo website\downloads\
  echo third_party\box2d\obj\
  echo __pycache__\
  echo .cursor\
  echo *.o
  echo *.exe
  echo *.dll
  echo *.lib
)
if exist "%SRC_7Z%" del /f /q "%SRC_7Z%"
"%SEVEN%" a -t7z -mx=9 -xr@"%LIST%" "%SRC_7Z%" "."
if errorlevel 1 (echo failed to pack source & exit /b 1)
del /f /q "%LIST%" >nul 2>&1

echo.
echo downloads\:
dir /b website\downloads
echo.
echo Wrote:
echo   %SETUP_7Z%
echo   %SRC_7Z%
exit /b 0
