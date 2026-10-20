# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 独立隔空投送（Apple AirDrop 接收端）组件。/ Standalone Apple AirDrop receiver component.
idf_component_register(
    SRCS
        "read_pico_airdrop.cpp"
        "aes_wrap.cpp"
        "wifi_init_wrap.c"
        "awdl/port/awdl_port_esp32.cpp"
        "airdrop/port/ad_port_esp32.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES ble_page_turner esp_wifi esp_netif esp_event nvs_flash fatfs wear_levelling esp_timer mbedtls lwip
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "-Wno-comment"
    "-Wno-missing-field-initializers"
    "-Wno-unused-parameter"
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>
)

# 包住的符号只在别的静态库里被引用，必须用 -u 先把 wrapper 目标文件拉进链接，
# 否则链接器扫过本库后不会再回来取定义。
# The wrapped symbols are only referenced from another archive, so -u must pull the
# wrapper objects in before the linker passes this library.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_aes_crypt_cbc"
    "-Wl,-u,__wrap_esp_aes_crypt_cbc"
    "-Wl,--wrap=esp_aes_crypt_ctr"
    "-Wl,-u,__wrap_esp_aes_crypt_ctr"
    "-Wl,--wrap=esp_aes_crypt_ecb"
    "-Wl,-u,__wrap_esp_aes_crypt_ecb"
    "-Wl,--wrap=esp_wifi_init"
    "-Wl,-u,__wrap_esp_wifi_init"
)
