# ESP-IDF v6.x Windows 开发环境配置指南

## 环境信息
- **ESP-IDF路径**: `C:\esp\release-v6.0\esp-idf`
- **工具路径**: `C:\Espressif\tools`
- **Python虚拟环境**: `C:\Espressif\tools\python\release-v6.0\venv`
- **默认目标芯片**: ESP32-S3 (ESP32-S3-BOX-3)

## 快速设置（每次打开新终端）

### PowerShell 设置
```powershell
# 1. 设置环境变量
$env:IDF_PATH = "C:\esp\release-v6.0\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"

# 2. 激活Python虚拟环境
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\activate.ps1"

# 3. 验证环境
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" --version
```

### CMD 设置
```cmd
:: 1. 设置环境变量
set IDF_PATH=C:\esp\release-v6.0\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools

:: 2. 激活Python虚拟环境
call "C:\Espressif\tools\python\release-v6.0\venv\Scripts\activate.bat"

:: 3. 验证环境
"C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" --version
```

## 项目编译和烧录

### 基本操作
```powershell
# 进入项目目录
cd D:\Work\esp32\projects\esp32-ai-box

# 设置目标芯片（ESP32-S3）
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" set-target esp32s3

# 清理并编译
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" fullclean
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build

# 烧录到COM3（ESP32-S3-BOX-3默认串口）
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" -p COM3 flash

# 烧录并监控串口输出
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" -p COM3 flash monitor
```

### 快捷命令（一键编译烧录）
```powershell
# 一键编译烧录监控
$env:IDF_PATH="C:\esp\release-v6.0\esp-idf"
cd D:\Work\esp32\projects\esp32-ai-box
& "C:\Espressif\tools\python\release-v6.0\venv\Scripts\python.exe" "C:\esp\release-v6.0\esp-idf\tools\idf.py" -p COM3 flash monitor
```
