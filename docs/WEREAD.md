# 微信读书 / WeRead

设备入口为「书架 → 微信读书」。Pico 与 S31 共用登录、缓存、页面与同步服务；本地 TXT/EPUB 扫描范围保持原样。当前为本地测试功能：Pico 已确认真实扫码登录、正文、自导入封面，以及本轮缓存书架更流畅、从目录找回的位置退书重开后保持；跨端长时同步和 S31 实屏仍待验收。
Open **Shelf → 微信读书**. Pico and S31 share login, caching, UI and synchronization; local TXT/EPUB enumeration is unchanged. Pico QR login, first-page shelf and text display have been confirmed on hardware. Cross-device positions and S31 panel acceptance remain pending.

## 使用 / Use

- 连接 WiFi 后扫码，在手机上完成确认或验证码；凭据保存在当前 TF 卡。
  Connect WiFi, scan, and confirm or complete verification on the phone. Credentials belong to the current TF card.
- 首次先加载第一页最多6本，显示后其余每批最多20本在后台补齐；已有缓存先显示，可直接点书中断后台加载并优先开书。部分详情缺失保留已显示书籍，未补齐时不前移同步游标。
  Fetch at most6 books for the first page, then up to20 per background batch. Show cached books immediately; opening interrupts background loading. Missing details retain visible books without advancing the sync cursor.
- 默认网格，可切换列表；支持书名和作者搜索、分页、逐本封面加载。单击阅读，长按清理该书离线内容。清理保留阅读进度，退出登录删除所有凭据副本。
- 书架缓存先上屏；完整同步后5分钟内重复进入不自动联网同步，过期后空闲1秒再后台增量检查。手动刷新立即检查；失败暂缓重试，首次/未完成书架继续分批加载。
- 增量检查确认游标相同且所有变更集合为空时，保留原书架，仅原子更新很小的核对时间记录；部分同步、缺失详情、游标或账号变化仍走完整事务。
- 当前页先读完本地封面点阵，再补远端缺图；切换列表/网格复用同组像素，书架重排按书号和封面版本转移。账号、书号、原图地址或点阵格式变化使对应缓存失效；失败封面短期负缓存，手动刷新可重试。
  Start in grid view and switch to a list if preferred; search titles/authors and load covers progressively. Tap to read or hold to clear that book's content while retaining progress. Logout removes every credential copy.
- 普通开书先检查本机和云端位置；存在冲突时明确选择。离线保留书架、目录和已完整下载的章节，未缓存章节需要联网。
  Normal opens compare local/cloud positions and ask when they conflict. Shelf, TOC and complete chapters remain available offline; uncached chapters require connectivity.
- 云端未读空位置从首章打开。睡眠恢复从 TF 上按账号、版本与章节 UID 校验的位置恢复；损坏记录或未知章节会报错。普通新开书的位置候选在下载自愈后绑定最终目录版本。
  An unread cloud position starts at the first chapter. Sleep restoration validates the TF account, version and chapter UID; corrupt records or unknown chapters fail explicitly. A normal new-open choice binds to the final TOC after download recovery.
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

开书优先 fresh 请求 reMarkable 原生 `/shelf/syncbook` 的单书完整 `bookProgress`；精确匹配书号并要求 UID/目录序号/偏移/百分比四字段。缺失或不完整才走原生 `getProgress` 及其必要兼容回退，不能把缺字段伪造为未读。完整原生路径将进度读取降到一次请求。选择位置后复用刚同步目录；正文就绪即进入阅读，封面预热移到同一 worker 的阅读空闲期。

目录按账号和书籍版本长期复用；本次原生书籍元数据确认同版本时不因时间自动失效。缺少可靠版本号或版本为0时，每24小时兜底核对；旧书架版本和阅读位置中的版本不作为本次确认。开书仍先取最新单书进度及原生版本元数据，版本变化、目标UID缺失、章节序号不匹配、最新章节索引超出缓存或无可用目录时刷新。同版本的有效原子恢复副本也可使用。服务端进度/目录/正文版本矛盾时拒绝继续，包括版本自愈失败后的冷恢复。缓存章节命中时把已校验的typed目录直接交给阅读器；未缓存章节在下载解压前释放该目录，维持峰值内存预算。进入阅读器复用本次已校验的账号/书架快照；换卡、冷入口仍重绑定。

书架快照单遍导出，仅保留展示字段、封面身份和是否有封面，完整URL留在worker；800本单份快照减少约425KiB。目录和书架按ID排序查重，待补齐书号通过临时哈希表去重并保持服务端顺序，渐进批次通过节点游标推进。可见封面齐全或均处于退避时立即接续书架批次，缺图仍保留下载机会。每批提交的掉电恢复语义保持。CSS仅缓存当前资源代次，切书释放、资源根变化失效；目录/文件读取失败不发布缓存，无CSS章节不申请256KiB工作区。封面预热命中成品只检查文件，HTTPS跳转的中间响应体不写卡。

同一次位置映射复用已经校验的章节根目录读取正文和CSS，保留正文大小、完整读取和最终位置校验；缓存章节命中后不再重复探测。解包复用本次已确认的父目录，但每个文件仍检查路径、冲突、CRC和取消。

进度采样合并为一次读盘事务：先保存本次读秒/导航，再按原节流规则上报并返回最终持久化状态；清dirty落盘失败时仍返回待报状态。无变化的开书检查点不重复写盘。串口 `open timing` 分别记录进度、目录、章节及校验准备毫秒数，阅读页切换日志用于继续核对前台解析、排版和上屏耗时。
Each open first fetches a fresh single-book native reMarkable `/shelf/syncbook` position, matching the book ID and requiring UID, ordinal, offset and percentage. Missing/incomplete records retain the native `getProgress` compatibility path; they are never invented as unread positions. Complete native positions need one request. Choices reuse the synchronized TOC; ready text opens immediately and the same worker warms covers during reader idle.

开书在写入所选位置前验证最终目录的 UID/序号、原文偏移范围及映射文件完整性；旧版本的矛盾位置不会被直接改成本版或裁到章节边界。失败保留原本机 dirty 和待上报时长，并提示先在手机重新打开该书。
Before persisting a choice, validate the final UID/ordinal, source bounds and complete map file. Conflicting old-edition coordinates are neither relabeled nor clamped to a chapter boundary. Failure retains local dirty progress and pending reading time, asking the user to reopen the book on the phone.

书架先取ID，首屏6本后每批最多20本补齐，过滤讲书并按最近阅读排序；批次失败保留已提交页面。原子同步接口仍保留整轮失败不提交语义。章节版本失效仅刷新当前书的元数据。目录按 UID 合并，支持 clearAll。首次 EPUB 请求包含公共章节0，归档以4KiB输出流式解包，校验CRC、路径、大小与取消状态；支持Deflate分块边界的需要输入状态，并按 info.txt 顺序组装多文件正文。清单仅在存在bookVersion时核对版本。支持402/499提示和一次-2063版本自愈。
Shelf ID sync publishes the first6 books, then up to20 per batch, filters lectures and sorts by recent reading. Failed batches retain committed pages; the atomic sync API still preserves the prior complete snapshot on failure. Chapter-version recovery refreshes only that book. TOCs merge UIDs or replace on clearAll. Initial EPUB downloads include common chapter0; archives stream4KiB chunks with CRC/path/size/cancellation checks, handle Deflate input boundaries and assemble files in info.txt order. Validate manifest bookVersion only when present. Handle402/499 and retry version error-2063 once.

`position.map` 记录属性重写前后的位置锚点，将排版 UTF8 字节与原文 HTML UTF16 坐标双向映射，覆盖标签、实体、代理对和多文件。禁止直接上传排版字节；映射失败不上传猜测位置。旧的无映射缓存须重取。协议位置仍需真实账号跨端核验。

恢复时保留经校验的原始协议坐标、百分比和摘要。首次显示、字体/排版重排不算位置变化；仅阅读时间先存本地，等真实导航再合并上传，避免只读秒覆盖手机刚移动的位置。位置待报标志独立持久化，旧dirty记录仍可补报。成功翻页、目录/书签或章内跳转在提交时冻结新位置，之后的重排不改待上报目标。无新位置且无待报位置时，开书再退出不发位置请求。书架缓存有效期不影响开书时重新获取单书云端进度。
`position.map` anchors before/after attribute rewriting map normalized UTF8 bytes to original HTML UTF16 coordinates, including markup, entities, surrogate pairs and multiple files. Never upload normalized bytes or guessed mappings. Old caches without maps require downloading again. Cross-device coordinates still require real-account validation.

新组装正文保留原 ID 样式，脚注采用按文档隔离的短别名，防止合并文件同名 ID 混淆；只登记实际引用的目标，普通段落不挤占固定脚注槽。已下载的旧正文继续可离线阅读；需要脚注修复时，长按清理该书离线内容后联网重取，阅读进度保留。目标文件未包含在本章合并文件内的脚注仍不提取。
Newly assembled chapters retain original CSS IDs and use document-specific note aliases. Only referenced targets occupy the fixed note slots. Existing downloaded chapters remain readable offline; hold to clear a book's content and download again for the note fix, retaining progress. Targets outside the chapter's merged files remain unsupported.

RTC 未校时时，微信读书对时服从同一取消标志及截止时间，并保留其他任务的网络/校时所有权。S31 此有界路径使用 NTP；如果网络封锁 UDP/NTP，可先在系统时间设置中用原有手动对时（含 HTTPS Date 备用）完成校时，再打开微信读书。
When the RTC is invalid, WeRead time calibration follows its cancellation/deadline without preempting another owner. S31 uses NTP on this bounded path; if UDP/NTP is blocked, first use the existing system manual calibration (including its HTTPS Date fallback).

根目录 `/sdcard/.kiikoread/weread`：
Root `/sdcard/.kiikoread/weread`:

| 路径 / Path | 用途 / Purpose |
| --- | --- |
| `auth.json` | 凭据 / Credentials |
| `shelf.json` | 账号归属的离线书架 / Account-bound offline shelf |
| `shelf-check.json` | 绑定账号和书架游标的核对时间 / Last check bound to owner and shelf key |
| `cache/<id>/chapters.json` | 目录与版本 / TOC and version |
| `cache/<id>/progress.json` | 账号归属的位置和dirty状态 / Account-bound position and dirty state |
| `cache/<id>/chapter-<uid>.json`, `resources.json` | 完整目录提交指针 / Committed chapter/resource directories |
| 目录内 `body.html`, `position.map` | 组装正文与原文坐标锚点 / Assembled body and coordinate anchors |
| `cache/<id>/cover.img`, `cover.gray` | 原图与176×240成品点阵 / Source art and176×240 raster |

目录创建先核对现存目录，兼容FatFS挂载根目录不能mkdir的行为。FAT提交保留 `.previous`，中断后可恢复；退出删除凭据的主文件、临时文件和previous。封面支持 HTTPS/t6、404原图回退和5/15/45秒退避；首次开书预热真实封面。首页快速模式只保留1位封面，票根只读取成品缓存。
Directory creation accepts existing directories before mkdir, including FatFS mount roots. FAT commits retain `.previous` for interrupted-rename recovery; logout removes primary, temporary and previous credentials. Covers support HTTPS/t6, original-size fallback after404 and5/15/45-second backoff, with real-art warming on open. Fast Home retains only1-bit art; tickets read finished rasters only.

自导入封面允许最多三次显式 HTTPS 跳转到腾讯云等存储域；每跳重建头，外域剥离会话凭据，拒绝降级、循环超限和过长地址。支持 PNG/JPEG/WebP，HTTP200但解码失败也进入退避，避免重复下载阻塞其他任务。
Imported covers may follow at most three explicit HTTPS redirects to storage hosts. Every hop rebuilds headers and strips credentials outside qq.com; downgrade, excessive loops and oversized URLs fail. PNG/JPEG/WebP are accepted; decode failures back off even after HTTP200.

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

Pico用户已确认书架更流畅、从目录找回的位置退书重开后保持；第一阶段性能版连续开同一本书的第二次明显更快且位置正确。第二阶段用户反馈功能正常，速度变化不明显；一次实测worker准备6243ms，其中云进度阶段5184ms（包含网络准备），不是两版对照耗时。自导入封面此前已确认正常；旧86%位置未做估算恢复。仍待长时验证：会话续期、手机与设备交替阅读、S31正文及封面实屏、慢网切章、拔卡/换卡、浅睡与深睡恢复。宿主测试不构成这些验收。
Device acceptance remains: updated session renewal, phone/device positions both ways (including the reported86%-to-start mismatch), S31 text and both panels' covers, slow-network chapter changes, media loss/replacement and light/deep sleep. Host results do not constitute these checks.
