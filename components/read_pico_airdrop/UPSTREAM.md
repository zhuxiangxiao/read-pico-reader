# Upstream and local changes

`components/read_pico_airdrop` is a port of the receive half of
[s-iwaki-d/ESP32Drop](https://github.com/s-iwaki-d/ESP32Drop), pinned at commit
`92bdc1221924ca522f3ccbebbd9353612ce5b2d7` (2026-09-07), licensed under ISC,
Copyright (c) 2026 MIRO. The verbatim license text is in `LICENSE`.

本组件把上游 ESP32Drop 的接收端（AWDL + AirDrop/TLS/HTTP + 文件归集）移植到本仓库的
ESP-IDF v6.1 组件体系，并增加设备侧生命周期、TF 卡写入与页面集成。

Local changes relative to upstream:

- ESP-IDF component wiring (`CMakeLists.txt`), `read_pico_airdrop.cpp` lifecycle/API,
  `wifi_init_wrap.c` and `aes_wrap.cpp` adaptation.
- `awdl/port/awdl_port_esp32.cpp`: ESP-IDF v6.1 adaptation (esp_timer millis, PSRAM
  buffers/static task stacks, bootstrap MIF cadence, teardown fixes).
- `airdrop/port/ad_port_esp32.cpp`: receive-side integration/teardown/completion fixes
  and the file handoff used by `read_pico_airdrop.cpp`.
- `airdrop_file_route.h` and `include/read_pico_airdrop.h` are local additions.
- Host tests imported under `tools/airdrop/` keep the upstream ISC notice.

本地改动以上述适配、修复和集成代码为主；解析器/协议/状态机以带注释的移植代码为主，
上游主机测试已导入 `tools/airdrop/` 并在 `tools/run_airdrop_host_tests.sh` 中运行。

The parser/protocol/state-machine code is mostly the annotated upstream port; the upstream
host suites are imported under `tools/airdrop/` and run by
`tools/run_airdrop_host_tests.sh`.
