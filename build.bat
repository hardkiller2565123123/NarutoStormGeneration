@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "ROOT=%CD%"
set "PROJECT_DIR=%ROOT%\work\NarutoStormGeneration"
set "SDK_DIR=%PROJECT_DIR%\third_party\rexglue-sdk"
set "BUILD_DIR=%PROJECT_DIR%\.build"
set "LOG_DIR=%BUILD_DIR%\logs"
set "GAME_DIR=%ROOT%\game"
set "BUILT_EXE=%BUILD_DIR%\NarutoStormGeneration.exe"
set "GAME_EXE=%GAME_DIR%\NarutoStormGeneration.exe"

if not exist "%PROJECT_DIR%\CMakeLists.txt" (
  echo Project source was not found at "%PROJECT_DIR%".
  goto fail
)
if not exist "%SDK_DIR%\CMakeLists.txt" (
  echo ReXGlue SDK source was not found at "%SDK_DIR%".
  goto fail
)
if not exist "%GAME_DIR%\default.xex" (
  echo Original game files were not found.
  echo Place your legally dumped Generations files in "%GAME_DIR%".
  goto fail
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE if exist "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

set "NINJA_EXE="
for /f "delims=" %%I in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%I"
if not defined NINJA_EXE if exist "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "NINJA_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "NINJA_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

set "LLVM_BIN=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin"
if not exist "%LLVM_BIN%\clang.exe" set "LLVM_BIN=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin"

set "VSDEV=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEV%" set "VSDEV=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

if not defined CMAKE_EXE (
  echo CMake was not found.
  goto fail
)
if not defined NINJA_EXE (
  echo Ninja was not found.
  goto fail
)
if not exist "%LLVM_BIN%\clang.exe" (
  echo LLVM clang was not found.
  goto fail
)
if not exist "%LLVM_BIN%\clang++.exe" (
  echo LLVM clang++ was not found.
  goto fail
)
if not exist "%VSDEV%" (
  echo Visual Studio developer tools were not found.
  goto fail
)

tasklist /FI "IMAGENAME eq NarutoStormGeneration.exe" /NH | find /I "NarutoStormGeneration.exe" >nul
if not errorlevel 1 (
  echo NarutoStormGeneration.exe is running.
  echo Close it before building so the game receives the new executable.
  exit /b 2
)

call "%VSDEV%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 goto fail

echo.
echo === Configuring Naruto Storm Generations Recomp ===
"%CMAKE_EXE%" -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G "Ninja" "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%" "-DCMAKE_C_COMPILER=%LLVM_BIN%\clang.exe" "-DCMAKE_CXX_COMPILER=%LLVM_BIN%\clang++.exe" -DCMAKE_BUILD_TYPE=Release "-DREXSDK_DIR=%SDK_DIR%" > "%LOG_DIR%\configure.log" 2>&1
if errorlevel 1 goto fail

echo.
echo === Building NarutoStormGeneration.exe ===
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --parallel 4 --target generations > "%LOG_DIR%\build.log" 2>&1
if errorlevel 1 goto fail
if not exist "%BUILT_EXE%" (
  echo The expected freshly built executable was not produced.
  goto fail
)

echo.
echo === Packaging game folder ===
copy /Y "%BUILT_EXE%" "%GAME_EXE%" > "%LOG_DIR%\package.log"
if errorlevel 1 goto fail

for %%F in ("%GAME_DIR%\generations.exe" "%GAME_DIR%\generations_runtime.exe" "%GAME_DIR%\NarutoGenerationsRecomp.exe" "%GAME_DIR%\NarutoStormGenerationRecomp.exe" "%GAME_DIR%\rexruntime.dll") do (
  if exist "%%~F" del /Q "%%~F"
)

echo.
echo Build complete.
echo Executable: "%GAME_EXE%"
echo Game data:  "%GAME_DIR%"
echo Build logs: "%LOG_DIR%"
exit /b 0

:fail
echo.
echo Build failed. Check "%LOG_DIR%" for details.
exit /b 1
