"""Compile the actual production cleanup + connect wrapper with counting TLS stubs."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1]/'src/ml_derp.c').read_text()
def function(start):
    a=s.index(start);b=s.index('\n}',a)+2;return s[a:b]
cleanup=function('static void derp_tls_cleanup(')
wrapper=function('esp_err_t ml_derp_connect(')
stubs=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ML_EVT_DERP_CONNECTED 1
struct TLS {int live;};
typedef struct {struct {bool connected,tls_initialized;int sockfd;struct TLS ssl,ssl_conf,ctr_drbg,entropy;} derp;int events;} microlink_t;
static int live,closed,mode;
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
static void xEventGroupClearBits(int event,int bits){(void)event;(void)bits;}
static void ml_close_sock(int fd){assert(fd>=0);closed++;}
static esp_err_t derp_connect_once(microlink_t *ml){if(mode!=1)ml->derp.sockfd=7;return mode==0?0:-1;}
'''
main=r'''
int main(void){microlink_t ml={0};ml.derp.sockfd=-1;
 for(int i=0;i<100;i++){mode=(i%2)+1;assert(ml_derp_connect(&ml)!=0);assert(live==0);assert(ml.derp.sockfd==-1);}
 assert(closed==50);mode=0;assert(ml_derp_connect(&ml)==0);assert(live==4);derp_tls_cleanup(&ml);assert(live==0&&closed==51);
 derp_tls_cleanup(&ml);assert(closed==51);
 // Reproduce former failure state: TLS alive, socket already marked invalid.
 mode=0;assert(ml_derp_connect(&ml)==0);ml.derp.sockfd=-1;derp_tls_cleanup(&ml);assert(live==0);
 puts("DERP actual cleanup/wrapper 100 retries, fd-less TLS and idempotency PASS");}
'''
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'test.c';exe=Path(d)/'test';source.write_text(stubs+cleanup+'\n'+wrapper+main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# All connection failures must unwind through the wrapper; never lose an owned fd.
once=function('static esp_err_t derp_connect_once(')
assert 'ml_close_sock(sock)' not in once and 'sockfd = -1' not in once
