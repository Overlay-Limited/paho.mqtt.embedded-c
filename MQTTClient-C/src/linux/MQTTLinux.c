/*******************************************************************************
 * Copyright (c) 2014, 2017 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Allan Stockdill-Mander - initial API and implementation and/or initial documentation
 *    Ian Craggs - return codes from linux_read
 *******************************************************************************/

#include <pthread.h>
#include <sys/ioctl.h>
#include "MQTTLinux.h"
#include "../MQTTClient.h"
#include <poll.h>

void TimerInit(Timer *timer) {
    timer->end_time = (struct timeval) {0, 0};
}

char TimerIsExpired(Timer *timer) {
    struct timeval now, res;
    gettimeofday(&now, NULL);
    timersub(&timer->end_time, &now, &res);
    return res.tv_sec < 0 || (res.tv_sec == 0 && res.tv_usec <= 0);
}


void TimerCountdownMS(Timer *timer, unsigned int timeout) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timeval interval = {timeout / 1000, (timeout % 1000) * 1000};
    timeradd(&now, &interval, &timer->end_time);
}


void TimerCountdown(Timer *timer, unsigned int timeout) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timeval interval = {timeout, 0};
    timeradd(&now, &interval, &timer->end_time);
}


int TimerLeftMS(Timer *timer) {
    struct timeval now, res;
    gettimeofday(&now, NULL);
    timersub(&timer->end_time, &now, &res);
    return (res.tv_sec < 0) ? 0 : res.tv_sec * 1000 + res.tv_usec / 1000;
}

/* Wait for a descriptor */
static int block_until(int fd, short event, int timeout) {
    struct pollfd fds[1];
    fds[0].fd = fd;
    fds[0].events = event;
    poll(fds, 1, timeout);
    return fds[0].revents & event;
}

int linux_read(Network *n, unsigned char *buffer, int len, int timeout_ms) {
    if (!block_until(n->my_socket, POLLIN | POLLRDBAND, timeout_ms)) {
        return 0;
    }

    struct timeval interval = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (interval.tv_sec < 0 || (interval.tv_sec == 0 && interval.tv_usec <= 0)) {
        interval.tv_sec = 0;
        interval.tv_usec = 100;
    }

    int rc;
    rc = pthread_mutex_lock(&n->mutex);
    if (rc != EXIT_SUCCESS) {
        printf("Lock Fail | rc: %d\n", rc);
        return FAILURE;
    }
    rc = setsockopt(n->my_socket, SOL_SOCKET, SO_RCVTIMEO, (char *) &interval, sizeof(struct timeval));
    if (rc != EXIT_SUCCESS) {
        printf("Socket Operation Failed\n");
    }

    int bytes = 0;
    while (bytes < len) {
        rc = (int) recv(n->my_socket, &buffer[bytes], (size_t) (len - bytes), 0);

        if (rc == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                bytes = -1;
            break;
        } else if (rc == 0) {
            bytes = 0;
            break;
        } else
            bytes += rc;
    }

    pthread_mutex_unlock(&n->mutex);
    return bytes;
}


int linux_write(Network *n, unsigned char *buffer, int len, int timeout_ms) {
    if (!block_until(n->my_socket, POLLOUT | POLLWRBAND, timeout_ms)) {
        return 0;
    }

    int sec, usec;
    sec = timeout_ms / 1000;
    usec = timeout_ms % 1000 * 1000;
    struct timeval tv;
    tv.tv_sec = sec;  /* 30 Secs Timeout */
    tv.tv_usec = usec;  // Not init'ing this can cause strange errors

    int rc;
    rc = pthread_mutex_lock(&n->mutex);
    if (rc != EXIT_SUCCESS) {
        printf("Lock Fail | rc: %d\n", rc);
        return FAILURE;
    }

    rc = setsockopt(n->my_socket, SOL_SOCKET, SO_SNDTIMEO, (char *) &tv, sizeof(struct timeval));
    if (rc != EXIT_SUCCESS) {
        printf("BAD\n");
    }

    rc = (int) send(n->my_socket, buffer, len, MSG_NOSIGNAL);
    if (rc < 0) {
        printf("Error Sending | rc: %d | errno: %d\n", rc, errno);
        if (errno == EPIPE || errno ==  ECONNRESET) {
            rc = NETWORK_FAILURE;
        }
    }
    pthread_mutex_unlock(&n->mutex);
    return rc;
}


void NetworkInit(Network *n) {
    n->my_socket = 0;
    n->mqttread = linux_read;
    n->mqttwrite = linux_write;
    pthread_mutex_init(&n->mutex, NULL);
}


int NetworkConnect(Network *n, char *addr, int port) {
    int type = SOCK_STREAM;
    struct sockaddr_in address;
    int rc = -1;
    sa_family_t family = AF_INET;
    struct addrinfo *result = NULL;
    struct addrinfo hints = {0, AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP, 0, NULL, NULL, NULL};

    if ((rc = getaddrinfo(addr, NULL, &hints, &result)) == 0) {
        struct addrinfo *res = result;

        /* prefer ip4 addresses */
        while (res) {
            if (res->ai_family == AF_INET) {
                result = res;
                break;
            }
            res = res->ai_next;
        }

        if (result->ai_family == AF_INET) {
            address.sin_port = htons(port);
            address.sin_family = family = AF_INET;
            address.sin_addr = ((struct sockaddr_in *) (result->ai_addr))->sin_addr;
        } else
            rc = -1;

        freeaddrinfo(result);
    }

    if (rc == 0) {
        n->my_socket = socket(family, type, 0);
        if (n->my_socket != -1)
            rc = connect(n->my_socket, (struct sockaddr *) &address, sizeof(address));
        else
            rc = -1;
    }

    // Set as High Priority
    int optval = 7; // valid values are in the range [1,7]
    setsockopt(n->my_socket, SOL_SOCKET, SO_PRIORITY, &optval, sizeof(optval));

    return rc;
}


void NetworkDisconnect(Network *n) {
    close(n->my_socket);
}


void NetworkDestroy(Network *n) {
    pthread_mutex_destroy(&n->mutex);
}
