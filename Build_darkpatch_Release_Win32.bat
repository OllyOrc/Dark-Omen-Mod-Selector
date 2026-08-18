@echo off
setlocal
cd /d "%~dp0"

echo Building darkpatch Release Win32...
where msbuild >nul 2>&1
if errorlevel 1 (
  echo MSBuild is not on PATH. Open darkpatch.sln in Visual Studio and build Release / Win32.
  pause
  exit /b 1
)
msbuild darkpatch.sln /m /p:Configuration=Release /p:Platform=Win32
if errorlevel 1 (
  echo BUILD FAILED
  pause
  exit /b 1
)
echo.
echo BUILD SUCCEEDED
if exist darkpatch.dll echo Output: %cd%\darkpatch.dll
pause
