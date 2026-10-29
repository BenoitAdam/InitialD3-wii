@echo off
rem Builds the Windows version of the Wii port and runs its tests.
rem Needs Visual Studio 2022 (or Build Tools) with the C++ workload, which
rem also provides CMake and Ninja.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
)
if not defined VSINSTALL goto :novs
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" goto :novs
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul || goto :novs
rem Visual Studio's CMake/Ninja first: devkitPro puts an MSYS cmake on PATH
rem that cannot drive MSVC.
set "PATH=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

set "SRC=%~dp0."
set "OUT=%~dp0..\build-port"
cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build "%OUT%"
if errorlevel 1 exit /b 1
ctest --test-dir "%OUT%" --output-on-failure
if errorlevel 1 exit /b 1
echo.
echo Built: %OUT%\idas3wii.exe
exit /b 0

:novs
echo Visual Studio 2022 with the C++ tools was not found.
exit /b 1
