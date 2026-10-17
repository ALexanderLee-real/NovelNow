@echo off
rem ===================================================================
rem  novelnow WebUI - build script (MSVC cl, fallback for no-MinGW)
rem  Keep this file pure ASCII (same reason as build.bat).
rem ===================================================================
setlocal

echo ==========================================
echo   novelnow WebUI - build (MSVC cl)
echo ==========================================
echo.

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere not found. Install Visual Studio 2022.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set VSROOT=%%i
if not defined VSROOT (
    echo [ERROR] No Visual Studio installation found.
    exit /b 1
)
echo Using VS at: %VSROOT%

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64 failed
    exit /b 1
)

set INC=/Iinclude /Ithird_party\webview2\include
set CFLAGS=/nologo /W3 /O2 /std:c11 /utf-8 %INC% /D_CRT_SECURE_NO_WARNINGS
rem WebView2Loader.dll is loaded at runtime; import lib lives next to sources
set LIBDIR=third_party\webview2\x64

if not exist obj mkdir obj
if not exist bin mkdir bin
if not exist bin\ui mkdir bin\ui

rem stale .obj from deleted sources would be linked in by obj\*.obj - clean first
if exist obj\*.obj del /q obj\*.obj
rem wildcard: new source files are picked up automatically, no list to maintain
set SRCS=src\*.c

echo [1/4] compiling sources...
for %%f in (%SRCS%) do (
    echo     %%~nf.c
    cl %CFLAGS% /c %%f /Foobj\%%~nf.obj
    if errorlevel 1 (
        echo [ERROR] compile failed: %%f
        exit /b 1
    )
)

echo [2/4] compiling resources...
rc /nologo /fo obj\novelnow_res.res novelnow.rc
if errorlevel 1 (
    echo [ERROR] rc failed
    exit /b 1
)

rem generate import lib from the loader DLL (SDK ships no .lib here)
if not exist %LIBDIR%\WebView2Loader.lib (
    lib /nologo /machine:x64 /def:%LIBDIR%\WebView2Loader.def /out:%LIBDIR%\WebView2Loader.lib
    if errorlevel 1 (
        echo [ERROR] lib def failed
        exit /b 1
    )
)

echo [3/4] linking...
link /nologo /SUBSYSTEM:WINDOWS obj\*.obj obj\novelnow_res.res ^
    /LIBPATH:%LIBDIR% WebView2Loader.lib ^
    wininet.lib ole32.lib oleaut32.lib uuid.lib shell32.lib comdlg32.lib gdi32.lib user32.lib advapi32.lib ^
    /OUT:bin\novelnow.exe
if errorlevel 1 (
    echo [ERROR] link failed
    exit /b 1
)

echo [4/4] copying assets...
copy /y third_party\webview2\x64\WebView2Loader.dll bin\ >nul
xcopy /y /q ui\* bin\ui\ >nul

echo.
echo ==========================================
echo   build succeeded
echo   output : bin\novelnow.exe
echo ==========================================
