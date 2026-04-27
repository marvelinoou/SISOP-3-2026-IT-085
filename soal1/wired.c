#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"

#define STATE_WAIT_NAME 0
#define STATE_WAIT_PASS 1
#define STATE_CONNECTED 2
#define STATE_ADMIN     3

typedef struct {
    int fd;
    char name[NAME_SIZE];
    int state;
} Client;

static Client clients[MAX_CLIENTS];
static int n_clients = 0;
static int server_fd;
static time_t start_time;
static fd_set master_fds;
static int max_fd;

// ==================== HELPER ====================

static void send_msg(int fd, const char *msg) {
    send(fd, msg, strlen(msg), 0);
}

static void broadcast(const char *msg, int exclude_fd) {
    for (int i = 0; i < n_clients; i++) {
        if (clients[i].fd != exclude_fd && clients[i].state == STATE_CONNECTED)
            send_msg(clients[i].fd, msg);
    }
}

static int find_idx(int fd) {
    for (int i = 0; i < n_clients; i++)
        if (clients[i].fd == fd) return i;
    return -1;
}

static int name_taken(const char *name) {
    for (int i = 0; i < n_clients; i++)
        if (strcmp(clients[i].name, name) == 0) return 1;
    return 0;
}

static void remove_client(int idx) {
    close(clients[idx].fd);
    FD_CLR(clients[idx].fd, &master_fds);
    clients[idx] = clients[--n_clients];
}

static void send_admin_menu(int fd) {
    send_msg(fd,
        "\n=== THE KNIGHTS CONSOLE ===\n"
        "1. Check Active Entities (Users)\n"
        "2. Check Server Uptime\n"
        "3. Execute Emergency Shutdown\n"
        "4. Disconnect\n"
        "Command >> "
    );
}

// ==================== SIGNAL HANDLER ====================

static void handle_sigint(int sig) {
    (void)sig;
    write_log("System", "SERVER SHUTDOWN");
    for (int i = 0; i < n_clients; i++) close(clients[i].fd);
    close(server_fd);
    exit(0);
}

// ==================== STATE HANDLERS ====================

static void handle_disconnect(int idx) {
    char log_msg[256], notify[256];
    if (clients[idx].name[0]) {
        snprintf(log_msg, sizeof(log_msg), "User '%s' disconnected", clients[idx].name);
        write_log("System", log_msg);
        if (clients[idx].state == STATE_CONNECTED) {
            snprintf(notify, sizeof(notify),
                "[System] User '%s' has left The Wired.\n> ", clients[idx].name);
            broadcast(notify, clients[idx].fd);
        }
    }
    remove_client(idx);
}

static void handle_wait_name(int idx, const char *buf) {
    int fd = clients[idx].fd;

    if (strlen(buf) == 0) {
        send_msg(fd, "Enter your name: ");
        return;
    }

    if (name_taken(buf)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "[System] The identity '%s' is already synchronized in The Wired.\nEnter your name: ",
            buf);
        send_msg(fd, msg);
        return;
    }

    strncpy(clients[idx].name, buf, NAME_SIZE - 1);

    if (strcmp(buf, ADMIN_NAME) == 0) {
        clients[idx].state = STATE_WAIT_PASS;
        send_msg(fd, "Enter Password: ");
    } else {
        clients[idx].state = STATE_CONNECTED;
        char welcome[256];
        snprintf(welcome, sizeof(welcome), "--- Welcome to The Wired, %s ---\n> ", buf);
        send_msg(fd, welcome);

        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "User '%s' connected", buf);
        write_log("System", log_msg);

        char notify[256];
        snprintf(notify, sizeof(notify),
            "[System] User '%s' has joined The Wired.\n> ", buf);
        broadcast(notify, fd);
    }
}

static void handle_wait_pass(int idx, const char *buf) {
    int fd = clients[idx].fd;

    if (strcmp(buf, ADMIN_PASSWORD) == 0) {
        clients[idx].state = STATE_ADMIN;
        send_msg(fd, "[System] Authentication Successful. Granted Admin privileges.");

        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "User '%s' connected", clients[idx].name);
        write_log("System", log_msg);

        send_admin_menu(fd);
    } else {
        send_msg(fd, "[System] Authentication Failed.\n[System] Disconnecting from The Wired...\n");
        remove_client(idx);
    }
}

static void handle_connected(int idx, const char *buf) {
    int fd = clients[idx].fd;

    if (strcmp(buf, "/exit") == 0) {
        send_msg(fd, "[System] Disconnecting from The Wired...\n");

        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "User '%s' disconnected", clients[idx].name);
        write_log("System", log_msg);

        char notify[256];
        snprintf(notify, sizeof(notify),
            "[System] User '%s' has left The Wired.\n> ", clients[idx].name);
        broadcast(notify, fd);
        remove_client(idx);
    } else {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "[%s]: %s\n> ", clients[idx].name, buf);
        broadcast(msg, fd);
        send_msg(fd, "> ");

        char log_msg[BUFFER_SIZE];
        snprintf(log_msg, sizeof(log_msg), "[%s]: %s", clients[idx].name, buf);
        write_log("User", log_msg);
    }
}

static void handle_admin(int idx, const char *buf) {
    int fd = clients[idx].fd;
    int choice = atoi(buf);
    char out[BUFFER_SIZE];

    switch (choice) {
        case 1: {
            write_log("Admin", "RPC_GET_USERS");
            int count = 0;
            strcpy(out, "Active Users:\n");
            for (int i = 0; i < n_clients; i++) {
                if (clients[i].state == STATE_CONNECTED) {
                    char line[128];
                    snprintf(line, sizeof(line), "  - %s\n", clients[i].name);
                    strcat(out, line);
                    count++;
                }
            }
            char total[64];
            snprintf(total, sizeof(total), "Total: %d user(s)\n", count);
            strcat(out, total);
            send_msg(fd, out);
            send_admin_menu(fd);
            break;
        }
        case 2: {
            write_log("Admin", "RPC_GET_UPTIME");
            long uptime = (long)(time(NULL) - start_time);
            snprintf(out, sizeof(out), "Server Uptime: %02ldh %02ldm %02lds\n",
                uptime / 3600, (uptime % 3600) / 60, uptime % 60);
            send_msg(fd, out);
            send_admin_menu(fd);
            break;
        }
        case 3: {
            write_log("Admin", "RPC_SHUTDOWN");
            write_log("System", "EMERGENCY SHUTDOWN INITIATED");
            const char *msg = "\n[System] Emergency shutdown initiated. Disconnecting...\n";
            for (int i = 0; i < n_clients; i++) {
                send_msg(clients[i].fd, msg);
                close(clients[i].fd);
            }
            close(server_fd);
            printf("[The Wired] Emergency shutdown by admin.\n");
            exit(0);
        }
        case 4: {
            send_msg(fd, "[System] Disconnecting from The Wired...\n");
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), "User '%s' disconnected", clients[idx].name);
            write_log("System", log_msg);
            remove_client(idx);
            break;
        }
        default:
            send_msg(fd, "Invalid command.\n");
            send_admin_menu(fd);
            break;
    }
}

// ==================== MAIN ====================

int main() {
    signal(SIGINT, handle_sigint);
    start_time = time(NULL);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    write_log("System", "SERVER ONLINE");
    printf("[The Wired] Server running on port %d\n", PORT);

    FD_ZERO(&master_fds);
    FD_SET(server_fd, &master_fds);
    max_fd = server_fd;

    while (1) {
        fd_set read_fds = master_fds;
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) continue;

        for (int fd = 0; fd <= max_fd; fd++) {
            if (!FD_ISSET(fd, &read_fds)) continue;

            if (fd == server_fd) {
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int new_fd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
                if (new_fd < 0 || n_clients >= MAX_CLIENTS) {
                    if (new_fd >= 0) close(new_fd);
                    continue;
                }
                FD_SET(new_fd, &master_fds);
                if (new_fd > max_fd) max_fd = new_fd;
                clients[n_clients].fd = new_fd;
                clients[n_clients].name[0] = '\0';
                clients[n_clients].state = STATE_WAIT_NAME;
                n_clients++;
                send_msg(new_fd, "Enter your name: ");
            } else {
                char buf[BUFFER_SIZE];
                int n = recv(fd, buf, sizeof(buf) - 1, 0);
                int idx = find_idx(fd);
                if (idx < 0) continue;

                if (n <= 0) {
                    handle_disconnect(idx);
                    continue;
                }

                buf[n] = '\0';
                buf[strcspn(buf, "\r\n")] = '\0';

                switch (clients[idx].state) {
                    case STATE_WAIT_NAME: handle_wait_name(idx, buf); break;
                    case STATE_WAIT_PASS: handle_wait_pass(idx, buf); break;
                    case STATE_CONNECTED: handle_connected(idx, buf); break;
                    case STATE_ADMIN:     handle_admin(idx, buf);     break;
                }
            }
        }
    }

    return 0;
}