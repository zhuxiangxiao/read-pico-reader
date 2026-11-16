# 微信读书 / WeRead

设备入口为「书架 → 微信读书」。Pico 与 S31 共用登录、缓存、页面与同步服务；本地 TXT/EPUB 扫描范围保持原样。当前为本地测试功能，尚未用真实账号和双板实屏验收。
Open **Shelf → 微信读书**. Pico and S31 share login, caching, UI and synchronization; local TXT/EPUB enumeration is unchanged. This is a local test feature, pending real-account and physical-device acceptance.

## 使用 / Use

- 连接 WiFi 后扫码，在手机上完成确认或验证码；凭据保存在当前 TF 卡。
  Connect WiFi, scan, and confirm or complete verification on the phone. Credentials belong to the current TF card.
- 首次先加载第一页最多6本，显示后其余每批最多20本在后台补齐；已有缓存先显示，可直接点书中断后台加载并优先开书。部分详情缺失保留已显示书籍，未补齐时不前移同步游标。
  Fetch at most6 books for the first page, then up to20 per background batch. Show cached books immediately; opening interrupts background loading. Missing details retain visible books without advancing the sync cursor.
- 支持列表/网格、书名和作者搜索、分页、逐本封面加载；单击阅读，长按清理该书离线内容。清理保留阅读进度，退出登录删除所有凭据副本。
  Browse list/grid pages, search titles/authors and load covers progressively. Tap to read or hold to clear that book's content while retaining progress. Logout removes every credential copy.
- 普通开书先检查本机和云端位置；存在冲突时明确选择。离线保留书架、目录和已完整下载的章节，未缓存章节需要联网。
  Normal opens compare local/cloud positions and ask when they conflict. Shelf, TOC and complete chapters remain available offline; uncached chapters require connectivity.
- 首页继续阅读、锁屏票根和开机恢复支持虚拟书籍。睡眠恢复沿用本机保存的位置与全屏状态；启动中断时可清除恢复记录，保留内容和进度。
  Home resume, ticket locks and boot restoration accept virtual books. Sleep resumes retain the saved local position/fullscreen state. Interrupted-opening recovery can clear its checkpoint while retaining content and progress.

## 调用与任务所有权 / Calls and ownership

底层服务是同步单实例，`WR_GUARD` 提供前后台可重入串行锁。`weread_runtime` 是唯一后台任务调度入口，工作结果通过有界任务槽和独占快照转交页面。render 只绘制页面已接管的内存快照。
Services are synchronous singletons serialized by the reentrant `WR_GUARD`. `weread_runtime` owns one worker and a bounded dispatch slot; result snapshots transfer ownership to the page. Render uses only owned memory snapshots.

网络worker使用20KiB内部RAM栈：WiFi/NVS会关闭缓存，不能使用PSRAM栈；大请求头、下载响应和章节工作区放入PSRAM，避免联网后内部RAM碎片导致任务无法启动。2026-11-16 Pico实机已复现并修复这两类问题。
The network worker uses a20KiB internal-RAM stack because WiFi/NVS can disable caches. Large headers, download responses and chapter workspaces use PSRAM, avoiding worker creation failures caused by internal-memory fragmentation after networking. Both faults were reproduced and fixed on a Pico on2026-11-16.

JSON分配器在开机只注册一次；仅`WR_GUARD`线程作用域内的JSON节点和字符串使用PSRAM，其他线程仍默认分配。请求缓冲、目录数组、封面、映射与zlib工作区显式使用PSRAM；JSON限制32层，深层清理/CSS路径不占用递归大栈。目录同步减少重复树，进度签名缓冲可回收；内存不足返回错误并保留已提交缓存。
Register JSON hooks once at boot: JSON nodes/strings in `WR_GUARD` task scopes use PSRAM while other tasks retain default allocation. Request buffers, TOCs, covers, maps and zlib workspaces use PSRAM explicitly. Bound JSON nesting at32, keep recursive paths off large stack frames, reduce duplicate TOC trees and reclaim signing buffers. Allocation failures retain committed caches.


- `weread_runtime_start` / `poll`：登录、书架同步、开书检查、封面、清理和退出；调用方通过 `weread_runtime_result_free` 释放未接管的结果。
  Dispatch login, shelf sync, opening checks, covers, cleanup and logout; release unclaimed result allocations with `weread_runtime_result_free`.
- `weread_reader_prepare(uri, chapter)`：阅读器解析前由 worker 确保章节完整；前台只解析缓存。阅读空闲预读下一章，触摸/按键/翻页先取消并等待，切章前强制检查点。
  Ensure complete chapter content on the worker before foreground parsing. Idle reading prefetches the next chapter; input/turns cancel and join first, and chapter switches force a checkpoint.
- `weread_reader_begin/resume/tick/flush/end`：保存本机位置，累计实际活跃读秒与翻页；按60秒或十页节流，切章、退书、锁屏强制上报。先保存 dirty 再联网；强制交接有截止时间，失败保留 dirty 待重试。
  Track local position, active reading time and turns. Reports throttle at60 seconds or ten pages and are forced on chapter/exit/lock boundaries. Persist dirty state before networking; forced handoffs have a deadline and failed reports remain dirty for retry.
- `cancel_join` / `stop` / `media_lost`：只 shutdown 自有 HTTP socket，等请求/解包完成退出后才释放媒体。WiFi 的启动与停止都归同一 worker；只关闭自己启动的射频会话。拔卡擦除内存账号，进入时重新绑定当前卡。
  Shut down only the owned HTTP socket and join before releasing media. One worker starts/stops its own WiFi session, retaining any externally owned connection. Media loss wipes account memory, and entry rebinds to the current card.

`book_open("weread://<id>")` 使用 `BOOK_KIND_WEREAD`，共享 HTML/CSS、排版和图片接口。`book_file_path_supported()` 仍只用于本地扫描；只有明确打开/恢复目标使用虚拟 URI 门禁。虚拟 stat 身份包含账号、书号和版本，并限制在32位有符号范围内，避免跨账号进度复用。
Virtual opens use `BOOK_KIND_WEREAD` with shared HTML/CSS, layout and image APIs. Local scans still use `book_file_path_supported()`; virtual targets are admitted only at explicit open/resume gates. Virtual stat identities bind account/book/version within signed32-bit bounds to isolate NVS progress.

## 协议与缓存 / Protocol and caches

扫码结果接口采用65秒长轮询预算（实测空闲约55秒返回），接受`scan:0/1`状态；普通请求15秒，有截止时间的上报仍按剩余预算缩短。退出/锁屏可中断当前socket，无须等长轮询结束。
QR results allow65-second long polls (idle replies measured at about55s), including `scan:0/1` states. Ordinary requests use15 seconds; deadline-bound reports cap that wait. Exit/lock interrupts the active socket immediately.

认证支持顶层/data字段别名和受信任登录响应的 Set-Cookie 令牌，支持单次续期重放。非 HTTPS/非 qq.com 目标不携带凭据，禁止自动重定向；loopback 例外只存在于宿主测试。
Authentication accepts root/data aliases and trusted login Set-Cookie tokens, with a single renewal/replay. Non-HTTPS/non-qq.com destinations receive no credentials and redirects are disabled; loopback exceptions exist only in host tests.

书架先取ID，首屏6本后每批最多20本补齐，过滤讲书并按最近阅读排序；批次失败保留已提交页面。原子同步接口仍保留整轮失败不提交语义。章节版本失效仅刷新当前书的元数据。目录按 UID 合并，支持 clearAll。首次 EPUB 请求包含公共章节0，归档以4KiB输出流式解包，校验CRC、路径、大小与取消状态；支持Deflate分块边界的需要输入状态，并按 info.txt 顺序组装多文件正文。清单仅在存在bookVersion时核对版本。支持402/499提示和一次-2063版本自愈。
Shelf ID sync publishes the first6 books, then up to20 per batch, filters lectures and sorts by recent reading. Failed batches retain committed pages; the atomic sync API still preserves the prior complete snapshot on failure. Chapter-version recovery refreshes only that book. TOCs merge UIDs or replace on clearAll. Initial EPUB downloads include common chapter0; archives stream4KiB chunks with CRC/path/size/cancellation checks, handle Deflate input boundaries and assemble files in info.txt order. Validate manifest bookVersion only when present. Handle402/499 and retry version error-2063 once.

`position.map` 记录属性重写前后的位置锚点，将排版 UTF8 字节与原文 HTML UTF16 坐标双向映射，覆盖标签、实体、代理对和多文件。禁止直接上传排版字节；映射失败不上传猜测位置。旧的无映射缓存须重取。协议位置仍需真实账号跨端核验。
`position.map` anchors before/after attribute rewriting map normalized UTF8 bytes to original HTML UTF16 coordinates, including markup, entities, surrogate pairs and multiple files. Never upload normalized bytes or guessed mappings. Old caches without maps require downloading again. Cross-device coordinates still require real-account validation.

根目录 `/sdcard/.kiikoread/weread`：
Root `/sdcard/.kiikoread/weread`:

| 路径 / Path | 用途 / Purpose |
| --- | --- |
| `auth.json` | 凭据 / Credentials |
| `shelf.json` | 账号归属的离线书架 / Account-bound offline shelf |
| `cache/<id>/chapters.json` | 目录与版本 / TOC and version |
| `cache/<id>/progress.json` | 账号归属的位置和dirty状态 / Account-bound position and dirty state |
| `cache/<id>/chapter-<uid>.json`, `resources.json` | 完整目录提交指针 / Committed chapter/resource directories |
| 目录内 `body.html`, `position.map` | 组装正文与原文坐标锚点 / Assembled body and coordinate anchors |
| `cache/<id>/cover.img`, `cover.gray` | 原图与176×240成品点阵 / Source art and176×240 raster |

目录创建先核对现存目录，兼容FatFS挂载根目录不能mkdir的行为。FAT提交保留 `.previous`，中断后可恢复；退出删除凭据的主文件、临时文件和previous。封面支持 HTTPS/t6、404原图回退和5/15/45秒退避；首次开书预热真实封面。首页快速模式只保留1位封面，票根只读取成品缓存。
Directory creation accepts existing directories before mkdir, including FatFS mount roots. FAT commits retain `.previous` for interrupted-rename recovery; logout removes primary, temporary and previous credentials. Covers support HTTPS/t6, original-size fallback after404 and5/15/45-second backoff, with real-art warming on open. Fast Home retains only1-bit art; tickets read finished rasters only.

上限：JSON2MiB、书架1024本、目录20000章、下载32MiB、单条目8MiB、展开总量64MiB、正文4MiB、CSS256KiB、位置锚点16384。旧完整目录由清理操作回收。
Limits:2MiB JSON,1024 shelf books,20000 chapters,32MiB downloads,8MiB entries,64MiB expanded data,4MiB body,256KiB CSS and16384 anchors. Explicit cleanup reclaims older committed directories.

## 验证与本地交付 / Validation and local delivery

```sh
bash tools/run_weread_host_tests.sh
python3 tools/test_weread_native_e2e.py
bash tools/run_weread_e2e_tests.sh
```

C测试启用ASan/UBSan。受限内存池另覆盖800本渐进书架、3000章目录、线程分配隔离、JSON嵌套与OOM回滚/重试；可用`WEREAD_NATIVE_MEMORY=1 python3 tools/test_weread_native_e2e.py`运行受限池生产HTTP链路。原生HTTP测试运行生产网络、认证、书源、worker和进度代码，覆盖JSON/仅Cookie登录、多文件映射、冲突选择、离线落盘/补报、预读、阻塞请求取消、换卡、缓存清理与退出。Python Tier另含协议模拟，不能替代生产C或设备验收。
C tests use ASan/UBSan. Constrained-pool tests cover800 staged books,3000 chapters, task allocation isolation, JSON nesting and OOM rollback/retry. Use `WEREAD_NATIVE_MEMORY=1 python3 tools/test_weread_native_e2e.py` for constrained production HTTP E2E. Native HTTP tests exercise production networking/auth/source/worker/progress, including JSON/cookie-only login, multi-file mapping, conflict choices, offline persistence/retry, prefetch, blocked-request cancellation, card replacement, cleanup and logout. Python tiers also contain simulations and do not replace production C or device acceptance.

双板使用 `tools/build_firmware.py --board all` 独立构建与验证，产物仅放在ignored build目录。菜单调度和冷解析按体积编译，阅读绘制及图片热路径保持O2；没有扩大分区、删字库或改变波形/供电。保留正式版本号，不覆盖已发布下载、不自动刷机、不发布。
Build/verify both boards independently with `tools/build_firmware.py --board all`; outputs stay under ignored build directories. Size-optimize menu dispatch/cold parsing while keeping reader/image paint hot paths atO2. Partitions, fonts, waveforms and power remain intact. Keep published version numbers without replacing downloads, flashing or publishing.

实机待验：真实扫码/续期、手机与设备双向位置、双板正文与封面实屏、慢网切章、拔卡/换卡、浅睡与深睡恢复。宿主测试不构成这些验收。
Device acceptance remains: real QR/renewal, phone/device positions both ways, both panels' text/covers, slow-network chapter changes, media loss/replacement and light/deep sleep. Host results do not constitute these checks.
