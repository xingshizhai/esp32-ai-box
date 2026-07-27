@echo off
rem Copy this file to idf-env.bat (same directory as build_esp32.bat) and
rem fill in the paths for your local ESP-IDF installation. idf-env.bat is
rem gitignored — it is local machine configuration, not part of the project.

set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v6.0.1"

rem Optional — set these to match your EIM installation if you have one.
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf6.0_py3.11_env"
