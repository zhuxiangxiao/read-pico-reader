/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_progress.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned writes,fail_on,net;
static bool armed;
esp_err_t actual_write_json(const char *,const cJSON *);
esp_err_t wr_write_json(const char *path,const cJSON *o){if(armed&&strstr(path,"progress.json")&&++writes==fail_on)return ESP_FAIL;return actual_write_json(path,o);}
static esp_err_t request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){(void)method;(void)headers;(void)body;(void)ctx;assert(strstr(url,"/book/read"));++net;*status=200;*out=strdup("{\"succ\":1}");*len=strlen(*out);return ESP_OK;}
int main(int argc,char **argv){
    assert(argc==3);unsigned mode=(unsigned)strtoul(argv[2],NULL,10);
    assert(!setenv("WEREAD_STORAGE_ROOT",argv[1],1));assert(weread_auth_init(argv[1])==ESP_OK);
    weread_auth_session_t s={0};strcpy(s.vid,"123");strcpy(s.access_token,"test-token");strcpy(s.refresh_token,"fixture-refresh");
    assert(weread_auth_save_session(&s)==ESP_OK&&weread_auth_load_session(&s)==ESP_OK);weread_client_set_transport(request,NULL);
    assert(weread_progress_note("test_book_101",1,1,10,25,7,1,"old",1700000000)==ESP_OK);
    weread_progress_t old;assert(weread_progress_load("test_book_101",&old)==ESP_OK);
    if(mode==4){old.last_report_ms=1000;assert(weread_progress_checkpoint("test_book_101",&old)==ESP_OK);}
    if(mode==5)assert(weread_progress_select("test_book_101",&old,true)==ESP_OK);
    weread_progress_t delta={.chapter_uid=2,.chapter_index=2,.chapter_offset=20,.progress=86,.reading_time=3,.pending_pages=2};strcpy(delta.summary,"new");
    armed=true;fail_on=mode<=2?mode:99;bool noted=false;
    esp_err_t err=weread_progress_update("test_book_101",&delta,mode!=5,mode!=4,1500,1700000100,&noted);
    weread_progress_t saved;assert(weread_progress_load("test_book_101",&saved)==ESP_OK);
    printf("mode=%u err=%d noted=%u net=%u writes=%u durable uid%u seconds%u dirty%u pending%u returned uid%u seconds%u dirty%u pending%u\n",mode,err,noted,net,writes,saved.chapter_uid,saved.reading_time,saved.dirty,saved.position_pending,delta.chapter_uid,delta.reading_time,delta.dirty,delta.position_pending);
    if(mode==1)assert(err==ESP_FAIL&&!noted&&!net&&saved.chapter_uid==1&&saved.reading_time==7);
    else if(mode==2)assert(err==ESP_FAIL&&noted&&net==1&&saved.chapter_uid==2&&saved.reading_time==10&&delta.dirty&&delta.position_pending&&delta.reading_time==10);
    else if(mode==3)assert(err==ESP_OK&&noted&&net==1&&!saved.dirty&&!delta.dirty&&!saved.reading_time&&!delta.reading_time);
    else if(mode==4){
        assert(err==ESP_OK&&noted&&!net&&saved.dirty&&delta.position_pending&&delta.reading_time==10);
        memset(&delta,0,sizeof(delta));assert(weread_progress_update("test_book_101",&delta,false,true,1500,1700000100,&noted)==ESP_OK&&noted&&net==1&&!delta.dirty);
    }else if(mode==5){
        assert(err==ESP_OK&&noted&&!net&&saved.dirty&&!delta.position_pending&&saved.reading_time==3);
        writes=0;memset(&delta,0,sizeof(delta));assert(weread_progress_update("test_book_101",&delta,false,true,1500,1700000100,&noted)==ESP_OK&&noted&&!writes&&!net&&delta.reading_time==3);
    }
    return 0;
}
