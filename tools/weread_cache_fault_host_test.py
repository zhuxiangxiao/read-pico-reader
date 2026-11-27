#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Extract production CSS/version functions for bounded host fault-injection tests."""
from pathlib import Path
import os,re,subprocess
root=next(p for p in Path(__file__).resolve().parents if (p/'main/weread').is_dir() and (p/'tools/weread_stubs').is_dir())
out=root/'build/weread-tests/cache-regression';out.mkdir(parents=True,exist_ok=True)
def function(path,name):
    s=(root/path).read_text();m=re.search(r'^.*\b'+re.escape(name)+r'\([^;]*?\)\s*\{',s,re.M);assert m,name
    start=m.start();p=m.end();depth=1;q=None
    while depth:
        c=s[p]
        if q:
            if c=='\\':p+=2;continue
            if c==q:q=None
        elif s.startswith('//',p):p=s.index('\n',p);continue
        elif s.startswith('/*',p):p=s.index('*/',p+2)+2;continue
        elif c in '\"\'':q=c
        elif c=='{':depth+=1
        elif c=='}':depth-=1
        p+=1
    return s[start:p]+'\n'
source=(root/'main/book/book_source_weread.c').read_text()
state=re.search(r'^typedef struct .* css_slot_t;\nstatic struct .* s_css;',source,re.M).group(0)
(out/'css.inc').write_text(state+'\n'+''.join(function(Path('main/book/book_source_weread.c'),n) for n in ['clear_css','collect_css','css_slot_load','chapter_css']))
(out/'shelf.inc').write_text(''.join(function(Path('main/weread/weread_store.c'),n) for n in ['wr_string','wr_uint','wr_valid_id','wr_copy'])+''.join(function(Path('main/weread/weread_shelf.c'),n) for n in ['books','item_id','find','number','weread_shelf_set_version']))
(out/'test.c').write_text(r'''
#include "weread_store.h"
#include "weread_shelf.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#undef WR_GUARD
#define WR_GUARD
static char root_dir[512],s_id[]="test_book";
static unsigned fault,dir_calls,big_allocs,persist_calls;
static bool fail_persist;
static cJSON*s_shelf;
static void *deny_alloc(size_t n){(void)n;return NULL;}
void *wr_alloc(size_t n){if(n==256u*1024u)++big_allocs;return calloc(1,n);}
void *wr_realloc(void*p,size_t n){return realloc(p,n);}
void wr_release(void*slot){void*p=NULL;memcpy(&p,slot,sizeof(p));free(p);memset(slot,0,sizeof(p));}
bool wr_owner_matches(const cJSON*j){(void)j;return true;}
static esp_err_t persist(cJSON*j){(void)j;++persist_calls;if(fail_persist){cJSON_Hooks h={.malloc_fn=deny_alloc,.free_fn=free};cJSON_InitHooks(&h);return ESP_ERR_NO_MEM;}return ESP_OK;}
static uint32_t book_weread_uid(size_t i){(void)i;return 1;}
static const char*bound_resource(const char*root){(void)root;return NULL;}
esp_err_t weread_chapter_directory(const char*id,uint32_t uid,char*out,size_t cap){(void)id;(void)uid;if(fault==9){fault=0;return ESP_FAIL;}snprintf(out,cap,"%s",root_dir);return ESP_OK;}
esp_err_t weread_chapter_resource_directory(const char*id,char*out,size_t cap){(void)id;if(fault==8){fault=0;return ESP_ERR_NO_MEM;}snprintf(out,cap,"%s",root_dir);return ESP_OK;}
static bool consume(unsigned kind){if(fault!=kind)return false;fault=0;errno=EIO;return true;}
static DIR*test_opendir(const char*p){++dir_calls;if(consume(1))return NULL;return opendir(p);}
static struct dirent*test_readdir(DIR*d){if(consume(2))return NULL;return readdir(d);}
static int test_lstat(const char*p,struct stat*s){if(consume(3))return -1;return lstat(p,s);}
static FILE*test_fopen(const char*p,const char*m){if(consume(4))return NULL;return fopen(p,m);}
static size_t test_fread(void*p,size_t n,size_t c,FILE*f){size_t got=fread(p,n,c,f);return consume(5)&&got?got-1:got;}
static int test_ferror(FILE*f){if(consume(6))return 1;return ferror(f);}
static int test_fclose(FILE*f){int r=fclose(f);return consume(7)?EOF:r;}
#define opendir test_opendir
#define readdir test_readdir
#define lstat test_lstat
#define fopen test_fopen
#define fread test_fread
#define ferror test_ferror
#define fclose test_fclose
#include "css.inc"
#undef opendir
#undef readdir
#undef lstat
#undef fopen
#undef fread
#undef ferror
#undef fclose
#include "shelf.inc"
static unsigned version(void){return(unsigned)number(find(cJSON_GetObjectItem(s_shelf,"books"),"book"),"bookVersion");}
int main(void){
    char temp[]="/tmp/weread-css-regression-XXXXXX";assert(mkdtemp(temp));snprintf(root_dir,sizeof(root_dir),"%s",temp);
    char path[600];snprintf(path,sizeof(path),"%s/main.css",temp);FILE*f=fopen(path,"wb");assert(f);assert(fputs("p {text-align:right}",f)>=0);assert(!fclose(f));
    for(unsigned kind=1;kind<=9;kind++){
        clear_css();fault=kind;dir_calls=0;size_t used=123;assert(chapter_css(0,NULL,&used)==NULL);assert(!s_css.res.root&&!s_css.res.text&&!s_css.ch.root&&!s_css.ch.text&&!s_css.merged);
        dir_calls=0;const char*css=chapter_css(0,NULL,&used);assert(css&&used&&strstr(css,"text-align:right")&&dir_calls);
        dir_calls=0;assert(chapter_css(0,NULL,&used)==css&&!dir_calls);
    }
    clear_css();assert(!unlink(path));big_allocs=0;size_t used=123;assert(chapter_css(0,NULL,&used)&&!used&&!big_allocs);clear_css();assert(!rmdir(temp));
    s_shelf=cJSON_Parse("{\"books\":[{\"bookId\":\"book\",\"bookVersion\":1}]}");assert(s_shelf);
    fail_persist=true;assert(weread_shelf_set_version("book",2)==ESP_ERR_NO_MEM);cJSON_InitHooks(NULL);assert(version()==1);
    cJSON_Hooks h={.malloc_fn=deny_alloc,.free_fn=free};cJSON_InitHooks(&h);assert(weread_shelf_set_version("book",2)==ESP_ERR_NO_MEM);cJSON_InitHooks(NULL);assert(version()==1);
    fail_persist=false;persist_calls=0;assert(weread_shelf_set_version("book",1)==ESP_OK&&!persist_calls);
    assert(weread_shelf_set_version("book",2)==ESP_OK&&version()==2&&persist_calls==1);cJSON_Delete(s_shelf);
    puts("CSS: nine transient metadata/IO failures retry; success reuses cache; empty CSS stays small.");
    puts("Version: persistent OOM and creation OOM retain old value; unchanged avoids persistence.");return 0;
}
''')
args=[os.environ.get('CC','cc'),'-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Imain/weread','-Itools/weread_stubs','-I'+str(out),str(out/'test.c'),'tools/weread_stubs/cJSON.c','-lm','-o',str(out/'test')]
subprocess.run(args,cwd=root,check=True);subprocess.run([str(out/'test')],cwd=root,check=True)
