#if defined(__linux__)
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "tcp_relay.h"

#if defined(__linux__)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_ORIGINAL_DST
#define SO_ORIGINAL_DST 80
#endif

#define TCP_RELAY_BACKLOG 64
#define TCP_RELAY_MAX_CONNECTIONS 64u
#define TCP_RELAY_BUFFER_BYTES 32768u
#define TCP_RELAY_CONNECT_TIMEOUT_MS 15000u

typedef struct tcp_relay_job tcp_relay_job;

struct um_tcp_relay {
    int listener;
    int transparent;
    int stopping;
    uint16_t listen_port;
    struct sockaddr_in fixed_destination;
    pthread_t accept_thread;
    pthread_mutex_t mutex;
    pthread_cond_t drained;
    tcp_relay_job *jobs;
    size_t active_jobs;
    um_log_callback logger;
    void *logger_context;
};

struct tcp_relay_job {
    um_tcp_relay *relay;
    tcp_relay_job *next;
    int client;
    int upstream;
    struct sockaddr_in destination;
    size_t uploaded;
    size_t downloaded;
};

typedef struct {
    tcp_relay_job *job;
    int source;
    int destination;
    size_t *count;
} tcp_relay_copy;

static void relay_log(um_tcp_relay *relay, const char *format, ...)
{
    char line[512];
    va_list arguments;
    if (relay == NULL || relay->logger == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    (void)pthread_mutex_lock(&relay->mutex);
    relay->logger(relay->logger_context, line);
    (void)pthread_mutex_unlock(&relay->mutex);
}

static int relay_is_stopping(um_tcp_relay *relay)
{
    int stopping;
    (void)pthread_mutex_lock(&relay->mutex);
    stopping = relay->stopping;
    (void)pthread_mutex_unlock(&relay->mutex);
    return stopping;
}

static void relay_abort_job(tcp_relay_job *job)
{
    if (job->client >= 0) {
        (void)shutdown(job->client, SHUT_RDWR);
    }
    if (job->upstream >= 0) {
        (void)shutdown(job->upstream, SHUT_RDWR);
    }
}

static int relay_send_all(int descriptor, const uint8_t *bytes,
                          size_t length)
{
    size_t offset = 0u;
    while (offset < length) {
        ssize_t count = send(descriptor, bytes + offset, length - offset,
                             MSG_NOSIGNAL);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return UM_ERR_NETWORK;
    }
    return UM_OK;
}

static void *relay_copy_thread(void *argument)
{
    tcp_relay_copy *copy = (tcp_relay_copy *)argument;
    uint8_t buffer[TCP_RELAY_BUFFER_BYTES];
    int failed = 0;
    while (1) {
        ssize_t count = recv(copy->source, buffer, sizeof(buffer), 0);
        if (count > 0) {
            if (relay_send_all(copy->destination, buffer,
                               (size_t)count) != UM_OK) {
                failed = 1;
                break;
            }
            *copy->count += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            failed = 1;
        }
        break;
    }
    if (failed != 0) {
        relay_abort_job(copy->job);
    } else {
        (void)shutdown(copy->destination, SHUT_WR);
    }
    return NULL;
}

static int relay_connect(tcp_relay_job *job)
{
    um_tcp_relay *relay = job->relay;
    int upstream;
    int flags;
    int status;
    uint64_t elapsed = 0u;
    upstream = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (upstream < 0) {
        return UM_ERR_NETWORK;
    }
    (void)pthread_mutex_lock(&relay->mutex);
    if (relay->stopping != 0) {
        (void)pthread_mutex_unlock(&relay->mutex);
        (void)close(upstream);
        return UM_ERR_NETWORK;
    }
    job->upstream = upstream;
    (void)pthread_mutex_unlock(&relay->mutex);
    flags = fcntl(job->upstream, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(job->upstream, F_SETFL, flags | O_NONBLOCK) != 0) {
        return UM_ERR_NETWORK;
    }
    status = connect(job->upstream,
                     (const struct sockaddr *)&job->destination,
                     sizeof(job->destination));
    if (status != 0 && errno != EINPROGRESS) {
        return UM_ERR_NETWORK;
    }
    while (status != 0 && elapsed < TCP_RELAY_CONNECT_TIMEOUT_MS) {
        struct pollfd item;
        int poll_status;
        int error = 0;
        socklen_t error_length = sizeof(error);
        item.fd = job->upstream;
        item.events = POLLOUT;
        item.revents = 0;
        poll_status = poll(&item, 1u, 250);
        if (poll_status < 0 && errno == EINTR) {
            continue;
        }
        if (poll_status < 0 || relay_is_stopping(job->relay) != 0) {
            return UM_ERR_NETWORK;
        }
        elapsed += 250u;
        if (poll_status == 0) {
            continue;
        }
        if (getsockopt(job->upstream, SOL_SOCKET, SO_ERROR, &error,
                       &error_length) != 0 || error != 0) {
            if (error != 0) {
                errno = error;
            }
            return UM_ERR_NETWORK;
        }
        status = 0;
    }
    if (status != 0 || fcntl(job->upstream, F_SETFL, flags) != 0) {
        return UM_ERR_NETWORK;
    }
    return UM_OK;
}

static void relay_remove_job(tcp_relay_job *job)
{
    um_tcp_relay *relay = job->relay;
    tcp_relay_job **cursor;
    (void)pthread_mutex_lock(&relay->mutex);
    cursor = &relay->jobs;
    while (*cursor != NULL && *cursor != job) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == job) {
        *cursor = job->next;
    }
    if (relay->active_jobs != 0u) {
        --relay->active_jobs;
    }
    (void)pthread_cond_broadcast(&relay->drained);
    (void)pthread_mutex_unlock(&relay->mutex);
}

static void *relay_connection_thread(void *argument)
{
    tcp_relay_job *job = (tcp_relay_job *)argument;
    um_tcp_relay *relay = job->relay;
    tcp_relay_copy reverse;
    tcp_relay_copy forward;
    pthread_t reverse_thread;
    char address[INET_ADDRSTRLEN] = "unknown";
    int reverse_started = 0;
    socklen_t destination_length = sizeof(job->destination);

    if (relay->transparent != 0) {
        if (getsockopt(job->client, SOL_IP, SO_ORIGINAL_DST,
                       &job->destination, &destination_length) != 0 ||
            destination_length != sizeof(job->destination) ||
            job->destination.sin_family != AF_INET) {
            relay_log(relay, "TCP relay could not recover original "
                             "destination: %s", strerror(errno));
            goto finished;
        }
    } else {
        job->destination = relay->fixed_destination;
    }
    (void)inet_ntop(AF_INET, &job->destination.sin_addr, address,
                    sizeof(address));
    if (relay_connect(job) != UM_OK) {
        relay_log(relay, "TCP relay connection to %s:%u failed: %s",
                  address, (unsigned)ntohs(job->destination.sin_port),
                  strerror(errno));
        goto finished;
    }

    reverse.job = job;
    reverse.source = job->upstream;
    reverse.destination = job->client;
    reverse.count = &job->downloaded;
    if (pthread_create(&reverse_thread, NULL, relay_copy_thread,
                       &reverse) != 0) {
        relay_log(relay, "TCP relay could not start reverse stream");
        goto finished;
    }
    reverse_started = 1;
    forward.job = job;
    forward.source = job->client;
    forward.destination = job->upstream;
    forward.count = &job->uploaded;
    (void)relay_copy_thread(&forward);
    (void)pthread_join(reverse_thread, NULL);

finished:
    if (reverse_started == 0) {
        relay_abort_job(job);
    }
    if (job->upstream >= 0) {
        (void)close(job->upstream);
    }
    if (job->client >= 0) {
        (void)close(job->client);
    }
    if (job->uploaded != 0u || job->downloaded != 0u) {
        relay_log(relay, "TCP relay closed %s:%u upload=%zuB "
                         "download=%zuB",
                  address, (unsigned)ntohs(job->destination.sin_port),
                  job->uploaded, job->downloaded);
    }
    relay_remove_job(job);
    free(job);
    return NULL;
}

static void relay_discard_job(um_tcp_relay *relay, tcp_relay_job *job)
{
    tcp_relay_job **cursor = &relay->jobs;
    while (*cursor != NULL && *cursor != job) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == job) {
        *cursor = job->next;
    }
    if (relay->active_jobs != 0u) {
        --relay->active_jobs;
    }
    (void)close(job->client);
    free(job);
}

static void *relay_accept_thread(void *argument)
{
    um_tcp_relay *relay = (um_tcp_relay *)argument;
    while (1) {
        tcp_relay_job *job;
        pthread_t thread;
        int client = accept4(relay->listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (relay_is_stopping(relay) != 0) {
                break;
            }
            relay_log(relay, "TCP relay accept failed: %s", strerror(errno));
            continue;
        }
        job = (tcp_relay_job *)calloc(1u, sizeof(*job));
        if (job == NULL) {
            (void)close(client);
            continue;
        }
        job->relay = relay;
        job->client = client;
        job->upstream = -1;
        (void)pthread_mutex_lock(&relay->mutex);
        if (relay->stopping != 0 ||
            relay->active_jobs >= TCP_RELAY_MAX_CONNECTIONS) {
            (void)pthread_mutex_unlock(&relay->mutex);
            (void)close(client);
            free(job);
            continue;
        }
        job->next = relay->jobs;
        relay->jobs = job;
        ++relay->active_jobs;
        if (pthread_create(&thread, NULL, relay_connection_thread, job) != 0) {
            relay_discard_job(relay, job);
            (void)pthread_mutex_unlock(&relay->mutex);
            continue;
        }
        (void)pthread_detach(thread);
        (void)pthread_mutex_unlock(&relay->mutex);
    }
    return NULL;
}

static int relay_open(um_tcp_relay **result, int transparent,
                      const char *destination_address,
                      uint16_t destination_port,
                      const char *listen_address,
                      um_log_callback logger, void *logger_context,
                      uint16_t *listen_port)
{
    um_tcp_relay *relay;
    struct sockaddr_in local;
    socklen_t local_length = sizeof(local);
    int reuse = 1;
    int mutex_ready = 0;
    int condition_ready = 0;
    if (result == NULL || listen_address == NULL || listen_port == NULL ||
        (transparent == 0 &&
         (destination_address == NULL || destination_port == 0u))) {
        return UM_ERR_ARGUMENT;
    }
    *result = NULL;
    relay = (um_tcp_relay *)calloc(1u, sizeof(*relay));
    if (relay == NULL) {
        return UM_ERR_MEMORY;
    }
    relay->listener = -1;
    relay->transparent = transparent;
    relay->logger = logger;
    relay->logger_context = logger_context;
    if (pthread_mutex_init(&relay->mutex, NULL) != 0) {
        goto failed;
    }
    mutex_ready = 1;
    if (pthread_cond_init(&relay->drained, NULL) != 0) {
        goto failed;
    }
    condition_ready = 1;
    if (transparent == 0) {
        relay->fixed_destination.sin_family = AF_INET;
        relay->fixed_destination.sin_port = htons(destination_port);
        if (inet_pton(AF_INET, destination_address,
                      &relay->fixed_destination.sin_addr) != 1) {
            goto failed;
        }
    }
    relay->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC,
                             IPPROTO_TCP);
    if (relay->listener < 0 ||
        setsockopt(relay->listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) != 0) {
        goto failed;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    if (inet_pton(AF_INET, listen_address, &local.sin_addr) != 1) {
        goto failed;
    }
    if (bind(relay->listener, (const struct sockaddr *)&local,
             sizeof(local)) != 0 ||
        listen(relay->listener, TCP_RELAY_BACKLOG) != 0 ||
        getsockname(relay->listener, (struct sockaddr *)&local,
                    &local_length) != 0) {
        goto failed;
    }
    relay->listen_port = ntohs(local.sin_port);
    if (pthread_create(&relay->accept_thread, NULL, relay_accept_thread,
                       relay) != 0) {
        goto failed;
    }
    *listen_port = relay->listen_port;
    *result = relay;
    return UM_OK;

failed:
    if (relay->listener >= 0) {
        (void)close(relay->listener);
    }
    if (condition_ready != 0) {
        (void)pthread_cond_destroy(&relay->drained);
    }
    if (mutex_ready != 0) {
        (void)pthread_mutex_destroy(&relay->mutex);
    }
    free(relay);
    return UM_ERR_NETWORK;
}

int um_tcp_relay_open_transparent(um_tcp_relay **relay,
                                  const char *listen_address,
                                  um_log_callback logger,
                                  void *logger_context,
                                  uint16_t *listen_port)
{
    return relay_open(relay, 1, NULL, 0u, listen_address, logger,
                      logger_context, listen_port);
}

int um_tcp_relay_open_fixed(um_tcp_relay **relay,
                            const char *destination_address,
                            uint16_t destination_port,
                            int loopback_only,
                            um_log_callback logger,
                            void *logger_context,
                            uint16_t *listen_port)
{
    return relay_open(relay, 0, destination_address, destination_port,
                      loopback_only != 0 ? "127.0.0.1" : "0.0.0.0",
                      logger, logger_context, listen_port);
}

void um_tcp_relay_close(um_tcp_relay *relay)
{
    tcp_relay_job *job;
    if (relay == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&relay->mutex);
    relay->stopping = 1;
    (void)shutdown(relay->listener, SHUT_RDWR);
    for (job = relay->jobs; job != NULL; job = job->next) {
        relay_abort_job(job);
    }
    (void)pthread_mutex_unlock(&relay->mutex);
    (void)pthread_join(relay->accept_thread, NULL);
    (void)close(relay->listener);
    (void)pthread_mutex_lock(&relay->mutex);
    while (relay->active_jobs != 0u) {
        (void)pthread_cond_wait(&relay->drained, &relay->mutex);
    }
    (void)pthread_mutex_unlock(&relay->mutex);
    (void)pthread_cond_destroy(&relay->drained);
    (void)pthread_mutex_destroy(&relay->mutex);
    free(relay);
}

#else

struct um_tcp_relay {
    int unused;
};

int um_tcp_relay_open_transparent(um_tcp_relay **relay,
                                  const char *listen_address,
                                  um_log_callback logger,
                                  void *logger_context,
                                  uint16_t *listen_port)
{
    (void)relay;
    (void)listen_address;
    (void)logger;
    (void)logger_context;
    (void)listen_port;
    return UM_ERR_UNSUPPORTED;
}

int um_tcp_relay_open_fixed(um_tcp_relay **relay,
                            const char *destination_address,
                            uint16_t destination_port,
                            int loopback_only,
                            um_log_callback logger,
                            void *logger_context,
                            uint16_t *listen_port)
{
    (void)relay;
    (void)destination_address;
    (void)destination_port;
    (void)loopback_only;
    (void)logger;
    (void)logger_context;
    (void)listen_port;
    return UM_ERR_UNSUPPORTED;
}

void um_tcp_relay_close(um_tcp_relay *relay)
{
    (void)relay;
}

#endif
