#include "proxy_parse.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>

#define MAX_CLIENTS 10
#define MAX_BYTES 4096
#define MAX_SIZE 200*(1<<20)     //size of the cache
#define MAX_ELEMENT_SIZE 10*(1<<20)     //max size of an element in cache

typedef struct cache_element cache_element;
struct cache_element{
        char* data;
        int len;
        char* url;
        time_t lru_time_track;
        cache_element* next;
};

cache_element* find(char* url);
int add_cache_element(char* data, int len, char* url);
void remove_cache_element();

int proxy_socketID;
int port_number = 8080;
pthread_t tid[MAX_CLIENTS];
sem_t semaphore;
pthread_mutex_t lock;

cache_element* head;
int cache_size;

int sendErrorMessage(int socket, int status_code)
{
        char str[1024];
        char currentTime[50];
        time_t now = time(0);

        struct tm data = *gmtime(&now);
        strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

        switch(status_code)
        {
                case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
                                  printf("400 Bad Request\n");
                                  send(socket, str, strlen(str), 0);
                                  break;

                case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
                                  printf("403 Forbidden\n");
                                  send(socket, str, strlen(str), 0);
                                  break;

                case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
                                  printf("404 Not Found\n");
                                  send(socket, str, strlen(str), 0);
                                  break;

                case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
                                  //printf("500 Internal Server Error\n");
                                  send(socket, str, strlen(str), 0);
                                  break;

                case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
                                  printf("501 Not Implemented\n");
                                  send(socket, str, strlen(str), 0);
                                  break;

                case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
                                  printf("505 HTTP Version Not Supported\n");
                                  send(socket, str, strlen(str), 0);
                                  break;

                default:  return -1;

        }
        return 1;
}

int connectRemoteServer(char* host_addr, int port_num){
        int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
        if(remoteSocket<0){
                printf("Error in creating your socket\n");
                return -1;
        }
        struct hostent* host = gethostbyname(host_addr);
        if(host==NULL){
                fprintf(stderr,"no such host exist\n");
                return -1;
        }
        struct sockaddr_in server_addr;
        bzero((char*)&server_addr, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_num);

        bcopy((char *)&host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);
        if(connect(remoteSocket,(struct sockaddr *)&server_addr,(size_t)sizeof(server_addr)<0)){
                fprintf(stderr,"Error in Connecting\n");
                return -1;
        }
        return remoteSocket;
}

int handle_request(int client_socketID,ParsedRequest *request, char* tempReq){
        char* buf = (char *)malloc(sizeof(char)*MAX_BYTES);
        strcpy(buf,"GET");
        strcat(buf,request->path);
        strcat(buf,"");
        strcat(buf,request->version);
        strcat(buf,"\r\n");

        size_t len = strlen(buf);

        if(ParsedHeader_set(request, "Connection", "close")<0){
                printf("set header key is not working");
        }

        if(ParsedHeader_get(request, "HOST")==NULL){
                if(ParsedHeader_set(request, "HOST", request->host)<0){
                        printf("set host header key is not working");
                }
        }

        if(ParsedRequest_unparse_headers(request, buf+len, (size_t)MAX_BYTES-len)<0){
                printf("unparse failed");
        }

        int server_port = 80;
        if(request->port !=NULL){
                server_port = atoi(request->port);
        }
        int remoteSocketID = connectRemoteServer(request->host, server_port);
        if(remoteSocketID<0){
                return -1;
        }

        int bytes_send = send(remoteSocketID, buf, strlen(buf),0);
        bzero(buf, MAX_BYTES);

        bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1,0);
        char * temp_buffer = (char *)malloc(sizeof(char)*MAX_BYTES);
        int temp_buffer_size = MAX_BYTES;
        int temp_buffer_index = 0;

        while(bytes_send>0){
                bytes_send = send(client_socketID, buf, bytes_send, 0);
                for(int i=0;i<bytes_send/sizeof(char);i++){
                        temp_buffer[temp_buffer_index] = buf[i];
                        temp_buffer_index++;
                }

                temp_buffer_size+=MAX_BYTES;
                temp_buffer = (char *)realloc(temp_buffer, temp_buffer_size);
                if(bytes_send<0){
                        perror("Error in sending data to the client\n");
                        break;
                }
                bzero(buf, MAX_BYTES);
                bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1,0);
        }
        temp_buffer[temp_buffer_index] = '\0';
        free(buf);
        add_cache_element(temp_buffer, strlen(temp_buffer), tempReq);
        free(temp_buffer);
        close(remoteSocketID);
        return 0;
}

int checkHTTPversion(char *msg){
        int version = -1;
        if(strncmp(msg, "HTTP/1.1", 8)==0){
                version = 1;
        }
        else if(strncmp(msg, "HTTP/1.0",8)==0){
                version = 1;
        }
        else{
                version = -1;
        }
        return version;
}



void *thread_fn(void *socketNew){
        sem_wait(&semaphore);
        int p;
        sem_getvalue(&semaphore,&p);
        printf("semophore value is %d\n",p);

        int *t = (int*) socketNew;
        int socket_fd = *(int *)socketNew;
        int bytes_send_client, len;

        char *buffer = (char *)calloc(MAX_BYTES,sizeof(char));
        bzero(buffer,MAX_BYTES);
        bytes_send_client=recv(socket_fd,buffer,MAX_BYTES,0);

        while(bytes_send_client>0){
                len = strlen(buffer);
                if(strstr(buffer, "\r\n\r\n")==NULL){
                        bytes_send_client=recv(socket_fd, buffer+len, MAX_BYTES-len,0);
                }else{
                        break;
                }
        }

        char *tempReq=(char *)malloc(strlen(buffer)*sizeof(char)+1);
        for(int i=0;i<strlen(buffer);i++){
                tempReq[i]=buffer[i];
        }
        struct cache_element* temp = find(tempReq);
        if(temp!=NULL){
                int size = temp->len/sizeof(char);
                int pos=0;
                char response[MAX_BYTES];
                while(pos<size){
                        bzero(response,MAX_BYTES);
                        for(int i=0;i<MAX_BYTES;i++){
                                response[i]=temp->data[i];
                                pos++;
                        }
                        send(socket_fd,response,MAX_BYTES,0);
                }
                printf("Data received from the cache\n");
                printf("%s\n\n",response);
        }else if(bytes_send_client>0){
                len = strlen(buffer);
                ParsedRequest *request = ParsedRequest_create();
                if(ParsedRequest_parse(request,buffer,len)<0){
                        printf("parsing failed");
                }else{
                        bzero(buffer,MAX_BYTES);
                        if(!strcmp(request->method,"GET")){
                                if(request->host && request->path && checkHTTPversion(request->version)==1){
                                        bytes_send_client=handle_request(socket_fd, request, tempReq);
                                        if(bytes_send_client==-1){
                                                sendErrorMessage(socket_fd, 500);
                                        }
                                }else{
                                        sendErrorMessage(socket_fd, 500);
                                }
                        }else{
                                printf("This code doesn't support any method other than GET\n");
                        }
                }
                ParsedRequest_destroy(request);
        }else if(bytes_send_client){
                printf("client is disconnected");
        }
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
        free(buffer);
        sem_post(&semaphore);
        sem_getvalue(&semaphore,&p);
        printf("Semaphore post value is %d\n",p);
        free(tempReq);
        return NULL;
}

int main(int argc, char* argv[]){
        int client_socketID;
        socklen_t client_len;
        struct sockaddr_in server_addr, client_addr;
        sem_init(&semaphore,0, MAX_CLIENTS);
        pthread_mutex_init(&lock, NULL);

        if(argc == 2){
                port_number = atoi(argv[1]);
        }else{
                printf("Too Few Arguments");
                exit(1);
        }
        printf("Starting proxy at port %d\n",port_number);

        proxy_socketID = socket(AF_INET,SOCK_STREAM,0);
        if(proxy_socketID<0){
                perror("socket failed");
                exit(EXIT_FAILURE);
        }

        int reuse = 1;
        if(setsockopt(proxy_socketID,SOL_SOCKET,SO_REUSEADDR,(const char*)&reuse, sizeof(reuse))<0){
                perror("setsockopt failed");
                exit(EXIT_FAILURE);
        }

        bzero((char *)&server_addr, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_number);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        if(bind(proxy_socketID,(struct sockaddr*)&server_addr,sizeof(server_addr))<0){
                perror("bind failed");
                exit(EXIT_FAILURE);
        }
        printf("Binding on port %d\n",port_number);

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

cache_element *find(char* url){
        cache_element * site = NULL;
        int temp_lock_val = pthread_mutex_lock(&lock);
        printf("Remove cache locked acquired %d\n", temp_lock_val);
        if(head!=NULL){
                site=head;
                while(site!=NULL){
                        if(!strcmp(site->url, url)){
                                printf("LRU time track before: %ld", site->lru_time_track);
                                printf("\n URL found \n");
                                site->lru_time_track = time(NULL);
                                printf("LRU time track after: %ld", site->lru_time_track);
                                break;
                        }
                        site = site->next;
                }
        }else{
                printf("URL not found");
        }
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Lock is removed\n");
        return site;
}

void remove_cache_element(){
    // If cache is not empty searches for the node which has the least lru_time_track and deletes it
        cache_element * p ;     // Cache_element Pointer (Prev. Pointer)
        cache_element * q ;             // Cache_element Pointer (Next Pointer)
        cache_element * temp;   // Cache element to remove
    //sem_wait(&cache_lock);
        int temp_lock_val = pthread_mutex_lock(&lock);
        printf("Remove Cache Lock Acquired %d\n",temp_lock_val);
        if( head != NULL) { // Cache != empty
                for (q = head, p = head, temp =head ; q -> next != NULL;
                        q = q -> next) { // Iterate through entire cache and search for oldest time track
                        if(( (q -> next) -> lru_time_track) < (temp -> lru_time_track)) {
                                temp = q -> next;
                                p = q;
                        }
                }
                if(temp == head) {
                        head = head -> next; /*Handle the base case*/
                } else {
                        p->next = temp->next;
                }
                cache_size = cache_size - (temp -> len) - sizeof(cache_element) -
                strlen(temp -> url) - 1;     //updating the cache size
                free(temp->data);
                free(temp->url); // Free the removed element
                free(temp);
        }
        //sem_post(&cache_lock);
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Remove Cache Lock Unlocked %d\n",temp_lock_val);
}

int add_cache_element(char* data,int size,char* url){
        // Adds element to the cache
        // sem_wait(&cache_lock);
        int temp_lock_val = pthread_mutex_lock(&lock);
        printf("Add Cache Lock Acquired %d\n", temp_lock_val);
        int element_size=size+1+strlen(url)+sizeof(cache_element); // Size of the new element which will be added to the cache
        if(element_size>MAX_ELEMENT_SIZE){
                //sem_post(&cache_lock);
                // If element size is greater than MAX_ELEMENT_SIZE we don't add the element to the cache
                temp_lock_val = pthread_mutex_unlock(&lock);
                printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
                // free(data);
                // printf("--\n");
                // free(url);
                return 0;
        }else
        {
                while(cache_size+element_size>MAX_SIZE){
                // We keep removing elements from cache until we get enough space to add the element
                remove_cache_element();
        }
        cache_element* element = (cache_element*) malloc(sizeof(cache_element)); // Allocating memory for the new cache element
        element->data= (char*)malloc(size+1); // Allocating memory for the response to be stored in the cache element
        strcpy(element->data,data);
        element -> url = (char*)malloc(1+( strlen( url )*sizeof(char)  )); // Allocating memory for the request to be stored in the cache element (as a key)
        strcpy( element -> url, url );
        element->lru_time_track=time(NULL);    // Updating the time_track
        element->next=head;
        element->len=size;
        head=element;
        cache_size+=element_size;
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
        //sem_post(&cache_lock);
        // free(data);
        // printf("--\n");
        // free(url);
        return 1;
    }
    return 0;
}