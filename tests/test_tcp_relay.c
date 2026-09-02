#define _POSIX_C_SOURCE 200809L

#include "../src/tcp_relay.h"
#include "../src/network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int listener;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t accepted_condition;
    int accepted;
    uint16_t port;
} echo_server;

static volatile sig_atomic_t serve_stop;

static void fail(const char *message)
{
    fprintf(stderr, "tcp relay test failed: %s\n", message);
    exit(1);
}

static int send_all(int descriptor, const void *buffer, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    size_t offset = 0u;
    while (offset < length) {
        ssize_t count = send(descriptor, bytes + offset, length - offset, 0);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return 0;
        }
    }
    return 1;
}

static int connect_loopback(uint16_t port)
{
    struct sockaddr_in address;
    int descriptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (descriptor < 0) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(descriptor, (const struct sockaddr *)&address,
                sizeof(address)) != 0) {
        (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static void *echo_thread(void *argument)
{
    echo_server *server = (echo_server *)argument;
    uint8_t buffer[1024];
    int client = accept(server->listener, NULL, NULL);
    if (client >= 0) {
        ssize_t count;
        (void)pthread_mutex_lock(&server->mutex);
        server->accepted = 1;
        (void)pthread_cond_broadcast(&server->accepted_condition);
        (void)pthread_mutex_unlock(&server->mutex);
        do {
            count = recv(client, buffer, sizeof(buffer), 0);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            (void)send_all(client, buffer, (size_t)count);
        }
        (void)shutdown(client, SHUT_RDWR);
        (void)close(client);
    }
    return NULL;
}

static int echo_wait_for_accept(echo_server *server, unsigned timeout_ms)
{
    struct timespec deadline;
    int accepted;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        fail("could not read clock");
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_mutex_lock(&server->mutex);
    while (server->accepted == 0) {
        int status = pthread_cond_timedwait(&server->accepted_condition,
                                            &server->mutex, &deadline);
        if (status == ETIMEDOUT) {
            break;
        }
        if (status != 0) {
            (void)pthread_mutex_unlock(&server->mutex);
            fail("could not wait for echo accept");
        }
    }
    accepted = server->accepted;
    (void)pthread_mutex_unlock(&server->mutex);
    return accepted;
}

static void echo_open(echo_server *server)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int reuse = 1;
    memset(server, 0, sizeof(*server));
    if (pthread_mutex_init(&server->mutex, NULL) != 0 ||
        pthread_cond_init(&server->accepted_condition, NULL) != 0) {
        fail("could not initialize echo synchronization");
    }
    server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->listener < 0 ||
        setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) != 0) {
        fail("could not create echo listener");
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener, (const struct sockaddr *)&address,
             sizeof(address)) != 0 ||
        listen(server->listener, 1) != 0 ||
        getsockname(server->listener, (struct sockaddr *)&address,
                    &length) != 0) {
        fail("could not bind echo listener");
    }
    server->port = ntohs(address.sin_port);
    if (pthread_create(&server->thread, NULL, echo_thread, server) != 0) {
        fail("could not start echo server");
    }
}

static void echo_close(echo_server *server)
{
    (void)pthread_join(server->thread, NULL);
    (void)close(server->listener);
    (void)pthread_cond_destroy(&server->accepted_condition);
    (void)pthread_mutex_destroy(&server->mutex);
}

static void test_local_relay(void)
{
    static const char request[] = "split TCP preserves this byte stream";
    echo_server echo;
    um_tcp_relay *relay = NULL;
    uint16_t relay_port = 0u;
    char response[sizeof(request)];
    size_t used = 0u;
    int client;
    echo_open(&echo);
    if (um_tcp_relay_open_fixed(&relay, "127.0.0.1", echo.port, 1,
                                NULL, NULL, &relay_port) != UM_OK) {
        fail("could not start fixed relay");
    }
    client = connect_loopback(relay_port);
    if (client < 0) {
        fail("could not connect to relay");
    }
    if (echo_wait_for_accept(&echo, 200u) != 0) {
        fail("relay opened upstream before receiving client data");
    }
    if (!send_all(client, request, sizeof(request)) ||
        echo_wait_for_accept(&echo, 2000u) == 0) {
        fail("could not send through relay");
    }
    (void)shutdown(client, SHUT_WR);
    while (used < sizeof(response)) {
        ssize_t count = recv(client, response + used,
                             sizeof(response) - used, 0);
        if (count > 0) {
            used += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    (void)close(client);
    um_tcp_relay_close(relay);
    echo_close(&echo);
    if (used != sizeof(request) ||
        memcmp(request, response, sizeof(request)) != 0) {
        fail("relayed bytes changed");
    }
}

static void test_log(void *context, const char *message)
{
    FILE *stream = (FILE *)context;
    fprintf(stream, "%s\n", message);
    fflush(stream);
}

static void stop_serving(int signal_number)
{
    (void)signal_number;
    serve_stop = 1;
}

static int serve_fixed(const char *address, const char *port_text)
{
    um_tcp_relay *relay = NULL;
    uint16_t relay_port = 0u;
    char *end = NULL;
    unsigned long parsed = strtoul(port_text, &end, 10);
    struct timespec pause_time = {0, 100000000L};
    if (end == port_text || *end != '\0' || parsed == 0ul ||
        parsed > 65535ul) {
        fail("invalid --serve port");
    }
    if (um_tcp_relay_open_fixed(&relay, address, (uint16_t)parsed, 0,
                                test_log, stderr, &relay_port) != UM_OK) {
        fail("could not start public fixed relay");
    }
    (void)signal(SIGINT, stop_serving);
    (void)signal(SIGTERM, stop_serving);
    printf("relay-port=%u\n", (unsigned)relay_port);
    fflush(stdout);
    while (serve_stop == 0) {
        (void)nanosleep(&pause_time, NULL);
    }
    um_tcp_relay_close(relay);
    return 0;
}

static int serve_transparent(void)
{
    um_tcp_relay *relay = NULL;
    uint16_t relay_port = 0u;
    struct timespec pause_time = {0, 100000000L};
    if (um_tcp_relay_open_transparent(&relay, "0.0.0.0", test_log, stderr,
                                      &relay_port) != UM_OK) {
        fail("could not start transparent relay");
    }
    (void)signal(SIGINT, stop_serving);
    (void)signal(SIGTERM, stop_serving);
    printf("relay-port=%u\n", (unsigned)relay_port);
    fflush(stdout);
    while (serve_stop == 0) {
        (void)nanosleep(&pause_time, NULL);
    }
    um_tcp_relay_close(relay);
    return 0;
}

static int test_real_http(const char *address, const char *hostname)
{
    um_tcp_relay *relay = NULL;
    uint16_t relay_port = 0u;
    char request[512];
    char response[1024];
    size_t used = 0u;
    int client;
    int request_length;
    if (um_tcp_relay_open_fixed(&relay, address, 80u, 1, test_log, stderr,
                                &relay_port) != UM_OK) {
        fail("could not start HTTP relay");
    }
    client = connect_loopback(relay_port);
    request_length = snprintf(request, sizeof(request),
                              "GET /words.html HTTP/1.1\r\nHost: %s\r\n"
                              "User-Agent: universal-modem-test\r\n"
                              "Connection: close\r\n\r\n",
                              hostname);
    if (client < 0 || request_length <= 0 ||
        (size_t)request_length >= sizeof(request) ||
        !send_all(client, request, (size_t)request_length)) {
        fail("could not send real HTTP request");
    }
    while (used + 1u < sizeof(response)) {
        ssize_t count = recv(client, response + used,
                             sizeof(response) - used - 1u, 0);
        if (count > 0) {
            used += (size_t)count;
            response[used] = '\0';
            if (strstr(response, "\r\n\r\n") != NULL) {
                break;
            }
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    response[used] = '\0';
    (void)shutdown(client, SHUT_RDWR);
    (void)close(client);
    um_tcp_relay_close(relay);
    if (strncmp(response, "HTTP/1.1 301", 12u) != 0 &&
        strncmp(response, "HTTP/1.1 200", 12u) != 0) {
        fprintf(stderr, "unexpected response: %.120s\n", response);
        fail("real HTTP server did not answer through relay");
    }
    printf("real HTTP relay response: %.32s\n", response);
    return 0;
}

static int test_gateway_setup(void)
{
    um_network *network = NULL;
    if (um_network_open(&network, UM_LIVE_GATEWAY, UM_NETWORK_MIN_MTU,
                        test_log, stderr) != UM_OK) {
        fail("gateway network setup failed");
    }
    printf("gateway-interface=%s\n", um_network_interface_name(network));
    um_network_close(network);
    return 0;
}
#endif

int main(int argc, char **argv)
{
#if defined(__linux__)
    if (argc == 4 && strcmp(argv[1], "--serve") == 0) {
        return serve_fixed(argv[2], argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "--http") == 0) {
        return test_real_http(argv[2], argv[3]);
    }
    if (argc == 2 && strcmp(argv[1], "--network-gateway") == 0) {
        return test_gateway_setup();
    }
    if (argc == 2 && strcmp(argv[1], "--transparent-serve") == 0) {
        return serve_transparent();
    }
    if (argc != 1) {
        fprintf(stderr,
                "usage: %s [--serve IPV4 PORT | --http IPV4 HOST | "
                "--network-gateway | --transparent-serve]\n",
                argv[0]);
        return 2;
    }
    test_local_relay();
    printf("tcp relay tests passed\n");
#else
    (void)argc;
    (void)argv;
    printf("tcp relay tests skipped on this platform\n");
#endif
    return 0;
}
