/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_client.h"
#include "esp_http_client.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool cleaning,released;
static const char *target;
esp_err_t actual_http_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client){
    assert(!pthread_mutex_lock(&gate));cleaning=true;assert(!pthread_cond_broadcast(&changed));
    while(!released)assert(!pthread_cond_wait(&changed,&gate));
    assert(!pthread_mutex_unlock(&gate));return actual_http_cleanup(client);
}
static void *request(void *unused){
    (void)unused;char *body=NULL;size_t bytes=0;int status=0;
    assert(weread_client_request(target,"GET",NULL,NULL,&body,&bytes,&status)==ESP_OK&&status==200&&bytes);
    free(body);return NULL;
}
static uint64_t milliseconds(void){struct timespec now;assert(!clock_gettime(CLOCK_MONOTONIC,&now));return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
int main(int argc,char **argv){
    assert(argc==2);target=argv[1];pthread_t worker;assert(!pthread_create(&worker,NULL,request,NULL));
    assert(!pthread_mutex_lock(&gate));while(!cleaning)assert(!pthread_cond_wait(&changed,&gate));assert(!pthread_mutex_unlock(&gate));
    uint64_t started=milliseconds();weread_client_abort_active();uint64_t elapsed=milliseconds()-started;assert(elapsed<100);
    assert(!pthread_mutex_lock(&gate));released=true;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_join(worker,NULL));
    printf("PASS: cancel returns in%llums while actual client cleanup owns its handle lock\n",(unsigned long long)elapsed);return 0;
}
