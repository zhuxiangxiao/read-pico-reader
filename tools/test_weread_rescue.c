/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#define main outbox_fixture_main
#include "test_weread_outbox.c"
#undef main
#include "test_weread_rescue_backend.h"
static esp_err_t primary_error;
static unsigned primary_fault;
static unsigned switch_media_stage;
static esp_err_t media_switch_transport(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    esp_err_t err=transport(url,method,headers,body,out,len,status,ctx);
    if((switch_media_stage==1&&strstr(url,"/shelf/syncbook"))||(switch_media_stage==2&&strstr(url,"/book/read"))){
        current_card[0]=2;switch_media_stage=0;
    }
    return err;
}
esp_err_t actual_read_json_primary(const char *path,cJSON **out);
esp_err_t wr_read_json_primary(const char *path,cJSON **out){
    if(primary_fault&&strstr(path,"progress.json")){--primary_fault;*out=NULL;return primary_error;}
    return actual_read_json_primary(path,out);
}

static void block_tf(bool block){
    char path[1200];snprintf(path,sizeof(path),"%s/cache/%s/progress.json.tmp",weread_auth_get_storage_root(),id);
    assert((block?mkdir(path,0700):rmdir(path))==0);
}
static esp_err_t rescue_move(uint32_t offset,const weread_progress_t *before,bool *noted,bool *rescued){
    weread_progress_t delta={.chapter_uid=1,.chapter_index=1,.chapter_offset=offset,.progress=20,.reading_time=2,.pending_pages=1};
    strcpy(delta.summary,"rescue");uint8_t captured[WEREAD_MEDIA_ID_BYTES]={1};
    return weread_progress_store_version_resilient(id,&delta,true,100,1700000101,noted,before,captured,rescued);
}
static weread_progress_t pending(void){weread_progress_t p;assert(weread_progress_load(id,&p)==ESP_OK);return p;}
static void begin(const char *root){seed();move(20,5);assert(!internal_writes);(void)root;}
int main(int argc,char **argv){
    assert(argc==4);snprintf(rescue_root,sizeof(rescue_root),"%s",argv[2]);
    weread_progress_rescue_backend_t backend={read_slot,write_slot,erase_slot,fixture_card,NULL};
    weread_progress_rescue_backend(&backend);login(argv[1],"123","access-fixture");weread_client_set_transport(transport,NULL);
    bool noted=false,rescued=false;const char *mode=argv[3];weread_progress_t before,p;
    if(!strcmp(mode,"recover")){
        p=pending();assert(p.chapter_offset==30&&p.reading_time==7&&p.pending_pages==2&&p.base_offset==10);
        block_tf(false);char next[64];assert(weread_progress_outbox_next(next,sizeof(next),1700000200)==ESP_OK&&!strcmp(next,id));
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_OK&&!p.dirty&&writes==1&&internal_erases==1);
        p=pending();assert(p.chapter_offset==30&&!p.dirty);puts("PASS cold process restores owner/CID-bound complete progress before ACK");return 0;
    }
    begin(argv[1]);before=pending();
    if(!strncmp(mode,"cid-",4)){
        // Clone coordinates/session/root deliberately match; the immutable physical CID is the only distinction.
        char path[1200];snprintf(path,sizeof(path),"%s/cache/%s/progress.json",argv[1],id);uint64_t original=file_hash(path);
        switch_media_stage=strstr(mode,"get")?1:2;weread_client_set_transport(media_switch_transport,NULL);
        esp_err_t err=weread_progress_outbox_report(id,90000,1700000200,&p);
        fprintf(stderr,"%s: result=%x GET=%u POST=%u dirty=%u card=%u\n",mode,(unsigned)err,reads,writes,p.dirty,current_card[0]);
        assert(err==ESP_ERR_INVALID_STATE&&reads==1&&writes==(strstr(mode,"get")?0u:1u)&&current_card[0]==2);
        assert(file_hash(path)==original&&pending().dirty&&pending().chapter_offset==20);
        current_card[0]=1;assert(pending().dirty&&pending().chapter_offset==20&&pending().reading_time==5);
        puts("PASS request-time CID handoff cannot POST an old card after GET or acknowledge it into a cloned replacement");return 0;
    }
    if(!strcmp(mode,"ordinary")){
        assert(rescue_move(30,&before,&noted,&rescued)==ESP_OK&&noted&&!rescued&&!internal_writes);
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_OK&&!internal_writes);return 0;
    }
    if(!strcmp(mode,"missing-read")){
        char cache[1200],away[1210];snprintf(cache,sizeof(cache),"%s/cache",argv[1]);snprintf(away,sizeof(away),"%s-away",cache);
        assert(!rename(cache,away));absent_card=true;
        assert(rescue_move(30,&before,&noted,&rescued)==ESP_OK&&noted&&rescued&&internal_writes==1);
        assert(weread_progress_load(id,&p)==ESP_ERR_INVALID_STATE&&p.chapter_uid==0);
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_ERR_INVALID_STATE&&!writes&&!reads);
        assert(!rename(away,cache));absent_card=false;p=pending();assert(p.chapter_offset==30&&p.base_offset==10);
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_OK&&!p.dirty);return 0;
    }
    block_tf(true);
    if(!strcmp(mode,"internal-fail")){
        internal_fail=true;assert(rescue_move(30,&before,&noted,&rescued)!=ESP_OK&&!noted&&!rescued&&!internal_writes);
        p=pending();assert(p.chapter_offset==20);return 0;
    }
    if(!strcmp(mode,"ack-race")){
        pthread_t thread=delayed(2);assert(rescue_move(30,&before,&noted,&rescued)==ESP_OK&&noted&&rescued);
        complete(thread);assert(report_result==ESP_OK&&result.chapter_offset==30&&result.dirty&&result.reading_time==2&&result.pending_pages==1);
        p=pending();assert(p.chapter_offset==30&&p.base_offset==20&&p.reading_time==2);
        block_tf(false);assert(weread_progress_outbox_report(id,100000,1700000201,&p)==ESP_OK&&!p.dirty&&writes==2);return 0;
    }
    assert(rescue_move(30,&before,&noted,&rescued)==ESP_OK&&noted&&rescued&&internal_writes==1);
    p=pending();assert(p.chapter_offset==30&&p.base_known&&p.base_offset==10&&p.reading_time==7);
    if(!strcmp(mode,"produce"))return 0;
    if(!strncmp(mode,"restore-",8)){
        block_tf(false);char path[1200];snprintf(path,sizeof(path),"%s/cache/%s/progress.json",argv[1],id);uint64_t original=file_hash(path);
        primary_error=strstr(mode,"oom")?ESP_ERR_NO_MEM:strstr(mode,"io")?ESP_FAIL:
            strstr(mode,"timeout")?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_RESPONSE;
        primary_fault=strstr(mode,"once")?1:UINT32_MAX;esp_err_t failed=weread_progress_outbox_report(id,90000,1700000200,&p);
        fprintf(stderr,"%s: error=%x expected=%x conflict=%u retries=%u HTTP=%u/%u\n",mode,(unsigned)failed,(unsigned)primary_error,p.sync_conflict,p.retry_count,reads,writes);
        assert(failed==primary_error&&p.dirty&&!p.sync_conflict&&p.chapter_offset==30&&p.retry_count==1&&!writes&&!reads);
        if(!strstr(mode,"once"))assert(file_hash(path)==original);primary_fault=0;
        assert(weread_progress_outbox_report(id,90001,1700000300,&p)==ESP_OK&&!p.dirty&&p.chapter_offset==30&&writes==1);
        printf("PASS %s remains retryable, preserves TF and rescue, then recovers without explicit conflict choice\n",mode);return 0;
    }else if(!strcmp(mode,"legacy-rescue")){
        assert(weread_progress_report(id,true,90000)==ESP_ERR_NOT_FINISHED&&!writes&&!reads);
        weread_progress_t delta={.reading_time=2};bool stored=false;
        assert(weread_progress_update(id,&delta,false,true,90000,1700000102,&stored)==ESP_ERR_NOT_FINISHED&&stored&&!writes&&!reads);
        assert(pending().dirty&&pending().chapter_offset==30);
    }else if(!strcmp(mode,"coalesce")){
        assert(rescue_move(40,&p,&noted,&rescued)==ESP_OK&&rescued);p=pending();assert(p.chapter_offset==40&&p.reading_time==9&&p.pending_pages==3);
        char path[1200];snprintf(path,sizeof(path),"%s/s1",rescue_root);assert(access(path,F_OK)!=0);
    }else if(!strcmp(mode,"blocked-report")){
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)!=ESP_OK&&!writes&&!reads&&p.chapter_offset==30&&p.dirty);
        assert(pending().chapter_offset==30);
    }else if(!strcmp(mode,"foreign")){
        block_tf(false);
        login(argv[1],"999","foreign");seed();move(77,1);assert(pending().chapter_offset==77);
        login(argv[1],"123","renewed-token");assert(pending().chapter_offset==30);
        assert(weread_progress_outbox_report(id,90000,1700000300,&p)==ESP_OK&&!p.dirty);return 0;
    }else if(!strcmp(mode,"different-card")){
        current_card[0]=2;assert(pending().chapter_offset==20);
        assert(rescue_move(40,&before,&noted,&rescued)==ESP_ERR_INVALID_STATE&&!noted&&!rescued);
        current_card[0]=1;assert(pending().chapter_offset==30);
    }else if(!strcmp(mode,"different-root")){
        char other[1200];snprintf(other,sizeof(other),"%s-other",argv[1]);login(other,"123","access-fixture");seed();move(77,1);
        assert(pending().chapter_offset==77);login(argv[1],"123","access-fixture");assert(pending().chapter_offset==30);
    }else if(!strcmp(mode,"full")){
        id="second";seed();move(20,1);weread_progress_t second=pending();block_tf(true);
        assert(rescue_move(40,&second,&noted,&rescued)==ESP_OK&&rescued);
        id="third";seed();move(20,1);weread_progress_t third=pending();block_tf(true);
        assert(rescue_move(50,&third,&noted,&rescued)==ESP_ERR_NO_MEM&&!noted&&!rescued);
        id="outbox_book";assert(pending().chapter_offset==30);id="second";assert(pending().chapter_offset==40);return 0;
    }else if(!strcmp(mode,"changed-primary")||!strcmp(mode,"choice")){
        block_tf(false);char path[1200];snprintf(path,sizeof(path),"%s/cache/%s/progress.json",argv[1],id);
        cJSON *json=NULL;assert(wr_read_json(path,&json)==ESP_OK);cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(json,"chapterOffset"),77);
        assert(actual_write_json(path,json)==ESP_OK);cJSON_Delete(json);
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==WEREAD_ERR_PROGRESS_CONFLICT&&!reads&&!writes&&p.sync_conflict);
        if(!strcmp(mode,"choice")){
            cloud.chapter_offset=55;bool conflict=false;weread_progress_t c;
            assert(weread_progress_sync(id,&p,&c,&conflict)==ESP_OK&&conflict);
            assert(weread_progress_select(id,&c,true)==ESP_OK);p=pending();assert(p.chapter_offset==55&&!p.dirty&&!p.position_pending);
            assert(internal_erases==1);return 0;
        }
        assert(wr_read_json(path,&json)==ESP_OK&&cJSON_GetObjectItemCaseSensitive(json,"chapterOffset")->valueint==77);cJSON_Delete(json);return 0;
    }else if(!strcmp(mode,"erase-fail")){
        erase_fail=true;block_tf(false);assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_FAIL&&!reads&&!writes);
        assert(rescue_move(40,&p,&noted,&rescued)==ESP_OK&&rescued&&pending().chapter_offset==40);
        erase_fail=false;assert(weread_progress_outbox_report(id,90000,1700000300,&p)==ESP_OK&&!p.dirty&&p.chapter_offset==40);return 0;
    }else if(!strcmp(mode,"crc")){
        char path[1200];snprintf(path,sizeof(path),"%s/s0",rescue_root);FILE *file=fopen(path,"r+b");assert(file);
        assert(!fseek(file,-1,SEEK_END));int byte=fgetc(file);assert(byte!=EOF&&!fseek(file,-1,SEEK_END));assert(fputc(byte^1,file)!=EOF&&!fclose(file));
        memset(&p,0xff,sizeof(p));assert(weread_progress_load(id,&p)==ESP_ERR_INVALID_CRC&&!p.chapter_uid);
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_ERR_INVALID_CRC&&!writes&&!reads&&!internal_erases);return 0;
    }else assert(!strcmp(mode,"version"));
    block_tf(false);
    if(!strcmp(mode,"version"))cloud.book_version=101;
    esp_err_t err=weread_progress_outbox_report(id,90000,1700000300,&p);
    if(cloud.book_version==101)assert(err==WEREAD_ERR_PROGRESS_CONFLICT&&!writes&&p.book_version==100&&p.sync_conflict);
    else assert(err==ESP_OK&&!p.dirty);
    printf("PASS %s\n",mode);return 0;
}
