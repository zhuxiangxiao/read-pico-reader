/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信章节异步准备，局部接入双板阅读器；本地TXT/EPUB不经过此路径。
 * English: Asynchronous WeRead chapter preparation; local TXT/EPUB retain their existing path.
 */
// 冷章提示文案的限速状态：下载阶段最多每 2 秒重绘一次，阶段切换立即刷新。
// Toast rate limiter: a download stage repaints at most every 2 s while a stage change shows immediately.
static uint64_t s_stage_notice_at;
static unsigned s_stage_notice_kind;
static esp_err_t wr_chapter_start(app_ctx_t *ctx){
    weread_runtime_prefetch_adopt(0);
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
    uint32_t target_uid=book_weread_uid(chapter);
    bool adopting=weread_runtime_prefetch_active(s_path+9,target_uid);
    s_wr_jump.pending=true;s_wr_jump.chapter=chapter;s_wr_jump.offset=offset;
    s_wr_jump.last_page=last_page;s_wr_jump.source_offset=source_offset;s_wr_jump.anchor=owned;s_wr_jump.toc=SIZE_MAX;
    if(adopting){weread_runtime_prefetch_adopt(target_uid);s_wr_jump.started=true;}
    else{weread_runtime_prefetch_adopt(0);weread_runtime_cancel();}
    s_view=READING;s_reader_panel=READER_PANEL_NONE;
    s_stage_notice_at=0;s_stage_notice_kind=0;
    copy_text(s_reader_notice,sizeof(s_reader_notice),"正在准备章节…");s_reader_notice_until=INT64_MAX;
    s_reader_text_frame=false;
    // 先启动worker再由主循环推反馈屏；若同章预取已在进行则直接接管。
    // Start the worker before feedback presentation, or adopt an already-running same-chapter prefetch.
    if(!adopting&&!weread_runtime_busy()&&!ctx->request_app)s_wr_jump.error=wr_chapter_start(ctx);
    return true;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void wr_chapter_poll(uint32_t expected_uid, bool *matching, esp_err_t *error, bool *prefetch_retry) {
    weread_result_t result = {0};
    bool completed = weread_runtime_poll(&result);
    *matching = completed && (result.job == WR_JOB_CHAPTER || result.job == WR_JOB_PREFETCH) &&
        !strcmp(result.id, s_path + 9) && result.chapter_uid == expected_uid;
    if (*matching) {
        *error = result.error;
        if (prefetch_retry && result.job == WR_JOB_PREFETCH && result.error != ESP_OK &&
            result.error != ESP_ERR_INVALID_STATE && result.error != WEREAD_ERR_PURCHASE &&
            result.error != WEREAD_ERR_SOLD_OUT && result.error != WEREAD_ERR_VERSION) {
            *prefetch_retry = true;
        }
    }
    weread_runtime_result_free(&result);
}
// 冷章等待期间用后台阶段文案替换同一条提示；文案变化且通过限速时才重绘提示区域。
// Replace the waiting toast with the worker stage text; repaint only when the text changes and the limiter allows.
static app_redraw_t wr_chapter_stage_notice(app_ctx_t *ctx){
    // 只有本页起的异步章节任务才有自己的阶段；子页面、触摸、消费态或无帧缓冲时不启动新的限速窗口。
    // Only this page's asynchronous chapter job owns a stage; subpages, touch, consumed ticks or a missing
    // framebuffer return before the rate limiter is armed, so a skipped update cannot swallow the next one.
    if(!s_wr_jump.started||s_view!=READING||!ctx->fb)return APP_REDRAW_NONE;
    if(ctx->consumed||(ctx->touch&&ctx->touch->touched))return APP_REDRAW_NONE;
    char stage[sizeof(s_reader_notice)];
    weread_stage_t kind=WEREAD_STAGE_NONE;
    if(!weread_reader_stage_text(stage,sizeof(stage)))return APP_REDRAW_NONE;
    if(!strcmp(stage,s_reader_notice))return APP_REDRAW_NONE;
    if(!weread_reader_stage(&kind,NULL))kind=WEREAD_STAGE_NONE;
    // 下载字节每 32 KiB 推进一次；同一阶段内按 3 秒合并，阶段切换与重试提示立即显示。
    // Download bytes advance every 32 KiB; coalesce within one stage to 3 s while stage changes and the retry label show at once.
    if((unsigned)kind==s_stage_notice_kind&&kind==WEREAD_STAGE_CHAPTER&&(uint64_t)ctx->now_ms<s_stage_notice_at)return APP_REDRAW_NONE;
    s_stage_notice_kind=(unsigned)kind;s_stage_notice_at=ctx->now_ms+3000;
    copy_text(s_reader_notice,sizeof(s_reader_notice),stage);
    s_reader_notice_until=INT64_MAX;
    // 区域刷新不会自动重绘帧缓冲；先重绘提示，再只更新提示所在区域。
    // Area refreshes do not rerender the framebuffer; repaint the toast before updating its region.
    render(ctx,ctx->fb);
    s_area=(EpdRect){208,150,268,68};s_mode=MODE_GL16;
    return APP_REDRAW_AREA;
}
static app_redraw_t wr_chapter_tick(app_ctx_t *ctx){
    if(!s_wr_jump.pending){
        if(ctx->request_app||ctx->consumed||(ctx->touch&&ctx->touch->touched)||s_view!=READING||!s_text)return APP_REDRAW_NONE;
        size_t chapter=0;uint32_t offset=0;
        if(!weread_reader_poll_cloud_jump(s_path,&chapter,&offset))return APP_REDRAW_NONE;
        s_wr_jump.committing=true;
        bool ok=load_chapter_at(ctx,chapter,offset,false,NULL,SIZE_MAX);
        s_wr_jump.committing=false;
        weread_reader_commit_cloud_jump(s_path,ok);
        if(!ok)return APP_REDRAW_NONE;
        s_selected_toc=SIZE_MAX;s_view=READING;s_reader_panel=READER_PANEL_NONE;
        copy_text(s_reader_notice,sizeof(s_reader_notice),"已同步云端进度");
        s_reader_notice_until=ctx->now_ms+2000;
        save_progress();return APP_REDRAW_PAGE;
    }
    if(ctx->request_app){wr_chapter_cancel();return APP_REDRAW_NONE;}
    app_redraw_t stage=wr_chapter_stage_notice(ctx);if(stage!=APP_REDRAW_NONE)return stage;
    if(weread_runtime_busy()||ctx->consumed||(ctx->touch&&ctx->touch->touched))return APP_REDRAW_NONE;
    bool matching=false,prefetch_retry=false;esp_err_t err=s_wr_jump.error;
    wr_chapter_poll(book_weread_uid(s_wr_jump.chapter),&matching,&err,&prefetch_retry);
    if(matching)weread_runtime_prefetch_adopt(0);
    if(prefetch_retry){
        s_wr_jump.started=false;err=wr_chapter_start(ctx);
        if(s_wr_jump.started)return APP_REDRAW_NONE;
    }else if(s_wr_jump.started&&!matching){
        if(!weread_reader_cached(s_path,s_wr_jump.chapter))return APP_REDRAW_NONE;
        err=ESP_OK;
    }
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
#if defined(ESP_PLATFORM)
        ESP_LOGW(TAG,"weread chapter failed err=%d msg=%s",(int)err,s_message);
#endif
        copy_text(s_reader_notice,sizeof(s_reader_notice),s_message[0]?s_message:
            err==WEREAD_ERR_PURCHASE?"请在手机端购买或领取此章节":
            err==WEREAD_ERR_SOLD_OUT?"章节已下架":
            err==WEREAD_ERR_VERSION?"书籍已更新，请返回书架重新打开":
            err==ESP_ERR_NO_MEM?"内存不足，请稍后重试":
            err==ESP_ERR_INVALID_STATE?"章节准备已取消":"加载失败，可返回目录重试");
        s_reader_notice_until=ctx->now_ms+3000;s_message[0]=0;
    }
    return APP_REDRAW_PAGE;
}
