/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_client.h"
#include "weread_auth.h"
#include "esp_http_client.h"
#include "http_parser.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

esp_http_client_handle_t actual_init(const esp_http_client_config_t *);
esp_err_t actual_open(esp_http_client_handle_t,int);
int actual_write(esp_http_client_handle_t,const char *,int);
static atomic_bool cancel_flag;
static atomic_uint reached;
static unsigned mode,opens,writes,reads,configured;
static unsigned header_polls;
static int (*event_handler)(esp_http_client_event_t*);
static void *event_user;
static esp_http_client_handle_t active;
static http_parser parser;
static http_parser_settings settings;
static char field[128],value[2048],decoded[2048];
static size_t decoded_used,decoded_read;
static bool have_value,headers_done,message_done;
static int status;
static int64_t content_length;
static const char *request_body="{\"bookId\":\"fixture\",\"chapterOffset\":123,\"summary\":\"partial transport\"}";
static char *response;static size_t response_len;static int response_status;static esp_err_t result;
static uint64_t millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static bool cancelled(void *unused){(void)unused;return atomic_load(&cancel_flag);}
static int budget(void *unused){(void)unused;return mode==10||mode==15?80:0;}
unsigned weread_test_http_idle_ms(const char *url){(void)url;return mode>=21?0:mode>=13&&mode<=16?20:15000;}
esp_err_t weread_test_http_dns(const char *host,char *out,size_t cap,weread_cancel_fn cancel,void *ctx,uint64_t deadline){
    assert(host&&cap>=10);
    if(mode==1){atomic_store(&reached,1);while(!cancelled(NULL)&&!(cancel&&cancel(ctx))&&millis()<deadline)usleep(1000);return ESP_ERR_INVALID_STATE;}
    if(mode==9)return ESP_ERR_NO_MEM;
    strcpy(out,"127.0.0.1");return ESP_OK;
}
static void header_event(void){
    if(!field[0])return;
    if(event_handler){esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_HEADER,.client=active,.user_data=event_user,.header_key=field,.header_value=value};assert(event_handler(&event)==ESP_OK);}
    field[0]=value[0]=0;have_value=false;
}
static int parser_field(http_parser *p,const char *text,size_t bytes){(void)p;if(have_value)header_event();assert(strlen(field)+bytes<sizeof(field));strncat(field,text,bytes);return 0;}
static int parser_value(http_parser *p,const char *text,size_t bytes){(void)p;have_value=true;assert(strlen(value)+bytes<sizeof(value));strncat(value,text,bytes);return 0;}
static int parser_headers(http_parser *p){header_event();headers_done=true;status=p->status_code;content_length=p->content_length==UINT64_MAX?-1:(int64_t)p->content_length;return 0;}
static int parser_body(http_parser *p,const char *text,size_t bytes){(void)p;assert(decoded_used+bytes<=sizeof(decoded));memcpy(decoded+decoded_used,text,bytes);decoded_used+=bytes;return 0;}
static int parser_done(http_parser *p){(void)p;message_done=true;return 0;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg){
    assert(cfg->is_async&&cfg->timeout_ms==50&&cfg->url==NULL&&!cfg->skip_cert_common_name_check&&cfg->crt_bundle_attach);
    assert(cfg->host&&!strcmp(cfg->host,"127.0.0.1")&&cfg->common_name&&strcmp(cfg->common_name,cfg->host));
    if(mode==11)return NULL;
    ++configured;opens=writes=reads=header_polls=0;
    decoded_used=decoded_read=0;headers_done=message_done=have_value=false;field[0]=value[0]=0;status=0;content_length=-1;
    http_parser_init(&parser,HTTP_RESPONSE);settings=(http_parser_settings){.on_header_field=parser_field,.on_header_value=parser_value,.on_headers_complete=parser_headers,.on_body=parser_body,.on_message_complete=parser_done};
    event_handler=cfg->event_handler;event_user=cfg->user_data;
    char url[2300];snprintf(url,sizeof(url),"https://%s:%d%s%s%s",cfg->common_name,cfg->port,cfg->path,cfg->query?"?":"",cfg->query?cfg->query:"");
    esp_http_client_config_t copy=*cfg;copy.url=url;copy.event_handler=NULL;copy.user_data=NULL;
    active=actual_init(&copy);return active;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client,int bytes){
    ++opens;
    if(mode==2||mode==10){atomic_store(&reached,2);errno=EAGAIN;return ESP_ERR_HTTP_EAGAIN;}
    if(opens<=3&&mode!=19&&mode!=20){errno=EAGAIN;return ESP_ERR_HTTP_EAGAIN;}
    char *host=NULL;assert(esp_http_client_get_header(client,"Host",&host)==ESP_OK&&host&&strncmp(host,"127.",4));
    esp_err_t err=actual_open(client,bytes);
    if(err==ESP_OK&&event_handler){esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_CONNECTED,.client=client,.user_data=event_user};assert(event_handler(&event)==ESP_OK);}
    if(err==ESP_OK&&(mode==20||(mode==19&&opens<=2))){errno=EAGAIN;return ESP_ERR_HTTP_EAGAIN;}
    return err;
}
int esp_http_client_write(esp_http_client_handle_t client,const char *data,int bytes){
    if(++writes%2==0){errno=EAGAIN;return -1;}
    return actual_write(client,data,bytes>3?3:bytes);
}
static int parse_more(esp_http_client_handle_t client){
    char raw[37];int n=(int)recv(esp_http_client_get_socket(client),raw,sizeof(raw),0);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return -ESP_ERR_HTTP_EAGAIN;
    if(n<=0)return n;
    size_t parsed=http_parser_execute(&parser,&settings,raw,(size_t)n);assert(parsed==(size_t)n);return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client){
    ++header_polls;
    if(mode==3||mode==14){atomic_store(&reached,3);errno=EAGAIN;return -ESP_ERR_HTTP_EAGAIN;}
    if(header_polls<=2){errno=EAGAIN;return -ESP_ERR_HTTP_EAGAIN;}
    if(mode==18)atomic_store(&reached,18);
    while(!headers_done){int n=parse_more(client);if(n<=0)return n?n:-1;}
    return content_length<0?0:content_length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client){assert(client==active);return status;}
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client){assert(client==active);return content_length;}
int esp_http_client_read(esp_http_client_handle_t client,char *out,int capacity){
    if(mode==4){atomic_store(&reached,4);errno=EAGAIN;return -ESP_ERR_HTTP_EAGAIN;}
    if(++reads%2==0){errno=EAGAIN;return -ESP_ERR_HTTP_EAGAIN;}
    while(decoded_read==decoded_used&&!message_done){decoded_read=decoded_used=0;int n=parse_more(client);if(n<=0)return n;}
    size_t n=decoded_used-decoded_read;if(n>(size_t)capacity)n=(size_t)capacity;if(n>7)n=7;
    memcpy(out,decoded+decoded_read,n);decoded_read+=n;return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client){assert(client==active);return message_done&&decoded_read==decoded_used;}
static void *run(void *unused){
    (void)unused;
    if(mode>=21&&mode<=23){
        char url[128];snprintf(url,sizeof(url),"https://i.weread.qq.com/foreground%u",mode);
        result=weread_client_request(url,"POST",NULL,request_body,&response,&response_len,&response_status);return NULL;
    }
    if(mode==24){
        weread_auth_clear_session();char uid[128],qr[512],code[128];weread_qr_state_t state;
        assert(weread_auth_start_qr_login(uid,sizeof(uid),qr,sizeof(qr))==ESP_OK&&!strcmp(uid,"poll-uid"));
        assert(weread_auth_poll_qr_login(uid,&state,code,sizeof(code))==ESP_OK&&state==WEREAD_QR_STATE_WAITING_SCAN);
        assert(weread_auth_poll_qr_login(uid,&state,code,sizeof(code))==ESP_OK&&state==WEREAD_QR_STATE_CONFIRMED&&!strcmp(code,"confirmed-code"));
        weread_auth_session_t session;result=weread_auth_exchange_code(code,&session);
        assert(result==ESP_OK&&!strcmp(session.vid,"456")&&!strcmp(session.access_token,"cookie-token")&&!strcmp(session.refresh_token,"cookie-refresh"));
        assert(weread_auth_load_session(&session)==ESP_OK&&!strcmp(session.access_token,"cookie-token"));return NULL;
    }
    if(mode==5||mode==6||mode==16){
        FILE *file=tmpfile();assert(file);weread_download_info_t info={0};
        result=weread_client_download(mode!=6?"https://res.weread.qq.com/redirect":"https://res.weread.qq.com/file",file,mode==6?64:2048,cancelled,NULL,&info);
        if(mode!=6){assert(result==ESP_OK&&info.status==200&&info.bytes==256);rewind(file);for(unsigned i=0;i<256;++i)assert(fgetc(file)==(int)('A'+i%26));}
        else assert(result==ESP_ERR_INVALID_SIZE&&info.bytes<=64);
        fclose(file);return NULL;
    }
    char url[256];snprintf(url,sizeof(url),"https://i.weread.qq.com:443/%s?utf8=%%E4%%B8%%80&n=1",mode==7?"expired":mode==12||mode==18?"slow":mode==17?"fragmented":"book/read");
    result=mode==7?weread_client_request(url,"POST",NULL,request_body,&response,&response_len,&response_status):
        weread_client_request_snapshot(url,"POST",NULL,request_body,&response,&response_len,&response_status);
    return NULL;
}
int main(int argc,char **argv){
    assert(argc==3);mode=(unsigned)strtoul(argv[2],NULL,10);assert(weread_auth_init(argv[1])==ESP_OK);
    weread_auth_session_t session={0};strcpy(session.vid,"123");strcpy(session.access_token,"fixture-token");strcpy(session.refresh_token,"fixture-refresh");
    assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    weread_client_set_cancel(cancelled,NULL);weread_client_set_timeout_budget(budget,NULL);weread_client_set_background(mode<21);weread_client_set_polling(true);
    if(mode==8){
        const char *bad[]={"https://qq.com/a\r\nX: y","https://qq.com/a b","https://qq.com/a\\b","https://qq.com/\tbad"};
        for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i){FILE*f=tmpfile();weread_download_info_t info={0};assert(weread_client_download(bad[i],f,64,cancelled,NULL,&info)==ESP_ERR_INVALID_ARG);fclose(f);}
        assert(!configured);puts("invalid raw URL rejected before DNS/HTTP");return 0;
    }
    uint64_t started=millis();pthread_t worker;assert(!pthread_create(&worker,NULL,run,NULL));
    if((mode>=1&&mode<=4)||mode==18){while(atomic_load(&reached)!=mode){assert(millis()-started<2000);usleep(1000);}started=millis();atomic_store(&cancel_flag,true);weread_client_abort_active();}
    assert(!pthread_join(worker,NULL));uint64_t elapsed=millis()-started;
    if((mode>=1&&mode<=4)||mode==18)assert(result==ESP_ERR_INVALID_STATE&&!response&&elapsed<100);
    else if(mode==21)assert(result==ESP_OK&&response_status==200&&weread_auth_is_authenticated()&&!strcmp(weread_auth_get_current_session()->access_token,"fresh-token"));
    else if(mode==22||mode==23)assert(result!=ESP_OK&&response_status==401&&!weread_auth_is_authenticated());
    else if(mode==24)assert(result==ESP_OK&&weread_auth_is_authenticated()&&elapsed>=16000);
    else if(mode==7)assert(result==ESP_OK&&response_status==401&&response&&strstr(response,"-2013")&&weread_auth_is_authenticated());
    else if(mode==9)assert(result==ESP_ERR_NO_MEM&&!configured&&!response);
    else if(mode==10)assert(result==ESP_ERR_TIMEOUT&&!response&&elapsed>=75&&elapsed<300);
    else if(mode==11)assert(result==ESP_FAIL&&!response);
    else if(mode==14||mode==15)assert(result==ESP_ERR_TIMEOUT&&!response&&elapsed<300);
    else if(mode==20)assert(result==ESP_FAIL&&!response&&opens==4);
    else if(mode!=5&&mode!=6&&mode!=16)assert(result==ESP_OK&&response_status==200&&response&&strstr(response,"true"));
    if(mode==12)assert(elapsed>=250);
    if(mode==13||mode==16)assert(elapsed>20);
    if(mode==17)assert(header_polls>=4);
    if(mode==19)assert(opens==3);
    free(response);printf("poll mode%u: result=%d elapsed=%llums opens=%u partial_writes=%u read_calls=%u\n",mode,result,(unsigned long long)elapsed,opens,writes,reads);return 0;
}
