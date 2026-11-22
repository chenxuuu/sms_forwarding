cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 固定长度缓冲区上的 snprintf 截断是有意为之（配置/日志字段限长），
# 这类告警在 -Werror 下会阻断构建，这里显式关闭。
add_compile_options(-Wno-format-truncation)

project(sms_forwarder)
