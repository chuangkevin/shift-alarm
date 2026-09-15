#pragma once
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
static inline esp_err_t alarm_tailnet_open_tcp(const char *ipv4,uint16_t port,uint32_t timeout_ms,int *out_fd){
 (void)timeout_ms;struct sockaddr_in destination={};destination.sin_family=AF_INET;destination.sin_port=htons(port);
 if(inet_pton(AF_INET,ipv4,&destination.sin_addr)!=1)return ESP_FAIL;
 int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(fd<0)return ESP_FAIL;
 if(connect(fd,(struct sockaddr*)&destination,sizeof(destination))!=0){close(fd);return ESP_FAIL;}
 *out_fd=fd;return ESP_OK;
}
