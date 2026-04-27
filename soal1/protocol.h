#ifndef PROTOCOL_H
#define PROTOCOL_H

#define PORT 8080
#define MAX_CLIENTS 100
#define BUFFER_SIZE 2048
#define NAME_SIZE 64
#define LOG_FILE "history.log"
#define ADMIN_NAME "The Knights"
#define ADMIN_PASSWORD "protocol7"

void write_log(const char *level, const char *message);

#endif