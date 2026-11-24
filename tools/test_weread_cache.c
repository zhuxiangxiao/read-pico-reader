/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * Production cache regression; UI/platform boundaries use host stubs.
 */

#include "weread_auth.h"
#include "weread_client.h"
#include "weread_chapter.h"
#include "weread_shelf.h"
#include "weread_store.h"
#include "book_source_weread.h"
#include "weread_memory_test.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static time_t wall=1800000000;
time_t cache_test_time(time_t *out){if(out)*out=wall;return wall;}
static int revision=1,requests;
static esp_err_t transport(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){(void)method;(void)headers;(void)body;(void)ctx;++requests;assert(strstr(url,"/shelf/sync"));char reply[1024];snprintf(reply,sizeof(reply),"{\"synckey\":1,\"books\":[{\"bookId\":\"cachebook\",\"title\":\"T\",\"cover\":\"https://res.weread.qq.com/%d.jpg\"}],\"bookIds\":[]}",revision);*out=strdup(reply);*len=strlen(reply);*status=200;return ESP_OK;}
static void account(const char *vid){weread_auth_session_t s={0};strcpy(s.vid,vid);strcpy(s.access_token,"test");strcpy(s.refresh_token,"refresh");assert(weread_auth_save_session(&s)==ESP_OK&&weread_auth_load_session(&s)==ESP_OK);}
int main(void){wr_memory_init();char root[]="/tmp/kiiko-review-cache-XXXXXX";assert(mkdtemp(root));assert(weread_auth_init(root)==ESP_OK);account("123");weread_client_set_transport(transport,NULL);assert(weread_shelf_sync(true)==ESP_OK&&requests==1);assert(weread_shelf_cache_fresh(wall)&&weread_shelf_cache_fresh(wall+299)&&!weread_shelf_cache_fresh(wall+300)&&!weread_shelf_cache_fresh(wall-1));
 uint8_t *pixels=wr_alloc(176u*240u),*loaded=wr_alloc(176u*240u);assert(pixels&&loaded);memset(pixels,97,176u*240u);char first[512],next[512],legacy[512];assert(weread_cover_cache_path("cachebook","gray",first,sizeof(first))==ESP_OK);assert(wr_write_bytes(first,pixels,176u*240u)==ESP_OK);assert(book_weread_cover("weread://cachebook",loaded,176u*240u)&&!memcmp(pixels,loaded,176u*240u));
 revision=2;assert(weread_shelf_sync(true)==ESP_OK);assert(weread_cover_cache_path("cachebook","gray",next,sizeof(next))==ESP_OK&&strcmp(first,next));assert(!book_weread_cover("weread://cachebook",loaded,176u*240u));assert(wr_cache_path("cachebook","cover.gray",legacy,sizeof(legacy))==ESP_OK&&wr_write_bytes(legacy,pixels,176u*240u)==ESP_OK);assert(book_weread_cover("weread://cachebook",loaded,176u*240u)&&access(legacy,F_OK)&&!access(next,F_OK));
 revision=3;assert(weread_shelf_sync(true)==ESP_OK&&!book_weread_cover("weread://cachebook",loaded,176u*240u));
 assert(weread_shelf_set_chapter_counts("cachebook",2,5)==ESP_OK);
 char ch_dir[512],ch_file[512],keep_p[512];
 assert(wr_cache_path("cachebook","chapters/v1-c1-g1",ch_dir,sizeof(ch_dir))==ESP_OK&&wr_mkdirs(ch_dir)==ESP_OK);
 assert(wr_cache_path("cachebook","chapters/v1-c1-g1/body.html",ch_file,sizeof(ch_file))==ESP_OK&&wr_write_bytes(ch_file,"<p>hi</p>",9)==ESP_OK);
 assert(wr_cache_path("cachebook","progress.json",keep_p,sizeof(keep_p))==ESP_OK&&wr_write_bytes(keep_p,"{}",2)==ESP_OK);
 weread_chapter_book_cache_info_t binfo={0};weread_chapter_cache_stats_t stats={0};uint64_t freed=0;uint32_t cleaned=0;
 assert(weread_chapter_inspect_book_cache("cachebook",&binfo,NULL,NULL)==ESP_OK&&binfo.reclaimable_bytes==9&&binfo.meta_bytes==2);
 assert(weread_chapter_inspect_cache_stats(&stats,NULL,NULL)==ESP_OK&&stats.reclaimable_bytes>=9);
 assert(weread_chapter_clean_all_caches(&freed,&cleaned,NULL,NULL)==ESP_OK&&freed>=9&&cleaned>=1&&access(ch_dir,F_OK)&&!access(keep_p,F_OK));
 assert(weread_shelf_set_chapter_counts("cachebook",0,5)==ESP_OK);
 account("999");assert(!weread_shelf_cache_fresh(wall));assert(weread_shelf_sync(true)==ESP_OK&&!book_weread_cover("weread://cachebook",loaded,176u*240u));free(pixels);free(loaded);weread_shelf_clear();assert(weread_auth_clear_session()==ESP_OK);weread_test_memory_report();puts("PASS: production TTL boundary/future time, keyed gray read, URL invalidation, one-time legacy rename and account isolation");}
