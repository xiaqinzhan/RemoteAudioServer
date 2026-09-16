# 顶层 CMakeLists.txt —— ESP-IDF v5.x 标准工程
# 多设备音频采集节点 (remote audio node)，目标芯片 ESP32-S3
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remote_audio_node)
