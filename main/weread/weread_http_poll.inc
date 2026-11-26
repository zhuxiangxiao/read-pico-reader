/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
// 只供WeRead工作线程传输使用；数字连接地址不替换原始Host、SNI或凭据边界。
// Only WeRead worker traffic uses this poller; numeric peers preserve original Host, SNI and credential boundaries.
#if defined(ESP_PLATFORM) || defined(WEREAD_TEST_ASYNC_HTTP)
#include <errno.h>
#if defined(ESP_PLATFORM)
#include <stdatomic.h>
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
typedef struct {
    atomic_uint refs;
    atomic_bool done;
    bool found;
    ip_addr_t address;
    char host[256];
} http_dns_t;
_Static_assert(sizeof(http_dns_t)<=384,"WeRead DNS callback memory budget");
static atomic_uint s_http_dns_pending;
static void http_dns_release(http_dns_t *q) {
    if(atomic_fetch_sub(&q->refs,1)==1){atomic_fetch_sub(&s_http_dns_pending,1);free(q);}
}
static void http_dns_found(const char *name,const ip_addr_t *address,void *arg) {
    (void)name;http_dns_t *q=arg;
    if(address){q->address=*address;q->found=true;}
    atomic_store(&q->done,true);http_dns_release(q);
}
static void http_dns_start(void *arg) {
    http_dns_t *q=arg;
    err_t err=dns_gethostbyname_addrtype(q->host,&q->address,http_dns_found,q,LWIP_DNS_ADDRTYPE_IPV4_IPV6);
    if(err!=ERR_INPROGRESS)http_dns_found(q->host,err==ERR_OK?&q->address:NULL,q);
}
#endif
typedef struct {
    char host[256],authority[272],peer[48];
    char *path,*query;
    int port;
    uint64_t deadline,hard_deadline;
    unsigned idle_ms;
    unsigned connections,header_restarts;
    bool connected;
    esp_err_t error;
    weread_cancel_fn cancel;
    void *cancel_ctx;
    int (*event_handler)(esp_http_client_event_t *);
    void *event_ctx;
    char target[];
} http_poll_t;
_Static_assert(sizeof(http_poll_t)<=704,"WeRead HTTP endpoint memory budget");
static void http_poll_pause(void) {
#if defined(ESP_PLATFORM)
    vTaskDelay(pdMS_TO_TICKS(10));
#else
    struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
#endif
}
static esp_err_t http_poll_guard(http_poll_t *p) {
    if(weread_client_cancelled()||(p->cancel&&p->cancel(p->cancel_ctx)))return p->error=ESP_ERR_INVALID_STATE;
    if(client_now_ms()>=p->deadline)return p->error=ESP_ERR_TIMEOUT;
    return ESP_OK;
}
static void http_poll_progress(http_poll_t *p) {
    p->deadline=client_now_ms()+p->idle_ms;
    if(p->hard_deadline&&p->deadline>p->hard_deadline)p->deadline=p->hard_deadline;
}
static esp_err_t http_poll_resolve(http_poll_t *p) {
#if defined(ESP_PLATFORM)
    ip_addr_t numeric;
    if(ipaddr_aton(p->host,&numeric)){ipaddr_ntoa_r(&numeric,p->peer,sizeof(p->peer));return ESP_OK;}
    if(atomic_fetch_add(&s_http_dns_pending,1)>=2){atomic_fetch_sub(&s_http_dns_pending,1);return ESP_ERR_NO_MEM;}
    http_dns_t *q=heap_caps_calloc(1,sizeof(*q),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!q){atomic_fetch_sub(&s_http_dns_pending,1);return ESP_ERR_NO_MEM;}
    memcpy(q->host,p->host,strlen(p->host)+1);atomic_init(&q->refs,2);atomic_init(&q->done,false);
    if(tcpip_try_callback(http_dns_start,q)!=ERR_OK){http_dns_release(q);http_dns_release(q);return ESP_ERR_NO_MEM;}
    esp_err_t err=ESP_OK;
    while(!atomic_load(&q->done)&&(err=http_poll_guard(p))==ESP_OK)http_poll_pause();
    if(err==ESP_OK){
        err=http_poll_guard(p);
        if(err==ESP_OK&&(!q->found||!ipaddr_ntoa_r(&q->address,p->peer,sizeof(p->peer))))err=ESP_FAIL;
    }
    // 域名与回调引用都由query持有，取消后迟到的DNS不会访问栈或已释放对象。
    // Query-owned names and callback references remain valid after cancellation.
    http_dns_release(q);return err;
#else
    extern esp_err_t weread_test_http_dns(const char *,char *,size_t,weread_cancel_fn,void *,uint64_t);
    return weread_test_http_dns(p->host,p->peer,sizeof(p->peer),p->cancel,p->cancel_ctx,p->deadline);
#endif
}
static esp_err_t http_poll_prepare(const char *url,bool enabled,weread_cancel_fn cancel,void *ctx,http_poll_t **out) {
    *out=NULL;if(!enabled||strncasecmp(url,"https://",8))return ESP_OK;
    size_t length=strlen(url);if(length>2048)return ESP_ERR_INVALID_SIZE;
    for(const unsigned char *c=(const unsigned char *)url;*c;++c)
        if(*c<=32||*c==127||*c=='\\')return ESP_ERR_INVALID_ARG;
    http_poll_t *p=wr_alloc(sizeof(*p)+length+2);if(!p)return ESP_ERR_NO_MEM;
    const char *authority=url+8;size_t n=strcspn(authority,"/?#");
    if(!n||n>=sizeof(p->authority)||memchr(authority,'@',n)){free(p);return ESP_ERR_INVALID_ARG;}
    memcpy(p->authority,authority,n);p->authority[n]=0;
    const char *host=authority,*port=NULL;size_t host_length=n;
    if(*host=='['){
        const char *end=memchr(host,']',n);
        if(!end){free(p);return ESP_ERR_INVALID_ARG;}
        host_length=(size_t)(end-host-1);++host;
        if(end+1<authority+n){if(end[1]!=':'){free(p);return ESP_ERR_INVALID_ARG;}port=end+2;}
    }else{
        const char *colon=memchr(host,':',n);
        if(colon){host_length=(size_t)(colon-host);port=colon+1;}
    }
    if(!host_length||host_length>=sizeof(p->host)){free(p);return ESP_ERR_INVALID_ARG;}
    memcpy(p->host,host,host_length);p->host[host_length]=0;p->port=443;
    if(port){
        char *end=NULL;unsigned long value=strtoul(port,&end,10);
        if(end!=authority+n||!value||value>65535){free(p);return ESP_ERR_INVALID_ARG;}p->port=(int)value;
    }
    const char *target=authority+n;size_t target_length=strcspn(target,"#");
    size_t prefix=*target=='/'?0:1;if(prefix)p->target[0]='/';
    memcpy(p->target+prefix,target,target_length);p->target[prefix+target_length]=0;p->path=p->target;
    p->query=strchr(p->target,'?');if(p->query)*p->query++=0;
    p->cancel=cancel;p->cancel_ctx=ctx;p->idle_ms=(unsigned)request_default_timeout_ms(url);
#if defined(WEREAD_TEST_ASYNC_HTTP)
    extern unsigned weread_test_http_idle_ms(const char *);
    unsigned test_idle=weread_test_http_idle_ms(url);if(test_idle)p->idle_ms=test_idle;
#endif
    int budget=s_timeout_budget?s_timeout_budget(s_timeout_ctx):0;
    if(budget>0)p->hard_deadline=client_now_ms()+(uint64_t)budget;
    http_poll_progress(p);
    esp_err_t err=http_poll_guard(p);
#if defined(ESP_PLATFORM)
    if(err==ESP_OK&&!http_pool_peer(p->host,p->port,p->peer))err=http_poll_resolve(p);
#else
    if(err==ESP_OK)err=http_poll_resolve(p);
#endif
    if(err!=ESP_OK){free(p);return err;}
    http_poll_progress(p);*out=p;return ESP_OK;
}
static esp_err_t http_poll_event(esp_http_client_event_t *event) {
    http_poll_t *p=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_CONNECTED){p->connected=true;++p->connections;}
    else if(event->event_id==HTTP_EVENT_DISCONNECTED)p->connected=false;
    if(event->event_id==HTTP_EVENT_ON_CONNECTED||event->event_id==HTTP_EVENT_HEADERS_SENT||
       event->event_id==HTTP_EVENT_ON_HEADER||event->event_id==HTTP_EVENT_ON_DATA)http_poll_progress(p);
    if(!p->event_handler)return ESP_OK;
    esp_http_client_event_t forwarded=*event;forwarded.user_data=p->event_ctx;
    return p->event_handler(&forwarded);
}
static void http_poll_config(http_poll_t *p,esp_http_client_config_t *cfg) {
    if(!p)return;
#if defined(ESP_PLATFORM)
    static bool s_http_log_quiet;
    if(!s_http_log_quiet){esp_log_level_set("HTTP_CLIENT",ESP_LOG_ERROR);s_http_log_quiet=true;}
#endif
    cfg->url=NULL;cfg->host=p->peer;cfg->port=p->port;cfg->path=p->path;cfg->query=p->query;
    cfg->common_name=p->host;cfg->transport_type=HTTP_TRANSPORT_OVER_SSL;cfg->is_async=true;cfg->timeout_ms=50;
    p->event_handler=cfg->event_handler;p->event_ctx=cfg->user_data;
    cfg->event_handler=http_poll_event;cfg->user_data=p;
}
static void http_poll_reset(http_poll_t *p) {
    if(!p)return;
    p->connected=false;p->connections=0;p->header_restarts=0;p->error=ESP_OK;
    http_poll_progress(p);
}
static const char *http_poll_host(http_poll_t *p) { return p?p->authority:NULL; }
static esp_err_t http_poll_error(http_poll_t *p,esp_err_t fallback) { return p&&p->error?p->error:fallback; }
static esp_err_t http_poll_open(esp_http_client_handle_t client,int length,http_poll_t *p) {
    if(!p)return esp_http_client_open(client,length);
#if defined(ESP_PLATFORM)
    uint64_t connect_started=client_now_ms();
    unsigned connect_retries=0;
#endif
    for(;;){
        esp_err_t err=http_poll_guard(p);if(err!=ESP_OK)return err;
        weread_client_yield_for_display(p->cancel,p->cancel_ctx);
        unsigned connections=p->connections;
        errno=0;err=esp_http_client_open(client,length);
        if(err==ESP_OK){http_poll_progress(p);return ESP_OK;}
        if(err!=ESP_ERR_HTTP_EAGAIN&&!(err==ESP_ERR_HTTP_WRITE_DATA&&(!errno||errno==EAGAIN||errno==EWOULDBLOCK)))
            return http_poll_guard(p)==ESP_OK?err:p->error;
        if(p->connected||p->connections!=connections){
            // open重入会重建请求头；先关闭未完成的头，正文尚未发送，不能在原连接重复头。
            // open rebuilds request headers on reentry; restart an incomplete header on a fresh connection before any body is sent.
            if(p->header_restarts++>=3)return ESP_FAIL;
            esp_http_client_close(client);p->connected=false;
#if defined(ESP_PLATFORM)
            connect_started=client_now_ms();
#endif
        }
#if defined(ESP_PLATFORM)
        else if(!p->connected&&!p->hard_deadline&&p->idle_ms==15000&&connect_retries<2&&
                client_now_ms()-connect_started>=3500){
            ESP_LOGW("weread","http connect stalled 3.5s (#%u), retrying peer=%s",connect_retries+1,p->peer);
            esp_http_client_close(client);p->connected=false;
            connect_started=client_now_ms();++connect_retries;
            p->deadline=connect_started+(connect_retries==1?11500:8000);
        }
#endif
        http_poll_pause();
    }
}
static int http_poll_write(esp_http_client_handle_t client,const char *data,int bytes,http_poll_t *p) {
    if(!p)return esp_http_client_write(client,data,bytes);
    for(;;){
        if(http_poll_guard(p)!=ESP_OK)return -1;
        weread_client_yield_for_display(p->cancel,p->cancel_ctx);
        errno=0;int n=esp_http_client_write(client,data,bytes);
        if(n>0){http_poll_progress(p);return n;}
        if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)return n;
        http_poll_pause();
    }
}
static int64_t http_poll_headers(esp_http_client_handle_t client,http_poll_t *p) {
    if(!p)return esp_http_client_fetch_headers(client);
    for(;;){
        if(http_poll_guard(p)!=ESP_OK)return -1;
        weread_client_yield_for_display(p->cancel,p->cancel_ctx);
        errno=0;int64_t n=esp_http_client_fetch_headers(client);
        if(n>=0){http_poll_progress(p);return n;}
        if(n!=-ESP_ERR_HTTP_EAGAIN&&errno!=EAGAIN&&errno!=EWOULDBLOCK)return n;
        http_poll_pause();
    }
}
static int http_poll_read(esp_http_client_handle_t client,char *data,int bytes,http_poll_t *p) {
    if(!p)return esp_http_client_read(client,data,bytes);
    for(;;){
        if(http_poll_guard(p)!=ESP_OK)return -1;
        weread_client_yield_for_display(p->cancel,p->cancel_ctx);
        errno=0;int n=esp_http_client_read(client,data,bytes);
        if(n>=0){if(n)http_poll_progress(p);return n;}
        if(n!=-ESP_ERR_HTTP_EAGAIN&&errno!=EAGAIN&&errno!=EWOULDBLOCK)return n;
        http_poll_pause();
    }
}
#else
typedef void http_poll_t;
#define http_poll_prepare(url,enabled,cancel,ctx,out) ((void)(url),(void)(enabled),(void)(cancel),(void)(ctx),*(out)=NULL,ESP_OK)
#define http_poll_config(p,cfg) ((void)(p),(void)(cfg))
#define http_poll_reset(p) ((void)(p))
#define http_poll_open(client,length,p) esp_http_client_open(client,length)
#define http_poll_write(client,data,bytes,p) esp_http_client_write(client,data,bytes)
#define http_poll_headers(client,p) esp_http_client_fetch_headers(client)
#define http_poll_read(client,data,bytes,p) esp_http_client_read(client,data,bytes)
#define http_poll_host(p) NULL
#define http_poll_error(p,fallback) (fallback)
#endif
