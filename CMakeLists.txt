# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 本地测试沿用当前正式版号；仅正式发布或明确的 OTA 验证指定新版本。
# Local tests keep the current release version; override only for publication or explicit OTA validation.
if(DEFINED ENV{PICO_PROJECT_VER} AND NOT "$ENV{PICO_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{PICO_PROJECT_VER}")
else()
    set(PROJECT_VER "0.3.3-rc97")
endif()
project(Read_Pico)

# 中文：定向体积优化。只给冷路径组件加 -Os；显示、触控、阅读热路径（main、epdiy、
# read_pico BSP、RTOS/内存/时钟/驱动）保持默认 -O2，避免把整机优化档位改成 -Os。
# English: targeted size optimization. Only cold-path components get -Os; the
# display/touch/reader hot paths (main, epdiy, read_pico BSP, RTOS/heap/clock/drivers)
# keep the default -O2 instead of switching the whole firmware to -Os.
#
# 依赖 -Os 省下的字节做发布门禁；列表变化后跑 idf.py build + tools/check_airdrop_build.py。
# The release gate depends on these bytes; after changing the list, rerun idf.py build and
# tools/check_airdrop_build.py.
set(PICO_SIZE_OPT_COMPONENTS
    read_pico_airdrop
    read_pico_transfer
    espressif__zlib
    fatfs
    esp_netif
    nvs_flash
    espressif__cjson
    espressif__tinyusb
    espressif__esp_tinyusb
    http_parser
    jpegdec
    espressif__qrcode
    esp_http_client
    esp_http_server
    tcp_transport
    esp-tls
    app_update
    ble_page_turner
    wear_levelling
    sdmmc
    esp_driver_sdmmc
    esp_driver_sdspi
    sc7a20h
    sy7636a
    read_pico_pmu
    pwm_audio
    esp_ringbuf
    lwip
)
foreach(_pico_cold_comp IN LISTS PICO_SIZE_OPT_COMPONENTS)
    idf_component_get_property(_pico_cold_lib ${_pico_cold_comp} COMPONENT_LIB)
    if(TARGET ${_pico_cold_lib})
        target_compile_options(${_pico_cold_lib} PRIVATE -Os)
    endif()
endforeach()
unset(_pico_cold_comp)
unset(_pico_cold_lib)

# 固件描述时间统一为 UTC，与概览页标识一致；保留已有编译缓存启动器。
# Compile app-description timestamps in UTC to match Overview; preserve any compiler-cache launcher.
idf_component_get_property(app_desc_lib esp_app_format COMPONENT_LIB)
get_target_property(app_desc_launcher ${app_desc_lib} C_COMPILER_LAUNCHER)
if(NOT app_desc_launcher)
    set(app_desc_launcher "")
endif()
set(app_desc_utc_launcher "${CMAKE_COMMAND}" -E env TZ=UTC)
if(app_desc_launcher)
    list(APPEND app_desc_utc_launcher ${app_desc_launcher})
endif()
set_property(TARGET ${app_desc_lib} PROPERTY C_COMPILER_LAUNCHER "${app_desc_utc_launcher}")

# panic 包装器的链接选项在 main/CMakeLists.txt 里挂到 main 组件上：写在 project() 之后
# 用 idf_build_set_property 设全局链接选项已经太晚，链接器看不到 --wrap。
# The panic wrapper's link option hangs off the main component in main/CMakeLists.txt: setting a
# global link option after project() is too late for the linker to see --wrap.
