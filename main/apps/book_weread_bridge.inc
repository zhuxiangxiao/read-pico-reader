/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：双板共用阅读位置采样；绘制路径不执行网络请求。
 * English: Shared reader sampling; rendering never performs network requests.
 */
#include "weread/weread_store.h"
#ifndef WR_UNDO_LOCAL
#define WR_UNDO_LOCAL 0
#define WR_UNDO_CLOUD_BACK 1
#define WR_UNDO_CLOUD_JUMP 2
#endif
static bool s_wr_undo_top_erase __attribute__((unused));
static uint32_t s_wr_content_version;
static weread_cloud_sync_ui_state_t s_wr_sync_ui_prev, s_wr_sync_pill_last;
static bool s_wr_sync_pill_shown;
static void wr_sync_ui_reset(void) {
    s_wr_sync_ui_prev = WEREAD_CLOUD_SYNC_IDLE;
    s_wr_sync_pill_last = WEREAD_CLOUD_SYNC_IDLE;
    s_wr_sync_pill_shown = false;
}
static bool wr_reader_text_page(void){
    return s_view==READING&&s_text&&!s_clear_confirm&&s_reader_panel==READER_PANEL_NONE&&
        s_page<book_layout_page_count()&&!book_layout_page_image_count(s_page);
}
static bool wr_reader_text_turn(int dir){
    if(!wr_reader_text_page())return false;
    if(dir>0)return s_page+1<book_layout_page_count()&&!book_layout_page_image_count(s_page+1);
    return dir<0&&s_page&&!book_layout_page_image_count(s_page-1);
}
static void wr_reader_move(void){
    if(!s_text||!book_path_is_weread(s_path)||!book_layout_page_count())return;
    if(s_chapter_jump_undo.valid&&s_chapter_jump_undo.mode!=WR_UNDO_LOCAL){
        uint8_t mode=s_chapter_jump_undo.mode;
        s_chapter_jump_undo.valid=false;
        s_wr_undo_top_erase=true;
        if(mode==WR_UNDO_CLOUD_JUMP)weread_reader_commit_cloud_jump(s_path,false);
    }
    if(s_wr_sync_pill_shown&&weread_reader_cloud_sync_state()==WEREAD_CLOUD_SYNC_IDLE&&
        !strcmp(s_reader_notice,"正在同步云端进度…")){
        s_reader_notice[0]=0;s_reader_notice_until=0;wr_sync_ui_reset();
    }
    size_t offset=reader_page_offset(s_page);if(offset>s_text_len)offset=s_text_len;
    weread_reader_navigated_version(s_path,s_chapter,(uint32_t)offset,percent(s_page),s_text+offset,s_wr_content_version);
}
static void wr_reader_sample(uint64_t now, bool active) {
    static uint64_t sampled;
    if (!s_text || !book_path_is_weread(s_path) || !book_layout_page_count()) return;
    if (active && now >= sampled && now - sampled < 200) return;
    sampled = now;
    size_t offset = reader_page_offset(s_page);
    if (offset > s_text_len) offset = s_text_len;
    weread_progress_state_t before=weread_reader_state();
    bool failed=weread_reader_save_failed(),pending=weread_reader_pending_save(),rescued=weread_reader_rescued();
    bool space=weread_reader_storage_blocked();
    weread_cloud_sync_ui_state_t sync_before=s_wr_sync_ui_prev;
    weread_reader_tick(s_path,s_chapter,(uint32_t)offset,percent(s_page),s_text+offset,now,active);
    weread_cloud_sync_ui_state_t sync_now=weread_reader_cloud_sync_state();
    if(s_view==READING&&!s_toolbar&&
        (before!=weread_reader_state()||failed!=weread_reader_save_failed()||pending!=weread_reader_pending_save()||
        rescued!=weread_reader_rescued()||space!=weread_reader_storage_blocked()||sync_now!=sync_before)){
        s_wr_sync_ui_prev=sync_now;
        if(!s_reader_fullscreen||!app_settings_reader_immersive()){
            s_reader_footer_pending=true;s_reader_footer_due_ms=now+800;
        }
    }
}
static const char *wr_reader_status(void){
    if(!book_path_is_weread(s_path))return "";
    if(weread_reader_save_failed())return "未保存 · ";
    if(weread_reader_pending_save())return "待保存 · ";
    weread_cloud_sync_ui_state_t sync=weread_reader_cloud_sync_state();
    s_wr_sync_ui_prev=sync;
    if(sync==WEREAD_CLOUD_SYNC_EXPIRED)return "登录过期 · ";
    if(sync==WEREAD_CLOUD_SYNC_ACTIVE)return "同步进度中 · ";
    if(sync==WEREAD_CLOUD_SYNC_TIMEOUT)return "同步超时 · ";
    if(sync==WEREAD_CLOUD_SYNC_OFFLINE)return "网络不可用 · ";
    if(sync==WEREAD_CLOUD_SYNC_VERSION)return "书目已更新 · ";
    if(sync==WEREAD_CLOUD_SYNC_FAILED)return "同步失败 · ";
    if(weread_reader_state()==WR_PROGRESS_CONFLICT)return "进度冲突 · ";
    if(weread_reader_rescued())return "暂存机内 · ";
    switch(weread_reader_state()){
        case WR_PROGRESS_PENDING:return "待同步 · ";
        case WR_PROGRESS_SYNCED:return "";
        case WR_PROGRESS_CONFLICT:return "进度冲突 · ";
        default:return weread_runtime_reader_has_turned(s_path)?"本地保存 · ":"";
    }
}

static void wr_reader_offline_caption(char *out,size_t cap){
    if(!out||!cap)return;
    out[0]=0;if(!book_path_is_weread(s_path))return;
    if(weread_reader_storage_blocked())snprintf(out,cap,"空间不足 · ");
}

// 工具栏第 6 按钮动态显示当前整本缓存进度或状态；静默后台下载不唤醒屏幕。
// Toolbar button 6 dynamically reflects whole-book cache progress or state without waking the screen on silent prefetch.
static void __attribute__((unused)) wr_reader_tool6_label(char *out,size_t cap){
    if(!out||!cap)return;
    unsigned cached=0,total=0;
    weread_reader_cached_progress(&cached,&total);
    bool enabled=weread_reader_cache_fill_enabled();
    bool stalled=enabled&&(weread_reader_cache_forward_stalled()||weread_reader_storage_blocked());
    if(total>0&&cached>=total)snprintf(out,cap,"已全存");
    else if(!enabled)snprintf(out,cap,"已暂停");
    else if(stalled)snprintf(out,cap,"重试缓存");
    else if(total>0)snprintf(out,cap,"缓存 %u/%u",cached,total);
    else snprintf(out,cap,"缓存中");
}

// 手动缓存开关：只翻转后台补齐开关并给出短提示，不等待下载、不改动当前正文。
// Manual cache toggle: flip the background fill switch and toast briefly; never wait for a download or touch the body.
static void wr_cache_forward_toggle(app_ctx_t *ctx){
    if(!book_path_is_weread(s_path))return;
    // 开关反映“整本向后补齐是否启用”：默认启用；点一次暂停，再点一次立即恢复并清掉失败/退避/空间暂停。
    // The toggle mirrors whether whole-book fill is enabled: on by default, one tap pauses and the next resumes
    // eagerly after clearing automatic-prefetch failures/backoff/space pauses.
    bool enabled=weread_reader_cache_fill_enabled();
    bool stalled=enabled&&weread_reader_cache_forward_stalled();
    // 启用但已因连续失败/空间暂停时，一次点击直接重新触发，不必先暂停再恢复。
    // When enabled but stalled by failures/low space, one tap retriggers instead of requiring pause then resume.
    bool on=!enabled||stalled;
    weread_reader_cache_forward_set(on);
    unsigned cached=0,total=0;
    weread_reader_cached_progress(&cached,&total);
    const char *text=on?(total&&cached>=total?"本书已缓存完":
        stalled?"重新开始缓存后续章节":"开始缓存后续章节"):"已暂停后台缓存";
    copy_text(s_reader_notice,sizeof(s_reader_notice),text);
    s_reader_notice_until=ctx->now_ms+2000;
    // 页脚下一次采样会看到缓存状态变化；这里只标记一次待刷新，避免依赖恰好快照。
    // The next footer sample observes the cache state change; flag one deferred refresh rather than rely on timing.
    s_reader_footer_pending=true;s_reader_footer_due_ms=ctx->now_ms+800;
}

// 目录缓存标记在进入/翻页时一次性准备，绘制阶段只读位图。/ TOC cache markers are prepared once on entry/paging; painting only reads the flags.
static size_t current_toc_position(void);
static bool toc_chapter_cached(size_t chapter,void *user){
    const char *path=(const char *)user;
    return book_path_is_weread(path)&&weread_reader_chapter_cached(path,chapter);
}
static void toc_cached_markers_prepare(int page){
    book_toc_set_cached_probe(book_path_is_weread(s_path)?toc_chapter_cached:NULL,s_path);
    book_toc_prepare_page(book_navigation_count(),current_toc_position(),page);
}

// 冷章只保存最新目标，旧正文保留到目标成功排版。/ Keep the old body until the latest cold target typesets successfully.
static struct {
    bool pending, started, committing, last_page, cloud_commit;
    esp_err_t error;
    size_t chapter, offset, source_offset, toc;
    int toc_page;
    char *anchor;
} s_wr_jump;
static bool wr_chapter_queue(app_ctx_t *ctx,size_t chapter,size_t offset,bool last_page,const char *anchor,size_t source_offset);
static app_redraw_t wr_chapter_tick(app_ctx_t *ctx);
static void wr_chapter_cancel(void){
    weread_runtime_prefetch_adopt(0);
    if(s_wr_jump.pending){
        if(s_wr_jump.cloud_commit)weread_reader_commit_cloud_jump(s_path,false);
        weread_runtime_cancel();s_reader_notice[0]=0;s_reader_notice_until=0;
    }
    free(s_wr_jump.anchor);memset(&s_wr_jump,0,sizeof(s_wr_jump));
}
