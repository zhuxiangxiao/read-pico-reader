#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Verify production icon decode and pixel rendering against every canonical mask on both boards."""
from pathlib import Path
import subprocess, tempfile
from home_cover_cache_host_test import function
from pack_ui_icons import generate

root=Path(__file__).resolve().parents[1]
for board in ('pico','metalio47'):
    generate(board,check=True)
    header=root/f'boards/{board}/src/main/assets/ui_icons.h'
    packed=header.with_name('ui_icons_packed.h')
    unit=r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
'''
    unit+=f'#include "{header}"\n#include "{packed}"\n'
    unit+=r'''
#define WIDTH 96
#define HEIGHT 104
static uint8_t a[WIDTH*HEIGHT],b[WIDTH*HEIGHT];
static int ui_read_pixel(const uint8_t *fb,int x,int y){return x<0||y<0||x>=WIDTH||y>=HEIGHT?15:fb[y*WIDTH+x];}
static uint8_t ui_contrast_gray(uint8_t v){return v;}
static void epd_draw_pixel(int x,int y,uint8_t gray,uint8_t *fb){if(x>=0&&y>=0&&x<WIDTH&&y<HEIGHT)fb[y*WIDTH+x]=gray>>4;}
'''
    source=root/('boards/pico/src/main/ui/ui_kit.c' if board=='pico' else 'main/ui/ui_kit.c')
    unit+=function('icon_unpack',root/'main/ui/ui_icon_unpack.inc')+'\n'+function('icon_nibble',source)+'\n'
    draw=function('ui_draw_icon',source)
    reference=draw.replace('void ui_draw_icon(','static void raw_draw_icon(',1)
    reference=reference.replace('''    uint8_t unpacked[UI_ICON_UNPACKED_BYTES];
    if (!icon_unpack(icon, unpacked)) return;
    const uint8_t *mask = unpacked;''','''    const uint8_t *mask=ui_icon_masks[icon];''')
    assert 'icon_unpack(icon' not in reference
    unit+=draw+'\n'+reference+'\n'+r'''
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static int cmp(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
int main(void){
 uint8_t guarded[UI_ICON_UNPACKED_BYTES+16];
 for(int icon=0;icon<UI_ICON_COUNT;++icon){
  memset(guarded,0xa5,sizeof(guarded));assert(icon_unpack((ui_icon_t)icon,guarded+8));
  assert(!memcmp(guarded+8,ui_icon_masks[icon],UI_ICON_UNPACKED_BYTES));
  for(int i=0;i<8;++i)assert(guarded[i]==0xa5&&guarded[UI_ICON_UNPACKED_BYTES+8+i]==0xa5);
 }
 assert(!icon_unpack((ui_icon_t)-1,guarded)&&!icon_unpack(UI_ICON_COUNT,guarded)&&!icon_unpack(0,NULL));
 const int sizes[]={1,7,16,24,32,48,72,96,144};unsigned frames=0;
 for(int icon=0;icon<UI_ICON_COUNT;++icon)for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s)
 for(unsigned corner=0;corner<3;++corner)for(unsigned ink=0;ink<3;++ink){
  for(unsigned i=0;i<sizeof(a);++i)a[i]=b[i]=(uint8_t)((i*13u^(i>>3))&15);
  int cx=corner==0?48:corner==1?0:95,cy=corner==0?50:corner==1?0:103;
  uint8_t gray=(uint8_t)(ink*112);
  ui_draw_icon(a,cx,cy,sizes[s],(ui_icon_t)icon,gray);
  raw_draw_icon(b,cx,cy,sizes[s],(ui_icon_t)icon,gray);
  assert(!memcmp(a,b,sizeof(a)));++frames;
 }
 uint64_t encoded[500],raw[500];
 for(unsigned i=0;i<500;++i){
  uint64_t t=ns();ui_draw_icon(a,48,50,24,(ui_icon_t)(i%UI_ICON_COUNT),0);encoded[i]=ns()-t;
  t=ns();raw_draw_icon(b,48,50,24,(ui_icon_t)(i%UI_ICON_COUNT),0);raw[i]=ns()-t;
 }
 qsort(encoded,500,sizeof(*encoded),cmp);qsort(raw,500,sizeof(*raw),cmp);
 printf("PASS: %u bit-identical frames, all %u canonical masks and decode guards; host ASan n=500 packed p50=%llu/p95=%llu ns, raw p50=%llu/p95=%llu ns\n",frames,UI_ICON_COUNT,(unsigned long long)encoded[249],(unsigned long long)encoded[474],(unsigned long long)raw[249],(unsigned long long)raw[474]);
}
'''
    with tempfile.TemporaryDirectory(prefix=f'icon-pack-{board}-') as folder:
        path=Path(folder);(path/'test.c').write_text(unit)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
