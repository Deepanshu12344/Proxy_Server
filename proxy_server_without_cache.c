#include <stdio.h>
#include <netdb.h>
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

int connectRemoteServer(const char *host, const char *port) {
    struct addrinfo hints, *res, *p;
    int sockfd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
            continue;

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }

        break;
    }

    freeaddrinfo(res);

    if (p == NULL) {
        fprintf(stderr, "Failed to connect to remote server\n");
        return -1;
    }

    return sockfd;
}

void sendErrorMessage(int socket_fd, int status_code) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer),
             "HTTP/1.0 %d Internal Server Error\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: 21\r\n\r\n"
             "500 Internal Error\n",
             status_code);
    send(socket_fd, buffer, strlen(buffer), 0);
}

int handle_request(int client_socketID, struct ParsedRequest *request) {
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

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", server_port);
    int remoteSocketID = connectRemoteServer(request->host, port_str);
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
        struct ParsedRequest *request = ParsedRequest_create();
        if (ParsedRequest_parse(request, buffer, strlen(buffer)) < 0) {
            printf("Parsing failed\n");
        } else {
            if (!strcmp(request->method, "GET")) {
                if (request->host && request->path) {
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

    free(socketNew);
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, &p);
    printf("Semaphore post value is %d\n", p);
    return NULL;
}

int main(int argc, char *argv[]) {
    int sock_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;

    // Check command line argument
    if (argc == 2) {
        port_number = atoi(argv[1]);
    } else {
        printf("Usage: %s <port_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Create socket
    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Reuse port
    int reuse = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&reuse, sizeof(reuse)) < 0) {
        perror("setsockopt error");
        exit(EXIT_FAILURE);
    }

    // Setup server address
    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind
    if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen
    if (listen(sock_fd, MAX_CLIENTS) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Proxy server listening on port %d...\n", port_number);

    // Initialize semaphore
    sem_init(&semaphore, 0, MAX_CLIENTS);

    int i = 0;
    int connected_socketID[MAX_CLIENTS];

    while (1) {
        client_len = sizeof(client_addr);
        client_fd = accept(sock_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        connected_socketID[i] = client_fd;

        // Print client IP and port
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), str, INET_ADDRSTRLEN);
        printf("Client connected from %s:%d\n", str, ntohs(client_addr.sin_port));

        // Create thread for handling this client
        int *pclient = malloc(sizeof(int));
        *pclient = client_fd;
        pthread_create(&tid[i], NULL, thread_fn, pclient);
        i = (i + 1) % MAX_CLIENTS;
    }

    close(sock_fd);
    return 0;
}