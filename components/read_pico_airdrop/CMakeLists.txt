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

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_aes_crypt_cbc"
    "-Wl,--wrap=esp_aes_crypt_ctr"
    "-Wl,--wrap=esp_aes_crypt_ecb"
    "-Wl,--wrap=esp_wifi_init"
)
