/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信章节异步准备，局部接入双板阅读器；本地TXT/EPUB不经过此路径。
 * English: Asynchronous WeRead chapter preparation; local TXT/EPUB retain their existing path.
 */
static esp_err_t wr_chapter_start(app_ctx_t *ctx){
    wr_reader_sample(ctx->now_ms,false);
    esp_err_t err=weread_reader_save(ctx->now_ms);
    if(err!=ESP_OK)return err;
    char uid[16];snprintf(uid,sizeof(uid),"%u",(unsigned)book_weread_uid(s_wr_jump.chapter));
    s_wr_jump.started=weread_runtime_start(WR_JOB_CHAPTER,s_path+9,uid);
    return s_wr_jump.started?ESP_OK:ESP_ERR_NO_MEM;
}
static bool wr_chapter_queue(app_ctx_t *ctx,size_t chapter,size_t offset,bool last_page,const char *anchor,size_t source_offset){
    char *owned=NULL;
    if(anchor){size_t bytes=strlen(anchor)+1;owned=wr_alloc(bytes);if(!owned)return false;memcpy(owned,anchor,bytes);}
    wr_chapter_cancel();
    s_wr_jump.pending=true;s_wr_jump.chapter=chapter;s_wr_jump.offset=offset;
    s_wr_jump.last_page=last_page;s_wr_jump.source_offset=source_offset;s_wr_jump.anchor=owned;s_wr_jump.toc=SIZE_MAX;
    weread_runtime_cancel();
    s_view=READING;s_reader_panel=READER_PANEL_NONE;
    copy_text(s_reader_notice,sizeof(s_reader_notice),"正在准备章节…");s_reader_notice_until=INT64_MAX;
    s_reader_text_frame=false;
    // 先启动worker再由主循环推反馈屏；不把物理刷新时间串到下载之前。
    // Start the worker before feedback presentation rather than delaying downloads behind a physical refresh.
    if(!weread_runtime_busy()&&!ctx->request_app)s_wr_jump.error=wr_chapter_start(ctx);
    return true;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void wr_chapter_poll(uint32_t expected_uid, bool *matching, esp_err_t *error) {
    weread_result_t result = {0};
    bool completed = weread_runtime_poll(&result);
    *matching = completed && result.job == WR_JOB_CHAPTER && !strcmp(result.id, s_path + 9) &&
        result.chapter_uid == expected_uid;
    if (*matching) *error = result.error;
    weread_runtime_result_free(&result);
}
static app_redraw_t wr_chapter_tick(app_ctx_t *ctx){
    if(!s_wr_jump.pending)return APP_REDRAW_NONE;
    if(ctx->request_app){wr_chapter_cancel();return APP_REDRAW_NONE;}
    if(weread_runtime_busy()||ctx->consumed||(ctx->touch&&ctx->touch->touched))return APP_REDRAW_NONE;
    bool matching=false;esp_err_t err=s_wr_jump.error;
    wr_chapter_poll(book_weread_uid(s_wr_jump.chapter),&matching,&err);
    if(s_wr_jump.started&&!matching)return APP_REDRAW_NONE;
    if(!s_wr_jump.started){
        if(err==ESP_OK)err=wr_chapter_start(ctx);
        if(s_wr_jump.started)return APP_REDRAW_NONE;
    }else if(err==ESP_OK){
        s_wr_jump.committing=true;
        bool ok=load_chapter_at(ctx,s_wr_jump.chapter,s_wr_jump.offset,s_wr_jump.last_page,s_wr_jump.anchor,s_wr_jump.source_offset);
        if(ok){
            s_selected_toc=s_wr_jump.toc==SIZE_MAX?SIZE_MAX:s_chapter;
            if(s_selected_toc!=SIZE_MAX){
                s_selected_toc_offset=reader_page_offset(s_page);
                (void)book_navigation_title(s_selected_toc,s_title,sizeof(s_title));
            }
            free(s_wr_jump.anchor);memset(&s_wr_jump,0,sizeof(s_wr_jump));
            s_view=READING;s_reader_panel=READER_PANEL_NONE;s_reader_notice[0]=0;
            wr_reader_move();save_progress();return APP_REDRAW_PAGE;
        }
        err=ESP_FAIL;
    }
    free(s_wr_jump.anchor);memset(&s_wr_jump,0,sizeof(s_wr_jump));
    if(err!=ESP_OK){
        copy_text(s_reader_notice,sizeof(s_reader_notice),s_message[0]?s_message:
            err==WEREAD_ERR_PURCHASE?"请在手机端购买或领取此章节":
            err==WEREAD_ERR_SOLD_OUT?"章节已下架":
            err==WEREAD_ERR_VERSION?"书籍已更新，请返回书架重新打开":
            err==ESP_ERR_INVALID_STATE?"章节准备已取消":"加载失败，可返回目录重试");
        s_reader_notice_until=ctx->now_ms+3000;s_message[0]=0;
    }
    return APP_REDRAW_PAGE;
}
