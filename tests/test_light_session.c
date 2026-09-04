#include "um.h"

#include <stdio.h>

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
