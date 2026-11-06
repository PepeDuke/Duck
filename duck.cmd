@echo off
setlocal
if exist "%~dp0build\Debug\duck.exe" (
  set "DUCK_BUILDER=%~dp0build\Debug\duck.exe"
  goto run
)
if exist "%~dp0build\Release\duck.exe" (
  set "DUCK_BUILDER=%~dp0build\Release\duck.exe"
  goto run
)
if exist "%~dp0build\duck.exe" (
  set "DUCK_BUILDER=%~dp0build\duck.exe"
  goto run
)
echo Duck build tool not found. Run cmake -S . -B build and cmake --build build --config Debug. 1>&2
exit /b 1

:run
"%DUCK_BUILDER%" %*
exit /b %errorlevel%
