@echo off
rem Builds the SD card layout with devkitPPC in port\wii\dist\
rem (apps\initiald3wii\boot.dol + an empty initiald3\ game folder).
rem Extra arguments go to make, e.g.  build_wii.bat clean
setlocal
set "DKP=C:\devkitPro"
if not exist "%DKP%\msys2\usr\bin\bash.exe" (
    echo devkitPro was not found in %DKP%.
    exit /b 1
)
pushd "%~dp0wii"
rem Run make inside devkitPro's own MSYS2 so DEVKITPRO/DEVKITPPC are set.
set CHERE_INVOKING=1
if "%~1"=="" (
    "%DKP%\msys2\usr\bin\bash.exe" -lc "make"
) else (
    "%DKP%\msys2\usr\bin\bash.exe" -lc "make %*"
)
set RC=%ERRORLEVEL%
popd
exit /b %RC%
