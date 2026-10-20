# AirDrop lifecycle host tests

本地回归，覆盖 AirDrop 接收生命周期与流式 ring 的 P1 修复：

- `ring_recycle_real_host_test.cpp` — R-05：把真实的 `ad_port_esp32.cpp` 编进测试 TU，
  断言每次中断后 4 个 64 KiB ring 块仍恰好归属一处。
- `airdrop_lifecycle_host_test.cpp` — R-06/R-08/R-09/R-10/R-18：真实
  `read_pico_airdrop.cpp` + 真实文件系统，验证临时文件/重命名、启动清理、失败清理、
  stop 前归档排空、resume 行为与 awdl 重试契约。

`stubs/` 只在本目录的宿主测试中使用：它们是 IDF/FreeRTOS/mbedTLS/AWDL 的最小平台桩，
不是 IDF 源码，也不参与固件构建。

Local regressions for the receive-lifecycle P1 fixes. `stubs/` is a minimal platform
surface used only by these host tests (not IDF sources, never built into firmware).

Run them through `bash tools/run_airdrop_host_tests.sh`.
