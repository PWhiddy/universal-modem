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
#define TCP_RELAY_FIRST_DATA_TIMEOUT_MS 30000u

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
    uint64_t next_job_id;
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
    uint64_t id;
    uint64_t accepted_ms;
    uint64_t first_client_ms;
    uint64_t upstream_connected_ms;
};

typedef enum {
    TCP_RELAY_STREAM_NOT_STARTED = 0,
    TCP_RELAY_STREAM_EOF,
    TCP_RELAY_STREAM_RECV_ERROR,
    TCP_RELAY_STREAM_SEND_ERROR
} tcp_relay_stream_outcome;

typedef struct {
    tcp_relay_stream_outcome outcome;
    int error_number;
    uint64_t first_byte_ms;
} tcp_relay_stream_result;

typedef struct {
    tcp_relay_job *job;
    int source;
    int destination;
    size_t *count;
    tcp_relay_stream_result *result;
} tcp_relay_copy;

static uint64_t relay_milliseconds(void)
{
    struct timespec time_value;
    if (clock_gettime(CLOCK_MONOTONIC, &time_value) != 0) {
        return 0u;
    }
    return (uint64_t)time_value.tv_sec * UINT64_C(1000) +
           (uint64_t)time_value.tv_nsec / UINT64_C(1000000);
}

static const char *relay_stream_outcome_name(tcp_relay_stream_outcome outcome)
{
    switch (outcome) {
    case TCP_RELAY_STREAM_EOF:
        return "eof";
    case TCP_RELAY_STREAM_RECV_ERROR:
        return "recv-error";
    case TCP_RELAY_STREAM_SEND_ERROR:
        return "send-error";
    default:
        return "not-started";
    }
}

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
    while (1) {
        ssize_t count = recv(copy->source, buffer, sizeof(buffer), 0);
        if (count > 0) {
            if (copy->result->first_byte_ms == 0u) {
                copy->result->first_byte_ms = relay_milliseconds();
            }
            if (relay_send_all(copy->destination, buffer,
                               (size_t)count) != UM_OK) {
                copy->result->outcome = TCP_RELAY_STREAM_SEND_ERROR;
                copy->result->error_number = errno;
                break;
            }
            *copy->count += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            copy->result->outcome = TCP_RELAY_STREAM_RECV_ERROR;
            copy->result->error_number = errno;
        } else {
            copy->result->outcome = TCP_RELAY_STREAM_EOF;
        }
        break;
    }
    if (copy->result->outcome == TCP_RELAY_STREAM_RECV_ERROR ||
        copy->result->outcome == TCP_RELAY_STREAM_SEND_ERROR) {
        relay_abort_job(copy->job);
    } else {
        (void)shutdown(copy->destination, SHUT_WR);
    }
    return NULL;
}

/* Wait until the local TCP stack can deliver application bytes before
 * opening the Internet-facing connection. This isolates remote servers from
 * the acoustic link's potentially long packet-assembly delay. A bounded
 * fallback preserves protocols in which the server speaks first. */
static int relay_receive_first_client_data(
    tcp_relay_job *job, uint8_t *buffer, size_t capacity, size_t *length,
    int *timed_out, tcp_relay_stream_result *result)
{
    uint64_t started = relay_milliseconds();
    *length = 0u;
    *timed_out = 0;
    while (relay_milliseconds() - started <
           TCP_RELAY_FIRST_DATA_TIMEOUT_MS) {
        struct pollfd item;
        int poll_status;
        item.fd = job->client;
        item.events = POLLIN;
        item.revents = 0;
        poll_status = poll(&item, 1u, 250);
        if (poll_status < 0 && errno == EINTR) {
            continue;
        }
        if (poll_status < 0) {
            result->outcome = TCP_RELAY_STREAM_RECV_ERROR;
            result->error_number = errno;
            return UM_ERR_NETWORK;
        }
        if (relay_is_stopping(job->relay) != 0) {
            result->outcome = TCP_RELAY_STREAM_RECV_ERROR;
            result->error_number = ECANCELED;
            return UM_ERR_NETWORK;
        }
        if (poll_status == 0) {
            continue;
        }
        while (1) {
            ssize_t count = recv(job->client, buffer, capacity, 0);
            if (count > 0) {
                *length = (size_t)count;
                result->first_byte_ms = relay_milliseconds();
                job->first_client_ms = result->first_byte_ms;
                return UM_OK;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                result->outcome = TCP_RELAY_STREAM_RECV_ERROR;
                result->error_number = errno;
                return UM_ERR_NETWORK;
            }
            result->outcome = TCP_RELAY_STREAM_EOF;
            return UM_ERR_NETWORK;
        }
    }
    *timed_out = 1;
    return UM_OK;
}

static int relay_connect(tcp_relay_job *job)
{
    um_tcp_relay *relay = job->relay;
    int upstream;
    int flags;
    int status;
    uint64_t started = relay_milliseconds();
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
    while (status != 0 &&
           relay_milliseconds() - started < TCP_RELAY_CONNECT_TIMEOUT_MS) {
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
    tcp_relay_stream_result reverse_result;
    tcp_relay_stream_result forward_result;
    pthread_t reverse_thread;
    uint8_t first_client_data[TCP_RELAY_BUFFER_BYTES];
    size_t first_client_length = 0u;
    uint64_t connect_started = 0u;
    uint64_t finished_ms;
    int first_data_timed_out = 0;
    char address[INET_ADDRSTRLEN] = "unknown";
    int reverse_started = 0;
    socklen_t destination_length = sizeof(job->destination);

    memset(&reverse_result, 0, sizeof(reverse_result));
    memset(&forward_result, 0, sizeof(forward_result));
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
    if (relay_receive_first_client_data(
            job, first_client_data, sizeof(first_client_data),
            &first_client_length, &first_data_timed_out,
            &forward_result) != UM_OK) {
        goto finished;
    }
    connect_started = relay_milliseconds();
    if (relay_connect(job) != UM_OK) {
        relay_log(relay, "TCP relay connection to %s:%u failed: %s",
                  address, (unsigned)ntohs(job->destination.sin_port),
                  strerror(errno));
        goto finished;
    }
    job->upstream_connected_ms = relay_milliseconds();
    relay_log(relay,
              "TCP relay id=%llu upstream-open %s:%u trigger=%s "
              "first-bytes=%zu client-wait=%llums connect=%llums",
              (unsigned long long)job->id, address,
              (unsigned)ntohs(job->destination.sin_port),
              first_data_timed_out != 0 ? "server-first-timeout" :
                                          "client-data",
              first_client_length,
              (unsigned long long)(
                  (job->first_client_ms != 0u ? job->first_client_ms :
                                                connect_started) -
                  job->accepted_ms),
              (unsigned long long)(job->upstream_connected_ms -
                                   connect_started));

    reverse.job = job;
    reverse.source = job->upstream;
    reverse.destination = job->client;
    reverse.count = &job->downloaded;
    reverse.result = &reverse_result;
    if (pthread_create(&reverse_thread, NULL, relay_copy_thread,
                       &reverse) != 0) {
        relay_log(relay, "TCP relay could not start reverse stream");
        goto finished;
    }
    reverse_started = 1;
    if (first_client_length != 0u) {
        if (relay_send_all(job->upstream, first_client_data,
                           first_client_length) != UM_OK) {
            forward_result.outcome = TCP_RELAY_STREAM_SEND_ERROR;
            forward_result.error_number = errno;
            relay_abort_job(job);
        } else {
            job->uploaded += first_client_length;
        }
    }
    forward.job = job;
    forward.source = job->client;
    forward.destination = job->upstream;
    forward.count = &job->uploaded;
    forward.result = &forward_result;
    if (forward_result.outcome != TCP_RELAY_STREAM_SEND_ERROR) {
        (void)relay_copy_thread(&forward);
    }
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
    finished_ms = relay_milliseconds();
    relay_log(relay,
              "TCP relay closed %s:%u upload=%zuB download=%zuB id=%llu "
              "lifetime=%llums upstream-open=%s client-end=%s "
              "client-errno=%d upstream-end=%s upstream-errno=%d "
              "first-client=%llums first-upstream=%llums",
              address, (unsigned)ntohs(job->destination.sin_port),
              job->uploaded, job->downloaded,
              (unsigned long long)job->id,
              (unsigned long long)(finished_ms - job->accepted_ms),
              job->upstream_connected_ms != 0u ? "yes" : "no",
              relay_stream_outcome_name(forward_result.outcome),
              forward_result.error_number,
              relay_stream_outcome_name(reverse_result.outcome),
              reverse_result.error_number,
              (unsigned long long)(
                  forward_result.first_byte_ms != 0u
                      ? forward_result.first_byte_ms - job->accepted_ms
                      : 0u),
              (unsigned long long)(
                  reverse_result.first_byte_ms != 0u
                      ? reverse_result.first_byte_ms - job->accepted_ms
                      : 0u));
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
        job->accepted_ms = relay_milliseconds();
        (void)pthread_mutex_lock(&relay->mutex);
        if (relay->stopping != 0 ||
            relay->active_jobs >= TCP_RELAY_MAX_CONNECTIONS) {
            (void)pthread_mutex_unlock(&relay->mutex);
            (void)close(client);
            free(job);
            continue;
        }
        job->id = ++relay->next_job_id;
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
