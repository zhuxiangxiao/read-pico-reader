/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#pragma once
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

static char rescue_root[1024];
static uint8_t current_card[WEREAD_MEDIA_ID_BYTES]={1};
static bool absent_card,internal_fail,erase_fail;
static unsigned internal_writes,internal_erases;
static esp_err_t read_slot(unsigned slot,void *data,size_t bytes,void *ctx){
    (void)ctx;char path[1200];snprintf(path,sizeof(path),"%s/s%u",rescue_root,slot);
    FILE *file=fopen(path,"rb");if(!file)return errno==ENOENT?ESP_ERR_NOT_FOUND:ESP_FAIL;
    bool valid=fread(data,1,bytes,file)==bytes&&fgetc(file)==EOF;assert(!fclose(file));return valid?ESP_OK:ESP_ERR_INVALID_SIZE;
}
static esp_err_t write_slot(unsigned slot,const void *data,size_t bytes,void *ctx){
    (void)ctx;if(internal_fail)return ESP_FAIL;
    char path[1200],tmp[1210];snprintf(path,sizeof(path),"%s/s%u",rescue_root,slot);snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    FILE *file=fopen(tmp,"wb");if(!file)return ESP_FAIL;
    assert(fwrite(data,1,bytes,file)==bytes&&!fflush(file)&&!fsync(fileno(file))&&!fclose(file));
    assert(!rename(tmp,path));++internal_writes;return ESP_OK;
}
static esp_err_t erase_slot(unsigned slot,void *ctx){
    (void)ctx;if(erase_fail)return ESP_FAIL;
    char path[1200];snprintf(path,sizeof(path),"%s/s%u",rescue_root,slot);++internal_erases;
    return unlink(path)==0?ESP_OK:(errno==ENOENT?ESP_ERR_NOT_FOUND:ESP_FAIL);
}
static bool fixture_card(uint8_t out[WEREAD_MEDIA_ID_BYTES],void *ctx){
    (void)ctx;memcpy(out,current_card,sizeof(current_card));return !absent_card;
}
