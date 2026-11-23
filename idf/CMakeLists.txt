cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 固定长度缓冲区上的 snprintf 截断是有意为之（配置/日志字段限长），
# 这类告警在 -Werror 下会阻断构建，这里显式关闭。
add_compile_options(-Wno-format-truncation)

# 固件版本号 = 构建时间（UTC），格式 YYYYMMDD.HHMM。
# 定宽纯数字，字典序即时间序，OTA 页面和 manifest 直接按字符串比较。
# CI 会用环境变量 PROJECT_VER 覆盖，保证"发布版本号 = CI 构建时间"。
if(NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "")
  string(TIMESTAMP PROJECT_VER "%Y%m%d.%H%M" UTC)
endif()
message(STATUS "固件版本号: ${PROJECT_VER}")

project(sms_forwarder)
