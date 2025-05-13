#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "proxy_parse.h"
#include <stdlib.h>

#define MAX_CLIENTS 10
#define MAX_BYTES 8192

int port_number = 8090;
pthread_t tid[MAX_CLIENTS];
sem_t semaphore;

int handle_request(int client_socketID, ParsedRequest *request) {
    char *buf = (char *)malloc(sizeof(char) * MAX_BYTES);
    snprintf(buf, MAX_BYTES, "GET %s %s\r\n", request->path, request->version);
    size_t len = strlen(buf);

    ParsedHeader_set(request, "Connection", "close");

    if (ParsedHeader_get(request, "HOST") == NULL) {
        ParsedHeader_set(request, "HOST", request->host);
    }

    ParsedRequest_unparse_headers(request, buf + len, MAX_BYTES - len);

    int server_port = 80;
    if (request->port != NULL) {
        server_port = atoi(request->port);
    }

    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if (remoteSocketID < 0) {
        free(buf);
        return -1;
    }

    send(remoteSocketID, buf, strlen(buf), 0);
    bzero(buf, MAX_BYTES);

    int bytes_received;
    while ((bytes_received = recv(remoteSocketID, buf, MAX_BYTES, 0)) > 0) {
        send(client_socketID, buf, bytes_received, 0);
        bzero(buf, MAX_BYTES);
    }

    free(buf);
    close(remoteSocketID);
    return 0;
}


void *thread_fn(void *socketNew) {
    sem_wait(&semaphore);
    int p;
    sem_getvalue(&semaphore, &p);
    printf("Semaphore value is %d\n", p);

    int socket_fd = *(int *)socketNew;
    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);
    int bytes_received = recv(socket_fd, buffer, MAX_BYTES, 0);

    while (bytes_received > 0) {
        int len = strlen(buffer);
        if (strstr(buffer, "\r\n\r\n") == NULL) {
            bytes_received = recv(socket_fd, buffer + len, MAX_BYTES - len, 0);
        } else {
            break;
        }
    }

    if (bytes_received > 0) {
        ParsedRequest *request = ParsedRequest_create();
        if (ParsedRequest_parse(request, buffer, strlen(buffer)) < 0) {
            printf("Parsing failed\n");
        } else {
            if (!strcmp(request->method, "GET")) {
                if (request->host && request->path == 1) {
                    if (handle_request(socket_fd, request) == -1) {
                        sendErrorMessage(socket_fd, 500);
                    }
                } else {
                    sendErrorMessage(socket_fd, 400);
                }
            } else {
                sendErrorMessage(socket_fd, 501);
            }
        }
        ParsedRequest_destroy(request);
    }

    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, &p);
    printf("Semaphore post value is %d\n", p);
    return NULL;
}


int main(int argc, char argv[]){
        int sock_fd,client_fd;
        struct sockaddr_in server_addr,client_addr;
        socklen_t client_len;

        if(argc==2){
                port_number = atoi(agrv[1]);
        }else{
                printf("Too Few Arguments");
                exit(EXIT_FAILURE);
        }

        if(sock_fd = socket(AF_INET,SOCK_STREAM,0)<0){
                perror("socket failed");
                exit(EXIT_FAILURE);
        }

        int reuse = 1;
        int(setsockopt(sock_fd,SOL_SOCKET,SO_REUSEADDR,(const char*)&reuse, sizeof(reuse))<0){
                perror("setsockeopt error");
                exit(EXIT_FAILURE);
        }

        bzero((char *)&server_addr, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_number);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        if(bind(sock_fd,(struct sockaddr*)&sock_addr,sizeof(sock_addr))<0){
                perror("bind failed");
                exit(EXIT_FAILURE);
        }

        int listen_status=listen(proxy_socketID,MAX_CLIENTS);
        if(listen_status<0){
                perror("listen failed");
                exit(EXIT_FAILURE);
        }

        int i=0;
        int connected_socketID[MAX_CLIENTS];

        while(1){
                bzero((char *)&client_addr, sizeof(client_addr));
                client_len = sizeof(client_addr);

                client_socketID=accept(proxy_socketID,(struct sockaddr *)&client_addr, &client_len);
                if(client_socketID<0){
                        perror("accept failed");
                        exit(EXIT_FAILURE);
                }else{
                        connected_socketID[i]=client_socketID;
                }

                // can skip this just printing ip addr and port of client
                struct sockaddr_in * client_pt = (struct sockaddr_in *)&client_addr;
                struct in_addr ip_addr = client_pt->sin_addr;
                char str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
                printf("client is connected with %d port and %s IP\n",ntohs(client_addr.sin_port),str);

                pthread_create(&tid[i],NULL,thread_fn,(void *)connected_socketID[i]);
                i++;
        }

        close(proxy_socketID);
        return 0;
}