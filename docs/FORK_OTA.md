# 自建双板 OTA 发布与上游同步指南 / Fork Dual-Board OTA & Upstream Sync Guide

本文档说明如何在自己的公开 GitHub Fork 仓库中，实现 **Pico (`esp32s3`)** 与 **Metalio S31 (`esp32s31`)** 的独立版本编译、GitHub Releases 归档、GitHub Pages 纯 OTA 分发，以及如何持续合并上游 `wegooo-cell/read-pico-reader` 更新。

---

## 1. 架构特点

- **不改上游 `flash/` 与 `boards/*/board.json`**：最大限度减少每次合并上游时的 Git 冲突。
- **独立 OTA 源隔离**：编译时自动注入当前仓库的 GitHub Pages 地址（`https://<owner>.github.io/<repo>/`）到 `KIIKO_OTA_ORIGIN`，并移除上游域名回退，防止设备被官方 OTA 覆盖掉自定义功能。
- **无痛切换兼容**：保留原有 `project_name`（`Read_Pico` / `Kiikoread_Metalio_S31`），已刷官方固件的设备可通过一次 TF 卡本地升级切入自建固件，之后全部走设备端联网 OTA。
- **版本号递增规则**：在保持 `boards/*/board.json` 上游版本不变的前提下，在发布期派生修订号：
  - 上游为 RC 版（如 `0.3.3-rc108`）→ `0.3.3-rc108.1`、`0.3.3-rc108.2`；合并上游 `rc109` 后自动归零为 `0.3.3-rc109.1`。
  - 上游为正式版（如 `0.3.3`）→ `0.3.3-r1`、`0.3.3-r2`。
- **双板 / 单板统一发布**：GitHub Releases 使用独立 Tag（`ota-pico-v<ver>` / `ota-s31-v<ver>`）；GitHub Pages 无 `gh-pages` 分支，每次部署自动拉取双板最新清单与最近 3 个版本的 `.bin`，即使只发单板也不会冲掉另一板的线上 OTA。

---

## 2. 首次开启设置（只需一次）

1. 确保你的 GitHub Fork 仓库为 **Public**（ESP32 设备联网检查 OTA 时无法携带 GitHub 登录凭据）。
2. 打开 GitHub 仓库页面：**Settings → Pages → Build and deployment → Source**，选择 **GitHub Actions**。
3. （可选）如果需要自定义域名或显式指定仓库，可编辑根目录 [fork_ota.json](../fork_ota.json)：
   - 默认留空时，GitHub Actions 会自动由 `${{ github.repository }}` 推导出 `https://<owner>.github.io/<repo>/`；
   - 若你为 GitHub Pages 绑定了自定义域名（例如 `https://ota.example.com/`），把 `ota_origin` 填为该 HTTPS 地址（需以 `/` 结尾）。
4. 原上游工作流 [.github/workflows/pages.yml](../.github/workflows/pages.yml) 已加上 `github.repository == 'wegooo-cell/read-pico-reader'` 守卫，不会在 Fork 中误覆盖你发布的 OTA 站点。

---

## 3. 日常发布流程（GitHub Actions）

支持以下两种触发方式：

### 方式 A：推送 `release-*` 标签自动发布（默认双板）

```bash
git tag -a release-20261120 -m "本次更新说明（将显示在固件 OTA 提示和 Release 正文中）"
git push origin release-20261120
```

> 若仓库 `Settings → Environments → github-pages` 的 Deployment branches and tags 仅限 `main`，请在该环境规则中添加 `release-*` 标签放行（或改在 `main` 分支通过下方方式 B 触发）。

### 方式 B：GitHub 网页手动触发

1. 在 GitHub 仓库打开 **Actions → Fork OTA Release → Run workflow**。
2. 按需填写参数：
   - `board`：`all`（双板同时发，默认）、`pico` 或 `metalio47`。
   - `revision`：留空自动查询历史 Releases 递增；或手动指定正整数。
   - `notes`：设备端升级提示显示的更新说明（留空则自动生成；超长时自动按 UTF-8 字符边界截断到 `< 384` 字节）。
   - `skip_host_tests`：紧急跳过宿主测试（默认关闭）。
   - `dry_run`：仅跑测试、编译与打包校验，不创建 Release 也不部署 Pages。
3. 运行完成后，工作流会：
   - 创建对应的 GitHub Release（含 `.bin`、`.json`、`-TF.zip`、`-USB.zip`）；
   - 自动更新并发布 GitHub Pages（提供 `update.json`、`metalio-s31-update.json`、最近 3 版 `.bin` 与状态页 `index.html`）。

---

## 4. 首次从官方固件切入与日常升级

1. **首次切入（仅一次）**：
   - 从你的 Fork 仓库 **Releases** 下载对应板型的 `kiikoread-<board>-<version>-TF.zip`。
   - 解压出 `Pico-update.bin`（Pico）或 `Metalio-S31-update.bin`（Metalio S31）放入 TF 卡根目录。
   - 在设备 **「设置 → 升级和恢复 / 系统升级」** 中完成一次 TF 卡升级。
2. **后续升级**：
   - 设备连接 Wi-Fi 后，直接在 **「设置 → 系统升级」** 检查更新并安装，即会自动从你的 GitHub Pages OTA 源拉取并完成 SHA-256 校验与无感切换。

---

## 5. 上游发布新版本后的同步与发布

仓库提供辅助脚本 [tools/fork_sync_upstream.py](../tools/fork_sync_upstream.py)：

```bash
# 1. 首次若尚未配置 upstream 远端，可加 --add-remote；检查上游新版本、提交和冲突预警
python3 tools/fork_sync_upstream.py --check --add-remote

# 2. 安全合并上游（要求工作区干净，使用 --no-ff）
python3 tools/fork_sync_upstream.py --merge

# 3. 如有冲突，按以下原则解决：
#    - flash/ 与 boards/*/board.json 直接采用上游版本
#    - ota_online.h / ota_release.c / ota_online.c / pages.yml / build_firmware.py 保留我们的 KIIKO_OTA_ORIGIN 与 5 段版本扩展

# 4. 运行本地宿主回归校验
bash tools/run_fork_ota_host_tests.sh
```

确认无误后推送分支到自己的 Fork，然后在 GitHub Actions 中运行 **Fork OTA Release** 即可生成基于上游新版本的下一个 OTA 包（例如 `0.3.3-rc108.2` 自动变成 `0.3.3-rc109.1`）。
