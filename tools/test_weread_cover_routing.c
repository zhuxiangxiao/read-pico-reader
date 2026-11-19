/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
typedef int esp_err_t;enum {ESP_OK,ESP_FAIL,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_SIZE,ESP_ERR_NOT_FOUND,ESP_ERR_TIMEOUT,ESP_ERR_INVALID_STATE,WR_JOB_COVER};
#define WEREAD_COVER_WIDTH 176
#define WEREAD_COVER_HEIGHT 240
typedef struct {char cover[512];} weread_book_t;
typedef struct {unsigned failures;uint64_t not_before_ms;bool original;} weread_cover_retry_t;
static struct {char id[64],value[512];}s_work={"book","remote"};static struct {unsigned cover_retry_seconds;}s_result;
static const char *current="http://cdn.qq.com/s_cover?token=a%2Bb";static char recorded[512];static unsigned requests;static bool missing,cancelled_flag;
static bool weread_shelf_find(const char *id,weread_book_t *out){assert(!strcmp(id,"book"));if(missing)return false;snprintf(out->cover,sizeof(out->cover),"%s",current);return true;}
static int cover_job(const char *url){if(cancelled_flag)return ESP_ERR_INVALID_STATE;++requests;snprintf(recorded,sizeof(recorded),"%s",url?url:"");return ESP_OK;}
static bool cancelled(void *ctx){(void)ctx;return cancelled_flag;}
static int weread_cover_cache_path(const char *id,const char *kind,char *out,size_t n){(void)id;(void)kind;(void)out;(void)n;return ESP_ERR_NOT_FOUND;}
#include "cover-routing.inc"
int main(void){assert(dispatch()==ESP_OK&&requests==1&&!strcmp(recorded,"https://cdn.qq.com/t6_cover?token=a%2Bb"));weread_cover_retry_t retry={0};weread_cover_retry_result(&retry,404,100);assert(retry.original&&retry.not_before_ms==100);strcpy(s_work.value,"original");assert(dispatch()==ESP_OK&&requests==2&&!strcmp(recorded,"https://cdn.qq.com/s_cover?token=a%2Bb"));
 current="https://cdn.qq.com/s_changed?sig=x";strcpy(s_work.value,"remote");assert(dispatch()==ESP_OK&&requests==3&&!strcmp(recorded,"https://cdn.qq.com/t6_changed?sig=x"));strcpy(s_work.value,"warm-disk");assert(dispatch()==ESP_OK&&requests==4&&!strcmp(recorded,""));strcpy(s_work.value,"remote");missing=true;assert(dispatch()==ESP_ERR_NOT_FOUND&&requests==4);missing=false;cancelled_flag=true;assert(dispatch()==ESP_ERR_INVALID_STATE&&requests==4);puts("PASS: production worker COVER commands fetch current ID metadata, preserve upgraded→404→original URL/query, reject missing owner entry and honour cancel");}
