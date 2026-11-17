/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书目录快照与离线章节适配；网络只在书源加载回调中执行。
 * English: Adapt WeRead TOC snapshots/offline chapters; networking belongs to source load callbacks.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "book_source_weread.h"
#include "book_weread_uri.h"
#include "weread_auth.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_store.h"
#include "weread_position.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(ESP_PLATFORM)
// TF 为 FAT，无符号链接。/ TF uses FAT and has no symbolic links.
#define lstat stat
#endif

static char s_id[64];
static weread_toc_t s_toc;
static bool s_txt;
static char s_prepared_id[64],s_prepared_owner[32];
static weread_toc_t s_prepared_toc;
typedef struct {
    char root[512],resources[512],owner[32],storage[256];
    uint32_t uid,version;
    size_t index,new_index;
    weread_toc_t toc;
    weread_position_point_t *map;
    size_t map_count,map_capacity;
    uint32_t text_bytes,source_units;
    bool map_ready;
} loaded_chapter_t;
static loaded_chapter_t *s_loaded,*s_candidate;
static bool binding_owned(const loaded_chapter_t *b){
    const weread_auth_session_t *s=weread_auth_get_current_session();
    return b&&s&&!strcmp(b->owner,s->vid)&&!strcmp(b->storage,weread_auth_get_storage_root());
}
static void binding_free(loaded_chapter_t **b){
    if(*b){weread_toc_free(&(*b)->toc);free((*b)->map);free(*b);*b=NULL;}
}
size_t book_weread_map_cache_bytes(void){WR_GUARD;return ((s_loaded?s_loaded->map_capacity:0)+(s_candidate?s_candidate->map_capacity:0))*sizeof(weread_position_point_t);}
void book_weread_discard_loaded(void){WR_GUARD;binding_free(&s_candidate);}
bool book_weread_directory_in_use(const char *root){WR_GUARD;
    return root&&((s_loaded&&(!strcmp(root,s_loaded->root)||!strcmp(root,s_loaded->resources)))||
        (s_candidate&&(!strcmp(root,s_candidate->root)||!strcmp(root,s_candidate->resources))));
}
static const char *bound_resource(const char *root){
    if(root&&binding_owned(s_candidate)&&!strcmp(root,s_candidate->root))return s_candidate->resources;
    if(root&&binding_owned(s_loaded)&&!strcmp(root,s_loaded->root))return s_loaded->resources;
    return NULL;
}
static struct {char *text,*roots;size_t len;} s_css;
static void clear_css(void){free(s_css.text);free(s_css.roots);memset(&s_css,0,sizeof(s_css));}
void book_weread_clear_prepared(void){WR_GUARD;weread_toc_free(&s_prepared_toc);s_prepared_id[0]=s_prepared_owner[0]=0;}
bool book_weread_prepared(const char *id){WR_GUARD;
    const weread_auth_session_t *s=weread_auth_get_current_session();
    return s&&id&&s_prepared_toc.count&&!strcmp(id,s_prepared_id)&&!strcmp(s->vid,s_prepared_owner);
}
bool book_weread_cached_version(const char *id,uint32_t *version){WR_GUARD;
    if(!id||!version)return false;
    if(book_weread_prepared(id)){*version=s_prepared_toc.book_version;return true;}
    if(s_id[0]&&!strcmp(s_id,id)){*version=s_loaded?s_loaded->version:s_toc.book_version;return true;}
    return false;
}
esp_err_t book_weread_prepare_open(const char *id){WR_GUARD;
    uint32_t known=weread_shelf_book_version(id);
    if(book_weread_prepared(id)&&(!known||known==s_prepared_toc.book_version))return ESP_OK;
    book_weread_clear_prepared();
    if(!wr_valid_id(id)||!weread_auth_is_authenticated())return ESP_ERR_INVALID_STATE;
    esp_err_t err=weread_toc_load(id,&s_prepared_toc);
    if(err!=ESP_OK){err=weread_toc_sync(id);if(err==ESP_OK)err=weread_toc_load(id,&s_prepared_toc);}
    if(err==ESP_OK&&!s_prepared_toc.count)err=ESP_ERR_NOT_FOUND;
    if(err==ESP_OK&&known&&s_prepared_toc.book_version!=known)err=WEREAD_ERR_VERSION;
    if(err!=ESP_OK){book_weread_clear_prepared();return err;}
    strcpy(s_prepared_id,id);strcpy(s_prepared_owner,weread_auth_get_current_session()->vid);return ESP_OK;
}
void book_weread_adopt_prepared(const char *id,weread_toc_t *toc){WR_GUARD;
    book_weread_clear_prepared();s_prepared_toc=*toc;memset(toc,0,sizeof(*toc));
    strcpy(s_prepared_id,id);strcpy(s_prepared_owner,weread_auth_get_current_session()->vid);
}
esp_err_t book_weread_validate_prepared(const char *id,uint32_t uid,uint32_t index,uint32_t offset,uint32_t *version){WR_GUARD;
    bool prepared=book_weread_prepared(id),active=id&&!strcmp(s_id,id)&&binding_owned(s_loaded)&&s_loaded->uid==uid;
    if(!version||(!prepared&&!active))return ESP_ERR_INVALID_STATE;
    const weread_toc_t *toc=prepared?&s_prepared_toc:&s_toc;
    uint32_t known=weread_shelf_book_version(id);
    if(known&&known!=toc->book_version)return WEREAD_ERR_VERSION;
    size_t i=0;
    while(i<toc->count&&toc->chapters[i].uid!=uid)++i;
    if(i==toc->count||(index&&index!=toc->chapters[i].index))return WEREAD_ERR_POSITION;
    char *path __attribute__((cleanup(wr_release)))=wr_alloc(1100);
    if(!path)return ESP_ERR_NO_MEM;
    esp_err_t err=active&&!prepared?(wr_copy(path,1024,s_loaded->root)?ESP_OK:ESP_ERR_INVALID_SIZE):weread_chapter_directory(id,uid,path,1024);
    if(err!=ESP_OK)return err;
    strcat(path,"/position.map");FILE *file=fopen(path,"rb");
    if(!file)return ESP_ERR_NOT_FOUND;
    weread_position_header_t header={0};bool ok=fread(&header,sizeof(header),1,file)==1;
    ok=ok&&header.magic==WEREAD_POSITION_MAGIC&&header.bytes&&header.count&&header.count<=16384;
    weread_position_point_t point={0},previous={0};
    for(uint32_t n=0;ok&&n<header.count;++n){
        if(!(n%128)&&weread_client_cancelled()){fclose(file);return ESP_ERR_INVALID_STATE;}
        ok=fread(&point,sizeof(point),1,file)==1&&point.byte<=header.bytes&&point.unit<=header.units&&
            (!n||(point.byte>=previous.byte&&point.unit>=previous.unit));
        previous=point;
    }
    ok=ok&&fgetc(file)==EOF&&!ferror(file);fclose(file);
    strcpy(strrchr(path,'/')+1,"body.html");struct stat st;
    if(!ok||stat(path,&st)||!S_ISREG(st.st_mode)||st.st_size!=(off_t)header.bytes)return ESP_ERR_INVALID_RESPONSE;
    if(offset>header.units)return WEREAD_ERR_POSITION;
    // 只有章节UID、目录序号及原文范围均一致，才把显式开书候选绑定最终版本。
    // Rebind a new-open choice only after UID, ordinal and original-source bounds agree.
    *version=toc->book_version;return ESP_OK;
}

static uint32_t content_identity(const char *owner,const char *id,uint32_t version) {
    uint32_t identity = 2166136261u;
    const char *keys[] = {owner,id};
    for (unsigned k = 0; k < 2; ++k) {
        for (const unsigned char *p = (const unsigned char *)keys[k]; *p; ++p) identity = (identity ^ *p) * 16777619u;
        identity = (identity ^ 0xffu) * 16777619u;
    }
    identity = (identity ^ version) * 16777619u;
    identity &= INT32_MAX;
    return identity ? identity : 1;
}
bool book_weread_content_identity(const char *uri,uint32_t *identity) { WR_GUARD;
    if (!identity) return false;
    *identity = 0;
    if (!book_path_is_weread(uri) || strcmp(uri + 9,s_id) || !binding_owned(s_loaded)) return false;
    *identity = content_identity(s_loaded->owner,s_id,s_loaded->version);
    return true;
}
bool book_weread_stat(const char *uri, struct stat *out) { WR_GUARD;
    if (!book_path_is_weread(uri) || !out) return false;
    weread_book_t book;
    bool metadata = weread_shelf_find(uri + 9,&book);
    uint32_t identity = 0;
    // 书签跟随已显示正文；后台更新元数据不能提前替换阅读中的NVS身份。
    // Bookmarks follow visible content; background metadata must not relabel the reader's NVS identity.
    if (!book_weread_content_identity(uri,&identity)) {
        if (!metadata) return false;
        const weread_auth_session_t *session = weread_auth_get_current_session();
        identity = content_identity(session ? session->vid : "",uri + 9,book.book_version);
    }
    memset(out,0,sizeof(*out));
    out->st_mode = S_IFREG | 0400;
    out->st_size = identity;
    out->st_mtime = metadata ? (time_t)book.update_time : 0;
    return true;
}
bool book_weread_is_open(void) { return s_id[0] != 0; }
bool book_weread_plain_text(void) { WR_GUARD; return s_id[0] && s_txt; }
void book_weread_close(void) { WR_GUARD; binding_free(&s_loaded);binding_free(&s_candidate);clear_css();s_id[0] = 0; s_txt = false; weread_toc_free(&s_toc); }
esp_err_t book_weread_open(const char *uri) { WR_GUARD;
    if (!book_path_is_weread(uri) || strlen(uri + 9) >= sizeof(s_id)) return ESP_ERR_INVALID_ARG;
    if (!weread_auth_is_authenticated()) {
        esp_err_t err = weread_auth_init(NULL); if (err != ESP_OK) return err;
    }
    if (!weread_shelf_count()) (void)weread_shelf_load();
    weread_book_t b;
    if (!weread_shelf_find(uri + 9, &b)) return ESP_ERR_NOT_FOUND;
    weread_toc_t toc = {0};
    esp_err_t err=ESP_OK;
    if(book_weread_prepared(uri+9)){toc=s_prepared_toc;memset(&s_prepared_toc,0,sizeof(s_prepared_toc));s_prepared_id[0]=s_prepared_owner[0]=0;}
    else err=weread_toc_load(uri+9,&toc);
    if (err != ESP_OK) {
        err = weread_toc_sync(uri + 9);
        if (err == ESP_OK) err = weread_toc_load(uri + 9, &toc);
    }
    if (err != ESP_OK) return err;
    uint32_t known=weread_shelf_book_version(uri+9);
    if(known&&toc.book_version!=known){weread_toc_free(&toc);return WEREAD_ERR_VERSION;}
    if (!toc.count) { weread_toc_free(&toc); return ESP_ERR_NOT_FOUND; }
    book_weread_close(); s_toc = toc;
    strcpy(s_id, uri + 9); s_txt = !strcasecmp(b.format, "txt");
    return ESP_OK;
}
size_t book_weread_count(void) { return s_toc.count; }
uint32_t book_weread_uid(size_t i) { return i < s_toc.count ? s_toc.chapters[i].uid : 0; }
uint32_t book_weread_chapter_index(size_t i) { return i < s_toc.count ? s_toc.chapters[i].index : 0; }
size_t book_weread_index(uint32_t uid) {
    for (size_t i = 0; i < s_toc.count; ++i) if (s_toc.chapters[i].uid == uid) return i;
    return SIZE_MAX;
}
esp_err_t book_weread_title(size_t i, char *out, size_t cap) {
    if (i >= s_toc.count || !out || !cap) return ESP_ERR_INVALID_ARG;
    const char *title=s_toc.chapters[i].title;
    if(binding_owned(s_candidate)&&s_candidate->index==i&&s_candidate->toc.count)title=s_candidate->toc.chapters[s_candidate->new_index].title;
    size_t n = strlen(title);
    if (n >= cap) return ESP_ERR_INVALID_SIZE;
    memcpy(out, title, n + 1); return ESP_OK;
}
uint32_t book_weread_offset(size_t index) {
    uint64_t n = 0;
    for (size_t i = 0; i < index && i < s_toc.count; ++i)
        n += s_toc.chapters[i].word_count ? (uint64_t)s_toc.chapters[i].word_count * 3 : 1;
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}
uint32_t book_weread_total(void) { return book_weread_offset(s_toc.count); }

static bool collect_css(const char *dir, char **css, size_t *used, size_t cap, unsigned depth) {
    if (depth > 8) return true;
    DIR *d = opendir(dir); if (!d) return false;
    char *path __attribute__((cleanup(wr_release)))=wr_alloc(1024);
    if(!path){closedir(d);return false;}
    bool ok=true;
    struct dirent *e;
    for(;;){
        errno=0;e=readdir(d);if(!e){if(errno)ok=false;break;}
        if (*e->d_name == '.') continue;
        int n = snprintf(path, 1024, "%s/%s", dir, e->d_name);
        if (n < 0 || n >= 1024) continue;
        struct stat st; if (lstat(path, &st)) {ok=false;break;}
        if (S_ISDIR(st.st_mode)) {if(!collect_css(path,css,used,cap,depth+1)){ok=false;break;}continue;}
        const char *ext = strrchr(e->d_name, '.');
        if (!S_ISREG(st.st_mode) || !ext || strcasecmp(ext, ".css") || st.st_size < 0 || (uint64_t)st.st_size + 2 > cap - *used) continue;
        if(!st.st_size)continue;
        if(!*css){*css=wr_alloc(cap);if(!*css){ok=false;break;}}
        FILE *f = fopen(path, "rb"); if (!f) {ok=false;break;}
        size_t len = (size_t)st.st_size;
        if (fread(*css + *used, 1, len, f) == len&&!ferror(f)) { *used += len; (*css)[(*used)++] = '\n'; }
        else ok=false;
        if(fclose(f))ok=false;
        if(!ok)break;
    }
    closedir(d); if(*css)(*css)[*used] = 0;return ok;
}
static const char *chapter_css(size_t i,const char *validated_root,size_t *used) {
    const size_t cap = 256u * 1024u;
    *used = 0;char resource[512]="", chapter[512]="";
    const char *bound=bound_resource(validated_root);
    esp_err_t res=bound?(wr_copy(resource,sizeof(resource),bound)?(*bound?ESP_OK:ESP_ERR_NOT_FOUND):ESP_ERR_INVALID_SIZE):weread_chapter_resource_directory(s_id,resource,sizeof(resource));
    esp_err_t ch=validated_root?(wr_copy(chapter,sizeof(chapter),validated_root)?ESP_OK:ESP_ERR_INVALID_SIZE):weread_chapter_directory(s_id,book_weread_uid(i),chapter,sizeof(chapter));
    if((res!=ESP_OK&&res!=ESP_ERR_NOT_FOUND)||ch!=ESP_OK)return NULL;
    bool have_res=res==ESP_OK,have_ch=ch==ESP_OK;
    if(!have_res)resource[0]=0;
    if(!have_ch)chapter[0]=0;
    if(s_css.roots&&!strcmp(s_css.roots,resource)&&!strcmp(s_css.roots+strlen(s_css.roots)+1,chapter)){*used=s_css.len;return s_css.text;}
    // 缓存只保留当前资源代次的CSS，避免每次映射遍历全部图片。/ Keep only the current resource generation's CSS, avoiding image-directory scans on each mapping.
    clear_css();char *css=NULL;
    if ((have_res&&!collect_css(resource,&css,used,cap,0)) ||
        (have_ch&&(!have_res||strcmp(resource,chapter))&&!collect_css(chapter,&css,used,cap,0))) {free(css);return NULL;}
    if(!css)css=wr_alloc(1);
    if(!css)return NULL;
    size_t rlen=strlen(resource)+1,clen=strlen(chapter)+1;
    s_css.roots=wr_alloc(rlen+clen);if(!s_css.roots){free(css);return NULL;}
    memcpy(s_css.roots,resource,rlen);memcpy(s_css.roots+rlen,chapter,clen);
    char *small=wr_realloc(css,*used+1);s_css.text=small?small:css;s_css.len=*used;
    return s_css.text;
}
typedef struct {
    loaded_chapter_t *binding;
    const char *html;
    const weread_position_point_t *source;
    size_t count,next,cursor,bytes;
    uint32_t units;
    bool complete;
} map_cache_writer_t;
static void cache_character(void *ctx,size_t text_offset,size_t html_offset){
    map_cache_writer_t *w=ctx;loaded_chapter_t *b=w->binding;
    if(!w->complete)return;
    if(b->map_count==8192||text_offset>UINT32_MAX||html_offset>w->bytes||html_offset<w->cursor){w->complete=false;return;}
    while(w->next<w->count&&w->source[w->next].byte<=html_offset){w->cursor=w->source[w->next].byte;w->units=w->source[w->next].unit;++w->next;}
    w->units+=weread_utf16_length(w->html+w->cursor,html_offset-w->cursor);w->cursor=html_offset;
    if(b->map_count&&(b->map[b->map_count-1].byte>text_offset||b->map[b->map_count-1].unit>w->units)){w->complete=false;return;}
    b->map[b->map_count++]=(weread_position_point_t){(uint32_t)text_offset,w->units};
}
static esp_err_t parse_loaded(loaded_chapter_t *b,const char *text,size_t len,const char *css,size_t css_len,const char *anchor,size_t *offset,html_text_t *out){
    if(anchor&&!s_txt)return html_to_blocks_with_css_anchor(text,len,css,css_len,anchor,offset,out);
    b->map=wr_alloc(8192u*sizeof(*b->map));
    if(!b->map){if(s_txt){out->utf8=(char*)text;out->len=len;return ESP_OK;}return html_to_blocks_with_css_anchor(text,len,css,css_len,NULL,offset,out);}
    b->map_capacity=8192;
    char path[600];snprintf(path,sizeof(path),"%s/position.map",b->root);FILE *f=fopen(path,"rb");
    weread_position_header_t h={0};
    bool valid=f&&fread(&h,sizeof(h),1,f)==1&&h.magic==WEREAD_POSITION_MAGIC&&h.count&&h.count<=16384&&h.bytes==len;
    weread_position_point_t *points=valid?wr_alloc((size_t)h.count*sizeof(*points)):NULL;
    if(valid&&!points){fclose(f);free(b->map);b->map=NULL;b->map_capacity=0;if(s_txt){out->utf8=(char*)text;out->len=len;return ESP_OK;}return html_to_blocks_with_css_anchor(text,len,css,css_len,NULL,offset,out);}
    valid=valid&&fread(points,sizeof(*points),h.count,f)==h.count&&fgetc(f)==EOF&&!ferror(f);
    if(f&&fclose(f))valid=false;
    for(uint32_t i=0;valid&&i<h.count;++i)valid=points[i].byte<=h.bytes&&points[i].unit<=h.units&&
        (!i||(points[i].byte>=points[i-1].byte&&points[i].unit>=points[i-1].unit));
    if(!valid){free(points);free(b->map);b->map=NULL;b->map_capacity=0;return ESP_ERR_INVALID_RESPONSE;}
    map_cache_writer_t writer={.binding=b,.html=text,.source=points,.count=h.count,.bytes=len,.complete=true};
    esp_err_t err=ESP_OK;
    if(s_txt){
        out->utf8=(char*)text;out->len=len;
        for(size_t at=0;at<len&&writer.complete;++at)if(((unsigned char)text[at]&0xc0)!=0x80)cache_character(&writer,at,at);
    }else err=html_to_blocks_mapped(text,len,css,css_len,cache_character,&writer,out);
    free(points);
    if(err==ESP_OK&&writer.complete&&(!b->map_count||b->map[b->map_count-1].byte<=out->len)){
        b->map_ready=true;b->source_units=h.units;b->text_bytes=(uint32_t)out->len;
        if(b->map_count){void *small=wr_realloc(b->map,b->map_count*sizeof(*b->map));if(small){b->map=small;b->map_capacity=b->map_count;}}
        else{free(b->map);b->map=NULL;b->map_capacity=0;}
    }else{free(b->map);b->map=NULL;b->map_count=b->map_capacity=0;}
    return err;
}
esp_err_t book_weread_load(size_t i, const char *anchor, size_t *offset, html_text_t *out) { WR_GUARD;
    if (!out || i >= s_toc.count) return ESP_ERR_INVALID_ARG;
    book_weread_discard_loaded();
    memset(out, 0, sizeof(*out)); if (offset) *offset = 0;
    loaded_chapter_t *candidate=wr_alloc(sizeof(*candidate));if(!candidate)return ESP_ERR_NO_MEM;
    candidate->uid=book_weread_uid(i);candidate->index=candidate->new_index=i;
    const weread_auth_session_t *owner=weread_auth_get_current_session();
    if(!owner||!wr_copy(candidate->owner,sizeof(candidate->owner),owner->vid)||!wr_copy(candidate->storage,sizeof(candidate->storage),weread_auth_get_storage_root())){binding_free(&candidate);return ESP_ERR_INVALID_STATE;}
    char *text = NULL; size_t len = 0;
    esp_err_t err=weread_chapter_snapshot(s_id,candidate->uid,candidate->root,sizeof(candidate->root),candidate->resources,sizeof(candidate->resources),&candidate->version);
    // 同一不可变正文根继续沿用已经呈现的公共资源，避免同版重建资源偷换旧排版身份。
    // Reuse the presented resource generation for an unchanged immutable body root.
    if(err==ESP_OK&&binding_owned(s_loaded)&&candidate->version==s_loaded->version&&!strcmp(candidate->root,s_loaded->root))
        wr_copy(candidate->resources,sizeof(candidate->resources),s_loaded->resources);
    if(err==ESP_OK&&candidate->version!=s_toc.book_version){
        err=weread_toc_load(s_id,&candidate->toc);
        if(err==ESP_OK&&candidate->toc.book_version!=candidate->version)err=WEREAD_ERR_VERSION;
        if(err==ESP_OK){
            size_t n=0;while(n<candidate->toc.count&&candidate->toc.chapters[n].uid!=candidate->uid)++n;
            if(n==candidate->toc.count)err=WEREAD_ERR_VERSION;else candidate->new_index=n;
        }
    }
    char path[600];snprintf(path,sizeof(path),"%s/body.html",candidate->root);
    if(err==ESP_OK)err=weread_chapter_read_validated(path,&text,&len);
    if(err==ESP_OK&&!len){free(text);text=NULL;err=ESP_ERR_INVALID_RESPONSE;}
    if(err!=ESP_OK){binding_free(&candidate);return err;}
    s_candidate=candidate;
    size_t used = 0; const char *css = s_txt?"":chapter_css(i,candidate->root,&used);
    if (!css) { free(text);book_weread_discard_loaded();return ESP_ERR_NO_MEM; }
    err = parse_loaded(candidate,text,len,css,used,anchor,offset,out);
    if(!s_txt||err!=ESP_OK)free(text);
    if(err!=ESP_OK)book_weread_discard_loaded();
    return err;
}
esp_err_t book_weread_commit_loaded(size_t *chapter,uint32_t *v){WR_GUARD;
    if(!chapter||!v||!binding_owned(s_candidate)||*chapter!=s_candidate->index)return ESP_ERR_INVALID_STATE;
    if(s_candidate->toc.count){weread_toc_free(&s_toc);s_toc=s_candidate->toc;memset(&s_candidate->toc,0,sizeof(s_candidate->toc));}
    *chapter=s_candidate->new_index;*v=s_candidate->version;s_candidate->index=*chapter;
    binding_free(&s_loaded);s_loaded=s_candidate;s_candidate=NULL;
    if(!strcmp(s_prepared_id,s_id))book_weread_clear_prepared();
    return ESP_OK;
}
typedef struct {
    const char *html;
    weread_position_point_t *points;
    size_t count, next, cursor;
    uint32_t unit, goal, result;
    bool to_cloud, found;
} position_mapper_t;
static void mapped_character(void *ctx, size_t text_offset, size_t html_offset) {
    position_mapper_t *m = ctx;
    if (m->found) return;
    while (m->next < m->count && m->points[m->next].byte <= html_offset) {
        m->cursor = m->points[m->next].byte; m->unit = m->points[m->next].unit; ++m->next;
    }
    m->unit += weread_utf16_length(m->html + m->cursor, html_offset - m->cursor); m->cursor = html_offset;
    if (m->to_cloud ? text_offset >= m->goal : m->unit >= m->goal) {
        m->result = m->to_cloud ? m->unit : (uint32_t)text_offset; m->found = true;
    }
}
static esp_err_t map_dense(const loaded_chapter_t *bound,bool to_cloud,uint32_t input,uint32_t *output) {
    if (!bound || !bound->map_ready || !bound->map_count) return ESP_ERR_NOT_FOUND;
    if (!to_cloud && input > bound->source_units) return WEREAD_ERR_POSITION;
    size_t lo = 0,hi = bound->map_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t key = to_cloud ? bound->map[mid].byte : bound->map[mid].unit;
        if (key < input) lo = mid + 1;
        else hi = mid;
    }
    *output = lo < bound->map_count ? (to_cloud ? bound->map[lo].unit : bound->map[lo].byte) :
        to_cloud ? bound->source_units : bound->text_bytes;
    return ESP_OK;
}
esp_err_t book_weread_map_cached(size_t i,uint32_t version,bool to_cloud,uint32_t input,uint32_t *output) { WR_GUARD;
    if (!output) return ESP_ERR_INVALID_ARG;
    if (!binding_owned(s_loaded) || s_loaded->index != i || s_loaded->version != version ||
        i >= s_toc.count || s_toc.chapters[i].uid != s_loaded->uid) return ESP_ERR_INVALID_STATE;
    if (!s_loaded->map_ready || !s_loaded->map_count) return ESP_ERR_NOT_FOUND;
    if (input > (to_cloud ? s_loaded->text_bytes : s_loaded->source_units)) return WEREAD_ERR_POSITION;
    return map_dense(s_loaded,to_cloud,input,output);
}
static esp_err_t map_files(const char *root,bool text_source,const char *css,size_t css_len,
    bool to_cloud,uint32_t input,uint32_t *output,bool strict) {
    char *path __attribute__((cleanup(wr_release))) = wr_alloc(1100);
    if (!path) return ESP_ERR_NO_MEM;
    int n = snprintf(path,1100,"%s/position.map",root);
    if (n < 0 || n >= 1100) return ESP_ERR_INVALID_SIZE;
    FILE *file = fopen(path,"rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    weread_position_header_t header = {0};
    bool valid = fread(&header,sizeof(header),1,file) == 1 && header.magic == WEREAD_POSITION_MAGIC &&
        header.count && header.count <= 16384 && header.bytes && header.bytes <= 4u * 1024u * 1024u;
    if (!valid) { fclose(file); return ESP_ERR_INVALID_RESPONSE; }
    if (!to_cloud && input > header.units) { fclose(file); return WEREAD_ERR_POSITION; }
    weread_position_point_t *points = wr_alloc(header.count * sizeof(*points));
    if (!points) { fclose(file); return ESP_ERR_NO_MEM; }
    valid = fread(points,sizeof(*points),header.count,file) == header.count && fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) valid = false;
    valid = valid && points[0].byte == 0 && points[0].unit == 0 &&
        points[header.count-1].byte == header.bytes && points[header.count-1].unit == header.units;
    for (size_t j = 0; j < header.count && valid; ++j) {
        valid = points[j].byte <= header.bytes && points[j].unit <= header.units &&
            (!j || (points[j].byte >= points[j-1].byte && points[j].unit >= points[j-1].unit));
    }
    if (!valid) { free(points); return ESP_ERR_INVALID_RESPONSE; }
    n = snprintf(path,1100,"%s/body.html",root);
    char *text = NULL;size_t len = 0;
    esp_err_t err = n < 0 || n >= 1100 ? ESP_ERR_INVALID_SIZE : weread_chapter_read_validated(path,&text,&len);
    if (err == ESP_OK && len != header.bytes) err = ESP_ERR_INVALID_RESPONSE;
    position_mapper_t mapper = {.html=text,.points=points,.count=header.count,.goal=input,.to_cloud=to_cloud};
    html_text_t parsed = {0};
    if (err == ESP_OK && text_source) {
        for (size_t at = 0; at < len && !mapper.found; ++at) {
            if (((unsigned char)text[at] & 0xc0) != 0x80) mapped_character(&mapper,at,at);
        }
        parsed.len = len;
    } else if (err == ESP_OK) {
        err = html_to_blocks_mapped(text,len,css,css_len,mapped_character,&mapper,&parsed);
    }
    if (err == ESP_OK && strict && to_cloud && input > parsed.len) err = WEREAD_ERR_POSITION;
    if (err == ESP_OK) *output = mapper.found ? mapper.result : to_cloud ? header.units : (uint32_t)parsed.len;
    html_text_free(&parsed);free(text);free(points);
    return err;
}
static esp_err_t map_bound(size_t i,bool to_cloud,uint32_t input,uint32_t *output,const loaded_chapter_t *bound) { WR_GUARD;
    if (!output || i >= s_toc.count) return ESP_ERR_INVALID_ARG;
    esp_err_t cached = map_dense(bound,to_cloud,input,output);
    if (cached != ESP_ERR_NOT_FOUND) return cached;
    char *resolved __attribute__((cleanup(wr_release))) = NULL;
    const char *root = bound ? bound->root : NULL;
    if (!root) {
        resolved = wr_alloc(1024);
        if (!resolved) return ESP_ERR_NO_MEM;
        esp_err_t err = weread_chapter_directory(s_id,book_weread_uid(i),resolved,1024);
        if (err != ESP_OK) return err;
        root = resolved;
    }
    size_t css_len = 0;const char *css = s_txt ? "" : chapter_css(i,root,&css_len);
    return css ? map_files(root,s_txt,css,css_len,to_cloud,input,output,false) : ESP_ERR_NO_MEM;
}
static esp_err_t detached_manifest(const char *id,uint32_t uid,uint32_t version,bool resource,
    char *root,size_t cap,cJSON **manifest) {
    char leaf[64],path[512];
    if (resource) strcpy(leaf,"resources.json");
    else snprintf(leaf,sizeof(leaf),"chapter-%u.json",(unsigned)uid);
    esp_err_t err = wr_cache_path(id,leaf,path,sizeof(path));
    if (err == ESP_OK) err = wr_read_json_primary(path,manifest);
    uint64_t stored_version = 0,stored_uid = 0;
    if (err == ESP_OK && (!wr_owner_matches(*manifest) || !wr_uint(*manifest,"bookVersion",&stored_version) ||
        (!resource && (!wr_uint(*manifest,"chapterUid",&stored_uid) || stored_uid != uid)))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && stored_version != version) err = WEREAD_ERR_VERSION;
    const char *name = wr_string(*manifest,"root");
    if (err == ESP_OK && (!name || !*name || name[0] == '/' || strstr(name,"..") || strchr(name,'\\'))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) err = wr_cache_path(id,name,root,cap);
    struct stat st;
    if (err == ESP_OK && (stat(root,&st) || !S_ISDIR(st.st_mode))) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}
esp_err_t book_weread_map_detached(const char *id,uint32_t uid,uint32_t version,bool text_source,
    uint32_t input,uint32_t *output) { WR_GUARD;
    if (!wr_valid_id(id) || !uid || !output) return ESP_ERR_INVALID_ARG;
    if (!weread_auth_is_authenticated()) return ESP_ERR_INVALID_STATE;
    struct { char root[512],resources[512]; } *paths __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*paths));
    if (!paths) return ESP_ERR_NO_MEM;
    cJSON *manifest = NULL;
    esp_err_t err = detached_manifest(id,uid,version,false,paths->root,sizeof(paths->root),&manifest);
    cJSON_Delete(manifest);manifest = NULL;
    if (err == ESP_OK && !text_source) {
        esp_err_t res = detached_manifest(id,0,version,true,paths->resources,sizeof(paths->resources),&manifest);
        if (res != ESP_OK && res != ESP_ERR_NOT_FOUND) err = res;
        if (res == ESP_ERR_NOT_FOUND) paths->resources[0] = 0;
        cJSON_Delete(manifest);
    }
    char *css __attribute__((cleanup(wr_release))) = NULL;size_t used = 0;
    if (err == ESP_OK && !text_source) {
        const size_t cap = 256u * 1024u;
        if ((paths->resources[0] && !collect_css(paths->resources,&css,&used,cap,0)) ||
            ((!paths->resources[0] || strcmp(paths->resources,paths->root)) && !collect_css(paths->root,&css,&used,cap,0))) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = map_files(paths->root,text_source,css ? css : "",used,true,input,output,true);
    return err;
}
esp_err_t book_weread_map_version(size_t i,uint32_t v,bool to_cloud,uint32_t input,uint32_t *output){WR_GUARD;
    loaded_chapter_t *bound=binding_owned(s_loaded)&&s_loaded->index==i&&s_loaded->version==v?s_loaded:NULL;
    if(!bound)return ESP_ERR_INVALID_STATE;
    return map_bound(i,to_cloud,input,output,bound);
}
esp_err_t book_weread_map(size_t i,bool to_cloud,uint32_t input,uint32_t *output){WR_GUARD;
    loaded_chapter_t *bound=binding_owned(s_loaded)&&s_loaded->index==i?s_loaded:
        !s_loaded&&binding_owned(s_candidate)&&s_candidate->index==i?s_candidate:NULL;
    return map_bound(i,to_cloud,input,output,bound);
}
esp_err_t book_weread_image(size_t i, const char *src, uint8_t **data, size_t *len, bool *png) { WR_GUARD;
    if (!data || !len || !png || i >= s_toc.count) return ESP_ERR_INVALID_ARG;
    *data = NULL; *len = 0; char name[512];
    if (!weread_resource_path(NULL, src, name, sizeof(name))) return ESP_ERR_INVALID_ARG;
    for (unsigned pass = 0; pass < 2; ++pass) {
        char root[512], path[1024];
        loaded_chapter_t *bound=binding_owned(s_candidate)&&s_candidate->index==i?s_candidate:
            binding_owned(s_loaded)&&s_loaded->index==i?s_loaded:NULL;
        esp_err_t err=bound?(wr_copy(root,sizeof(root),pass?bound->resources:bound->root)&&root[0]?ESP_OK:ESP_ERR_NOT_FOUND):
            pass?weread_chapter_resource_directory(s_id,root,sizeof(root)):weread_chapter_directory(s_id,book_weread_uid(i),root,sizeof(root));
        if (err != ESP_OK) continue;
        int n = snprintf(path, sizeof(path), "%s/%s", root, name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        FILE *f = fopen(path, "rb"); if (!f) continue;
        struct stat st;
        if (fstat(fileno(f), &st) || st.st_size <= 0 || st.st_size > 8 * 1024 * 1024) { fclose(f); return ESP_ERR_INVALID_SIZE; }
        size_t size = (size_t)st.st_size; uint8_t *buf = wr_alloc(size);
        if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
        bool ok = fread(buf, 1, size, f) == size; fclose(f);
        if (!ok) { free(buf); return ESP_FAIL; }
        *data = buf; *len = size; *png = size >= 8 && !memcmp(buf, "\x89PNG\r\n\x1a\n", 8); return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

bool book_weread_metadata(const char *uri, char *title, size_t tc, char *author, size_t ac) { WR_GUARD;
    weread_book_t b;
    if (!book_path_is_weread(uri) || !weread_shelf_find(uri+9,&b)) return false;
    if (title && tc) wr_display_copy(title,tc,b.title);
    if (author && ac) wr_display_copy(author,ac,b.author);
    return true;
}
bool book_weread_cover(const char *uri, uint8_t *pixels, size_t cap) { WR_GUARD;
    char path[512],legacy[512];bool migrated=false;
    if (!book_path_is_weread(uri) || !pixels || cap < 176u*240u ||
        weread_cover_cache_path(uri+9,"gray",path,sizeof(path)) != ESP_OK) return false;
    FILE *f=fopen(path,"rb");
    if(!f&&wr_cache_path(uri+9,"cover.gray",legacy,sizeof(legacy))==ESP_OK){f=fopen(legacy,"rb");migrated=f!=NULL;}
    if(!f)return false;
    bool ok=fread(pixels,1,176u*240u,f)==176u*240u && fgetc(f)==EOF;
    fclose(f);if(ok&&migrated)(void)rename(legacy,path);return ok;
}
