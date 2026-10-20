#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#define ESP_MAC_WIFI_STA 0
#define ESP_MAC_BT 2
int esp_read_mac(uint8_t *mac, int type);

#ifdef __cplusplus
}
#endif