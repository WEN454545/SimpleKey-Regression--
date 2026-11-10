# ESP32-S3 三模键盘固件 - 顶层 CMakeLists.txt
# 作者：Regression
# 版本：1.0.0
# 日期：2026-09-18

cmake_minimum_required(VERSION 3.16)

# 包含 ESP-IDF 构建系统
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 定义项目（目标芯片通过 set-target 命令设置）
project(SimpleKey_TriMode)
