# 微信读书 / WeRead

底部导航为「首页 / 书架 / 微信读书 / 文件 / 设置」，直接点击「微信读书」进入。Pico 与 S31 共用登录、缓存、页面与同步服务；本地 TXT/EPUB 扫描范围保持原样。当前为本地测试功能：Pico 已确认真实扫码登录、正文、自导入封面，以及本轮缓存书架更流畅、从目录找回的位置退书重开后保持；跨端长时同步和 S31 实屏仍待验收。
Open **微信读书** in the five-tab bar: Home / Shelf / WeRead / Files / Settings. Pico and S31 share login, caching, UI and synchronization; local TXT/EPUB enumeration is unchanged. Pico QR login, first-page shelf and text display have been confirmed on hardware. Cross-device positions and S31 panel acceptance remain pending.

## 使用 / Use

- 进入先从本地确认登录状态；此时显示书架加载状态，已登录不会闪出扫码页。确认未登录后立即尝试联网生成二维码，在手机上完成确认或验证码；可随时从底栏切换页面，网络失败可点「连接 WiFi」。凭据保存在当前 TF 卡。
  Entry checks local authentication first without flashing the login screen. A confirmed unauthenticated entry immediately requests a QR code. Confirm or complete verification on the phone; the bottom navigation remains available. Use Connect WiFi if needed. Credentials belong to the current TF card.
- 首次先加载一批最多6本，显示后其余每批最多20本在后台补齐；已有缓存先显示，点书立即显示「正在准备阅读」，取消旧任务后只打开最后选择的书。缺失详情单独进入持久待补队列；原生成员变化、进度、待补队列与同步游标同一事务保存。
  Fetch at most6 books initially, then up to20 per background batch. Show cached books immediately; a tap gives immediate preparation feedback and opens the latest choice after cancelling old work. Persist unresolved details together with native membership, progress and cursor in one transaction.
- 与本地书架共用外观设置：深色书轨、亚克力、液态玻璃均为9宫格，列表每页8本；搜索时临时使用列表。封面尺寸、裁切、进度角标、搜索、分页和排序控件共用绘制组件。
- 两个书架分别保留搜索、页码和排序，切换入口再回来恢复各自状态。云端排序使用书名、最近阅读、更新时间和进度；书目以云端为准。题头「更多」提供刷新书架、WiFi和退出登录；长按书籍清理其离线内容，保留进度。退出登录删除所有凭据副本并立即请求新二维码。
- 从微信书架开书返回微信书架，从首页继续阅读返回首页，本地书籍返回本地书架；睡眠恢复保持阅读来源、位置和全屏状态。老固件保存的文件/设置页编号继续兼容。
- 书架缓存先上屏；完整同步后5分钟内重复进入不自动同步，过期后空闲1秒再后台增量检查。手动刷新立即检查；首次加载继续分批，最终仍缺失的详情延迟5分钟补取，已有书籍保持可读。
- 增量检查确认游标相同且所有变更集合为空时，保留原书架，仅原子更新很小的核对时间记录；部分同步、缺失详情、游标或账号变化仍走完整事务。
- 后续只获取原生增量中发生变化的书及持久待补详情；阅读进度令全局游标变化时，不会把已成功详情全部重取。切页、开书或重启后继续使用已提交缓存。旧格式缓存安全迁移：已有完整缓存沿用增量，旧中断批次仅在目标游标相同且有确认标记时复用，否则重新核实。
- 成功响应缺少书号仅表示详情尚未取到，不将其伪造成永久不可用。只有返回有效详情、明确不支持的格式、讲书或删除记录才从待补队列移除；尚无详情的新书也保存原生进度，后续补到详情时恢复。原子同步接口仍保持整轮失败不提交。
- 当前页先读取本地点阵，再补远端缺图；切换列表/网格复用同组像素，书架重排按书号和封面版本转移。成品灰阶封面在两板上各保留一个小容量 PSRAM LRU（Pico 24 槽 / 768 KiB，字节上限约 18 张成品封面；S31 96 槽 / 4 MiB），翻回看过的页直接从内存出图；本地 `.gray` 或内存命中的结果立即合并上屏，不再逐本等 150ms，只有远端下载完成仍按 150ms 合并刷新。后台仅为翻页方向相邻的两本书预热磁盘已有原图，不提前下载邻页，也不常驻整页像素。开书前 Pico 把该缓存收到 128 KiB（S31 512 KiB），避免挤占阅读与字体 PSRAM。隐形预热和预连接不刷新画面。账号、书号、原图地址或点阵格式变化使对应缓存失效（RAM 键含账号代次与书号）；失败封面短期负缓存，手动刷新可重试。
  The current page reads local rasters first and fills missing covers remotely; list/grid reuse the same pixel group and shelf reorders follow the book id and cover version. Finished grayscale covers stay in a small PSRAM LRU on both boards (Pico 24 slots / 768 KiB, capping ~18 finished covers; S31 96 slots / 4 MiB) so revisiting a page paints from RAM; local .gray or RAM hits merge immediately instead of one 150 ms delay per book, and only remote completions keep the 150 ms merge. Background warming still touches only the two neighbours in the turn direction, never downloads ahead and never keeps a whole pixel page. Before opening a book the Pico trims this cache to 128 KiB (S31 512 KiB) so reading and font PSRAM keep priority. Invisible warming and preconnection never repaint; account/book-id/raw-URL/raster-format changes invalidate entries (RAM keys include the account epoch and book id); failed covers keep a short negative cache and a manual refresh retries them.
- 书架空闲时可提前准备网络；每次进入最多尝试一次。仅自己启动的WiFi在最近实际网络活动60秒后空闲关闭，关闭后不会在同一次进入反复重连；其他功能持有的网络连接不关闭。
  Both shelves share appearance and drawing components, with nine grid entries or eight list rows. Each retains its own query/page/order on tab switches. More provides refresh, WiFi and logout; hold a book to clear its content while retaining progress. Returning from reading follows its Home/local/WeRead origin, including sleep restoration.
- 点开书籍优先检查本地进度、目录与缓存章节：若本地完整就绪则直接打开阅读，不阻塞等待网络；入书后后台异步执行 `WR_JOB_CLOUD_SYNC` 核对云端进度。仅云端 `(chapter_index, chapter_offset)` 严格领先当前实时阅读位置时，在后台准备好目标目录与章节后静默跳转并提示「已同步云端进度」；本地领先或持平则仅静默对齐云端基线，取消全部「继续本机 / 使用云端」二选一弹窗。首次未缓存开书仍联网准备并直接打开。离线保留书架、目录和已完整下载的章节。
  Book opens check local progress, TOC and cached chapter first: complete local caches open immediately into reading without waiting on the network, while background `WR_JOB_CLOUD_SYNC` reconciles cloud progress asynchronously. Only when cloud `(chapter_index, chapter_offset)` is strictly ahead of the live reader position does background sync fetch the target chapter and jump automatically with a brief “已同步云端进度” notice; when local is ahead or tied, only the sync baseline is silently rebased, with all two-choice conflict modals removed. Uncached first opens still fetch over the network directly into reading. Shelf, TOC and complete chapters remain available offline.
- 首次开书与冷切章按阶段更新提示：书架「正在准备阅读…」之后依次显示「正在同步进度…」「正在获取目录…」「正在下载章节 xx KB…」「正在校验章节…」，阅读内冷切章同样用阶段文案替换静态提示。单次瞬时网络超时（ESP_ERR_TIMEOUT）自动静默重试一次，重试期间显示「网络超时，正在重试…」；阅读页同步等待的目录准备不重试（避免开书冻结加倍）；权限、下架、版本、内存和取消类错误不重试，仍按原错误提示处理。
  First opens and cold chapter loads update their label by phase: after the shelf's preparation notice they show syncing progress, fetching the TOC, downloading the chapter (with KB) and verifying the chapter; in-reader cold turns replace the static notice the same way. One transient network timeout (ESP_ERR_TIMEOUT) is retried silently once with a retrying label; the TOC preparation that the reader page joins synchronously never retries so an open cannot freeze twice as long; permission, delisted, version, memory and cancellation errors never retry and keep their existing messages.
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
- `weread_reader_prepare(uri, chapter)` / `weread_reader_cached`：已缓存章不因其他章节预取、封面预热、后台云端同步或进度上报而阻塞，直接本地翻章；阅读内所有翻页（含文字与图片）及面板/目录操作均非阻塞（不调用 `cancel_join`），仅触发前景活跃冷却暂停后台连续拉取。冷章若恰逢同章正在后台预取，则直接接管该预取任务而不再取消重来；后台在翻页静止 300ms 后优先预取下一章（先于云端进度上报与封面预热）。下载、解包与网络传输全程不持 `WR_GUARD`。普通退书（返回书架/主页）0ms 非阻塞退出：保存本机进度，正在上报的 `WR_JOB_NOTE`/`WR_JOB_REPORT` 留在后台完成，整本预取温和取消；仅锁屏/休眠与拔卡等待安全停机。
- `weread_reader_stage` / `weread_reader_stage_text`：只读前台开书/冷切章阶段快照；`weread_reader_chapter_cached` 供目录标记，`weread_reader_cache_forward_set/get` 控制会话级手动缓存开关。均不改变单 worker 所有权。
  `weread_reader_stage` / `weread_reader_stage_text` expose foreground open/cold-chapter stage snapshots; `weread_reader_chapter_cached` feeds TOC markers and `weread_reader_cache_forward_set/get` control the session-only manual cache switch. Single-worker ownership is unchanged.
  `weread_reader_prepare(uri, chapter)` / `weread_reader_cached`: cached chapters flip immediately without holding `WR_GUARD` over network HTTP or archive extraction, and all in-reader interactions (both text and image turns, TOC, and panels) plus ordinary shelf/home exit are non-blocking without `cancel_join`. Reaching an uncached boundary while the same UID is already being prefetched adopts that in-flight job instead of restarting from scratch; next-chapter prefetch leads at 300ms ahead of reporting and cover warming. Normal book exits save locally, let in-flight `WR_JOB_NOTE`/`WR_JOB_REPORT` finish in the background, and cancel whole-book prefetch non-blockingly; only lock/sleep and card loss perform blocking hardware teardown.
- `weread_reader_begin/resume/tick/flush/end`：真实导航和活跃读秒先存本地。普通位置上报按60秒或十页节流，切章后的真实导航优先补报；切章、退书和锁屏不再同步等待云端写入。只有业务响应明确 `succ=1/true` 才清除已确认代次的待报状态；确认写卡失败仍保留待报。睡眠只做可靠本地保存和有界取消，不为一次网络成功延长等待。
  Persist real navigation and active reading time first. Ordinary position reports wait60 seconds or ten pages, with committed chapter changes prioritized. Chapter, exit and lock no longer synchronously wait for cloud writes. Only explicit `succ=1/true` acknowledges a revision; failed acknowledgement commits remain pending. Sleep saves locally and cancels work without waiting for a successful network exchange.
- 持久outbox合并同一本书的新位置，书架空闲时也可补报已关闭书籍；单次扫描最多8个目录。失败按15、30秒递增退避，最长240秒，取消不增加退避。每次补报先取新鲜原生云端基线，核对账号、完整会话与实际版本：若云端基线变化且严格领先本地，则自动采用云端进度并清掉旧待报；若本地领先或同基线，则自动对齐云端基线后上行，不再锁定冲突状态（仅 NVS/TF 应急账本防篡改校验仍保留冲突错误）。
  A durable coalescing outbox drains closed books during shelf idle, scanning at most eight directories per slice. Failures back off from15 seconds to a240-second cap; cancellation does not penalize retries. Each send checks a fresh native cloud baseline, owner/session and actual edition: if cloud drifted ahead of local, cloud is adopted automatically; if local is ahead or matches baseline, the baseline rebases cleanly before POSTing without conflict lockouts (retaining only rescue-journal tamper protection).
- 阅读页区分待保存、本地保存、待同步、已同步和进度冲突；状态变化合入既有800ms空闲页脚更新。写卡失败时可用设备内部两槽应急账本（每槽≤1280B），绑定卡CID、账号、书籍、设备与版本；显示「已暂存设备」，不等同云端成功。正常写卡不写该账本。恢复原卡后先以旧/新快照核对恢复TF，再联网。两槽已满或内部写失败时保留RAM并阻止普通退书/锁屏清掉位置；断电前仍须解决存储错误。
  Footer states distinguish unsaved, local, pending, acknowledged and conflicting positions, coalesced into the existing800ms idle update. TF failures may use two device rescue slots of at most1280 bytes each, bound to card CID/owner/book/device/edition. “Saved on device” is not a cloud acknowledgement. Healthy TF writes never use rescue storage. Restore the original card with snapshot comparison before networking. Full slots/internal failures retain RAM and block ordinary exit/lock from discarding it; storage errors must be resolved before power loss.
- `cancel_join` / `stop` / `media_lost`：网络轮询由worker退出并关闭自有句柄，资源释放前仍等待完成。WiFi启停归同一worker，只关闭自己启动的会话。拔卡在清账号前尝试抢救已验证位置；无法映射或两种存储均失败时保留待保存状态，不能报告成功；重新进入须绑定当前卡。
  The polling worker closes its own handle; teardown joins before freeing resources. One worker owns WiFi lifecycle and leaves external sessions intact. Media loss attempts rescue before forgetting credentials; unavailable mappings or both storage failures remain unsaved rather than reporting success. Entry rebinds to the current card.

`book_open("weread://<id>")` 使用 `BOOK_KIND_WEREAD`，共享 HTML/CSS、排版和图片接口。`book_file_path_supported()` 仍只用于本地扫描；只有明确打开/恢复目标使用虚拟 URI 门禁。虚拟 stat 身份包含账号、书号和版本，并限制在32位有符号范围内，避免跨账号进度复用。
Virtual opens use `BOOK_KIND_WEREAD` with shared HTML/CSS, layout and image APIs. Local scans still use `book_file_path_supported()`; virtual targets are admitted only at explicit open/resume gates. Virtual stat identities bind account/book/version within signed32-bit bounds to isolate NVS progress.

阅读中的虚拟身份只随后续成功接管的实际正文版本变化；书架元数据后台更新不提前重标书签。新版目录重排后旧版按序号保存的NVS书签不会当作新版书签；布局失败保留旧身份。长章缺少RAM映射又发生拔卡时，保留本次冻结的UID/序号/版本/解析类型；插回原卡后可独立深验原章节成品完成精确映射，不临时打开或覆盖另一书源。原版成品已损坏或被换版时继续保留待保存状态并报错。
The virtual identity follows only successfully committed content, not background shelf metadata. Reordered new editions reject old ordinal-based NVS bookmarks; failed layout retains the old identity. If a long chapter lacks a RAM map when media disappears, retain its captured UID/ordinal/edition/parser. The original card can later supply a deeply validated detached mapping without opening or replacing a source. Missing, damaged or replaced original content remains an explicit unsaved error.

## 协议与缓存 / Protocol and caches

扫码结果接口采用65秒无进展预算（实测空闲约55秒返回），接受`scan:0/1`状态；普通请求与下载采用15秒无进展预算，成功读写后刷新，有独立截止时间的调用仍受硬截止约束，不因持续传输延长。WeRead唯一worker的HTTPS传输使用异步DNS及SDK异步open/write/header/read轮询；单次socket等待50ms，DNS、连接、响应头和正文阶段均检查取消。取消不必等待整段15/65秒预算，但证书解析等单次SDK内部CPU工作、SDK关闭与WiFi资源交接仍需真机计时，不能承诺整条退出流程严格50ms。
QR polls retain a65-second no-progress budget; other requests/downloads use15 seconds, refreshed by successful I/O and capped by any caller hard deadline. The sole WeRead worker polls asynchronous DNS and SDK HTTPS operations with50ms socket waits, checking cancellation throughout. This avoids waiting for the full network timeout; certificate CPU work, cleanup and WiFi handoff still require hardware timing and do not have a claimed50ms total bound.

认证支持顶层/data字段别名和受信任登录响应的 Set-Cookie 令牌，支持单次续期重放。非 HTTPS/非 qq.com 目标不携带凭据，禁止自动重定向；loopback 例外只存在于宿主测试。
Authentication accepts root/data aliases and trusted login Set-Cookie tokens, with a single renewal/replay. Non-HTTPS/non-qq.com destinations receive no credentials and redirects are disabled; loopback exceptions exist only in host tests.

数字IP仅用于连接，Host保留原authority，TLS的SNI、证书名称与证书bundle继续验证原域名；凭据是否允许发送也按原始HTTPS域名判断。DNS最多保留两个取消后的迟回调对象，每个≤384B内部RAM；端点固定部分≤704B及最长2048字节URL放在PSRAM，不新增网络线程。后台预读、封面或补报遇401保留凭据并返回失败，前台操作继续走原单次续期/撤销流程。网络日志只记匿名路由类别、请求计数、DNS/连接与响应头/正文/清理耗时、字节数和结果，不输出令牌或完整URL。
Numeric peers preserve the original Host, SNI, certificate name/bundle and credential origin. DNS retains at most two late-callback contexts, each bounded to384 internal-RAM bytes; endpoint metadata (at most704 bytes) and URLs (at most2048 bytes) use PSRAM. No extra network worker is created. Background401 failures retain credentials; foreground calls retain their single renewal/revocation policy. Logs expose route categories, counts, phase timings, sizes and results without tokens or full URLs.

开书优先 fresh 请求 reMarkable 原生 `/shelf/syncbook` 的单书完整 `bookProgress`；精确匹配书号并要求 UID/目录序号/偏移/百分比四字段。缺失或不完整才走原生 `getProgress` 及其必要兼容回退，不能把缺字段伪造为未读。完整原生路径将进度读取降到一次请求。选择位置后复用刚同步目录；正文就绪即进入阅读，封面预热移到同一 worker 的阅读空闲期。

目录按账号和书籍版本长期复用；本次原生书籍元数据确认同版本时不因时间自动失效。缺少可靠版本号或版本为0时，每24小时兜底核对；旧书架版本和阅读位置中的版本不作为本次确认。开书仍先取最新单书进度及原生版本元数据，版本变化、目标UID缺失、章节序号不匹配、最新章节索引超出缓存或无可用目录时刷新。同版本的有效原子恢复副本也可使用。服务端进度/目录/正文版本矛盾时拒绝继续，包括版本自愈失败后的冷恢复。缓存章节命中时把已校验的typed目录直接交给阅读器；未缓存章节在下载解压前释放该目录，维持峰值内存预算。进入阅读器复用本次已校验的账号/书架快照；换卡、冷入口仍重绑定。

书架快照单遍导出，仅保留展示字段、封面身份和是否有封面，完整URL留在worker；800本单份快照减少约425KiB。目录和书架按ID排序查重，待补齐书号通过临时哈希表去重并保持服务端顺序，渐进批次通过节点游标推进。可见封面齐全或均处于退避时立即接续书架批次，缺图仍保留下载机会。每批提交的掉电恢复语义保持。CSS仅缓存当前资源代次，切书释放、资源根变化失效；目录/文件读取失败不发布缓存，无CSS章节不申请256KiB工作区。封面预热命中成品只检查文件，HTTPS跳转的中间响应体不写卡。

同一次位置映射复用已经校验的章节根目录读取正文和CSS，保留正文大小、完整读取和最终位置校验；缓存章节命中后不再重复探测。解包复用本次已确认的父目录，但每个文件仍检查路径、冲突、CRC和取消。

进度采样只保存本地，网络补报由独立任务消费同一持久记录；ACK不得清除较新导航。无变化的开书检查点不重复写盘。账号切换前原子归档旧账号进度，清内容缓存保留全部进度副本。串口 `open timing` 分别记录进度、目录、章节及校验准备毫秒数，阅读页日志区分绘制、推屏与前台等待。
Each open first fetches a fresh single-book native reMarkable `/shelf/syncbook` position, matching the book ID and requiring UID, ordinal, offset and percentage. Missing/incomplete records retain the native `getProgress` compatibility path; they are never invented as unread positions. Complete native positions need one request. Choices reuse the synchronized TOC; ready text opens immediately and the same worker warms covers during reader idle.

开书在写入所选位置前验证最终目录的 UID/序号、原文偏移范围及映射文件完整性；旧版本的矛盾位置不会被直接改成本版或裁到章节边界。失败保留原本机 dirty 和待上报时长，并提示先在手机重新打开该书。
Before persisting a choice, validate the final UID/ordinal, source bounds and complete map file. Conflicting old-edition coordinates are neither relabeled nor clamped to a chapter boundary. Failure retains local dirty progress and pending reading time, asking the user to reopen the book on the phone.

书架先取ID，首屏6本后每批最多20本补齐，过滤讲书并按最近阅读排序；批次失败保留已提交页面。原子同步接口仍保留整轮失败不提交语义。章节版本失效仅刷新当前书的元数据。目录按 UID 合并，支持 clearAll。首次 EPUB 请求包含公共章节0，归档以4KiB输出流式解包，校验CRC、路径、大小与取消状态；支持Deflate分块边界的需要输入状态，并按 info.txt 顺序组装多文件正文。清单仅在存在bookVersion时核对版本。支持402/499提示和一次-2063版本自愈。
Shelf ID sync publishes the first6 books, then up to20 per batch, filters lectures and sorts by recent reading. Failed batches retain committed pages; the atomic sync API still preserves the prior complete snapshot on failure. Chapter-version recovery refreshes only that book. TOCs merge UIDs or replace on clearAll. Initial EPUB downloads include common chapter0; archives stream4KiB chunks with CRC/path/size/cancellation checks, handle Deflate input boundaries and assemble files in info.txt order. Validate manifest bookVersion only when present. Handle402/499 and retry version error-2063 once.

`position.map` 记录属性重写前后的位置锚点，将排版 UTF8 字节与原文 HTML UTF16 坐标双向映射，覆盖标签、实体、代理对和多文件。禁止直接上传排版字节；映射失败不上传猜测位置。旧的无映射缓存须重取。协议位置仍需真实账号跨端核验。

当前正文与待接管正文分别绑定真实资源目录、UID和版本；排版成功后才交接。精确映射表每份最多8192点（64KiB PSRAM），两份并发上限128KiB，正文过长或分配失败回到原有完整映射，不稀疏猜测位置。空闲预读从当前章起沿目录顺序向后连续缓存后续全部章节直至全书末尾（遇前台翻页/触控暂停并冷却后继续），页脚显示「已缓存 X/Y 章」；最多连续3次实际失败后停止，输入取消不计失败。后台缺章预留104MiB最坏事务空间，空间不足暂停预读并提示「缓存空间不足(X/Y)」，已有完整章继续可读。下载就绪、已缓存和已呈现分开，页脚不把下载完成说成已经显示。目录页在章节行右缘用深灰圆点标出本机已缓存章节（仅微信读书，进入/翻页时按可见 7 行一次性查询，绘制阶段不再读卡）；阅读工具栏为微信读书追加「缓存」工具，默认后台整本补齐处于启用态（图标带点），点一次暂停整本向后补齐，再点一次立即恢复并清除此前的失败/退避/空间暂停；若后台补齐已因连续失败或空间不足自动停住（开关仍亮），点一次即重新触发，不必先暂停。暂停只停新的派发，自动下一章预读、进度上报与已开始章节照常进行。
  Download readiness, offline availability and presentation stay separate; the footer never reports a download as already displayed. TOC chapter rows mark locally cached chapters with a dark-gray dot (WeRead only; the visible seven rows are probed once on entry/page change, never while painting), and the reader toolbar gains a WeRead-only cache tool: whole-book fill is enabled by default (icon dot lit), one tap pauses the forward fill and the next resumes eagerly after clearing earlier failures/backoff/space pauses; if the automatic fill has already stalled on repeated failures or low space (switch still lit), one tap retriggers it without pausing first, and pausing only halts new dispatches while the urgent next-chapter pre-read, progress reports and already-started chapters continue.
Active and staged content bind their actual directory, UID and edition, committing only after successful layout. Exact maps use at most8192 points/64KiB each,128KiB across two PSRAM bindings; long content/OOM uses the full mapping path without sparse guesses. Idle prefetch sequentially caches forward from the current chapter through the end of the book (pausing during foreground touch/turn activity and showing “已缓存 X/Y 章” in the footer), stops after three actual failures and does not count input cancellation. Missing background chapters reserve104MiB of worst-case transaction space; insufficient space pauses prefetch with “缓存空间不足(X/Y)” while complete cached chapters remain readable. Download readiness, offline availability and presentation are distinct states.

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
| `shelf.json` | 账号归属的离线书架、原生游标及`detailPending`待补详情队列 / Account-bound shelf, native cursor and unresolved-detail queue |
| `shelf-check.json` | 绑定账号和书架游标的核对时间 / Last check bound to owner and shelf key |
| `cache/<id>/chapters.json` | 目录与版本 / TOC and version |
| `cache/<id>/progress.json` | 账号归属的位置和dirty状态 / Account-bound position and dirty state |
| `cache/<id>/progress.json.<vid>` | 切账号前保存的独立进度；内容清理保留 / Atomic owner archive, retained by content cleanup |
| `cache/<id>/chapter-<uid>.json`, `resources.json` | 完整目录提交指针 / Committed chapter/resource directories |
| 目录内 `body.html`, `position.map` | 组装正文与原文坐标锚点 / Assembled body and coordinate anchors |
| `cache/<id>/cover.img`, `cover.gray` | 原图与176×240成品点阵 / Source art and176×240 raster |

目录创建先核对现存目录，兼容FatFS挂载根目录不能mkdir的行为。FAT提交保留 `.previous`，中断后可恢复；退出删除凭据的主文件、临时文件和previous。封面支持 HTTPS/t6、404原图回退和5/15/45秒退避；首次开书预热真实封面。首页快速模式只保留1位封面，票根只读取成品缓存。
Directory creation accepts existing directories before mkdir, including FatFS mount roots. FAT commits retain `.previous` for interrupted-rename recovery; logout removes primary, temporary and previous credentials. Covers support HTTPS/t6, original-size fallback after404 and5/15/45-second backoff, with real-art warming on open. Fast Home retains only1-bit art; tickets read finished rasters only.

自导入封面允许最多三次显式 HTTPS 跳转到腾讯云等存储域；每跳重建头，外域剥离会话凭据，拒绝降级、循环超限和过长地址。支持 PNG/JPEG/WebP，HTTP200但解码失败也进入退避，避免重复下载阻塞其他任务。
Imported covers may follow at most three explicit HTTPS redirects to storage hosts. Every hop rebuilds headers and strips credentials outside qq.com; downgrade, excessive loops and oversized URLs fail. PNG/JPEG/WebP are accepted; decode failures back off even after HTTP200.

上限：JSON2MiB、书架与待补详情的书号并集1024本、目录20000章、下载32MiB、单条目8MiB、展开总量64MiB、正文4MiB、CSS256KiB、位置锚点16384。书架成员和队列去重使用临时16KiB目标PSRAM索引，在详情HTTP之前释放；无成员变化时不分配。旧完整目录由清理操作回收。
Limits:2MiB JSON,1024 distinct IDs across cached books and unresolved details,20000 chapters,32MiB downloads,8MiB entries,64MiB expanded data,4MiB body,256KiB CSS and16384 anchors. Temporary shelf deduplication indexes use16KiB of target PSRAM, are released before detail HTTP and are omitted without membership changes. Explicit cleanup reclaims older committed directories.

微信EPUB的纯空正文内联背景图可提升为首屏插图；真正空章节仍保留空页语义，混合图文不强制改成整页图。仅处理有界扫描能明确证明的样式，不新增通用外部CSS背景渲染。旧已组装的背景图章节可清该书离线内容后重取，进度保留；本地TXT/EPUB解析默认行为不变。
WeRead EPUB chapters with provably empty text may promote an inline background image to a page image. True empty chapters remain empty, and mixed text/images retain their layout. Bounded conservative detection does not add general external-CSS backgrounds. Clear and redownload old assembled content to apply the fix while retaining progress; local TXT/EPUB parsing stays unchanged.

## 验证与本地交付 / Validation and local delivery

```sh
export IDF_PATH=/path/to/firmware/esp-idf
bash tools/run_weread_host_tests.sh
python3 tools/test_weread_native_e2e.py
bash tools/run_weread_e2e_tests.sh
```

总入口包含真实SDK HTTP解析器的25场景轮询测试、DNS迟回调与取消、持久待补队列及紧急持久化恢复测试。需要Python3、支持ASan/UBSan的C编译器、zlib和ESP-IDF源码；`CC`可指定编译器。解析器优先取`IDF_PATH`或`KIIKO_IDF_PICO`，未配置时检查PATH里的`idf.py`和通用`~/esp/esp-idf`；依赖缺失明确失败，不静默跳过。可单独运行`IDF_PATH=/path/to/sdk python3 tools/weread_http_poll_host_test.py`，或用`WEREAD_TEST_BUILD_DIR`指定这些新增测试的独立输出根目录。
The entry point includes25 production polling scenarios using the SDK's unchanged HTTP parser, DNS late-reply/cancellation, durable detail queues and emergency persistence recovery. Supply Python3, an ASan/UBSan-capable C compiler, zlib and ESP-IDF sources; `CC` selects the compiler. Parser discovery uses `IDF_PATH` or `KIIKO_IDF_PICO`, then PATH's `idf.py` and `~/esp/esp-idf`. Missing dependencies fail explicitly. Run the polling script separately with `IDF_PATH`; `WEREAD_TEST_BUILD_DIR` isolates the newly added tests' outputs.

C测试启用ASan/UBSan。受限内存池另覆盖800本渐进书架、3000章目录、线程分配隔离、JSON嵌套与OOM回滚/重试；可用`WEREAD_NATIVE_MEMORY=1 python3 tools/test_weread_native_e2e.py`运行受限池生产HTTP链路。原生HTTP测试运行生产网络、认证、书源、worker和进度代码，覆盖JSON/仅Cookie登录、多文件映射、冲突选择、离线落盘/补报、预读、阻塞请求取消、换卡、缓存清理与退出。Python Tier另含协议模拟，不能替代生产C或设备验收。
C tests use ASan/UBSan. Constrained-pool tests cover800 staged books,3000 chapters, task allocation isolation, JSON nesting and OOM rollback/retry. Use `WEREAD_NATIVE_MEMORY=1 python3 tools/test_weread_native_e2e.py` for constrained production HTTP E2E. Native HTTP tests exercise production networking/auth/source/worker/progress, including JSON/cookie-only login, multi-file mapping, conflict choices, offline persistence/retry, prefetch, blocked-request cancellation, card replacement, cleanup and logout. Python tiers also contain simulations and do not replace production C or device acceptance.

双板使用 `tools/build_firmware.py --board all` 独立构建与验证，产物仅放在ignored build目录。菜单调度、冷解析、首页概览、时钟控件与封面裁剪/缩放按体积编译，阅读绘制及图片热路径保持O2；没有扩大分区、删字库或改变波形/供电。保留正式版本号，不覆盖已发布下载、不自动刷机、不发布。
Build/verify both boards independently with `tools/build_firmware.py --board all`; outputs stay under ignored build directories. Size-optimize menu dispatch/cold parsing while keeping reader/image paint hot paths atO2. Partitions, fonts, waveforms and power remain intact. Keep published version numbers without replacing downloads, flashing or publishing.

宿主与SDK验证不能代替实际刷新、残影、真实网络耗时、耗电、栈水位及手机/设备交替阅读验收。普通在线开书仍须取得新鲜单书云进度；设备离线、休眠或后台尚未确认时不承诺云端即时保存。补报先核对新鲜云端基线，但服务端接口不提供跨设备事务锁，同时在手机和设备导航仍需核对冲突。历史百分比不能用于恢复或上报猜测位置。
Host/SDK checks do not replace device refresh, ghosting, network, power, stack-watermark or alternating phone/device acceptance. Normal online opening still fetches a fresh cloud position. Offline, sleeping or unacknowledged work cannot promise immediate cloud persistence. Fresh-baseline checks protect retries, but the service offers no cross-device transaction lock; simultaneous navigation still needs conflict checks. Never recover or report guessed coordinates from a historical percentage.
