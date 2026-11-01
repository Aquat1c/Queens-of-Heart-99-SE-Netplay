@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
if /I "%CONFIG%"=="install" (
    set "CONFIG=Debug"
    set "INSTALL=1"
) else (
    set "INSTALL=0"
)
if /I "%~2"=="install" set "INSTALL=1"

if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo [ERROR] Unknown config "%CONFIG%". Use Debug or Release.
    exit /b 1
)

set "OUT_DIR=%SCRIPT_DIR%build\%CONFIG%"
set "OUT_DLL=%OUT_DIR%\ddraw.dll"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

echo.
echo === QOH99 DirectDraw Wrapper: %CONFIG% ===
echo.

where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake was not found on PATH.
    exit /b 1
)

call :ensure_msvc
if not errorlevel 1 (
    echo [INFO] Configuring the complete MSVC x86 target with stack checks enabled
    set "CMAKE_BUILD_DIR=%SCRIPT_DIR%build\cmake-msvc-x86"
    cmake -S "%SCRIPT_DIR%." -B "!CMAKE_BUILD_DIR!" -A Win32 -DBUILD_TESTING=OFF
    if errorlevel 1 exit /b 1
    cmake --build "!CMAKE_BUILD_DIR!" --config "%CONFIG%" --target ddraw
    if errorlevel 1 exit /b 1
    set "BUILT_DLL=!CMAKE_BUILD_DIR!\%CONFIG%\ddraw.dll"
    goto :success
)

where gcc >nul 2>&1
if not errorlevel 1 (
    where g++ >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] GCC was found, but G++ is required by the renderer.
        exit /b 1
    )
    echo [INFO] Configuring the complete MinGW x86 target
    set "CMAKE_BUILD_DIR=%SCRIPT_DIR%build\cmake-mingw-x86"
    cmake -S "%SCRIPT_DIR%." -B "!CMAKE_BUILD_DIR!" -G "MinGW Makefiles" ^
        -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE="%CONFIG%"
    if errorlevel 1 exit /b 1
    cmake --build "!CMAKE_BUILD_DIR!" --target ddraw
    if errorlevel 1 exit /b 1
    set "BUILT_DLL=!CMAKE_BUILD_DIR!\ddraw.dll"
    goto :success
)

where i686-w64-mingw32-gcc >nul 2>&1
if not errorlevel 1 (
    where i686-w64-mingw32-g++ >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] The i686 GCC compiler was found, but its G++ companion is required.
        exit /b 1
    )
    echo [INFO] Configuring the complete cross-MinGW x86 target
    set "CMAKE_BUILD_DIR=%SCRIPT_DIR%build\cmake-cross-mingw-x86"
    cmake -S "%SCRIPT_DIR%." -B "!CMAKE_BUILD_DIR!" -G "MinGW Makefiles" ^
        -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE="%CONFIG%" ^
        -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc ^
        -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++
    if errorlevel 1 exit /b 1
    cmake --build "!CMAKE_BUILD_DIR!" --target ddraw
    if errorlevel 1 exit /b 1
    set "BUILT_DLL=!CMAKE_BUILD_DIR!\ddraw.dll"
    goto :success
)

echo [ERROR] No supported 32-bit C compiler found.
exit /b 1

:success
if not exist "!BUILT_DLL!" (
    echo [ERROR] Build completed without producing !BUILT_DLL!
    exit /b 1
)
copy /Y "!BUILT_DLL!" "%OUT_DLL%" >nul
if errorlevel 1 exit /b 1
echo.
echo [OK] Built: %OUT_DLL%
copy /Y "%OUT_DLL%" "%ROOT_DIR%\ddraw.dll" >nul
echo [OK] Copied to: %ROOT_DIR%\ddraw.dll

if "%INSTALL%"=="1" (
    echo [INSTALL] Copying ddraw.dll to nearby game folders...
    for %%D in (
        "%ROOT_DIR%"
    ) do (
        if exist "%%~D" (
            copy /Y "%OUT_DLL%" "%%~D\ddraw.dll" >nul
            echo   %%~D
        )
    )
)
exit /b 0

:ensure_msvc
where cl >nul 2>&1
if not errorlevel 1 exit /b 0

set "VCVARS="
for %%V in (
    "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist "%%~V" (
        set "VCVARS=%%~V"
        goto :call_vcvars
    )
)
exit /b 1

:call_vcvars
call "%VCVARS%" x86 >nul
where cl >nul 2>&1
exit /b %errorlevel%
