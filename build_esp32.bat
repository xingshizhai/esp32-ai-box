@echo off
rem =============================================================================
rem build_esp32.bat - load idf-env.bat, then delegate to build_esp32.ps1
rem
rem Usage:
rem   build_esp32.bat              full build (idf.py build)
rem   build_esp32.bat -ActivateOnly  activate env only
rem
rem Config: idf-env.bat in same directory (copy from idf-env.example.bat)
rem =============================================================================
setlocal

rem Find config file
set "CONFIG="
if defined ESP_IDF_BUILD_CONFIG (
    if exist "%ESP_IDF_BUILD_CONFIG%" set "CONFIG=%ESP_IDF_BUILD_CONFIG%"
)
if not defined CONFIG if exist "%~dp0idf-env.bat" set "CONFIG=%~dp0idf-env.bat"
if not defined CONFIG if exist "%USERPROFILE%\.esp-idf-build.bat" set "CONFIG=%USERPROFILE%\.esp-idf-build.bat"

if not defined CONFIG (
    echo ERROR: No config file found. Copy idf-env.example.bat to idf-env.bat.
    exit /b 1
)

rem Load config to set IDF_PATH, IDF_TOOLS_PATH, IDF_PYTHON_ENV_PATH
call "%CONFIG%"

rem Delegate to build_esp32.ps1.
rem It will use the inherited env vars (IDF_PATH etc.) to find EIM/activate.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_esp32.ps1" %*
