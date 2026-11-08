@echo off
setlocal
rem Locate vcvars64.bat via vswhere, falling back to common VS editions/years.
set VCVARS=
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set VCVARS=%%i
)
if not defined VCVARS (
    for %%y in (2022 2026) do (
        for %%e in (Community Professional Enterprise BuildTools) do (
            if exist "C:\Program Files\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" set VCVARS=C:\Program Files\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat
        )
    )
)
if not defined VCVARS ( echo vcvars64.bat not found - install Visual Studio with the C++ toolset & exit /b 1 )
call "%VCVARS%" >nul
cl /nologo /O2 /LD /MD "%~dp0PhotosSlideshowHook.cpp" /Fe:"%~dp0PhotosSlideshowHook.dll" 2>&1
