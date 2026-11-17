#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise production DNS callbacks and ownership through cancellation and late replies."""
from pathlib import Path
import os
import subprocess
import tempfile
from home_cover_cache_host_test import function

root=Path(__file__).resolve().parents[1]
source=root/"main/weread/weread_http_poll.inc"
text=source.read_text()
unit=r'''
#define ESP_PLATFORM 1
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
typedef int esp_err_t;
typedef struct{int unused;}esp_http_client_event_t;
enum{ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=259,ESP_ERR_NO_MEM=257,ESP_ERR_TIMEOUT=263};
typedef bool(*weread_cancel_fn)(void*);
typedef struct{unsigned value;}ip_addr_t;
typedef int err_t;
enum{ERR_OK,ERR_INPROGRESS,ERR_MEM,ERR_VAL};
#define LWIP_DNS_ADDRTYPE_IPV4_IPV6 1
#define pdMS_TO_TICKS(x) (x)
static uint64_t mono,callback_at,cancel_at;
static bool cancel_now,queue_error,alloc_error;
static unsigned live,immediate,pending_count;
static void(*callbacks[8])(const char*,const ip_addr_t*,void*);
static void *arguments[8];
static const char *names[8];
static void*tracked_calloc(size_t n,size_t size){if(alloc_error)return NULL;void*p=calloc(n,size);if(p)++live;return p;}
static void tracked_free(void*p){if(p)--live;free(p);}
#define calloc tracked_calloc
#define free tracked_free
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_calloc(n,size,caps) (assert((caps)==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),calloc(n,size))
static bool weread_client_cancelled(void){return cancel_now;}
static uint64_t client_now_ms(void){return mono;}
static int ipaddr_aton(const char*name,ip_addr_t*out){if(!strcmp(name,"127.0.0.1")){out->value=4;return 1;}return 0;}
static char *ipaddr_ntoa_r(const ip_addr_t*ip,char*out,size_t cap){snprintf(out,cap,"%s",ip->value==6?"2001:db8::1":"1.2.3.4");return out;}
static void fire(unsigned i,bool success){assert(callbacks[i]);void(*fn)(const char*,const ip_addr_t*,void*)=callbacks[i];callbacks[i]=NULL;ip_addr_t ip={6};fn(names[i],success?&ip:NULL,arguments[i]);}
static void vTaskDelay(unsigned ms){mono+=ms;if(cancel_at&&mono>=cancel_at)cancel_now=true;if(callback_at&&mono>=callback_at&&callbacks[0])fire(0,true);}
static err_t dns_gethostbyname_addrtype(const char*name,ip_addr_t*out,void(*fn)(const char*,const ip_addr_t*,void*),void*arg,int type){
 assert(type==LWIP_DNS_ADDRTYPE_IPV4_IPV6);if(immediate==1){out->value=4;return ERR_OK;}if(immediate==2)return ERR_VAL;
 unsigned i=pending_count++;assert(i<8);callbacks[i]=fn;arguments[i]=arg;names[i]=name;return ERR_INPROGRESS;
}
static err_t tcpip_try_callback(void(*fn)(void*),void*arg){if(queue_error)return ERR_MEM;fn(arg);return ERR_OK;}
'''
unit+=text[text.index("typedef struct {"):text.index("#endif",text.index("typedef struct {"))]
unit+=text[text.index("typedef struct {\n    char host"):text.index("static void http_poll_pause")]
for name in ("http_poll_pause","http_poll_guard","http_poll_resolve"):
    unit+=function(name,source)+"\n"
unit+=r'''
static http_poll_t *poller(const char*host){http_poll_t*p=calloc(1,sizeof(*p));assert(p);--live;strcpy(p->host,host);p->deadline=mono+100;return p;}
static void drop(http_poll_t*p){++live;free(p);}
static void reset(void){assert(!live&&!atomic_load(&s_http_dns_pending));mono=callback_at=cancel_at=0;cancel_now=queue_error=alloc_error=false;immediate=pending_count=0;memset(callbacks,0,sizeof(callbacks));}
int main(void){
 reset();http_poll_t*p=poller("127.0.0.1");assert(http_poll_resolve(p)==ESP_OK&&!live&&!strcmp(p->peer,"1.2.3.4"));drop(p);
 reset();p=poller("owned.example");immediate=1;assert(http_poll_resolve(p)==ESP_OK&&!live);drop(p);
 reset();p=poller("owned.example");immediate=2;assert(http_poll_resolve(p)==ESP_FAIL&&!live);drop(p);
 reset();p=poller("owned.example");callback_at=20;assert(http_poll_resolve(p)==ESP_OK&&mono==20&&!live&&!strcmp(p->peer,"2001:db8::1"));drop(p);
 reset();p=poller("owned.example");cancel_at=20;assert(http_poll_resolve(p)==ESP_ERR_INVALID_STATE&&mono==20&&live==1);
 strcpy(p->host,"reused-stack.example");assert(!strcmp(names[0],"owned.example"));drop(p);fire(0,true);assert(!live);
 reset();p=poller("owned.example");assert(http_poll_resolve(p)==ESP_ERR_TIMEOUT&&mono==100&&live==1);drop(p);fire(0,false);assert(!live);
 reset();p=poller("owned.example");callback_at=cancel_at=20;assert(http_poll_resolve(p)==ESP_ERR_INVALID_STATE&&!live);drop(p);
 reset();p=poller("owned.example");queue_error=true;assert(http_poll_resolve(p)==ESP_ERR_NO_MEM&&!live);drop(p);
 reset();p=poller("owned.example");alloc_error=true;assert(http_poll_resolve(p)==ESP_ERR_NO_MEM&&!live);alloc_error=false;drop(p);
 reset();cancel_at=10;
 for(unsigned i=0;i<2;++i){p=poller(i?"second.example":"first.example");assert(http_poll_resolve(p)==ESP_ERR_INVALID_STATE);drop(p);cancel_now=false;cancel_at=mono+10;}
 assert(live==2&&atomic_load(&s_http_dns_pending)==2);cancel_at=0;p=poller("third.example");immediate=1;
 assert(http_poll_resolve(p)==ESP_ERR_NO_MEM&&live==2);drop(p);fire(0,false);fire(1,true);assert(!live);
 p=poller("retry.example");assert(http_poll_resolve(p)==ESP_OK&&!live);drop(p);
 puts("PASS: production DNS immediate/async IPv4/IPv6, cancellation, deadline, races, owned names, late callbacks, OOM and two-query cap");
}
'''
with tempfile.TemporaryDirectory(prefix="weread-dns-host-") as folder:
    path=Path(folder)
    (path/"test.c").write_text(unit)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",str(path/"test.c"),"-o",str(path/"test")],check=True)
    subprocess.run([str(path/"test")],check=True,timeout=10)
