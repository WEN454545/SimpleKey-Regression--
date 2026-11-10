@echo off
setlocal

REM ESP32-S3 三模键盘固件 - 编译脚本
REM 作者：Regression
REM 版本：1.0.0
REM 日期：2026-09-18

set IDF_PATH=D:\programme\ESP32\Espressif\frameworks\esp-idf-v5.4
set IDF_PYTHON_ENV_PATH=D:\programme\ESP32\Espressif\python_env\idf5.4_py3.11_env
set PYTHON_PATH=D:\programme\ESP32\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe

set PATH=D:\programme\ESP32\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;D:\programme\ESP32\Espressif\tools\cmake\3.30.2\bin;D:\programme\ESP32\Espressif\tools\ninja\1.12.1;D:\programme\ESP32\Espressif\tools\idf-python\3.11.2\Scripts;D:\programme\ESP32\Espressif\tools\cmake\3.30.2\bin;%PATH%

cd /d D:\programme\ESP32\Working_Place\Three_SimpleKey\SimpleKey_Debug

echo ========================================
echo ESP32-S3 Tri-mode Keyboard Build Script
echo ========================================
echo.

if "%1"=="clean" (
    echo Cleaning build directory...
    if exist build rmdir /s /q build
    echo Clean complete.
    goto :end
)

if "%1"=="flash" (
    echo Flashing firmware...
    "%PYTHON_PATH%" "%IDF_PATH%\tools\idf.py" -p %2 flash
    goto :end
)

if "%1"=="monitor" (
    echo Starting serial monitor...
    "%PYTHON_PATH%" "%IDF_PATH%\tools\idf.py" -p %2 monitor
    goto :end
)

if "%1"=="menuconfig" (
    echo Opening menuconfig...
    "%PYTHON_PATH%" "%IDF_PATH%\tools\idf.py" menuconfig
    goto :end
)

echo Setting target to ESP32-S3...
"%PYTHON_PATH%" "%IDF_PATH%\tools\idf.py" set-target esp32s3
if errorlevel 1 (
    echo Error setting target!
    goto :end
)

echo.
echo Building project...
"%PYTHON_PATH%" "%IDF_PATH%\tools\idf.py" build
if errorlevel 1 (
    echo Build failed!
    goto :end
)

echo.
echo ========================================
echo Build successful!
echo ========================================
echo.
echo To flash: build.bat flash COM_PORT
echo To monitor: build.bat monitor COM_PORT
echo To clean: build.bat clean
echo.

:end
pause
