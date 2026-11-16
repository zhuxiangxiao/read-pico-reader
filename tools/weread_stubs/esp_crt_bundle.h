/* 中文：仅宿主 loopback 测试的证书接口替身。/ English: Certificate shim for host loopback tests only. */
#pragma once
static inline int esp_crt_bundle_attach(void *conf) { (void)conf; return 0; }
