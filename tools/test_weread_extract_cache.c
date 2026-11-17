/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

#define wr_mkdirs review_mkdirs
#include "weread_store.h"
#include "weread_chapter.c"
#include <assert.h>
#undef wr_mkdirs
extern esp_err_t wr_mkdirs(const char *path);
static unsigned review_mkdir_calls;
esp_err_t review_mkdirs(const char *path){++review_mkdir_calls;return wr_mkdirs(path);}
static bool flag;
static bool check(void *ctx){(void)ctx;return flag;}
int main(void){char dir[]="/tmp/kiiko-parent-cache-XXXXXX";assert(mkdtemp(dir));extract_t *x=calloc(1,sizeof(*x));assert(x);strcpy(x->root,dir);weread_chapter_set_cancel(check,NULL);const uint8_t data[]={1,2,3};
 for(unsigned i=0;i<100;i++){char name[64];snprintf(name,sizeof(name),"Text/f%u.xhtml",i);assert(extract(x,name,data,sizeof(data),0,sizeof(data)));}assert(review_mkdir_calls==1);printf("same_parent_files=100 mkdir_calls=%u context_bytes=%zu\n",review_mkdir_calls,sizeof(*x));
 assert(extract(x,"Images/a.png",data,3,0,3)&&review_mkdir_calls==2);assert(extract(x,"Text/chunked.xhtml",data,1,0,3)&&review_mkdir_calls==3);assert(extract(x,"Text/chunked.xhtml",data+1,2,1,3)&&!x->file&&review_mkdir_calls==3);
 assert(!extract(x,"Text/f0.xhtml",data,3,0,3)&&x->error==ESP_ERR_INVALID_RESPONSE);x->error=ESP_OK;assert(!extract(x,"../../escape",data,3,0,3));assert(!extract(x,"body.html",data,3,0,3));assert(!extract(x,"position.map",data,3,0,3));assert(!extract(x,"C:escape",data,3,0,3));
 flag=true;unsigned before=review_mkdir_calls;assert(!extract(x,"new/cancel",data,3,0,3)&&before==review_mkdir_calls);flag=false;
 assert(extract(x,"Images/block",data,3,0,3));assert(!extract(x,"Images/block/evil",data,3,0,3)&&!x->parent[0]);x->error=ESP_OK;assert(extract(x,"Text/after-failure",data,3,0,3));
 char file[1024];snprintf(file,sizeof(file),"%s/Text/f0.xhtml",dir);char *read=NULL;size_t n=0;assert(weread_chapter_read_validated(file,&read,&n)==ESP_OK&&n==3&&!memcmp(read,data,3));free(read);
 FILE *large=tmpfile();assert(large&&!ftruncate(fileno(large),CHAPTER_LIMIT+1));char largepath[1024];snprintf(largepath,sizeof(largepath),"%s/oversize",dir);FILE *named=fopen(largepath,"wb");assert(named&&!ftruncate(fileno(named),CHAPTER_LIMIT+1)&&!fclose(named));assert(weread_chapter_read_validated(largepath,&read,&n)==ESP_ERR_INVALID_SIZE&&!read&&!n);fclose(large);free(x);puts("PASS: production extraction parent cache preserves chunk data, duplicate/path/reserved-name rejection, cancel and conflict recovery; validated read retains size checks");}
