"""Compile actual session lifecycle functions with counting TLS/socket stubs."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1]/'src/ml_derp.c').read_text()
def function(start):
    a=s.index(start);return s[a:s.index('\n}',a)+2]
cleanup=function('static void derp_tls_cleanup(microlink_t *ml, ml_derp_conn_t *conn) {')
wrapper=function('static esp_err_t derp_connect_session(microlink_t *ml, ml_derp_conn_t *conn, uint16_t region) {')
stubs=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ML_EVT_DERP_CONNECTED 1
struct TLS {int live;};
typedef struct {bool connected,tls_initialized;int sockfd,events;uint16_t region;struct TLS ssl,ssl_conf,ctr_drbg,entropy;} ml_derp_conn_t;
typedef struct {ml_derp_conn_t derp,derp_remote[2];int events;} microlink_t;
static int live,closed,mode,home_clears,openfds;
static bool home_event;
static void init(struct TLS *c){assert(!c->live);c->live=1;live++;}
static void release(struct TLS *c){assert(c->live);c->live=0;live--;}
#define mbedtls_ssl_init init
#define mbedtls_ssl_config_init init
#define mbedtls_ctr_drbg_init init
#define mbedtls_entropy_init init
#define mbedtls_ssl_free release
#define mbedtls_ssl_config_free release
#define mbedtls_ctr_drbg_free release
#define mbedtls_entropy_free release
static void xEventGroupClearBits(int event,int bits){assert(event==9&&bits==1);home_clears++;home_event=false;}
static void ml_close_sock(int fd){assert(fd>=0);closed++;openfds--;assert(openfds>=0);}
static esp_err_t derp_connect_once(microlink_t *ml,ml_derp_conn_t *c,uint16_t region){
 assert(c->region==region&&c->events==ml->events);
 if(mode!=1){c->sockfd=7+region;openfds++;}
 if(mode==0){c->connected=true;if(c==&ml->derp)home_event=true;return 0;}
 return -1;
}
'''
main=r'''
int main(void){microlink_t ml={0};ml.events=9;ml.derp.sockfd=ml.derp_remote[0].sockfd=ml.derp_remote[1].sockfd=-1;
 mode=0;assert(derp_connect_session(&ml,&ml.derp,9)==0);assert(home_event&&live==4&&openfds==1);
 assert(derp_connect_session(&ml,&ml.derp_remote[0],20)==0);assert(derp_connect_session(&ml,&ml.derp_remote[1],3)==0);assert(live==12&&openfds==3);
 int before=home_clears;derp_tls_cleanup(&ml,&ml.derp_remote[0]);assert(home_clears==before&&home_event&&ml.derp.connected&&ml.derp_remote[1].connected&&live==8&&openfds==2);
 for(int i=0;i<100;i++){mode=(i%2)+1;assert(derp_connect_session(&ml,&ml.derp_remote[0],20)!=0);assert(live==8&&openfds==2&&home_event&&home_clears==before);}
 derp_tls_cleanup(&ml,&ml.derp_remote[1]);derp_tls_cleanup(&ml,&ml.derp);assert(live==0&&openfds==0&&!home_event);
 int final=closed;derp_tls_cleanup(&ml,&ml.derp);derp_tls_cleanup(&ml,&ml.derp_remote[0]);assert(closed==final);
 // TLS cleanup still works after another owner has already invalidated the fd.
 mode=0;assert(derp_connect_session(&ml,&ml.derp_remote[0],20)==0);ml_close_sock(ml.derp_remote[0].sockfd);ml.derp_remote[0].sockfd=-1;derp_tls_cleanup(&ml,&ml.derp_remote[0]);assert(live==0&&openfds==0);
 puts("DERP actual three-session cleanup: isolation, 100 failures, fd-less TLS, idempotency PASS");}
'''
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'test.c';exe=Path(d)/'test';source.write_text(stubs+cleanup+'\n'+wrapper+main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
once=function('static esp_err_t derp_connect_once(microlink_t *ml, ml_derp_conn_t *conn, uint16_t region) {')
assert 'ml_close_sock(sock)' not in once and 'sockfd = -1' not in once
