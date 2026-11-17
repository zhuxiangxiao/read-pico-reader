#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise the exact board CID snapshot without a bus, heap, mount or power operation."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
for board in ('pico','metalio47'):
    unit=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define READ_PICO_SD_MEDIA_ID_BYTES 28
static int state_lock,locks;
#define portENTER_CRITICAL(p) do{assert((p)==&state_lock&&!locks);++locks;}while(0)
#define portEXIT_CRITICAL(p) do{assert((p)==&state_lock&&locks==1);--locks;}while(0)
static bool suspended,media_invalidated;
static int probe_state;
static struct{bool mounted;}cached_info;
typedef struct {struct{int mfg_id,oem_id;char name[8];int revision,serial,date;}cid;}card_t;
static card_t sample,*card=&sample;
'''
    unit+=function('read_pico_sd_media_id',root/f'boards/{board}/src/components/read_pico/read_pico_sd.c')+'\n'
    unit+=r'''
int main(void){
 uint8_t guard[44];memset(guard,0xa5,sizeof(guard));uint8_t *out=guard+8;
 cached_info.mounted=true;sample.cid.mfg_id=0x12;sample.cid.oem_id=0x1234;sample.cid.revision=0x34;
 sample.cid.serial=0x12345678;sample.cid.date=0x5678;memcpy(sample.cid.name,"CARD_A",7);
 assert(read_pico_sd_media_id(out)&&!locks);
 assert(out[0]==0x12&&out[1]==0&&out[4]==0x34&&out[5]==0x12&&out[8]==0x34);
 assert(out[12]==0x78&&out[13]==0x56&&out[14]==0x34&&out[15]==0x12&&out[16]==0x78&&out[17]==0x56);
 assert(!memcmp(out+20,"CARD_A",7));uint8_t first[28];memcpy(first,out,28);
 sample.cid.serial++;assert(read_pico_sd_media_id(out)&&memcmp(first,out,28));
 for(int kind=0;kind<6;++kind){
  card=&sample;suspended=media_invalidated=false;probe_state=0;cached_info.mounted=true;
  if(kind==0)suspended=true;else if(kind==1)probe_state=1;else if(kind==2)cached_info.mounted=false;
  else if(kind==3)media_invalidated=true;else if(kind==4)card=NULL;else memset(&sample,0,sizeof(sample));
  memset(out,0xff,28);assert(!read_pico_sd_media_id(out)&&!locks);
  for(int i=0;i<28;++i)assert(!out[i]);
 }
 assert(!read_pico_sd_media_id(NULL)&&!locks);
 for(int i=0;i<8;++i)assert(guard[i]==0xa5&&guard[36+i]==0xa5);
 puts("PASS: exact CID byte order, distinct media, unavailable/USB lifecycle gates, no IO and bounded output");
}
'''
    with tempfile.TemporaryDirectory(prefix=f'cid-{board}-') as folder:
        p=Path(folder);(p/'test.c').write_text(unit)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
