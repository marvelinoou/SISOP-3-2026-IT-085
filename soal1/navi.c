#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"

static int sock_fd;
static volatile int running = 1;

static void *recv_thread(void *arg) {
    (void)arg;
    char buf[BUFFER_SIZE];
    while (running) {
        int n = recv(sock_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (running) printf("\n[System] Disconnected from The Wired.\n");
            running = 0;
            exit(0);
        }
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }
    return NULL;
}

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
    send(sock_fd, "/exit", 5, 0);
    usleep(200000);
    close(sock_fd);
    exit(0);
}

int main() {
    signal(SIGINT, handle_sigint);

    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\nSocket creation error\n");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address / Address not supported\n");
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed\n");
        return -1;
    }

    pthread_t tid;
    int err = pthread_create(&tid, NULL, recv_thread, NULL);
    if (err != 0) {
        printf("\nThread can't be created : [%s]", strerror(err));
        return -1;
    }
    pthread_detach(tid);

    char buf[BUFFER_SIZE];
    while (running) {
        if (!fgets(buf, sizeof(buf), stdin)) break;
        if (!running) break;
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strlen(buf) == 0) continue;

        send(sock_fd, buf, strlen(buf), 0);

        if (strcmp(buf, "/exit") == 0) {
            usleep(200000);
            break;
        }
    }

    close(sock_fd);
    return 0;
}