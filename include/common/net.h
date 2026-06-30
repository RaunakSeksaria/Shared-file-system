#ifndef NET_H
#define NET_H

// Small networking helpers for server creation, client connections,
// and simple line-based send/receive utilities.

#include <stddef.h>

int create_server_socket(const char *host, int port);
int connect_to_host(const char *host, int port);
int recv_line(int fd, char *buf, size_t buflen);
int send_all(int fd, const char *buf, size_t len);

#endif

