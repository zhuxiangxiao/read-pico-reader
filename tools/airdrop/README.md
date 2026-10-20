# AirDrop host tests

Imported/adapted host tests for `components/read_pico_airdrop` plus the in-tree file
routing/CPIO streaming test.

## Origin and license

`tests/*_test.c` and `testdata/` come from
[s-iwaki-d/ESP32Drop](https://github.com/s-iwaki-d/ESP32Drop)
(commit `92bdc1221924ca522f3ccbebbd9353612ce5b2d7`), licensed under **ISC**,
Copyright (c) 2026 MIRO. The verbatim license text is kept in
`components/read_pico_airdrop/LICENSE`.

本地改动仅限 include 路径（指向 `components/read_pico_airdrop`）与统一的测试运行脚本；
测试逻辑未改。运行方式见 `tools/run_airdrop_host_tests.sh`。

Local changes are limited to include paths pointing at `components/read_pico_airdrop`
and a shared runner; the test logic is unchanged.

## Running

```sh
bash tools/run_airdrop_host_tests.sh
```

Artifacts go to `build/airdrop-tests/` (ignored). The runner exits non-zero on the
first failing compile or test, and prints a short log tail on failure.
