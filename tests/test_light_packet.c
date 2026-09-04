#include "um.h"

#include <stdio.h>
#include <string.h>

#define TEST_PACKET_COUNT 18u

typedef struct {
    uint8_t bytes[1500];
    size_t length;
} test_packet;

static int failures;
static int tests_run;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
            ++failures;                                                        \
            return;                                                            \
        }                                                                      \
    } while (0)

static void fill_packet(test_packet *packet, size_t length, uint32_t seed)
{
    size_t index;
    uint32_t value = seed;
    packet->length = length;
    for (index = 0u; index < length; ++index) {
        value ^= value << 13u;
        value ^= value >> 17u;
        value ^= value << 5u;
        packet->bytes[index] = (uint8_t)value;
    }
}

static int dequeue_expected(um_light_peer *peer,
                            const test_packet *expected,
                            size_t expected_count, size_t *received_count)
{
    uint8_t packet[UM_LIGHT_MAX_PACKET];
    size_t packet_length;
    int status;
    for (;;) {
        status = um_light_peer_dequeue_packet(
            peer, packet, sizeof(packet), &packet_length);
        if (status == UM_ERR_TIMEOUT) {
            return UM_OK;
        }
        if (status != UM_OK || *received_count >= expected_count ||
            packet_length != expected[*received_count].length ||
            memcmp(packet, expected[*received_count].bytes,
                   packet_length) != 0) {
            return UM_ERR_RELIABILITY;
        }
        ++*received_count;
    }
}

static void test_packet_arguments_and_capacity(void)
{
    um_light_packet_peer_config config =
        um_light_packet_peer_default_config();
    um_light_peer *peer = NULL;
    uint8_t packet[16] = {0u};
    size_t length = 0u;
    ++tests_run;

    CHECK(config.max_packet_bytes == 1500u);
    CHECK(config.queue_packets == 64u);
    CHECK(um_light_packet_peer_create(NULL, UM_LIVE_CLIENT, &config,
                                      NULL, NULL) == UM_ERR_ARGUMENT);
    config.queue_packets = 2u;
    CHECK(um_light_packet_peer_create(&peer, UM_LIVE_CLIENT, &config,
                                      NULL, NULL) == UM_OK);
    CHECK(um_light_peer_enqueue_packet(peer, packet, sizeof(packet)) ==
          UM_OK);
    CHECK(um_light_peer_enqueue_packet(peer, packet, sizeof(packet)) ==
          UM_OK);
    CHECK(um_light_peer_enqueue_packet(peer, packet, sizeof(packet)) ==
          UM_ERR_CAPACITY);
    CHECK(um_light_peer_enqueue_packet(peer, packet, 0u) ==
          UM_ERR_ARGUMENT);
    CHECK(um_light_peer_dequeue_packet(peer, packet, sizeof(packet),
                                       &length) == UM_ERR_TIMEOUT);
    um_light_peer_destroy(peer);

    config = um_light_packet_peer_default_config();
    config.max_packet_bytes = UM_LIGHT_MAX_PACKET + 1u;
    CHECK(um_light_packet_peer_create(&peer, UM_LIVE_CLIENT, &config,
                                      NULL, NULL) == UM_ERR_ARGUMENT);
}

static void test_packet_mode_does_not_mix_with_finite_mode(void)
{
    um_light_packet_peer_config packet_config =
        um_light_packet_peer_default_config();
    um_light_peer_config finite_config = um_light_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer_frame client_frame;
    um_light_peer_frame gateway_frame;
    um_light_peer_status status;
    uint8_t outgoing[1] = {1u};
    uint8_t incoming[1] = {0u};
    size_t frame;
    ++tests_run;

    CHECK(um_light_packet_peer_create(&client, UM_LIVE_CLIENT,
                                      &packet_config, NULL, NULL) == UM_OK);
    CHECK(um_light_peer_create(&gateway, UM_LIVE_GATEWAY, &finite_config,
                               outgoing, sizeof(outgoing), incoming,
                               sizeof(incoming), NULL, NULL) == UM_OK);
    for (frame = 0u; frame < 30u; ++frame) {
        CHECK(um_light_peer_build(client, frame, &client_frame) == UM_OK);
        CHECK(um_light_peer_build(gateway, frame, &gateway_frame) == UM_OK);
        CHECK(um_light_peer_process(client, frame, &gateway_frame) == UM_OK);
        CHECK(um_light_peer_process(gateway, frame, &client_frame) == UM_OK);
    }
    um_light_peer_get_status(client, &status);
    CHECK(status.connected == 0);
    um_light_peer_get_status(gateway, &status);
    CHECK(status.connected == 0);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

static void test_recovers_when_peer_vanishes_during_handshake(void)
{
    um_light_packet_peer_config config =
        um_light_packet_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer_frame client_frame;
    um_light_peer_frame gateway_frame;
    um_light_peer_status client_status;
    um_light_peer_status gateway_status;
    size_t client_clock = 0u;
    size_t gateway_clock = 0u;
    size_t step;
    ++tests_run;

    config.link.link_timeout_frames = 5u;
    CHECK(um_light_packet_peer_create(&client, UM_LIVE_CLIENT, &config,
                                      NULL, NULL) == UM_OK);
    config.link.random_seed ^= UINT32_C(0x12589abc);
    CHECK(um_light_packet_peer_create(&gateway, UM_LIVE_GATEWAY, &config,
                                      NULL, NULL) == UM_OK);

    /* Advance exactly far enough for the client to accept an OFFER, then
     * replace the gateway before it can receive the CONFIRM. */
    CHECK(um_light_peer_build(client, client_clock, &client_frame) ==
          UM_OK);
    CHECK(um_light_peer_process(gateway, gateway_clock, &client_frame) ==
          UM_OK);
    CHECK(um_light_peer_build(gateway, gateway_clock, &gateway_frame) ==
          UM_OK);
    CHECK(um_light_peer_process(client, client_clock, &gateway_frame) ==
          UM_OK);
    ++client_clock;
    um_light_peer_destroy(gateway);

    config.link.random_seed ^= UINT32_C(0xa6c3017d);
    CHECK(um_light_packet_peer_create(&gateway, UM_LIVE_GATEWAY, &config,
                                      NULL, NULL) == UM_OK);
    for (step = 0u; step < 40u; ++step) {
        CHECK(um_light_peer_build(client, client_clock, &client_frame) ==
              UM_OK);
        CHECK(um_light_peer_build(gateway, gateway_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(client, client_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(gateway, gateway_clock, &client_frame) ==
              UM_OK);
        ++client_clock;
        ++gateway_clock;
        um_light_peer_get_status(client, &client_status);
        um_light_peer_get_status(gateway, &gateway_status);
        if (client_status.connected != 0 &&
            gateway_status.connected != 0) {
            break;
        }
    }
    CHECK(step < 40u);
    CHECK(client_status.link_timeouts > 0u);
    CHECK(client_status.connected != 0);
    CHECK(gateway_status.connected != 0);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

static void test_recovers_when_packet_peer_restarts(void)
{
    test_packet warm_client;
    test_packet warm_gateway;
    test_packet next_client;
    test_packet next_gateway;
    um_light_packet_peer_config config =
        um_light_packet_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer_frame client_frame;
    um_light_peer_frame gateway_frame;
    um_light_peer_status client_status;
    um_light_peer_status gateway_status;
    size_t client_clock = 0u;
    size_t gateway_clock = 0u;
    size_t client_received = 0u;
    size_t gateway_received = 0u;
    size_t step;
    ++tests_run;

    fill_packet(&warm_client, 333u, UINT32_C(0xc11e0001));
    fill_packet(&warm_gateway, 271u, UINT32_C(0x6a7e0001));
    fill_packet(&next_client, 417u, UINT32_C(0xc11e0002));
    fill_packet(&next_gateway, 509u, UINT32_C(0x6a7e0002));
    config.link.link_timeout_frames = 6u;
    config.link.retransmit_after_frames = 2u;
    config.queue_packets = 8u;
    CHECK(um_light_packet_peer_create(&client, UM_LIVE_CLIENT, &config,
                                      NULL, NULL) == UM_OK);
    config.link.random_seed ^= UINT32_C(0x73d9a421);
    CHECK(um_light_packet_peer_create(&gateway, UM_LIVE_GATEWAY, &config,
                                      NULL, NULL) == UM_OK);

    for (step = 0u; step < 40u; ++step) {
        CHECK(um_light_peer_build(client, client_clock, &client_frame) ==
              UM_OK);
        CHECK(um_light_peer_build(gateway, gateway_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(client, client_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(gateway, gateway_clock, &client_frame) ==
              UM_OK);
        ++client_clock;
        ++gateway_clock;
        um_light_peer_get_status(client, &client_status);
        um_light_peer_get_status(gateway, &gateway_status);
        if (client_status.connected != 0 && gateway_status.connected != 0) {
            break;
        }
    }
    CHECK(step < 40u);
    CHECK(um_light_peer_enqueue_packet(client, warm_client.bytes,
                                       warm_client.length) == UM_OK);
    CHECK(um_light_peer_enqueue_packet(gateway, warm_gateway.bytes,
                                       warm_gateway.length) == UM_OK);
    for (step = 0u; step < 120u; ++step) {
        CHECK(um_light_peer_build(client, client_clock, &client_frame) ==
              UM_OK);
        CHECK(um_light_peer_build(gateway, gateway_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(client, client_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(gateway, gateway_clock, &client_frame) ==
              UM_OK);
        CHECK(dequeue_expected(client, &warm_gateway, 1u,
                               &client_received) == UM_OK);
        CHECK(dequeue_expected(gateway, &warm_client, 1u,
                               &gateway_received) == UM_OK);
        ++client_clock;
        ++gateway_clock;
        um_light_peer_get_status(client, &client_status);
        um_light_peer_get_status(gateway, &gateway_status);
        if (client_received == 1u && gateway_received == 1u &&
            client_status.outgoing_cells_in_flight == 0u &&
            gateway_status.outgoing_cells_in_flight == 0u) {
            break;
        }
    }
    CHECK(step < 120u);

    /* A new process has fresh packet sequence state and a fresh local frame
     * clock.  The surviving client must recognize that this is not merely a
     * camera outage and synchronize a new packet generation. */
    um_light_peer_destroy(gateway);
    config.link.random_seed ^= UINT32_C(0xd8510f36);
    CHECK(um_light_packet_peer_create(&gateway, UM_LIVE_GATEWAY, &config,
                                      NULL, NULL) == UM_OK);
    gateway_clock = 0u;
    for (step = 0u; step < 80u; ++step) {
        CHECK(um_light_peer_build(client, client_clock, &client_frame) ==
              UM_OK);
        CHECK(um_light_peer_build(gateway, gateway_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(client, client_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(gateway, gateway_clock, &client_frame) ==
              UM_OK);
        ++client_clock;
        ++gateway_clock;
        um_light_peer_get_status(client, &client_status);
        um_light_peer_get_status(gateway, &gateway_status);
        if (client_status.connected != 0 && gateway_status.connected != 0) {
            break;
        }
    }
    CHECK(step < 80u);
    CHECK(client_status.packet_generation_resets == 1u);
    client_received = 0u;
    gateway_received = 0u;
    CHECK(um_light_peer_enqueue_packet(client, next_client.bytes,
                                       next_client.length) == UM_OK);
    CHECK(um_light_peer_enqueue_packet(gateway, next_gateway.bytes,
                                       next_gateway.length) == UM_OK);
    for (step = 0u; step < 240u; ++step) {
        CHECK(um_light_peer_build(client, client_clock, &client_frame) ==
              UM_OK);
        CHECK(um_light_peer_build(gateway, gateway_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(client, client_clock, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(gateway, gateway_clock, &client_frame) ==
              UM_OK);
        CHECK(dequeue_expected(client, &next_gateway, 1u,
                               &client_received) == UM_OK);
        CHECK(dequeue_expected(gateway, &next_client, 1u,
                               &gateway_received) == UM_OK);
        ++client_clock;
        ++gateway_clock;
        if (client_received == 1u && gateway_received == 1u) {
            break;
        }
    }
    CHECK(step < 240u);
    CHECK(client_received == 1u);
    CHECK(gateway_received == 1u);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

static void test_receive_backpressure_is_lossless(void)
{
    enum { packet_count = 10 };
    test_packet packets[packet_count];
    um_light_packet_peer_config config =
        um_light_packet_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer_frame client_frame;
    um_light_peer_frame gateway_frame;
    um_light_peer_status client_status;
    um_light_peer_status gateway_status;
    size_t accepted = 0u;
    size_t received = 0u;
    size_t frame;
    size_t index;
    ++tests_run;

    config.queue_packets = 2u;
    config.link.retransmit_after_frames = 3u;
    for (index = 0u; index < packet_count; ++index) {
        fill_packet(&packets[index], 300u,
                    UINT32_C(0xbacc0000) + (uint32_t)index);
    }
    CHECK(um_light_packet_peer_create(&client, UM_LIVE_CLIENT, &config,
                                      NULL, NULL) == UM_OK);
    config.link.random_seed ^= UINT32_C(0x33445566);
    CHECK(um_light_packet_peer_create(&gateway, UM_LIVE_GATEWAY, &config,
                                      NULL, NULL) == UM_OK);

    for (frame = 0u; frame < 600u; ++frame) {
        int enqueue_status;
        while (accepted < packet_count) {
            enqueue_status = um_light_peer_enqueue_packet(
                client, packets[accepted].bytes,
                packets[accepted].length);
            if (enqueue_status == UM_ERR_CAPACITY) {
                break;
            }
            CHECK(enqueue_status == UM_OK);
            ++accepted;
        }
        CHECK(um_light_peer_build(client, frame, &client_frame) == UM_OK);
        CHECK(um_light_peer_build(gateway, frame, &gateway_frame) == UM_OK);
        CHECK(um_light_peer_process(client, frame, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(gateway, frame, &client_frame) ==
              UM_OK);
        if (frame == 100u) {
            um_light_peer_get_status(gateway, &gateway_status);
            CHECK(gateway_status.incoming_packets_queued == 2u);
            CHECK(received == 0u);
        }
        if (frame > 100u) {
            CHECK(dequeue_expected(gateway, packets, packet_count,
                                   &received) == UM_OK);
        }
        um_light_peer_get_status(client, &client_status);
        if (accepted == packet_count && received == packet_count &&
            client_status.outgoing_cells_in_flight == 0u &&
            client_status.outgoing_packets_queued == 0u) {
            break;
        }
    }
    CHECK(frame < 600u);
    CHECK(accepted == packet_count);
    CHECK(received == packet_count);
    um_light_peer_get_status(client, &client_status);
    um_light_peer_get_status(gateway, &gateway_status);
    CHECK(client_status.protocol_rejections == 0u);
    CHECK(gateway_status.protocol_rejections == 0u);
    CHECK(gateway_status.incoming_packets_received == packet_count);
    CHECK(gateway_status.incoming_packets_queued == 0u);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

static void test_persistent_packets_with_outage_and_independent_clocks(void)
{
    static const size_t lengths[TEST_PACKET_COUNT] = {
        20u, 31u, 44u, 57u, 72u, 90u, 181u, 511u, 1300u,
        19u, 28u, 64u, 83u, 166u, 333u, 576u, 900u, 1499u
    };
    test_packet client_packets[TEST_PACKET_COUNT];
    test_packet gateway_packets[TEST_PACKET_COUNT];
    um_light_packet_peer_config config =
        um_light_packet_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer_frame client_display = {0};
    um_light_peer_frame gateway_display = {0};
    um_light_peer_status client_status;
    um_light_peer_status gateway_status;
    size_t client_received = 0u;
    size_t gateway_received = 0u;
    size_t client_frame = 0u;
    size_t gateway_frame = 0u;
    size_t expected_payload = 0u;
    size_t step;
    size_t index;
    ++tests_run;

    config.queue_packets = 32u;
    config.link.retransmit_after_frames = 3u;
    config.link.link_timeout_frames = 12u;
    for (index = 0u; index < TEST_PACKET_COUNT; ++index) {
        fill_packet(&client_packets[index], lengths[index],
                    UINT32_C(0xc1000000) + (uint32_t)index);
        fill_packet(&gateway_packets[index],
                    lengths[TEST_PACKET_COUNT - 1u - index],
                    UINT32_C(0x6a000000) + (uint32_t)index);
        expected_payload += lengths[index];
    }
    CHECK(um_light_packet_peer_create(&client, UM_LIVE_CLIENT, &config,
                                      NULL, NULL) == UM_OK);
    config.link.random_seed ^= UINT32_C(0xa5b4c3d2);
    CHECK(um_light_packet_peer_create(&gateway, UM_LIVE_GATEWAY, &config,
                                      NULL, NULL) == UM_OK);
    for (index = 0u; index < TEST_PACKET_COUNT / 2u; ++index) {
        CHECK(um_light_peer_enqueue_packet(
                  client, client_packets[index].bytes,
                  client_packets[index].length) == UM_OK);
        CHECK(um_light_peer_enqueue_packet(
                  gateway, gateway_packets[index].bytes,
                  gateway_packets[index].length) == UM_OK);
    }

    for (step = 0u; step < 4000u; ++step) {
        int client_due = step % 2u == 0u;
        int gateway_due = step >= 90u && step % 3u == 0u;
        int visible = step < 240u || step >= 390u;
        if (step == 420u) {
            for (index = TEST_PACKET_COUNT / 2u;
                 index < TEST_PACKET_COUNT; ++index) {
                CHECK(um_light_peer_enqueue_packet(
                          client, client_packets[index].bytes,
                          client_packets[index].length) == UM_OK);
                CHECK(um_light_peer_enqueue_packet(
                          gateway, gateway_packets[index].bytes,
                          gateway_packets[index].length) == UM_OK);
            }
        }
        if (client_due != 0) {
            CHECK(um_light_peer_build(client, client_frame,
                                      &client_display) == UM_OK);
        }
        if (gateway_due != 0) {
            CHECK(um_light_peer_build(gateway, gateway_frame,
                                      &gateway_display) == UM_OK);
        }
        if (client_due != 0) {
            const um_light_peer_frame *received =
                visible != 0 && gateway_display.present != 0 &&
                        step % 17u != 0u
                    ? &gateway_display
                    : NULL;
            CHECK(um_light_peer_process(client, client_frame, received) ==
                  UM_OK);
            CHECK(dequeue_expected(client, gateway_packets,
                                   TEST_PACKET_COUNT,
                                   &client_received) == UM_OK);
            ++client_frame;
        }
        if (gateway_due != 0) {
            const um_light_peer_frame *received =
                visible != 0 && client_display.present != 0 &&
                        step % 19u != 0u
                    ? &client_display
                    : NULL;
            CHECK(um_light_peer_process(gateway, gateway_frame, received) ==
                  UM_OK);
            CHECK(dequeue_expected(gateway, client_packets,
                                   TEST_PACKET_COUNT,
                                   &gateway_received) == UM_OK);
            ++gateway_frame;
        }
        um_light_peer_get_status(client, &client_status);
        um_light_peer_get_status(gateway, &gateway_status);
        if (client_received == TEST_PACKET_COUNT &&
            gateway_received == TEST_PACKET_COUNT &&
            client_status.outgoing_cells_in_flight == 0u &&
            gateway_status.outgoing_cells_in_flight == 0u) {
            break;
        }
    }
    CHECK(step < 4000u);
    CHECK(client_received == TEST_PACKET_COUNT);
    CHECK(gateway_received == TEST_PACKET_COUNT);
    CHECK(client_status.connected != 0);
    CHECK(gateway_status.connected != 0);
    CHECK(client_status.outgoing_packets_accepted == TEST_PACKET_COUNT);
    CHECK(gateway_status.outgoing_packets_accepted == TEST_PACKET_COUNT);
    CHECK(client_status.incoming_packets_received == TEST_PACKET_COUNT);
    CHECK(gateway_status.incoming_packets_received == TEST_PACKET_COUNT);
    CHECK(client_status.outgoing_bytes_acked == expected_payload);
    CHECK(gateway_status.outgoing_bytes_acked == expected_payload);
    CHECK(client_status.outgoing_packets_queued == 0u);
    CHECK(gateway_status.outgoing_packets_queued == 0u);
    CHECK(client_status.link_timeouts > 0u);
    CHECK(gateway_status.link_timeouts > 0u);
    CHECK(client_status.reconnects > 0u);
    CHECK(client_status.retransmissions > 0u);
    CHECK(gateway_status.retransmissions > 0u);
    CHECK(client_status.protocol_rejections == 0u);
    CHECK(gateway_status.protocol_rejections == 0u);
    CHECK(client_status.data_frames + gateway_status.data_frames <
          2u * expected_payload / 40u);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

int main(void)
{
    test_packet_arguments_and_capacity();
    test_packet_mode_does_not_mix_with_finite_mode();
    test_recovers_when_peer_vanishes_during_handshake();
    test_recovers_when_packet_peer_restarts();
    test_receive_backpressure_is_lossless();
    test_persistent_packets_with_outage_and_independent_clocks();
    if (failures != 0) {
        fprintf(stderr, "%d of %d light packet tests failed\n", failures,
                tests_run);
        return 1;
    }
    printf("all %d light packet tests passed\n", tests_run);
    return 0;
}
