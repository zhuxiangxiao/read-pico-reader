/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_store.h"
#include "book_source_weread.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *fixtures;
static atomic_bool entered;
static bool delay_report;
static unsigned downloads,reports;
static uint32_t remote_offset;
static uint64_t micros(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static void settle(void){
    uint64_t until=micros()+5000000;
    while(weread_runtime_busy()){assert(micros()<until);usleep(1000);}
    weread_result_t result;if(weread_runtime_poll(&result))weread_runtime_result_free(&result);
}
static esp_err_t transport(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)ctx;*status=200;char response[768];
    if(strstr(url,"chapterInfos")){
        strcpy(response,"{\"data\":[{\"bookId\":\"bench_book\",\"bookVersion\":100,\"synckey\":1,\"clearAll\":1,\"updated\":[{\"chapterUid\":11,\"chapterIdx\":1,\"title\":\"First\"},{\"chapterUid\":27,\"chapterIdx\":2,\"title\":\"Next\"}]}]}");
    }else if(strstr(url,"/book/read")){
        ++reports;
        if(delay_report){
            atomic_store(&entered,true);
            // Controlled socket-before-visibility model: deliberately ignore cancellation for1000ms.
            struct timespec wait={.tv_sec=1,.tv_nsec=0};nanosleep(&wait,NULL);
        }
        cJSON *request=wr_parse_json(body,strlen(body)+1);assert(request);
        remote_offset=(uint32_t)cJSON_GetObjectItemCaseSensitive(request,"chapterOffset")->valuedouble;cJSON_Delete(request);
        strcpy(response,"{\"succ\":1}");
    }else{
        snprintf(response,sizeof(response),"{\"synckey\":1,\"books\":[{\"bookId\":\"bench_book\",\"title\":\"Benchmark\",\"format\":\"epub\",\"bookVersion\":100}],\"bookProgress\":[{\"bookId\":\"bench_book\",\"chapterUid\":11,\"chapterIdx\":1,\"chapterOffset\":%u,\"progress\":0}]}",remote_offset);
    }
    *out=strdup(response);assert(*out);*len=strlen(*out);return ESP_OK;
}
static esp_err_t download(const char *url,const char *headers,FILE *out,size_t limit,weread_cancel_fn cancel,void *cc,weread_download_info_t *info,void *ctx){
    (void)headers;(void)ctx;++downloads;const char *p=strstr(url,"chapters=");assert(p);p+=9;if(!strncmp(p,"0,",2))p+=2;
    unsigned uid=(unsigned)strtoul(p,NULL,10);char path[1024];snprintf(path,sizeof(path),"%s/%u.zip",fixtures,uid);
    FILE *in=fopen(path,"rb");assert(in);info->status=200;strcpy(info->encrypt_key,"pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
    char chunk[97];size_t n;while((n=fread(chunk,1,sizeof(chunk),in))){
        assert(!cancel||!cancel(cc));assert(n<=limit-info->bytes&&fwrite(chunk,1,n,out)==n);info->bytes+=n;
    }
    assert(!fclose(in));return ESP_OK;
}
int main(int argc,char **argv){
    assert(argc==3);fixtures=argv[2];assert(!setenv("WEREAD_STORAGE_ROOT",argv[1],1));
    assert(weread_auth_init(argv[1])==ESP_OK);weread_auth_session_t session={0};
    strcpy(session.vid,"123");strcpy(session.access_token,"fixture-token");strcpy(session.refresh_token,"fixture-refresh");
    assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    weread_client_set_transport(transport,NULL);weread_client_set_download_transport(download,NULL);
    assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("bench_book")==ESP_OK);
    assert(weread_chapter_download("bench_book",11)==ESP_OK&&weread_chapter_download("bench_book",27)==ESP_OK);
    assert(downloads==2);weread_runtime_init();unsigned failures=0;
    for(unsigned sample=0;sample<10;++sample){
        remote_offset=0;delay_report=false;atomic_store(&entered,false);
        weread_progress_t cloud={.chapter_uid=11,.chapter_index=1,.book_version=100};
        assert(weread_progress_select("bench_book",&cloud,true)==ESP_OK&&book_weread_open("weread://bench_book")==ESP_OK);
        size_t chapter=99;uint32_t offset=99;assert(weread_reader_resume("weread://bench_book",&chapter,&offset)==ESP_OK&&chapter==0);
        html_text_t text={0};assert(book_weread_load(0,NULL,NULL,&text)==ESP_OK&&text.len>10);
#if defined(WEREAD_BENCH_CURRENT)
        uint32_t edition=0;assert(book_weread_commit_loaded(&chapter,&edition)==ESP_OK&&edition==100);
#endif
        weread_reader_begin("weread://bench_book",1000);
        for(unsigned move=0;move<10;++move){uint32_t target=move%2?7:4;
            weread_reader_navigated("weread://bench_book",0,target,10,text.utf8+target);
            weread_reader_tick("weread://bench_book",0,target,10,text.utf8+target,1000,false);
        }
        uint32_t expected=0;assert(book_weread_map(0,true,7,&expected)==ESP_OK);
        delay_report=true;unsigned before_reports=reports;
        weread_reader_tick("weread://bench_book",0,7,10,text.utf8+7,2000,true);
#if defined(WEREAD_BENCH_CURRENT)
        // Current builds verify the next-chapter cache bit asynchronously before issuing urgent reports.
        weread_reader_tick("weread://bench_book",0,7,10,text.utf8+7,2001,true);
        settle();
        weread_reader_tick("weread://bench_book",0,7,10,text.utf8+7,2001,true);
#else
        weread_reader_tick("weread://bench_book",0,7,10,text.utf8+7,2001,true);
#endif
        uint64_t until=micros()+5000000;while(!atomic_load(&entered)){assert(micros()<until);usleep(1000);}
        uint64_t begin=micros();esp_err_t err=weread_reader_prepare("weread://bench_book",1);uint64_t elapsed=micros()-begin;
        settle();delay_report=false;weread_progress_t durable;
        bool valid=err==ESP_OK&&downloads==2&&reports==before_reports+1&&weread_progress_load("bench_book",&durable)==ESP_OK&&
            durable.chapter_uid==11&&durable.chapter_offset==expected&&!durable.dirty;
        if(!valid)++failures;
        printf("SAMPLE %u %llu %d %u %u\n",sample,(unsigned long long)elapsed,err,(unsigned)valid,expected);fflush(stdout);
        html_text_free(&text);weread_reader_end(2002);book_weread_close();weread_runtime_stop();
    }
    weread_shelf_clear();assert(!failures);return 0;
}
