/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：双板共用阅读位置采样；绘制路径不执行网络请求。
 * English: Shared reader sampling; rendering never performs network requests.
 */
#include "weread/weread_store.h"
static uint32_t s_wr_content_version;
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
    size_t offset=reader_page_offset(s_page);if(offset>s_text_len)offset=s_text_len;
    weread_reader_navigated_version(s_path,s_chapter,(uint32_t)offset,percent(s_page),s_text+offset,s_wr_content_version);
}
static void wr_reader_sample(uint64_t now, bool active) {
    static uint64_t sampled;
    static char footer_id[64];
    static unsigned footer_cached, footer_total;
    static bool footer_space;
    if (!s_text || !book_path_is_weread(s_path) || !book_layout_page_count()) return;
    if (active && now >= sampled && now - sampled < 200) return;
    sampled = now;
    if (strcmp(footer_id, s_path + 9)) {
        wr_copy(footer_id, sizeof(footer_id), s_path + 9);
        weread_reader_cached_progress(&footer_cached, &footer_total);
        footer_space = weread_reader_storage_blocked();
    }
    size_t offset = reader_page_offset(s_page);
    if (offset > s_text_len) offset = s_text_len;
    weread_progress_state_t before=weread_reader_state();
    bool failed=weread_reader_save_failed(),pending=weread_reader_pending_save(),rescued=weread_reader_rescued();
    unsigned offline=weread_reader_offline();
    bool caching=weread_reader_caching();
    weread_reader_tick(s_path,s_chapter,(uint32_t)offset,percent(s_page),s_text+offset,now,active);
    unsigned next_cached=0,next_total=0;
    weread_reader_cached_progress(&next_cached,&next_total);
    bool next_space=weread_reader_storage_blocked();
    bool cache_delta=next_cached!=footer_cached||next_total!=footer_total||next_space!=footer_space;
    footer_cached=next_cached;footer_total=next_total;footer_space=next_space;
    if(s_view==READING&&!s_toolbar&&
        (before!=weread_reader_state()||failed!=weread_reader_save_failed()||pending!=weread_reader_pending_save()||rescued!=weread_reader_rescued()||
        offline!=weread_reader_offline()||cache_delta||caching!=weread_reader_caching())){s_reader_footer_pending=true;s_reader_footer_due_ms=now+800;}
}
static const char *wr_reader_status(void){
    if(!book_path_is_weread(s_path))return "";
    if(weread_reader_save_failed())return "保存失败 · ";
    if(weread_reader_pending_save())return "待保存 · ";
    if(weread_reader_state()==WR_PROGRESS_CONFLICT)return "进度冲突，请重开 · ";
    if(weread_reader_rescued())return "已暂存设备 · ";
    switch(weread_reader_state()){
        case WR_PROGRESS_PENDING:return "待同步 · ";
        case WR_PROGRESS_SYNCED:return "已同步 · ";
        case WR_PROGRESS_CONFLICT:return "进度冲突 · ";
        default:return "本地保存 · ";
    }
}

static void wr_reader_offline_caption(char *out,size_t cap){
    out[0]=0;if(!book_path_is_weread(s_path))return;
    unsigned cached=0,total=0;
    weread_reader_cached_progress(&cached,&total);
    if(weread_reader_storage_blocked()){
        if(total)snprintf(out,cap,"缓存空间不足(%u/%u) · ",cached,total);
        else snprintf(out,cap,"缓存空间不足 · ");
    }else if(total){
        snprintf(out,cap,"已缓存 %u/%u 章 · ",cached,total);
    }else{
        unsigned ready=weread_reader_offline();
        if(ready)snprintf(out,cap,"后续%u章已缓存 · ",ready);
        else if(weread_reader_caching())snprintf(out,cap,"缓存后续章节 · ");
        else if(weread_reader_cache_forward())snprintf(out,cap,"缓存后续章节已开启 · ");
        else snprintf(out,cap,"本章已缓存 · ");
    }
}

// 手动缓存开关：只翻转后台补齐开关并给出短提示，不等待下载、不改动当前正文。
// Manual cache toggle: flip the background fill switch and toast briefly; never wait for a download or touch the body.
static void wr_cache_forward_toggle(app_ctx_t *ctx){
    if(!book_path_is_weread(s_path))return;
    bool on=!weread_reader_cache_forward();
    weread_reader_cache_forward_set(on);
    unsigned cached=0,total=0;
    weread_reader_cached_progress(&cached,&total);
    const char *text=on?"开始缓存后续章节":"已停止后台缓存";
    if(on&&weread_reader_storage_blocked())text="缓存空间不足";
    else if(on&&total&&cached>=total)text="本书已缓存完";
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
    bool pending, started, committing, last_page;
    esp_err_t error;
    size_t chapter, offset, source_offset, toc;
    char *anchor;
} s_wr_jump;
static bool wr_chapter_queue(app_ctx_t *ctx,size_t chapter,size_t offset,bool last_page,const char *anchor,size_t source_offset);
static app_redraw_t wr_chapter_tick(app_ctx_t *ctx);
static void wr_chapter_cancel(void){
    weread_runtime_prefetch_adopt(0);
    if(s_wr_jump.pending){weread_runtime_cancel();s_reader_notice[0]=0;s_reader_notice_until=0;}
    free(s_wr_jump.anchor);memset(&s_wr_jump,0,sizeof(s_wr_jump));
}
