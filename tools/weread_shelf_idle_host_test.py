#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Run the real WeRead page scheduler through invisible warming and repaint coalescing."""
from pathlib import Path
import runpy
import subprocess

root = Path(__file__).resolve().parents[1]
baseline = runpy.run_path(str(root / "tools/weread_cache_ui_host_test.py"))
out = root / "build/night-optimization/shelf-idle"
out.mkdir(parents=True, exist_ok=True)
unit = (root / "tools/test_weread_cache_ui.c").read_text().split("int main(void){", 1)[0]
unit += r'''
static void ready_page(void){
    for(int i=0;i<page_rows();++i){
        weread_shelf_item_t*b=row_book(i);if(!b)break;
        s_covers[i].pixels=malloc(1);assert(s_covers[i].pixels);
        s_covers[i].checked=true;s_covers[i].key=b->cover_key;
    }
}
int main(void){
    reset();enter(&ctx);
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(30,false),.count=30});
    assert(tick(&ctx)==APP_REDRAW_PAGE);ready_page();ctx.now_ms+=1600;
    char previous_message[sizeof(s_message)];strcpy(previous_message,s_message);
    assert(tick(&ctx)==APP_REDRAW_NONE&&starts[nstarts-1].job==WR_JOB_CONNECT);
    complete((weread_result_t){.job=WR_JOB_CONNECT,.authenticated=true,.error=ESP_ERR_TIMEOUT});
    assert(tick(&ctx)==APP_REDRAW_NONE&&!strcmp(previous_message,s_message));
    assert(weread_runtime_start(WR_JOB_DISCONNECT,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_DISCONNECT,.authenticated=true});
    assert(tick(&ctx)==APP_REDRAW_NONE&&!strcmp(previous_message,s_message)&&s_preconnected);
    int before=nstarts;
    for(int i=0;i<2;++i){
        assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==before+i+1);
        assert(starts[nstarts-1].job==WR_JOB_COVER&&!strcmp(starts[nstarts-1].value,"warm-disk"));
        char expected[64];snprintf(expected,sizeof(expected),"id_%d",9+i);
        assert(!strcmp(starts[nstarts-1].id,expected));
        weread_result_t r={.job=WR_JOB_COVER,.authenticated=true,.cover=malloc(1)};assert(r.cover);strcpy(r.id,expected);
        complete(r);assert(tick(&ctx)==APP_REDRAW_NONE&&!strcmp(previous_message,s_message));
    }
    for(int i=0;i<5;++i){ctx.now_ms+=500;assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==before+2);}
    for(int i=0;i<9;++i)assert(s_covers[i].pixels&&s_covers[i].key==row_book(i)->cover_key);
    puts("PASS: one silent preconnect; two invisible neighbour covers; no retained extra pixels or repeated requests");

    release_covers();s_page=2;ready_page();s_key_mode=true;
    app_redraw_t redraw;assert(main_key(&ctx,UI_KEY_1,&redraw)&&redraw==APP_REDRAW_PAGE&&s_page==1&&s_warm_direction==-1);
    ready_page();ctx.now_ms+=1100;assert(tick(&ctx)==APP_REDRAW_NONE);
    assert(s_cover_warm&&!strcmp(starts[nstarts-1].id,"id_0"));
    assert(main_key(&ctx,UI_KEY_3,&redraw)&&s_page==2&&!s_cover_warm);
    // A cancelled invisible job cannot consume the new page's visible local cover result.
    assert(tick(&ctx)==APP_REDRAW_NONE&&starts[nstarts-1].job==WR_JOB_COVER&&!starts[nstarts-1].value[0]);
    complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.cover=malloc(1)});
    strcpy(mock_result.id,"id_18");assert(tick(&ctx)==APP_REDRAW_NONE&&s_covers[0].pixels);
    puts("PASS: latest direction wins; page input cancels warm and visible covers retain priority");

    reset();initialized(9);ready_page();
    for(int i=0;i<2;++i){
        assert(weread_runtime_start(WR_JOB_COVER,"id_0","remote"));s_cover_remote=true;
        complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.cover=malloc(1)});
        snprintf(mock_result.id,sizeof(mock_result.id),"id_%d",i);
        assert(tick(&ctx)==APP_REDRAW_NONE&&s_cover_dirty);
        ctx.now_ms+=50;
    }
    assert(tick(&ctx)==APP_REDRAW_NONE);ctx.now_ms+=50;
    assert(tick(&ctx)==APP_REDRAW_PAGE&&!s_cover_dirty);
    puts("PASS: two remote cover completions within150ms commit one page refresh");

    assert(weread_runtime_start(WR_JOB_REPORT,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_REPORT,.local={.chapter_uid=1,.dirty=true,.position_pending=true},.error=ESP_ERR_TIMEOUT});
    strcpy(mock_result.id,"id_0");assert(tick(&ctx)==APP_REDRAW_AREA&&s_area.height==24&&strstr(s_message,"待同步")&&s_auth);
    assert(weread_runtime_start(WR_JOB_REPORT,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_REPORT,.local={.chapter_uid=1,.dirty=true,.position_pending=true},.error=ESP_ERR_TIMEOUT});
    strcpy(mock_result.id,"id_0");assert(tick(&ctx)==APP_REDRAW_NONE&&s_auth);
    assert(weread_runtime_start(WR_JOB_REPORT,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_REPORT,.local={.chapter_uid=1,.dirty=true,.position_pending=true,.sync_conflict=true}});
    strcpy(mock_result.id,"id_0");assert(tick(&ctx)==APP_REDRAW_AREA&&strstr(s_message,"存储卡")&&s_auth);
    puts("PASS: report status changes use footer area only; retries neither repaint nor clear login");
    reset();initialized(3);assert(weread_runtime_start(WR_JOB_COVER,"id_0","remote"));
    before=nstarts;open_id("id_1");assert(s_open_queued&&mock_busy&&nstarts==before&&strstr(s_message,"正在准备阅读"));
    assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==before);
    open_id("id_2");complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.cover=malloc(1)});
    strcpy(mock_result.id,"id_0");assert(tick(&ctx)==APP_REDRAW_NONE&&!s_open_queued&&nstarts==before+1);
    assert(starts[nstarts-1].job==WR_JOB_OPEN&&!strcmp(starts[nstarts-1].id,"id_2")&&!s_covers[0].pixels);
    complete((weread_result_t){.job=WR_JOB_OPEN,.authenticated=true,.open_ready=true});strcpy(mock_result.id,"id_2");
    ctx.request_app=(void*)2;assert(tick(&ctx)==APP_REDRAW_NONE&&ctx.request_app==(void*)2);leave(&ctx);assert(!s_open_queued&&!mock_ready);
    puts("PASS: open feedback precedes background completion; latest book wins and accepted navigation drops late opens");
    reset();initialized(3);assert(weread_runtime_start(WR_JOB_COVER,"id_0","remote"));open_id("id_1");
    mock_epoch++;complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true});
    assert(tick(&ctx)==APP_REDRAW_PAGE&&!s_open_queued&&starts[nstarts-1].job==WR_JOB_INIT&&s_sync_after_init);
    puts("PASS: queued open rejects a changed media epoch before starting an old book");

    reset();initialized(1);assert(weread_runtime_start(WR_JOB_COVER,"id_0","remote"));s_cover_remote=true;
    complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_ERR_NO_MEM});strcpy(mock_result.id,"id_0");
    before=nstarts;assert(tick(&ctx)==APP_REDRAW_NONE&&!s_covers[0].retry.failures&&s_covers[0].retry.not_before_ms==ctx.now_ms+15000);
    ctx.now_ms+=1000;assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==before);
    puts("PASS: decoder OOM backs off without consuming network failures or immediate remote retry");
    reset();initialized(1);assert(tick(&ctx)==APP_REDRAW_NONE&&starts[nstarts-1].job==WR_JOB_COVER);
    complete((weread_result_t){.job=WR_JOB_COVER,.error=ESP_ERR_INVALID_STATE,.authenticated=false});
    strcpy(mock_result.id,"id_0");assert(tick(&ctx)==APP_REDRAW_PAGE&&!s_auth&&starts[nstarts-1].job==WR_JOB_QR);
    assert(strstr(s_message,"登录已失效"));
    puts("PASS: visible-cover session expiry repaints login and starts a fresh QR rather than leaving stale shelf pixels");
    weread_job_t expiry_jobs[]={WR_JOB_OPEN,WR_JOB_SYNC,WR_JOB_LOCAL,WR_JOB_CLOUD};
    for(unsigned stale=0;stale<2;++stale)for(unsigned i=0;i<sizeof(expiry_jobs)/sizeof(*expiry_jobs);++i){
        reset();initialized(1);s_order=2;
        assert(weread_runtime_start(expiry_jobs[i],"id_0",NULL));
        complete((weread_result_t){.job=expiry_jobs[i],.error=ESP_FAIL,.authenticated=false});
        if(stale){mock_authenticated=true;strcpy(mock_session.access_token,"fresh-replacement-token");}
        assert(tick(&ctx)==APP_REDRAW_PAGE);
        if(stale){
            assert(s_auth&&s_sync_after_init&&starts[nstarts-1].job==WR_JOB_INIT);render(&ctx,NULL);
            assert(!drawn_qr&&!strstr(drawn_text,"扫码"));
            complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(1,false),.count=1});
            tick(&ctx);assert(s_auth&&!s_sync_after_init&&s_order==2);
        }else assert(!s_auth&&starts[nstarts-1].job==WR_JOB_QR&&strstr(s_message,"登录已失效"));
    }
    puts("PASS: foreground OPEN/SYNC/LOCAL/CLOUD confirmed revocation starts QR; replacement tokens reject stale expiry without logout");
    reset();initialized(1);assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_SYNC,.error=ESP_ERR_TIMEOUT,.authenticated=true,.books=books(1,false),.count=1});
    tick(&ctx);assert(s_auth&&starts[nstarts-1].job==WR_JOB_SYNC);
    puts("PASS: ordinary network failure with retained credentials does not enter QR");
    reset();enter(&ctx);complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});tick(&ctx);
    complete((weread_result_t){.job=WR_JOB_QR,.uid="still-valid",.qr_url="https://weread.qq.com/test"});tick(&ctx);
    before_lock(&ctx);tick(&ctx);complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});
    before=nstarts;tick(&ctx);assert(nstarts==before&&s_uid[0]&&!s_auth);
    assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});
    before=nstarts;tick(&ctx);assert(nstarts==before&&!strcmp(s_uid,"still-valid"));
    s_uid[0]=s_qr[0]=0;assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_QR);
    puts("PASS: repeated INIT/wake preserves a valid QR uid and requests one when confirmed unauthenticated without a uid");
    reset();initialized(1);before_lock(&ctx);tick(&ctx);
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});tick(&ctx);
    assert(!s_auth&&starts[nstarts-1].job==WR_JOB_QR);
    reset();initialized(1);before_lock(&ctx);tick(&ctx);
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});mock_authenticated=true;
    tick(&ctx);assert(s_auth&&starts[nstarts-1].job==WR_JOB_INIT&&s_sync_after_init);
    complete((weread_result_t){.job=WR_JOB_INIT,.error=ESP_ERR_INVALID_STATE,.authenticated=false});mock_authenticated=true;
    before=nstarts;tick(&ctx);assert(nstarts==before&&s_sync_after_init&&strstr(s_message,"TF 卡"));
    puts("PASS: wake revocation enters QR, stale wake auth does not clear a new session, failed INIT cannot loop or flash QR");

    /* M3 WR-UX-07c: visible-row probe runs once per page without looping and re-arms on page change/clear */
    reset();mock_probe_enabled=true;initialized(12);ready_page();s_preconnected=true;
    for(int i=0;i<2;++i){
        ctx.now_ms+=1100;assert(tick(&ctx)==APP_REDRAW_NONE);
        assert(starts[nstarts-1].job==WR_JOB_COVER&&!strcmp(starts[nstarts-1].value,"warm-disk"));
        weread_result_t wr={.job=WR_JOB_COVER,.authenticated=true,.cover=malloc(1)};assert(wr.cover);
        snprintf(wr.id,sizeof(wr.id),"id_%d",9+i);complete(wr);assert(tick(&ctx)==APP_REDRAW_NONE);
    }
    before=nstarts;
    assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==before+1&&starts[nstarts-1].job==WR_JOB_PROBE_SHELF);
    complete((weread_result_t){.job=WR_JOB_PROBE_SHELF,.authenticated=true});
    assert(tick(&ctx)==APP_REDRAW_NONE);
    for(int i=0;i<4;++i){ctx.now_ms+=500;assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==before+1);}
    assert(shelf_turn_page_to(1)==APP_REDRAW_PAGE);ready_page();ctx.now_ms+=1100;
    before=nstarts;assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==before+1&&starts[nstarts-1].job==WR_JOB_PROBE_SHELF);
    complete((weread_result_t){.job=WR_JOB_PROBE_SHELF,.authenticated=true});assert(tick(&ctx)==APP_REDRAW_NONE);
    puts("PASS: visible-row WR_JOB_PROBE_SHELF runs once per page without looping");

    /* M3 WR-UX-01c: passive -2013 mid-session expiry keeps offline shelf without WR_JOB_QR and permits local covers */
    reset();initialized(2);
    assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_SYNC,.error=ESP_ERR_INVALID_STATE,.authenticated=false,.auth_expired=true});
    before=nstarts;assert(tick(&ctx)==APP_REDRAW_PAGE&&!s_auth&&s_auth_expired&&offline_readonly()&&nstarts==before);
    assert(strstr(s_message,"登录已过期"));
    ctx.now_ms+=200;assert(tick(&ctx)==APP_REDRAW_NONE&&starts[nstarts-1].job==WR_JOB_COVER&&!starts[nstarts-1].value[0]);
    complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=false,.auth_expired=true,.error=ESP_ERR_NOT_FOUND});
    strcpy(mock_result.id,"id_0");assert(tick(&ctx)==APP_REDRAW_NONE&&offline_readonly());
    ctx.now_ms+=1200;before=nstarts;tick(&ctx);
    for(int k=before;k<nstarts;++k)assert(!strcmp(starts[k].value,"")||starts[k].job!=WR_JOB_COVER);
    puts("PASS: mid-session auth_expired retains offline shelf, skips WR_JOB_QR, and skips remote cover fetch");

    reset();return 0;
}
'''
source = out / "probe.c"
source.write_text(unit)
args = list(baseline["args"])
args[args.index(str(root / "tools/test_weread_cache_ui.c"))] = str(source)
args[-1] = str(out / "probe")
subprocess.run(args, cwd=root, check=True)
subprocess.run([str(out / "probe")], cwd=root, check=True)
