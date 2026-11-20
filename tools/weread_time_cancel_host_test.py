# SPDX-FileCopyrightText: 2026 kiikoread
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise production WeRead/transfer cancellation, ownership and late DNS callbacks."""
from pathlib import Path
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/weread-time-cancel'
BUILD.mkdir(parents=True, exist_ok=True)
TRANSFER = (ROOT / 'components/read_pico_transfer/read_pico_transfer.c').read_text()
RUNTIME = (ROOT / 'main/weread/weread_runtime.c').read_text()
NTP = (ROOT / 'components/read_pico_transfer/transfer_ntp_time.c').read_text()


def function(source, name, occurrence=0):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;{}]*\)\s*\{', source, re.M))
    match = matches[occurrence]
    at, depth = match.end(), 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[match.start():at] + '\n'


COMMON = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
typedef int esp_err_t;
enum {ESP_OK,ESP_FAIL,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM,ESP_ERR_TIMEOUT,ESP_ERR_NOT_FINISHED,ESP_ERR_INVALID_RESPONSE};
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(x) (x)
static int64_t mono;
static int64_t esp_timer_get_time(void){return mono;}
static int s_lock;
static bool s_config_busy,s_time_active,s_time_automatic,s_time_cancel,s_stopping;
static bool (*s_time_owner_cancel)(void *);
static void *s_time_owner_ctx;
static uint32_t s_time_pending_utc;
enum {READ_PICO_TRANSFER_STOPPED,READ_PICO_TRANSFER_READY};
static struct {int state;} s_status;
static int foreign_cancels,impl_calls,impl_error;
static bool request_cancel;
static int64_t request_deadline;
static bool owner_cancel(void *ctx){assert(ctx==&request_cancel);return request_cancel||(request_deadline&&mono>=request_deadline);}
static bool cancel_time_sync(bool automatic_only){assert(automatic_only);++foreign_cancels;return !s_time_active;}
'''
# Both boards use exactly the same admission and owner callback lifecycle.
for name in ('time_sync_cancelled', 'sync_time_run_cancelable', 'sync_time_run',
             'read_pico_transfer_sync_time_online_cancelable'):
    assert function(TRANSFER, name) == function(TRANSFER, name, 1)

OWNER = COMMON + function(TRANSFER, 'time_sync_cancelled') + r'''
static bool cancel_inside;
static esp_err_t sync_time_impl(uint32_t *utc){assert(s_time_active&&s_config_busy);++impl_calls;*utc=1800000000;if(cancel_inside)request_cancel=true;return impl_error;}
static esp_err_t sync_time_online_impl(uint32_t *utc){return sync_time_impl(utc);}
''' + function(TRANSFER, 'sync_time_run_cancelable') + function(TRANSFER, 'sync_time_run') + function(TRANSFER, 'read_pico_transfer_sync_time_online_cancelable') + r'''
static void reset(void){s_config_busy=s_time_active=s_time_automatic=s_time_cancel=s_stopping=request_cancel=cancel_inside=false;s_time_owner_cancel=NULL;s_time_owner_ctx=NULL;s_time_pending_utc=0;s_status.state=READ_PICO_TRANSFER_READY;impl_calls=foreign_cancels=impl_error=0;request_deadline=mono=0;}
int main(void){uint32_t utc=123;
 reset();assert(read_pico_transfer_sync_time_online_cancelable(&utc,owner_cancel,&request_cancel)==ESP_OK&&utc==1800000000&&s_time_pending_utc==utc&&!foreign_cancels&&!s_time_owner_cancel&&!s_time_active&&!s_config_busy);
 reset();request_cancel=true;assert(read_pico_transfer_sync_time_online_cancelable(&utc,owner_cancel,&request_cancel)==ESP_ERR_INVALID_STATE&&!impl_calls&&!foreign_cancels);
 reset();request_deadline=1;mono=1;assert(read_pico_transfer_sync_time_online_cancelable(&utc,owner_cancel,&request_cancel)==ESP_ERR_INVALID_STATE&&!impl_calls&&!foreign_cancels);
 for(int automatic=0;automatic<2;++automatic){reset();s_time_active=s_config_busy=true;s_time_automatic=automatic;assert(read_pico_transfer_sync_time_online_cancelable(&utc,owner_cancel,&request_cancel)==ESP_ERR_INVALID_STATE&&!foreign_cancels&&!s_time_cancel&&!s_time_owner_cancel&&s_time_active&&s_config_busy);}
 reset();cancel_inside=true;assert(read_pico_transfer_sync_time_online_cancelable(&utc,owner_cancel,&request_cancel)==ESP_ERR_INVALID_STATE&&!utc&&!s_time_pending_utc&&!s_time_owner_cancel&&!s_time_active);
 reset();impl_error=ESP_FAIL;assert(read_pico_transfer_sync_time_online_cancelable(&utc,owner_cancel,&request_cancel)==ESP_FAIL&&!s_time_pending_utc&&!s_time_owner_cancel&&!s_time_active);
 assert(read_pico_transfer_sync_time_online_cancelable(&utc,NULL,NULL)==ESP_ERR_INVALID_ARG);
 reset();assert(sync_time_run(&utc,true,false)==ESP_OK&&foreign_cancels==1);assert(sync_time_run(&utc,true,true)==ESP_OK&&foreign_cancels==1);
 puts("PASS: admitted owner callback, external manual/automatic isolation, pre-cancel/deadline and completion race cleanup");
}
'''
PICO = COMMON + function(TRANSFER, 'time_sync_cancelled') + r'''
typedef struct {int count;const char *servers[3];} esp_sntp_config_t;
#define ESP_SNTP_SERVER_LIST(...) {__VA_ARGS__}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n,list) {.count=n,.servers=list}
static int inits,deinits,waits,cancel_at,success_at;
static int esp_netif_sntp_init(esp_sntp_config_t *cfg){assert(cfg->count==3);++inits;return ESP_OK;}
static int esp_netif_sntp_sync_wait(int ms){mono+=ms*1000;++waits;if(waits==cancel_at)request_cancel=true;return waits==success_at?ESP_OK:ESP_ERR_TIMEOUT;}
static void esp_netif_sntp_deinit(void){++deinits;}
static time_t test_time(time_t *out){if(out)*out=1800000000;return 1800000000;}
#define time test_time
''' + function(TRANSFER, 'time_sync_ntp') + r'''
static void reset(void){mono=0;request_cancel=false;request_deadline=0;s_time_owner_cancel=owner_cancel;s_time_owner_ctx=&request_cancel;inits=deinits=waits=cancel_at=success_at=0;}
int main(void){uint32_t utc=0;
 reset();cancel_at=2;assert(time_sync_ntp(&utc)==ESP_ERR_INVALID_STATE&&mono==200000&&inits==1&&deinits==1);
 reset();request_deadline=1800000;assert(time_sync_ntp(&utc)==ESP_ERR_INVALID_STATE&&mono==1800000&&deinits==1);
 reset();success_at=cancel_at=1;assert(time_sync_ntp(&utc)==ESP_ERR_INVALID_STATE&&deinits==1&&!utc);
 reset();success_at=1;assert(time_sync_ntp(&utc)==ESP_OK&&utc==1800000000&&deinits==1);
 puts("PASS: Pico SNTP cancellation and 1800ms caller deadline with one owner init/deinit");
}
'''
CONNECT = r'''
#define ESP_PLATFORM
''' + COMMON + r'''
static atomic_bool s_cancel;
static atomic_uint_fast64_t s_deadline;
static bool s_owns_network;
// 生产代码用该标志阻止建连超时被静默重放。/ Production uses this flag to keep connect timeouts out of the silent replay.
static bool s_connect_timed_out;
static uint64_t s_network_active;
static uint64_t now_ms(void){return mono/1000;}
static void pause_ms(unsigned ms){mono+=(int64_t)ms*1000;}
''' + function(RUNTIME, 'cancelled') + r'''
#define READ_PICO_TRANSFER_MODE_STA 1
#define READ_PICO_TRANSFER_MODE_AP 0
#define READ_PICO_TRANSFER_ERROR 4
typedef struct {bool network_ready;int mode,state,last_error;} read_pico_transfer_status_t;
typedef struct {int mode;bool network_only,cover_lookup;} read_pico_transfer_cfg_t;
static time_t wall;
static bool sync_cancel;
static bool connect_starting,started;
static int syncs,sync_error;
static void read_pico_transfer_get_status(read_pico_transfer_status_t *out){*out=(read_pico_transfer_status_t){!connect_starting||started,READ_PICO_TRANSFER_MODE_STA,connect_starting&&!started?READ_PICO_TRANSFER_STOPPED:READ_PICO_TRANSFER_READY,0};}
static esp_err_t read_pico_transfer_start(const read_pico_transfer_cfg_t *cfg){assert(connect_starting&&!started&&cfg->mode==READ_PICO_TRANSFER_MODE_STA&&cfg->network_only);started=true;return ESP_OK;}
static void read_pico_transfer_service_poll(void){}
static esp_err_t read_pico_transfer_sync_time_online_cancelable(uint32_t *utc,bool(*check)(void*),void *ctx){++syncs;assert(ctx==NULL);for(int i=0;i<30;++i){mono+=100000;if(sync_cancel&&i==1)atomic_store(&s_cancel,true);if(check(ctx))return ESP_ERR_INVALID_STATE;}if(sync_error)return sync_error;*utc=wall=1800000000;return ESP_OK;}
static time_t test_time(time_t *out){if(out)*out=wall;return wall;}
#define time test_time
#define weread_client_set_radio_awake(awake) ((void)(awake))
''' + function(RUNTIME, 'connect_network') + r'''
static void reset(void){mono=0;wall=0;syncs=sync_error=0;s_network_active=0;connect_starting=started=sync_cancel=s_owns_network=s_connect_timed_out=false;atomic_store(&s_cancel,false);atomic_store(&s_deadline,0);}
int main(void){
 reset();sync_cancel=true;assert(connect_network()==ESP_ERR_INVALID_STATE&&mono==200000&&syncs==1);
 reset();atomic_store(&s_deadline,1800);assert(connect_network()==ESP_ERR_INVALID_STATE&&mono==1800000&&syncs==1);
 reset();sync_error=ESP_ERR_TIMEOUT;assert(connect_network()==ESP_ERR_TIMEOUT);
 reset();assert(connect_network()==ESP_OK&&syncs==1&&wall==1800000000);assert(connect_network()==ESP_OK&&syncs==1);
 // 成功建连清除超时标记：后续 HTTP 瞬时超时仍可静默重试。
 // A successful connect clears the timeout marker so a later HTTP timeout can still replay.
 reset();s_connect_timed_out=true;assert(connect_network()==ESP_OK&&!s_connect_timed_out);
 reset();atomic_store(&s_cancel,true);assert(connect_network()==ESP_ERR_INVALID_STATE&&!syncs);
 reset();connect_starting=true;wall=1800000000;assert(connect_network()==ESP_OK&&started&&s_owns_network&&s_network_active==100);
 mono+=600000;assert(connect_network()==ESP_OK&&s_network_active==700&&!syncs);
 puts("PASS: production WeRead connect propagates its cancel/deadline and time failure; first TLS calibration retained");
}
'''
S31 = COMMON + function(TRANSFER, 'time_sync_cancelled') + r'''
typedef int wifi_ps_type_t;
#define WIFI_PS_NONE 0
static int bounded,legacy,https,ps=1;
static esp_err_t esp_wifi_get_ps(wifi_ps_type_t *out){*out=ps;return ESP_OK;}
static esp_err_t esp_wifi_set_ps(wifi_ps_type_t v){ps=v;return ESP_OK;}
static esp_err_t transfer_sync_time_ntp_bounded(uint32_t *utc,bool(*cancel)(void)){(void)utc;++bounded;assert(!ps);return cancel()?ESP_ERR_INVALID_STATE:ESP_ERR_TIMEOUT;}
static esp_err_t transfer_sync_time_ntp_cancelable(uint32_t *utc,bool(*cancel)(void)){(void)utc;(void)cancel;++legacy;return ESP_ERR_TIMEOUT;}
static esp_err_t transfer_sync_time_https(uint32_t *utc){++https;*utc=1800000000;return ESP_OK;}
''' + function(TRANSFER, 'sync_network_time') + r'''
int main(void){uint32_t utc=0;
 s_time_owner_cancel=owner_cancel;s_time_owner_ctx=&request_cancel;assert(sync_network_time(&utc)==ESP_ERR_TIMEOUT&&bounded==1&&!legacy&&!https&&ps==1);
 request_cancel=true;assert(sync_network_time(&utc)==ESP_ERR_INVALID_STATE&&bounded==2&&!https&&ps==1);
 s_time_owner_cancel=NULL;assert(sync_network_time(&utc)==ESP_OK&&legacy==1&&https==1&&ps==1);
 s_time_automatic=true;assert(sync_network_time(&utc)==ESP_ERR_TIMEOUT&&legacy==2&&https==1&&ps==1);
 puts("PASS: S31 deadline calls use bounded NTP; ordinary/manual Date fallback and automatic isolation retained");
}
'''
DNS_COMMON = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>
typedef int esp_err_t;
enum {ESP_OK,ESP_FAIL,ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM,ESP_ERR_TIMEOUT};
typedef int err_t;
enum {ERR_OK,ERR_INPROGRESS,ERR_MEM};
typedef struct {uint32_t ip;} ip_addr_t;
#define IP_IS_V4(p) ((p)!=NULL)
#define ip_2_ip4(p) (p)
#define ip4_addr_get_u32(p) ((p)->ip)
#define LWIP_DNS_ADDRTYPE_IPV4 0
#define pdMS_TO_TICKS(x) (x)
static int64_t mono;
static int live,queue_error,dns_immediate,callback_at,cancel_at;
static bool cancel_query;
static void (*pending[4])(const char*,const ip_addr_t*,void*);
static void *pending_args[4];static int pending_count;
static void *tracked_calloc(size_t n,size_t size){void *p=calloc(n,size);if(p)++live;return p;}
static void tracked_free(void *p){if(p)--live;free(p);}
#define calloc tracked_calloc
#define free tracked_free
static int64_t esp_timer_get_time(void){return mono;}
static bool cancelled(void){return cancel_query;}
static void fire(int i,bool success){assert(pending[i]);void(*fn)(const char*,const ip_addr_t*,void*)=pending[i];void *arg=pending_args[i];pending[i]=NULL;ip_addr_t addr={htonl(0x01020304)};fn("ntp",success?&addr:NULL,arg);}
static void vTaskDelay(unsigned ms){mono+=(int64_t)ms*1000;if(cancel_at&&mono>=cancel_at)cancel_query=true;if(callback_at&&mono>=callback_at&&pending[0])fire(0,true);}
static err_t dns_gethostbyname_addrtype(const char *name,ip_addr_t *out,void(*fn)(const char*,const ip_addr_t*,void*),void *arg,int type){assert(name&&type==0);if(dns_immediate){out->ip=htonl(0x01020304);return ERR_OK;}int i=pending_count++;assert(i<4);pending[i]=fn;pending_args[i]=arg;return ERR_INPROGRESS;}
static err_t tcpip_try_callback(void(*fn)(void*),void *arg){if(queue_error)return ERR_MEM;fn(arg);return ERR_OK;}
'''
DNS = DNS_COMMON + NTP[NTP.index('typedef struct {'):NTP.index('\n#endif')] + r'''
static void reset(void){assert(!live);mono=0;queue_error=dns_immediate=callback_at=cancel_at=pending_count=0;cancel_query=false;memset(pending,0,sizeof(pending));}
int main(void){struct sockaddr_in peer={0};
 reset();dns_immediate=1;assert(time_dns_resolve("ntp",&peer,cancelled)==ESP_OK&&!live&&peer.sin_port==htons(123));
 reset();callback_at=20000;assert(time_dns_resolve("ntp",&peer,cancelled)==ESP_OK&&mono==20000&&!live);
 reset();cancel_at=20000;assert(time_dns_resolve("ntp",&peer,cancelled)==ESP_ERR_INVALID_STATE&&mono==20000&&live==1);fire(0,true);assert(!live);
 reset();assert(time_dns_resolve("ntp",&peer,cancelled)==ESP_ERR_TIMEOUT&&mono==2000000&&live==1);fire(0,false);assert(!live);
 reset();callback_at=cancel_at=20000;assert(time_dns_resolve("ntp",&peer,cancelled)==ESP_ERR_INVALID_STATE&&!live);
 reset();queue_error=1;assert(time_dns_resolve("ntp",&peer,cancelled)==ESP_ERR_NO_MEM&&!live);
 reset();cancel_at=10000;assert(time_dns_resolve("old",&peer,cancelled)==ESP_ERR_INVALID_STATE&&live==1);cancel_query=false;cancel_at=0;dns_immediate=1;assert(time_dns_resolve("new",&peer,cancelled)==ESP_OK&&live==1);fire(0,false);assert(!live&&peer.sin_addr.s_addr==htonl(0x01020304));
 puts("PASS: S31 async DNS cancel/2s cap, completion races, late callbacks and retry own separate references");
}
'''
for name, unit in [('owner', OWNER), ('pico', PICO), ('connect', CONNECT), ('s31', S31), ('dns', DNS)]:
    source, binary = BUILD / f'{name}.c', BUILD / name
    source.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                    '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
