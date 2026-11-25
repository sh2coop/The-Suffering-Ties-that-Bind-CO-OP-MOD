@echo off
rem Runs a command with the build tools on PATH, from the project folder:
rem   tools\dev.bat cmake --build out\build\release
setlocal
set NoDefaultCurrentDirectoryInExePath=
cd /d "%~dp0.."
set "VSINST=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq tokens=*" %%i in (`"%VSINST%\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR echo Visual Studio with the C++ workload was not found.& exit /b 1
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%CD%\tools\llvm\bin\clang++.exe" set "PATH=%CD%\tools\llvm\bin;%PATH%"
%*
exit /b %errorlevel%
