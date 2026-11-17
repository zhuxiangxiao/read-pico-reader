/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：生产缓存的空间、内容与故障边界。/ English: Production cache space, content and failure boundaries.
 */
#include "weread_chapter.h"
#include "weread_auth.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_store.h"
#include "weread_position.h"
#include "book_source_weread.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

static uint64_t free_bytes=UINT64_C(512)*1024*1024;
static unsigned calls,downloads,plain_parses,html_allocations,mapped_parses;
static bool deny_map;
void *actual_wr_alloc(size_t);
void *wr_alloc(size_t bytes){return deny_map&&bytes==8192u*sizeof(weread_position_point_t)?NULL:actual_wr_alloc(bytes);}
esp_err_t actual_html_to_blocks_mapped(const char *,size_t,const char *,size_t,html_source_observer_fn,void *,html_text_t *);
esp_err_t html_to_blocks_mapped(const char *html,size_t len,const char *css,size_t css_len,html_source_observer_fn fn,void *ctx,html_text_t *out){++mapped_parses;return actual_html_to_blocks_mapped(html,len,css,css_len,fn,ctx,out);}
void *offline_html_malloc(size_t bytes,unsigned caps){(void)caps;++html_allocations;return malloc(bytes);}
void *offline_html_realloc(void *p,size_t bytes,unsigned caps){(void)caps;++html_allocations;return realloc(p,bytes);}
esp_err_t actual_html_to_blocks(const char *html,size_t len,html_text_t *out);
esp_err_t html_to_blocks(const char *html,size_t len,html_text_t *out){++plain_parses;return actual_html_to_blocks(html,len,out);}
static bool no_space_query,fail_publish;
static const char *fixtures;
int weread_test_statvfs(const char *path,struct statvfs *out){
    (void)path;++calls;if(no_space_query){errno=EIO;return -1;}
    memset(out,0,sizeof(*out));out->f_frsize=4096;out->f_bavail=free_bytes/4096;return 0;
}
esp_err_t actual_write_json(const char *,const cJSON *);
esp_err_t wr_write_json(const char *path,const cJSON *o){
    if(fail_publish&&strstr(path,"/chapter-"))return ESP_FAIL;
    return actual_write_json(path,o);
}
static esp_err_t request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)body;(void)ctx;*status=200;
    char toc[4096]="";size_t used=0;
    if(strstr(url,"chapterInfos")){
        used=(size_t)snprintf(toc,sizeof(toc),"{\"data\":[{\"bookId\":\"offline_book\",\"bookVersion\":100,\"synckey\":1,\"clearAll\":1,\"updated\":[");
        for(unsigned i=1;i<=16;++i)used+=(size_t)snprintf(toc+used,sizeof(toc)-used,"%s{\"chapterUid\":%u,\"chapterIdx\":%u,\"title\":\"测试%u\"}",i==1?"":",",i*10,i,i);
        snprintf(toc+used,sizeof(toc)-used,"]}]}");
    }
    const char *r=toc[0]?toc:"{\"synckey\":1,\"books\":[{\"bookId\":\"offline_book\",\"title\":\"离线测试\",\"format\":\"epub\",\"bookVersion\":100}]}";
    *out=strdup(r);assert(*out);*len=strlen(*out);return ESP_OK;
}
static esp_err_t download(const char *url,const char *headers,FILE *out,size_t limit,weread_cancel_fn cancel,void *cc,weread_download_info_t *info,void *ctx){
    (void)headers;(void)ctx;++downloads;
    const char *p=strstr(url,"chapters=");assert(p);p+=9;if(!strncmp(p,"0,",2))p+=2;
    unsigned uid=(unsigned)strtoul(p,NULL,10);char path[1024];snprintf(path,sizeof(path),"%s/%u.zip",fixtures,uid);
    FILE *f=fopen(path,"rb");assert(f);info->status=200;
    strcpy(info->encrypt_key,"pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
    char buf[97];size_t n;
    while((n=fread(buf,1,sizeof(buf),f))){
        if(cancel&&cancel(cc)){fclose(f);return ESP_ERR_INVALID_STATE;}
        assert(n<=limit-info->bytes&&fwrite(buf,1,n,out)==n);info->bytes+=n;
    }
    fclose(f);return ESP_OK;
}
static void paths(uint32_t uid,char root[512],char map[600]){
    assert(weread_chapter_directory("offline_book",uid,root,512)==ESP_OK);
    snprintf(map,600,"%s/position.map",root);
}
int main(int argc,char **argv){
    assert(argc==3);fixtures=argv[2];assert(weread_auth_init(argv[1])==ESP_OK);
    weread_auth_session_t s={0};strcpy(s.vid,"123");strcpy(s.access_token,"test-token");strcpy(s.refresh_token,"test-refresh");
    assert(weread_auth_save_session(&s)==ESP_OK&&weread_auth_load_session(&s)==ESP_OK);
    weread_client_set_transport(request,NULL);weread_client_set_download_transport(download,NULL);
    assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("offline_book")==ESP_OK);
    uint64_t available=0;
    free_bytes=103u*1024*1024;assert(weread_chapter_prefetch_space(&available)==WEREAD_ERR_STORAGE&&available==free_bytes);
    assert(weread_chapter_download_ex("offline_book",10,true)==WEREAD_ERR_STORAGE&&!downloads);
    no_space_query=true;assert(weread_chapter_download_ex("offline_book",10,true)==ESP_FAIL&&!downloads);no_space_query=false;
    free_bytes=104u*1024*1024;assert(weread_chapter_prefetch_space(NULL)==ESP_OK);
    for(uint32_t uid=10;uid<=160;uid+=10)assert(weread_chapter_download_ex("offline_book",uid,true)==ESP_OK&&weread_chapter_cached("offline_book",uid));
    assert(!plain_parses&&!html_allocations);
    puts("PASS: assembling16 chapters including1MiB decorative background invokes no HTML parser and allocates no parser text/workspace");
    unsigned old_downloads=downloads,old_calls=calls;free_bytes=0;no_space_query=true;
    assert(weread_chapter_download_ex("offline_book",10,true)==ESP_OK&&downloads==old_downloads&&calls==old_calls);no_space_query=false;
    assert(book_weread_open("weread://offline_book")==ESP_OK&&book_weread_count()==16);
    html_text_t text={0};assert(book_weread_load(0,NULL,NULL,&text)==ESP_OK&&text.utf8&&!text.len&&!text.image_count);html_text_free(&text);
    assert(book_weread_load(1,NULL,NULL,&text)==ESP_OK);
    printf("background-only images=%u bytes=%u\n",(unsigned)text.image_count,(unsigned)text.len);fflush(stdout);
    assert(text.image_count==1&&!strcmp(text.images[0],"Images/bg.png"));
    uint32_t source=0,back=99;assert(book_weread_map(1,true,0,&source)==ESP_OK&&book_weread_map(1,false,source,&back)==ESP_OK&&back==0);html_text_free(&text);
    assert(book_weread_load(2,NULL,NULL,&text)==ESP_OK&&text.image_count==2&&strstr(text.utf8,"甲")&&strstr(text.utf8,"乙"));html_text_free(&text);
    assert(book_weread_load(3,NULL,NULL,&text)==ESP_OK&&strstr(text.utf8,"普通正文"));html_text_free(&text);
    // The anchored path is an independent production-parser oracle; no display commit is guessed from a load.
    size_t current=3;uint32_t actual_version=0;
    assert(book_weread_load(3,"missing-anchor",NULL,&text)==ESP_OK);
    assert(book_weread_commit_loaded(&current,&actual_version)==ESP_OK&&current==3&&actual_version==100);
    uint32_t expected[128],expected_back[128],end_units=0;
    assert(book_weread_map_version(3,100,true,(uint32_t)text.len,&end_units)==ESP_OK&&end_units);
    for(unsigned i=0;i<128;++i)assert(book_weread_map_version(3,100,true,i%text.len,&expected[i])==ESP_OK);
    for(unsigned i=0;i<128;++i)assert(book_weread_map_version(3,100,false,i%(end_units+1),&expected_back[i])==ESP_OK);
    html_text_free(&text);
    assert(book_weread_load(3,NULL,NULL,&text)==ESP_OK&&book_weread_commit_loaded(&current,&actual_version)==ESP_OK);
    assert(book_weread_map_cache_bytes()>0&&book_weread_map_cache_bytes()<=65536);
    printf("exact map cache resident=%u bytes for%u UTF8 bytes\n",(unsigned)book_weread_map_cache_bytes(),(unsigned)text.len);
    unsigned before_maps=mapped_parses;
    for(unsigned i=0;i<128;++i){uint32_t cloud=0;assert(book_weread_map_version(3,100,true,i%text.len,&cloud)==ESP_OK&&cloud==expected[i]);}
    for(unsigned i=0;i<128;++i){uint32_t local=0;assert(book_weread_map_version(3,100,false,i%(end_units+1),&local)==ESP_OK&&local==expected_back[i]);}
    assert(mapped_parses==before_maps);html_text_free(&text);
    deny_map=true;assert(book_weread_load(3,NULL,NULL,&text)==ESP_OK&&book_weread_commit_loaded(&current,&actual_version)==ESP_OK);
    before_maps=mapped_parses;uint32_t cloud=0;assert(book_weread_map_version(3,100,true,0,&cloud)==ESP_OK&&cloud==expected[0]&&mapped_parses==before_maps+1);
    deny_map=false;html_text_free(&text);assert(!book_weread_map_cache_bytes());
    puts("PASS:256 exact bidirectional map queries match anchored parser oracle without reparsing; map-cache OOM falls back to complete mapping");
    for(size_t i=4;i<15;++i){
        assert(book_weread_load(i,NULL,NULL,&text)==ESP_OK);
        printf("content uid=%u images=%u bytes=%u\n",(unsigned)(i+1)*10,(unsigned)text.image_count,(unsigned)text.len);fflush(stdout);
        if(i==5||i==11||i==14)assert(text.image_count==1&&text.len==3);
        else if(i==12)assert(!text.image_count&&text.len==1024u*1024u);
        else if(i==6)assert(!text.image_count&&strstr(text.utf8,"装饰背景保留文字"));
        else assert(!text.image_count&&!text.len);
        assert(book_weread_map_cache_bytes()<=128u*1024u);
        if(i==12)assert(!book_weread_map_cache_bytes());
        html_text_free(&text);
    }
    current=15;assert(book_weread_load(15,NULL,NULL,&text)==ESP_OK&&text.len==18000&&book_weread_commit_loaded(&current,&actual_version)==ESP_OK);
    assert(book_weread_map_cache_bytes()==48000);before_maps=mapped_parses;
    for(unsigned i=0;i<6000;i+=43){uint32_t cloud=0,local=0;assert(book_weread_map_version(15,100,true,i*3,&cloud)==ESP_OK&&cloud==15+i);assert(book_weread_map_version(15,100,false,cloud,&local)==ESP_OK&&local==i*3);}
    assert(mapped_parses==before_maps);html_text_free(&text);
    puts("PASS: typical6000-Han chapter uses48000 exact-map bytes;280 bidirectional save queries perform zero reparse");
    char root[512],map_path[600];paths(40,root,map_path);
    FILE *f=fopen(map_path,"r+b");assert(f);weread_position_header_t h={0};assert(fread(&h,sizeof(h),1,f)==1);h.bytes++;rewind(f);assert(fwrite(&h,sizeof(h),1,f)==1&&!fclose(f));
    assert(!weread_chapter_cached("offline_book",40));
    char manifest[512];assert(wr_cache_path("offline_book","chapter-40.json",manifest,sizeof(manifest))==ESP_OK);
    cJSON *before=NULL,*after=NULL;assert(wr_read_json(manifest,&before)==ESP_OK);
    free_bytes=512u*1024*1024;fail_publish=true;assert(weread_chapter_download_ex("offline_book",40,true)==ESP_FAIL);fail_publish=false;
    assert(wr_read_json(manifest,&after)==ESP_OK&&!strcmp(wr_string(before,"root"),wr_string(after,"root")));cJSON_Delete(before);cJSON_Delete(after);
    assert(weread_chapter_download_ex("offline_book",40,true)==ESP_OK&&weread_chapter_cached("offline_book",40));
    paths(40,root,map_path);f=fopen(map_path,"ab");assert(f&&fputc(0,f)!=EOF&&!fclose(f));assert(!weread_chapter_cached("offline_book",40));
    assert(weread_chapter_download_ex("offline_book",40,true)==ESP_OK);paths(40,root,map_path);assert(!unlink(map_path)&&!weread_chapter_cached("offline_book",40));
    assert(weread_chapter_download_ex("offline_book",40,true)==ESP_OK);
    book_weread_close();strcpy(s.vid,"456");assert(weread_auth_save_session(&s)==ESP_OK&&weread_auth_load_session(&s)==ESP_OK&&!weread_chapter_cached("offline_book",10));
    puts("PASS: prefetch budget, cached low-space reads, empty/background/mixed EPUB, source coordinates, malformed maps, failed publication and owner isolation");return 0;
}
