@echo off
rem Builds Grond and installs it into VCV Rack. Run from any prompt.
rem Needs MSYS2 in C:\msys64 with mingw-w64-x86_64-gcc, make, zip and jq installed,
rem and the Rack 2 SDK. Point RACK_DIR at the SDK folder first, for example:
rem   set RACK_DIR=C:\path\to\Rack-SDK
if "%RACK_DIR%"=="" (
  echo Set the RACK_DIR environment variable to your Rack 2 SDK folder first.
  exit /b 1
)
set "HERE=%~dp0"
"C:\msys64\usr\bin\bash.exe" -lc "export MSYSTEM=MINGW64; export PATH=/mingw64/bin:$PATH; cd \"$(cygpath -u '%HERE%')\" && RACK_DIR=\"$(cygpath -u '%RACK_DIR%')\" make install"
echo.
echo Done. Close and reopen VCV Rack to load the new build.
pause
