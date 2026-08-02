# Copy this file to idf-env.ps1 (same directory as build_esp32.ps1) and
# fill in the paths for your local ESP-IDF installation. idf-env.ps1 is
# gitignored — it is local machine configuration, not part of the project.
#
# Usage: build_esp32.ps1 dot-sources this file to set $IDF_PATH (required)
# and, optionally, $IDF_TOOLS_PATH / $IDF_PYTHON_ENV_PATH so they match the
# values used by an existing ESP-IDF EIM (Espressif IDE / Installation
# Manager) installation.

$IDF_PATH = "C:\Espressif\frameworks\esp-idf-v6.0.1"

# Optional — set these to match your EIM installation if you have one;
# build_esp32.ps1 will then activate via EIM's activation script instead of
# IDF_PATH\export.ps1.
$IDF_TOOLS_PATH       = "C:\Espressif\tools"
$IDF_PYTHON_ENV_PATH  = "C:\Espressif\python_env\idf6.0_py3.11_env"
