/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * Shared shelf presentation, extracted from the existing local shelf.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "ui_shelf.h"
#include "ui_root_style.h"
#include "ui_image_dither.h"
#include "settings.h"
#include "ttf_font.h"
#include "book_cover.h"
#include <stdio.h>
#include <string.h>
static void copy_text(char *out,size_t cap,const char *text){
    if(!cap)return;
    if(!text)text="";
    size_t n=strlen(text);
    if(n>=cap){n=cap-1;while(n&&((unsigned char)text[n]&0xc0)==0x80)--n;}
    memcpy(out,text,n);out[n]=0;
}
int ui_shelf_rows(int style){return style==5?8:9;}
EpdRect ui_shelf_entry(int style,int row){
    if(style==5)return (EpdRect){36,184+row*96,612,96};
    int cell=(ui_content_width()-2*UI_GAP)/3;
    return (EpdRect){UI_MARGIN+(row%3)*(cell+UI_GAP),192+(row/3)*282,cell,260};
}
EpdRect ui_shelf_cover(int style,int row){
    EpdRect r=ui_shelf_entry(style,row);
    return style==5?(EpdRect){48,r.y+13,51,70}:(EpdRect){r.x+(r.width-164)/2,r.y,164,214};
}
void ui_shelf_cover_begin(uint8_t *fb,EpdRect image,int style){
    ui_root_shadow(fb,image,2);
    if(style==6)for(int step=0;step<3;++step)
        ui_fill_rect_fast((EpdRect){image.x+image.width+step*2,image.y+4+step,2,image.height-2},0xd0+step*0x10,fb);
    ui_fill_rect_fast(image,UI_GRAY_LIGHT,fb);
}
void ui_shelf_cover_end(uint8_t *fb,EpdRect image,int style,bool binary){
    ui_draw_round_rect(fb,image,0,UI_GRAY_BLACK);
    if(style==6){
        epd_draw_line(image.x+4,image.y+1,image.x+4,image.y+image.height-2,0xa0,fb);
        epd_draw_line(image.x+5,image.y+1,image.x+5,image.y+image.height-2,0xe0,fb);
        if(binary)ui_acrylic_bw_rect(fb,(EpdRect){image.x,image.y,image.width+6,image.height+4});
    }
}
void ui_shelf_cover_pixels(uint8_t *fb,EpdRect image,const uint8_t *gray,unsigned sw,unsigned sh,bool binary){
    if(!gray||!sw||!sh||image.width<=0||image.height<=0)return;
    book_crop_t crop=book_cover_crop(sw,sh,image.width,image.height);
    unsigned xs[164];uint8_t tones[256];bool sampled=image.width<=164;
    if(sampled)for(int x=0;x<image.width;++x)xs[x]=crop.x+(uint64_t)(unsigned)x*crop.width/image.width;
    for(unsigned v=0;v<256;++v)tones[v]=ui_contrast_gray(v);
    for(int y=0;y<image.height;++y){unsigned sy=crop.y+(uint64_t)(unsigned)y*crop.height/image.height;
        for(int x=0;x<image.width;++x){unsigned sx=sampled?xs[x]:crop.x+(uint64_t)(unsigned)x*crop.width/image.width;
            uint8_t tone=tones[gray[sy*sw+sx]];
            epd_draw_pixel(image.x+x,image.y+y,binary?ui_image_dither_cover_bw((tone>>4)*17u,image.x+x,image.y+y):ui_image_dither_gray(tone,image.x+x,image.y+y),fb);
        }
    }
}
void ui_shelf_cover_placeholder(uint8_t *fb,EpdRect image,const char *title){
    char name[80];copy_text(name,sizeof(name),title);
    int px=26;while(px>12&&ui_text_fixed_width_px(px,name)>image.width-22)--px;
    ui_text_vc(fb,image.x+image.width/2,image.y+image.height/2,px,name,EPD_DRAW_ALIGN_CENTER,false);
}
static void draw_list_title(uint8_t *fb,const char *name,int top,int width) {
    char title[260];const int px=ttf_em_height_px(26);
    copy_text(title,sizeof(title),name);
    // 按UTF-8字符计数，保留最多13字；...及右侧真实信息都计入像素边界。
    // Keep up to13 UTF-8 characters; dots and actual right-hand metadata share the pixel budget.
    size_t used=0;unsigned count=0;
    while(title[used]&&count<13){
        ++used;while(((unsigned char)title[used]&0xc0)==0x80)++used;
        ++count;
    }
    if(title[used])memcpy(title+used,"...",4);
    if(ui_text_fixed_width_px(px,title)>width){
        do{
            if(used){--used;while(used&&((unsigned char)title[used]&0xc0)==0x80)--used;}
            memcpy(title+used,"...",4);
        }while(used&&ui_text_fixed_width_px(px,title)>width);
    }
    ui_text_fixed_vc(fb,120,top+22,px,title,EPD_DRAW_ALIGN_LEFT,false);
}
void ui_shelf_list_row(uint8_t *fb,EpdRect card,const char *name,const char *writer,unsigned progress,unsigned chapter,unsigned chapters) {
    if(progress>100)progress=100;
    char label[16],current[16],suffix[32];
    snprintf(label,sizeof(label),"%u%%",progress);
    // 使用真实阅读分段编号；没有可用索引时不推算总章节数。
    // Use actual reader chapter IDs; never infer a total when the index is unavailable.
    if(chapters&&chapter>chapters)
        copy_text(current,sizeof(current),"—");
    else snprintf(current,sizeof(current),"%u",chapter);
    if(chapters)snprintf(suffix,sizeof(suffix)," / %u 章",(unsigned)chapters);
    else copy_text(suffix,sizeof(suffix)," / — 章");
    int current_width=ui_text_fixed_width_px(22,current),suffix_width=ui_text_fixed_width_px(22,suffix);
    int chapter_col_width=ui_text_fixed_width_px(22,"00 / 00 章");
    if(current_width+suffix_width>chapter_col_width)chapter_col_width=current_width+suffix_width;
    int bar_width=ui_text_fixed_width_px(24,"100%")+16+chapter_col_width+24;
    EpdRect track={644-bar_width,card.y+60,bar_width,7};
    draw_list_title(fb,name,card.y+10,track.x-128);
    char author[128];copy_text(author,sizeof(author),writer&&*writer?writer:"作者未知");
    while(author[0]&&ui_text_fixed_width_px(20,author)>220){size_t n=strlen(author)-1;while(n&&((unsigned char)author[n]&0xc0)==0x80)--n;author[n]=0;}
    ui_text_fixed_ink_vc(fb,120,card.y+63,20,author,EPD_DRAW_ALIGN_LEFT,0x70);
    ui_text_fixed_ink_vc(fb,644-chapter_col_width-16,card.y+32,24,label,EPD_DRAW_ALIGN_RIGHT,UI_GRAY_BLACK);
    ui_text_fixed_ink_vc(fb,644-suffix_width,card.y+32,22,current,EPD_DRAW_ALIGN_RIGHT,UI_GRAY_BLACK);
    ui_text_fixed_ink_vc(fb,644,card.y+32,22,suffix,EPD_DRAW_ALIGN_RIGHT,UI_GRAY_BLACK);
    ui_fill_round_rect(fb,track,3,UI_GRAY_WHITE);
    if(progress){
        EpdRect fill={track.x+1,track.y+1,(track.width-2)*(int)progress/100,track.height-2};
        if(fill.width<2)fill.width=2;
        ui_fill_round_rect(fb,fill,2,UI_GRAY_BLACK);
    }
    ui_draw_round_rect(fb,track,3,UI_GRAY_BLACK);
    ui_hairline(fb,card.y+95,36,612,0xd8);
}
// 中文：角标在成品封面缓存之后实时绘制；只用一个1792B字形遮罩，不新增整帧或TF文件。
// English: Paint badges live after cached artwork, using one1792-byte glyph mask, without another frame or SD file.
static uint8_t s_shelf_read_mask[64*28];
static bool s_shelf_read_ready;
static int s_shelf_read_x,s_shelf_read_y,s_shelf_read_w,s_shelf_read_h;
static bool shelf_read_mask_prepare(void){
    if(s_shelf_read_ready)return s_shelf_read_w>0;
    s_shelf_read_ready=true;memset(s_shelf_read_mask,0,sizeof(s_shelf_read_mask));
    s_shelf_read_x=64;s_shelf_read_y=28;s_shelf_read_w=s_shelf_read_h=0;
    if(!ttf_text_mask_px(s_shelf_read_mask,64,28,0,ttf_ascender_px(18),18,"已读"))return false;
    int right=-1,bottom=-1;
    for(int y=0;y<28;++y)for(int x=0;x<64;++x)if(s_shelf_read_mask[y*64+x]){
        if(x<s_shelf_read_x)s_shelf_read_x=x;
        if(y<s_shelf_read_y)s_shelf_read_y=y;
        if(x>right)right=x;
        if(y>bottom)bottom=y;
    }
    s_shelf_read_w=right-s_shelf_read_x+1;s_shelf_read_h=bottom-s_shelf_read_y+1;return s_shelf_read_w>0;
}
void ui_shelf_progress(uint8_t*fb,EpdRect image,int style,unsigned progress){
    if(!app_settings_shelf_progress_badges()||!progress||(style!=1&&style!=2&&style!=6))return;
    if(progress>=100&&image.width>=66&&image.height>=66&&shelf_read_mask_prepare()){
        int left=image.x+image.width-66;
        for(int y=0;y<66;++y)for(int x=0;x<66;++x){
            if(x-y<8||x-y>42)continue;
            epd_draw_pixel(left+x,image.y+y,UI_GRAY_BLACK,fb);
            int sx=((x-44+y-19)*181)/256+s_shelf_read_w/2;
            int sy=((-x+44+y-19)*181)/256+s_shelf_read_h/2;
            if(sx>=0&&sx<s_shelf_read_w&&sy>=0&&sy<s_shelf_read_h){
                uint8_t alpha=s_shelf_read_mask[(s_shelf_read_y+sy)*64+s_shelf_read_x+sx];
                if(alpha)epd_draw_pixel(left+x,image.y+y,alpha,fb);
            }
        }
    }else{
        char text[12];if(progress>=100)copy_text(text,sizeof(text),"已读");else snprintf(text,sizeof(text),"%u%%",progress);
        int width=ui_text_fixed_width_px(26,text)+18;
        if(width>image.width-10)width=image.width-10;
        EpdRect r={image.x+image.width-width-5,image.y+5,width,36};
        ui_fill_round_rect(fb,r,18,UI_GRAY_BLACK);ui_text_fixed_vc(fb,r.x+r.width/2,r.y+18,26,text,EPD_DRAW_ALIGN_CENTER,true);
    }
}

void ui_shelf_begin(void){s_shelf_read_ready=false;}

void ui_shelf_furniture(uint8_t*fb,int style,int base,bool binary,void (*guard)(uint8_t*,EpdRect,int)) {
    if (!style || style == 5) return;
    for (int row = 0; row < 3; ++row) {
        int top = base + row * 282;
        if (style == 1) {
            ui_fill_rect_fast((EpdRect){36, top + 214, 612, 12}, 0x30, fb);
            ui_hairline(fb, top + 214, 36, 612, 0x58);
            ui_hairline(fb, top + 225, 36, 612, 0x10);
        } else if (style == 2) {
            EpdRect acrylic = {20, top + 136, 644, 90};
            if(guard)guard(fb,acrylic,row);else {ui_draw_acrylic_guard(fb,acrylic);if(binary)ui_acrylic_bw_rect(fb,acrylic);}
        } else if (style == 6) {
            EpdRect r={20,top+188,644,42};if(guard)guard(fb,r,row);else {ui_draw_liquid_shelf_rail(fb,r);if(binary)ui_acrylic_bw_rect(fb,r);}
        }
    }
}
static EpdRect shelf_pager_group_rect(int page,int pages) {
    char label[64];snprintf(label,sizeof(label),"%02d/%02d",page+1,pages);
    int width=ui_text_fixed_width_px(32,label);
    int search_width=ui_text_fixed_width_px(26,"搜索");
    if(width>154-search_width)width=154-search_width;
    int total=width+search_width+106;
    return (EpdRect){(UI_LOCK_WIDTH-total)/2,1004,total,54};
}
EpdRect ui_shelf_page_label_rect(int page,int pages) {
    EpdRect group=shelf_pager_group_rect(page,pages);
    int search_width=ui_text_fixed_width_px(26,"搜索");
    return (EpdRect){group.x+88+search_width,1006,group.width-106-search_width,50};
}
EpdRect ui_shelf_search_rect(int page,int pages) {
    EpdRect group=shelf_pager_group_rect(page,pages);
    int search_width=ui_text_fixed_width_px(26,"搜索");
    return (EpdRect){group.x+11,1006,search_width+49,50};
}
static void draw_shelf_search_button(uint8_t *fb,int page,int pages,bool filtered,bool pressed) {
    EpdRect r=ui_shelf_search_rect(page,pages);int offset=pressed?2:0;
    int search_width=r.width-49;
    ui_draw_icon(fb,r.x+20,r.y+r.height/2+offset,26,filtered?UI_ICON_X:UI_ICON_SEARCH,0x30);
    EpdRect text={r.x+41,1006+2*offset,search_width,50-2*offset};
    ui_text_fixed_vc(fb,text.x+text.width/2,text.y+text.height/2,26,"搜索",EPD_DRAW_ALIGN_CENTER,false);
    if(filtered)ui_hairline(fb,1053,text.x,text.width,0x30);
}
void ui_shelf_pager(uint8_t *fb,int page,int pages,bool filtered,bool key_mode,int pressed,bool search_pressed) {
    char label[64];snprintf(label,sizeof(label),"%02d/%02d",page+1,pages);
    ui_pager_draw(fb,"",page,pages,key_mode,pressed);
    EpdRect group=shelf_pager_group_rect(page,pages);
    int search_width=ui_text_fixed_width_px(26,"搜索");
    epd_fill_circle(group.x+70+search_width,1031,2,0x80,fb);
    EpdRect page_label=ui_shelf_page_label_rect(page,pages);int label_px=32;
    while(label_px>1&&ui_text_fixed_width_px(label_px,label)>page_label.width)--label_px;
    ui_text_fixed_vc(fb,page_label.x+page_label.width/2,page_label.y+page_label.height/2,
                     label_px,label,EPD_DRAW_ALIGN_CENTER,false);
    draw_shelf_search_button(fb,page,pages,filtered,search_pressed);
}

int ui_shelf_order_mode(unsigned o){return o>=6?3:o<2?(int)o:o<4?(int)o-2:2;}
bool ui_shelf_order_desc(unsigned o){return o==1||o==2||o==5||o==7;}
unsigned ui_shelf_order_value(int mode,bool desc){static const uint8_t values[4][2]={{0,2},{3,1},{4,5},{6,7}};return mode>=0&&mode<4?values[mode][desc]:0;}
EpdRect ui_shelf_order_rect(int row) {
    return row<0?(EpdRect){378,160,270,382}:(EpdRect){390,172+row*70,246,70};
}
void ui_shelf_order_menu(uint8_t *fb,unsigned order,const char *time_label) {
    EpdRect panel=ui_shelf_order_rect(-1);
    ui_fill_round_rect(fb,panel,18,UI_GRAY_WHITE);ui_draw_control_frame(fb,panel,18,0x70);
    const char *labels[]={"最近阅读",time_label,"书名排序","阅读进度"};const int modes[]={1,2,0,3};
    for(int i=0;i<4;++i){
        EpdRect r=ui_shelf_order_rect(i);bool selected=ui_shelf_order_mode(order)==modes[i];
        if(selected)ui_fill_round_rect(fb,(EpdRect){r.x,r.y+3,r.width,64},12,0xee);
        ui_text_box(fb,(EpdRect){r.x+12,r.y+12,186,46},27,labels[i],EPD_DRAW_ALIGN_LEFT,false);
        if(selected){epd_draw_circle(615,r.y+35,9,0x20,fb);epd_fill_circle(615,r.y+35,4,0x20,fb);}
    }
    ui_hairline(fb,466,402,222,0xd0);
    for(int i=0;i<2;++i){EpdRect r={390+i*124,482,122,46};bool selected=ui_shelf_order_desc(order)==(i==1);
        ui_fill_round_rect(fb,r,13,selected?UI_GRAY_BLACK:UI_GRAY_WHITE);ui_draw_control_frame(fb,r,13,0x60);
        ui_text_box(fb,ui_inset_rect(r,5),22,i?"降序":"升序",EPD_DRAW_ALIGN_CENTER,selected);
    }
}

void ui_shelf_order_button(uint8_t *fb,EpdRect r,unsigned order,bool open,bool pressed,const char *time_label,bool compact){
    const char *labels[]={"书名排序","最近阅读",time_label,"阅读进度"};
    if(pressed)ui_draw_pressed_round_rect(fb,r,14);else ui_draw_choice_round_rect(fb,r,14,false);
    ui_text_box(fb,(EpdRect){r.x+(compact?8:10),r.y+4,r.width-(compact?34:42),r.height-8},compact?20:24,labels[ui_shelf_order_mode(order)],EPD_DRAW_ALIGN_CENTER,false);
    ui_draw_icon(fb,r.x+r.width-(compact?18:19),r.y+r.height/2,compact?18:21,open?UI_ICON_CHEVRON_UP:UI_ICON_CHEVRON_DOWN,0x30);
}
bool ui_shelf_order_select(int x,int y,unsigned order,unsigned *selected){
    static const int modes[]={1,2,0,3};
    for(int i=0;i<4;++i)if(ui_rect_hit(ui_shelf_order_rect(i),x,y)){*selected=ui_shelf_order_value(modes[i],ui_shelf_order_desc(order));return true;}
    for(int i=0;i<2;++i)if(ui_rect_hit((EpdRect){390+i*124,482,122,46},x,y)){*selected=ui_shelf_order_value(ui_shelf_order_mode(order),i==1);return true;}
    return false;
}
