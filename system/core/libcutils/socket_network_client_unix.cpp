/*
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing, software 
** distributed under the License is distributed on an "AS IS" BASIS, 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#include <cutils/sockets.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>

static int toggle_O_NONBLOCK(int s) {
    int flags = fcntl(s, F_GETFL);
    if (flags == -1 || fcntl(s, F_SETFL, flags ^ O_NONBLOCK) == -1) {
        close(s);
        return -1;
    }
    return s;
}

// Connect to the given host and port.
// 'timeout' is in seconds (0 for no timeout).
// Returns a file descriptor or -1 on error.
// On error, check *getaddrinfo_error (for use with gai_strerror) first;
// if that's 0, use errno instead.
int socket_network_client_timeout(const char* host, int port, int type, int timeout,
                                  int* getaddrinfo_error) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo* addrs;
    *getaddrinfo_error = getaddrinfo(host, port_str, &hints, &addrs);
    if (*getaddrinfo_error != 0) {
        return -1;
    }

    int result = -1;
    for (struct addrinfo* addr = addrs; addr != NULL; addr = addr->ai_next) {
        // The Mac doesn't have SOCK_NONBLOCK.
        int s = socket(addr->ai_family, type, addr->ai_protocol);
        if (s == -1 || toggle_O_NONBLOCK(s) == -1) break;

        int rc = connect(s, addr->ai_addr, addr->ai_addrlen);
        if (rc == 0) {
            result = toggle_O_NONBLOCK(s);
            break;
        } else if (rc == -1 && errno != EINPROGRESS) {
            close(s);
            continue;
        }

        // libadb: poll, а не select. fd_set вмещает дескрипторы только до FD_SETSIZE (1024): в процессе
        // с тысячами открытых сокетов (сервер, к которому подключены тысячи ТВ) FD_SET(s) писал за
        // пределы fd_set на стеке — порча стека и SIGSEGV при подключении (test_022).
        struct pollfd pfd;
        pfd.fd = s;
        pfd.events = POLLIN | POLLOUT;
        pfd.revents = 0;
        do {
            rc = poll(&pfd, 1, (timeout != 0) ? timeout * 1000 : -1);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1) {
            close(s);
            break;
        }
        if (rc == 0) {  // we had a timeout
            errno = ETIMEDOUT;
            close(s);
            break;
        }

        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
            close(s);
            break;
        }

        if (error) {  // check if we had a socket error
            // TODO: Update the timeout.
            errno = error;
            close(s);
            continue;
        }

        result = toggle_O_NONBLOCK(s);
        break;
    }

    freeaddrinfo(addrs);
    return result;
}

int socket_network_client(const char* host, int port, int type) {
    int getaddrinfo_error;
    return socket_network_client_timeout(host, port, type, 0, &getaddrinfo_error);
}
