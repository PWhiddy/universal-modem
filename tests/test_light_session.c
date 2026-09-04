#include "um.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int tests_run = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
            ++failures;                                                        \
            return;                                                            \
        }                                                                      \
    } while (0)

static void fill_bytes(uint8_t *bytes, size_t length, uint32_t seed)
{
    size_t index;
    uint32_t value = seed;
    for (index = 0u; index < length; ++index) {
        value ^= value << 13u;
        value ^= value >> 17u;
        value ^= value << 5u;
        bytes[index] = (uint8_t)value;
    }
}

static int pass_peer_frame_through_image(
    const um_light_peer_frame *source,
    const um_light_channel_config *channel, uint32_t salt,
    um_light_peer_frame *decoded)
{
    um_light_channel_config varied = *channel;
    um_light_rx_metrics metrics;
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t *pixels = NULL;
    size_t pixel_count = 0u;
    size_t payload_length = 0u;
    int status;

    memset(decoded, 0, sizeof(*decoded));
    varied.random_seed ^= salt * UINT32_C(0x9e3779b9);
    status = um_light_encode_frame(
        source->type, source->session_id, source->sequence,
        source->payload, source->payload_length, modules, sizeof(modules));
    if (status == UM_OK) {
        status = um_light_render_frame(modules, sizeof(modules), &varied,
                                       &pixels, &pixel_count);
    }
    if (status == UM_OK) {
        memset(&metrics, 0, sizeof(metrics));
        status = um_light_decode_frame(
            pixels, varied.image_width, varied.image_height,
            varied.image_width, &decoded->type, &decoded->session_id,
            &decoded->sequence, decoded->payload,
            sizeof(decoded->payload), &payload_length, &metrics);
    }
    free(pixels);
    if (status == UM_OK) {
        decoded->present = 1;
        decoded->payload_length = payload_length;
    }
    return status;
}

static void test_full_duplex_session_with_erasures(void)
{
    um_light_session_simulation_config config =
        um_light_session_simulation_default_config();
    um_light_session_simulation_result result;
    ++tests_run;

    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.final_connected != 0);
    CHECK(result.gateway_received_bytes == config.client_payload_bytes);
    CHECK(result.client_received_bytes == config.gateway_payload_bytes);
    CHECK(result.handshake_frames >= 4u);
    CHECK(result.data_frames > 0u);
    CHECK(result.acknowledgement_frames > 0u);
    CHECK(result.simultaneous_data_frames > 0u);
    CHECK(result.scheduled_frame_drops > 0u);
    CHECK(result.retransmissions > 0u);
    CHECK(result.frames_elapsed < config.max_frames);
    CHECK(result.payload_goodput_bps > 7000.0f);
}

static void test_recovers_after_bidirectional_blackout(void)
{
    um_light_session_simulation_config config =
        um_light_session_simulation_default_config();
    um_light_session_simulation_result result;
    ++tests_run;

    config.client_payload_bytes = 2500u;
    config.gateway_payload_bytes = 2200u;
    config.client_to_gateway_drop_period = 0u;
    config.gateway_to_client_drop_period = 0u;
    config.blackout_start_frame = 20u;
    config.blackout_frame_count = 16u;
    config.link_timeout_frames = 6u;
    config.max_frames = 260u;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.gateway_received_bytes == config.client_payload_bytes);
    CHECK(result.client_received_bytes == config.gateway_payload_bytes);
    CHECK(result.scheduled_frame_drops ==
          2u * config.blackout_frame_count);
    CHECK(result.link_timeouts >= 2u);
    CHECK(result.reconnects >= 1u);
    CHECK(result.retransmissions > 0u);
    CHECK(result.final_connected != 0);
}

static void test_arbitrary_peer_start_times(void)
{
    um_light_session_simulation_config config =
        um_light_session_simulation_default_config();
    um_light_session_simulation_result result;
    ++tests_run;

    config.client_payload_bytes = 700u;
    config.gateway_payload_bytes = 600u;
    config.client_to_gateway_drop_period = 0u;
    config.gateway_to_client_drop_period = 0u;
    config.max_frames = 500u;
    config.gateway_start_frame = 120u;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.final_connected != 0);
    CHECK(result.frames_elapsed > config.gateway_start_frame);
    CHECK(result.gateway_received_bytes == config.client_payload_bytes);
    CHECK(result.client_received_bytes == config.gateway_payload_bytes);
    CHECK(result.frames_elapsed < config.gateway_start_frame + 100u);

    config = um_light_session_simulation_default_config();
    config.client_payload_bytes = 700u;
    config.gateway_payload_bytes = 600u;
    config.client_to_gateway_drop_period = 0u;
    config.gateway_to_client_drop_period = 0u;
    config.max_frames = 500u;
    config.client_start_frame = 120u;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.final_connected != 0);
    CHECK(result.frames_elapsed > config.client_start_frame);
    CHECK(result.gateway_received_bytes == config.client_payload_bytes);
    CHECK(result.client_received_bytes == config.gateway_payload_bytes);
    CHECK(result.frames_elapsed < config.client_start_frame + 100u);
}

static void test_long_late_and_intermittent_peer(void)
{
    um_light_session_simulation_config config =
        um_light_session_simulation_default_config();
    um_light_session_simulation_result result;
    const size_t ten_minutes = 10u * 60u * config.frames_per_second;
    const size_t six_seconds = 6u * config.frames_per_second;
    const size_t five_minutes = 5u * 60u * config.frames_per_second;
    const size_t two_minutes = 2u * 60u * config.frames_per_second;
    size_t return_frame;
    ++tests_run;

    config.client_payload_bytes = 20000u;
    config.gateway_payload_bytes = 20000u;
    config.client_to_gateway_drop_period = 0u;
    config.gateway_to_client_drop_period = 0u;
    config.client_start_frame = ten_minutes;
    config.blackout_start_frame = ten_minutes + six_seconds;
    config.blackout_frame_count = five_minutes;
    return_frame = config.blackout_start_frame + config.blackout_frame_count;
    config.max_frames = return_frame + two_minutes;

    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.final_connected != 0);
    CHECK(result.frames_elapsed > return_frame);
    CHECK(result.frames_elapsed < config.max_frames);
    CHECK(result.gateway_received_bytes == config.client_payload_bytes);
    CHECK(result.client_received_bytes == config.gateway_payload_bytes);
    CHECK(result.link_timeouts >= 2u);
    CHECK(result.reconnects >= 1u);
    CHECK(result.retransmissions > 0u);
    CHECK(result.scheduled_frame_drops ==
          2u * config.blackout_frame_count);
}

static void test_role_local_peers_with_independent_clocks(void)
{
    uint8_t client_outgoing[8000];
    uint8_t gateway_outgoing[7300];
    uint8_t client_incoming[7300] = {0u};
    uint8_t gateway_incoming[8000] = {0u};
    um_light_peer_config config = um_light_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer_frame client_display = {0};
    um_light_peer_frame gateway_display = {0};
    um_light_peer_status client_status;
    um_light_peer_status gateway_status;
    size_t client_frame = 0u;
    size_t gateway_frame = 0u;
    size_t step;
    ++tests_run;

    fill_bytes(client_outgoing, sizeof(client_outgoing),
               UINT32_C(0x6c696e6b));
    fill_bytes(gateway_outgoing, sizeof(gateway_outgoing),
               UINT32_C(0x70656572));
    config.retransmit_after_frames = 3u;
    config.link_timeout_frames = 12u;
    CHECK(um_light_peer_create(
              &client, UM_LIVE_CLIENT, &config, client_outgoing,
              sizeof(client_outgoing), client_incoming,
              sizeof(client_incoming), NULL, NULL) == UM_OK);
    config.random_seed ^= UINT32_C(0x91a2b3c4);
    CHECK(um_light_peer_create(
              &gateway, UM_LIVE_GATEWAY, &config, gateway_outgoing,
              sizeof(gateway_outgoing), gateway_incoming,
              sizeof(gateway_incoming), NULL, NULL) == UM_OK);

    for (step = 0u; step < 3000u; ++step) {
        int client_due = step % 2u == 0u;
        int gateway_due = step >= 150u && step % 3u == 0u;
        int visible = step < 300u || step >= 500u;
        if (client_due != 0) {
            CHECK(um_light_peer_build(client, client_frame,
                                      &client_display) == UM_OK);
        }
        if (gateway_due != 0) {
            CHECK(um_light_peer_build(gateway, gateway_frame,
                                      &gateway_display) == UM_OK);
        }
        if (client_due != 0) {
            CHECK(um_light_peer_process(
                      client, client_frame,
                      visible != 0 && gateway_display.present != 0
                          ? &gateway_display
                          : NULL) == UM_OK);
            ++client_frame;
        }
        if (gateway_due != 0) {
            CHECK(um_light_peer_process(
                      gateway, gateway_frame,
                      visible != 0 && client_display.present != 0
                          ? &client_display
                          : NULL) == UM_OK);
            ++gateway_frame;
        }
        if (um_light_peer_complete(client) != 0 &&
            um_light_peer_complete(gateway) != 0) {
            break;
        }
    }
    CHECK(step < 3000u);
    CHECK(memcmp(client_incoming, gateway_outgoing,
                 sizeof(client_incoming)) == 0);
    CHECK(memcmp(gateway_incoming, client_outgoing,
                 sizeof(gateway_incoming)) == 0);
    um_light_peer_get_status(client, &client_status);
    um_light_peer_get_status(gateway, &gateway_status);
    CHECK(client_status.connected != 0);
    CHECK(gateway_status.connected != 0);
    CHECK(client_status.outgoing_bytes_acked == sizeof(client_outgoing));
    CHECK(gateway_status.outgoing_bytes_acked == sizeof(gateway_outgoing));
    CHECK(client_status.incoming_bytes_received == sizeof(client_incoming));
    CHECK(gateway_status.incoming_bytes_received ==
          sizeof(gateway_incoming));
    CHECK(client_status.link_timeouts >= 1u);
    CHECK(gateway_status.link_timeouts >= 1u);
    CHECK(client_status.reconnects >= 1u);
    CHECK(client_status.retransmissions > 0u);
    CHECK(gateway_status.retransmissions > 0u);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

static void test_role_local_peer_arguments(void)
{
    uint8_t outgoing[8] = {0u};
    uint8_t incoming[8] = {0u};
    um_light_peer_config config = um_light_peer_default_config();
    um_light_peer_frame frame;
    um_light_peer *peer = NULL;
    ++tests_run;

    CHECK(config.transmit_window > 0u);
    CHECK(config.retransmit_after_frames > 0u);
    CHECK(config.link_timeout_frames > 0u);
    CHECK(um_light_peer_create(NULL, UM_LIVE_CLIENT, &config, outgoing,
                               sizeof(outgoing), incoming,
                               sizeof(incoming), NULL, NULL) ==
          UM_ERR_ARGUMENT);
    config.transmit_window = 0u;
    CHECK(um_light_peer_create(&peer, UM_LIVE_CLIENT, &config, outgoing,
                               sizeof(outgoing), incoming,
                               sizeof(incoming), NULL, NULL) ==
          UM_ERR_ARGUMENT);
    config = um_light_peer_default_config();
    CHECK(um_light_peer_create(&peer, UM_LIVE_CLIENT, &config, outgoing,
                               sizeof(outgoing), incoming,
                               sizeof(incoming), NULL, NULL) == UM_OK);
    CHECK(um_light_peer_build(peer, 10u, &frame) == UM_OK);
    CHECK(um_light_peer_process(peer, 10u, NULL) == UM_OK);
    CHECK(um_light_peer_build(peer, 9u, &frame) == UM_ERR_ARGUMENT);
    frame.present = 1;
    frame.payload_length = UM_LIGHT_MAX_PAYLOAD + 1u;
    CHECK(um_light_peer_process(peer, 11u, &frame) == UM_ERR_ARGUMENT);
    um_light_peer_destroy(peer);
}

static void test_foreign_discovery_waits_for_gateway_timeout(void)
{
    uint8_t client_outgoing[256];
    uint8_t gateway_outgoing[256];
    uint8_t intruder_outgoing[1] = {0u};
    uint8_t client_incoming[256] = {0u};
    uint8_t gateway_incoming[256] = {0u};
    uint8_t intruder_incoming[1] = {0u};
    um_light_peer_config config = um_light_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer *intruder = NULL;
    um_light_peer_frame client_frame = {0};
    um_light_peer_frame gateway_frame = {0};
    um_light_peer_frame intruder_frame = {0};
    um_light_peer_status gateway_status;
    uint32_t connected_session;
    size_t frame;
    ++tests_run;

    fill_bytes(client_outgoing, sizeof(client_outgoing),
               UINT32_C(0x636c6965));
    fill_bytes(gateway_outgoing, sizeof(gateway_outgoing),
               UINT32_C(0x67617465));
    config.link_timeout_frames = 5u;
    CHECK(um_light_peer_create(
              &client, UM_LIVE_CLIENT, &config, client_outgoing,
              sizeof(client_outgoing), client_incoming,
              sizeof(client_incoming), NULL, NULL) == UM_OK);
    config.random_seed ^= UINT32_C(0x12345678);
    CHECK(um_light_peer_create(
              &gateway, UM_LIVE_GATEWAY, &config, gateway_outgoing,
              sizeof(gateway_outgoing), gateway_incoming,
              sizeof(gateway_incoming), NULL, NULL) == UM_OK);
    config.random_seed ^= UINT32_C(0x87654321);
    CHECK(um_light_peer_create(
              &intruder, UM_LIVE_CLIENT, &config, intruder_outgoing,
              sizeof(intruder_outgoing), intruder_incoming,
              sizeof(intruder_incoming), NULL, NULL) == UM_OK);

    for (frame = 0u; frame < 20u; ++frame) {
        CHECK(um_light_peer_build(client, frame, &client_frame) == UM_OK);
        CHECK(um_light_peer_build(gateway, frame, &gateway_frame) == UM_OK);
        CHECK(um_light_peer_process(client, frame, &gateway_frame) ==
              UM_OK);
        CHECK(um_light_peer_process(gateway, frame, &client_frame) ==
              UM_OK);
        um_light_peer_get_status(gateway, &gateway_status);
        if (gateway_status.connected != 0) {
            break;
        }
    }
    CHECK(frame < 20u);
    CHECK(um_light_peer_build(gateway, frame, &gateway_frame) == UM_OK);
    connected_session = gateway_frame.session_id;
    CHECK(connected_session != 0u);

    CHECK(um_light_peer_build(intruder, 0u, &intruder_frame) == UM_OK);
    CHECK(intruder_frame.session_id != connected_session);
    ++frame;
    CHECK(um_light_peer_process(gateway, frame, &intruder_frame) == UM_OK);
    um_light_peer_get_status(gateway, &gateway_status);
    CHECK(gateway_status.connected != 0);
    CHECK(um_light_peer_build(gateway, frame, &gateway_frame) == UM_OK);
    CHECK(gateway_frame.session_id == connected_session);

    do {
        ++frame;
        CHECK(um_light_peer_process(gateway, frame, NULL) == UM_OK);
        um_light_peer_get_status(gateway, &gateway_status);
    } while (gateway_status.connected != 0 && frame < 40u);
    CHECK(gateway_status.connected == 0);

    ++frame;
    CHECK(um_light_peer_process(gateway, frame, &intruder_frame) == UM_OK);
    CHECK(um_light_peer_build(gateway, frame, &gateway_frame) == UM_OK);
    CHECK(gateway_frame.session_id == intruder_frame.session_id);

    um_light_peer_destroy(intruder);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

static void test_role_local_peers_through_optical_codec(void)
{
    uint8_t client_outgoing[3600];
    uint8_t gateway_outgoing[3300];
    uint8_t client_incoming[3300] = {0u};
    uint8_t gateway_incoming[3600] = {0u};
    um_light_channel_config client_channel =
        um_light_channel_default_config();
    um_light_channel_config gateway_channel =
        um_light_channel_default_config();
    um_light_peer_config config = um_light_peer_default_config();
    um_light_peer *client = NULL;
    um_light_peer *gateway = NULL;
    um_light_peer_frame client_display = {0};
    um_light_peer_frame gateway_display = {0};
    um_light_peer_frame received;
    um_light_peer_status client_status;
    size_t client_frame = 0u;
    size_t gateway_frame = 0u;
    size_t step;
    ++tests_run;

    fill_bytes(client_outgoing, sizeof(client_outgoing),
               UINT32_C(0x6f707469));
    fill_bytes(gateway_outgoing, sizeof(gateway_outgoing),
               UINT32_C(0x63616d65));
    gateway_channel.black_level = 0.12f;
    gateway_channel.white_level = 0.88f;
    gateway_channel.noise_stddev = 0.08f;
    gateway_channel.blur_radius = 2u;
    config.retransmit_after_frames = 3u;
    config.link_timeout_frames = 12u;
    CHECK(um_light_peer_create(
              &client, UM_LIVE_CLIENT, &config, client_outgoing,
              sizeof(client_outgoing), client_incoming,
              sizeof(client_incoming), NULL, NULL) == UM_OK);
    config.random_seed ^= UINT32_C(0xa791bc53);
    CHECK(um_light_peer_create(
              &gateway, UM_LIVE_GATEWAY, &config, gateway_outgoing,
              sizeof(gateway_outgoing), gateway_incoming,
              sizeof(gateway_incoming), NULL, NULL) == UM_OK);

    for (step = 0u; step < 700u; ++step) {
        int client_due = step % 2u == 0u;
        int gateway_due = step >= 60u && step % 3u == 0u;
        int visible = step < 120u || step >= 180u;
        if (client_due != 0) {
            CHECK(um_light_peer_build(client, client_frame,
                                      &client_display) == UM_OK);
        }
        if (gateway_due != 0) {
            CHECK(um_light_peer_build(gateway, gateway_frame,
                                      &gateway_display) == UM_OK);
        }
        if (client_due != 0) {
            const um_light_peer_frame *input = NULL;
            if (visible != 0 && gateway_display.present != 0 &&
                step % 17u != 0u &&
                pass_peer_frame_through_image(
                    &gateway_display, &gateway_channel, (uint32_t)step,
                    &received) == UM_OK) {
                input = &received;
            }
            CHECK(um_light_peer_process(client, client_frame, input) ==
                  UM_OK);
            ++client_frame;
        }
        if (gateway_due != 0) {
            const um_light_peer_frame *input = NULL;
            if (visible != 0 && client_display.present != 0 &&
                step % 19u != 0u &&
                pass_peer_frame_through_image(
                    &client_display, &client_channel, (uint32_t)step,
                    &received) == UM_OK) {
                input = &received;
            }
            CHECK(um_light_peer_process(gateway, gateway_frame, input) ==
                  UM_OK);
            ++gateway_frame;
        }
        if (um_light_peer_complete(client) != 0 &&
            um_light_peer_complete(gateway) != 0) {
            break;
        }
    }
    CHECK(step < 700u);
    CHECK(memcmp(client_incoming, gateway_outgoing,
                 sizeof(client_incoming)) == 0);
    CHECK(memcmp(gateway_incoming, client_outgoing,
                 sizeof(gateway_incoming)) == 0);
    um_light_peer_get_status(client, &client_status);
    CHECK(client_status.link_timeouts >= 1u);
    CHECK(client_status.reconnects >= 1u);
    CHECK(client_status.retransmissions > 0u);
    um_light_peer_destroy(gateway);
    um_light_peer_destroy(client);
}

static void test_decoder_erasures_are_recovered(void)
{
    um_light_session_simulation_config config =
        um_light_session_simulation_default_config();
    um_light_session_simulation_result result;
    ++tests_run;

    config.client_payload_bytes = 1200u;
    config.gateway_payload_bytes = 1200u;
    config.client_to_gateway_drop_period = 0u;
    config.gateway_to_client_drop_period = 0u;
    config.client_to_gateway.noise_stddev = 0.43f;
    config.gateway_to_client.noise_stddev = 0.43f;
    config.retransmit_after_frames = 3u;
    config.link_timeout_frames = 24u;
    config.max_frames = 500u;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.decode_failures > 0u);
    CHECK(result.retransmissions > 0u);
    CHECK(result.gateway_received_bytes == config.client_payload_bytes);
    CHECK(result.client_received_bytes == config.gateway_payload_bytes);
}

static void test_terminal_one_way_blackout_fails_closed(void)
{
    um_light_session_simulation_config config =
        um_light_session_simulation_default_config();
    um_light_session_simulation_result result;
    ++tests_run;

    config.client_payload_bytes = 100u;
    config.gateway_payload_bytes = 100u;
    config.max_frames = 40u;
    config.client_to_gateway_drop_period = 1u;
    config.gateway_to_client_drop_period = 0u;
    config.blackout_frame_count = 0u;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) ==
          UM_ERR_TIMEOUT);
    CHECK(result.final_connected == 0);
    CHECK(result.frames_elapsed == config.max_frames);
    CHECK(result.gateway_received_bytes == 0u);
    CHECK(result.client_received_bytes == 0u);
    CHECK(result.scheduled_frame_drops == config.max_frames);
}

static void test_session_arguments_and_empty_stream(void)
{
    um_light_session_simulation_config config =
        um_light_session_simulation_default_config();
    um_light_session_simulation_result result;
    ++tests_run;

    config.client_payload_bytes = 0u;
    config.gateway_payload_bytes = 0u;
    config.client_to_gateway_drop_period = 2u;
    config.gateway_to_client_drop_period = 3u;
    config.blackout_frame_count = 0u;
    config.max_frames = 100u;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.frames_elapsed > 0u);
    CHECK(result.handshake_frames > 4u);
    CHECK(result.scheduled_frame_drops > 0u);
    CHECK(result.gateway_received_bytes == 0u);
    CHECK(result.client_received_bytes == 0u);

    config.transmit_window = 17u;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) ==
          UM_ERR_ARGUMENT);
    config.transmit_window = 8u;
    config.corner_jitter_pixels = -1.0f;
    CHECK(um_simulate_light_session(&config, &result, NULL, NULL) ==
          UM_ERR_ARGUMENT);
}

int main(void)
{
    test_full_duplex_session_with_erasures();
    test_recovers_after_bidirectional_blackout();
    test_arbitrary_peer_start_times();
    test_long_late_and_intermittent_peer();
    test_role_local_peers_with_independent_clocks();
    test_role_local_peer_arguments();
    test_foreign_discovery_waits_for_gateway_timeout();
    test_role_local_peers_through_optical_codec();
    test_decoder_erasures_are_recovered();
    test_terminal_one_way_blackout_fails_closed();
    test_session_arguments_and_empty_stream();

    if (failures != 0) {
        fprintf(stderr, "%d of %d light session tests failed\n", failures,
                tests_run);
        return 1;
    }
    printf("all %d light session tests passed\n", tests_run);
    return 0;
}
