/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

#define ESP_PLATFORM
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#define WR_GUARD ((void)0)
#define WR_MEMORY_SCOPE ((void)0)
#define WEREAD_COVER_WIDTH 176
#define WEREAD_COVER_HEIGHT 240
typedef int esp_err_t;
enum {ESP_OK,ESP_FAIL,ESP_ERR_NO_MEM,ESP_ERR_INVALID_STATE,ESP_ERR_INVALID_ARG,ESP_ERR_NOT_FOUND,ESP_ERR_INVALID_RESPONSE,ESP_ERR_NOT_SUPPORTED};
typedef struct {int status;size_t bytes;} weread_download_info_t;
static struct {char id[64];} s_work={"book"};
static struct {uint8_t *cover;int http_status;unsigned cover_retry_seconds;} s_result;
static char root[256];static bool decode_ok,alloc_fail,cancel_flag;static int downloads,decodes;
static time_t wall=1800000000;
static time_t test_time(time_t *out){if(out)*out=wall;return wall;}
#define time test_time
static void *wr_alloc(size_t n){return alloc_fail?NULL:calloc(1,n);}
static void wr_release(void *p){void **q=p;free(*q);*q=NULL;}
static bool cancelled(void *ctx){(void)ctx;return cancel_flag;}
static bool read_pico_sd_io_ready(void){return true;}
static bool book_weread_cover(const char *uri,uint8_t *out,size_t cap){(void)uri;(void)out;(void)cap;return false;}
static esp_err_t weread_cover_cache_path(const char *id,const char *kind,char *out,size_t cap){(void)id;snprintf(out,cap,"%s/cover.%s",root,kind);return ESP_OK;}
static bool weread_cover_is_original(const char *id,const char *url){(void)id;(void)url;return true;}
static esp_err_t wr_mkdirs(const char *path){(void)path;return ESP_OK;}
static esp_err_t wr_write_bytes(const char *path,const void *data,size_t n){FILE *f=fopen(path,"wb");assert(f);bool ok=fwrite(data,1,n,f)==n&&!fclose(f);return ok?ESP_OK:ESP_FAIL;}
static esp_err_t weread_client_download(const char *url,FILE *file,size_t limit,bool(*fn)(void*),void *ctx,weread_download_info_t *info){(void)url;(void)limit;(void)fn;(void)ctx;++downloads;info->status=200;info->bytes=12;uint8_t bytes[12]={255,216,255,1};assert(fwrite(bytes,1,12,file)==12);return ESP_OK;}
static bool book_image_file_grayscale(const char *path,bool png,unsigned w,unsigned h,uint8_t *out){(void)path;(void)png;assert(w==176&&h==240);++decodes;if(decode_ok)memset(out,97,w*h);return decode_ok;}
#include "weread-cover-production.inc"
static void reset_result(void){free(s_result.cover);memset(&s_result,0,sizeof(s_result));}
int main(void){strcpy(root,"/tmp/kiiko-review-negative-XXXXXX");assert(mkdtemp(root));char failed[512],raw[512];weread_cover_cache_path("book","failed",failed,sizeof(failed));weread_cover_cache_path("book","img",raw,sizeof(raw));
 // Valid downloaded source, transient decoder failure: preserve source and mark remote backoff.
 decode_ok=false;assert(cover_job("https://example.test/image")==ESP_ERR_INVALID_RESPONSE&&downloads==1&&decodes==1&&s_result.cover_retry_seconds==300&&!access(raw,F_OK)&&!access(failed,F_OK));reset_result();
 // Recover locally while marker is still fresh: no extra network, valid gray and marker removed.
 decode_ok=true;assert(cover_job(NULL)==ESP_OK&&downloads==1&&decodes==2&&s_result.cover&&!s_result.cover_retry_seconds&&access(failed,F_OK));reset_result();
 // If decode still fails, preserve marker/backoff and avoid the remote request.
 assert(wr_write_bytes(failed,&wall,sizeof(wall))==ESP_OK);decode_ok=false;assert(cover_job(NULL)==ESP_ERR_INVALID_RESPONSE&&downloads==1&&decodes==3&&s_result.cover_retry_seconds==300&&!access(failed,F_OK));reset_result();
 // Explicit plane OOM and cancellation never create failure metadata.
 assert(!unlink(failed));alloc_fail=true;assert(cover_job("https://example.test/image")==ESP_ERR_NO_MEM&&access(failed,F_OK));alloc_fail=false;reset_result();cancel_flag=true;assert(cover_job("https://example.test/image")==ESP_ERR_INVALID_RESPONSE&&access(failed,F_OK));reset_result();puts("PASS: production ESP cover functions recover fresh-failed+valid raw locally without network, retain backoff on continued decode failure, omit markers for explicit OOM/cancel");}
