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

static void test_default_mixed_ipv4_traffic(void)
{
    um_light_network_simulation_config config =
        um_light_network_simulation_default_config();
    um_light_network_simulation_result result;
    ++tests_run;

    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.session.final_connected != 0);
    CHECK(result.client_packets_sent == config.client_packet_count);
    CHECK(result.gateway_packets_sent == config.gateway_packet_count);
    CHECK(result.gateway_packets_received == config.client_packet_count);
    CHECK(result.client_packets_received == config.gateway_packet_count);
    CHECK(result.gateway_ip_bytes_received == result.client_ip_bytes_sent);
    CHECK(result.client_ip_bytes_received == result.gateway_ip_bytes_sent);
    CHECK(result.client_ip_bytes_sent == 3049u);
    CHECK(result.gateway_ip_bytes_sent == 4269u);
    CHECK(result.session.gateway_received_bytes == 3089u);
    CHECK(result.session.client_received_bytes == 4317u);
    CHECK(result.udp_packets == 22u);
    CHECK(result.tcp_packets == 22u);
    CHECK(result.udp_packets + result.tcp_packets ==
          config.client_packet_count + config.gateway_packet_count);
    CHECK(result.packets_spanning_optical_frames == 35u);
    CHECK(result.framing_overhead_bytes ==
          2u * (config.client_packet_count + config.gateway_packet_count));
    CHECK(result.framing_errors == 0u);
    CHECK(result.checksum_errors == 0u);
    CHECK(result.session.frames_elapsed == 63u);
    CHECK(result.session.scheduled_frame_drops == 9u);
    CHECK(result.session.retransmissions == 8u);
    CHECK(result.session.simultaneous_data_frames == 41u);
    CHECK(result.ip_goodput_bps > 13900.0f);
    CHECK(result.ip_goodput_bps < 14000.0f);
    CHECK(result.client_stream_crc32 == UINT32_C(0x0294c5de));
    CHECK(result.gateway_stream_crc32 == UINT32_C(0x5a867729));
}

static void test_full_mtu_packets_cross_many_frames(void)
{
    um_light_network_simulation_config config =
        um_light_network_simulation_default_config();
    um_light_network_simulation_result result;
    ++tests_run;

    config.mtu = 1500u;
    config.client_packet_count = 12u;
    config.gateway_packet_count = 13u;
    config.session.max_frames = 600u;
    config.session.client_to_gateway_drop_period = 7u;
    config.session.gateway_to_client_drop_period = 11u;
    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.gateway_packets_received == config.client_packet_count);
    CHECK(result.client_packets_received == config.gateway_packet_count);
    CHECK(result.client_ip_bytes_sent >= 1500u);
    CHECK(result.gateway_ip_bytes_sent >= 1500u);
    CHECK(result.packets_spanning_optical_frames == 18u);
    CHECK(result.session.data_frames >
          config.client_packet_count + config.gateway_packet_count);
    CHECK(result.session.retransmissions > 0u);
    CHECK(result.framing_errors == 0u);
    CHECK(result.checksum_errors == 0u);
}

static void test_packet_queues_survive_rediscovery(void)
{
    um_light_network_simulation_config config =
        um_light_network_simulation_default_config();
    um_light_network_simulation_result result;
    ++tests_run;

    config.client_packet_count = 18u;
    config.gateway_packet_count = 18u;
    config.session.client_to_gateway_drop_period = 0u;
    config.session.gateway_to_client_drop_period = 0u;
    config.session.blackout_start_frame = 20u;
    config.session.blackout_frame_count = 16u;
    config.session.link_timeout_frames = 6u;
    config.session.max_frames = 400u;
    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.session.link_timeouts >= 2u);
    CHECK(result.session.reconnects >= 1u);
    CHECK(result.session.retransmissions > 0u);
    CHECK(result.gateway_packets_received == config.client_packet_count);
    CHECK(result.client_packets_received == config.gateway_packet_count);
    CHECK(result.gateway_ip_bytes_received == result.client_ip_bytes_sent);
    CHECK(result.client_ip_bytes_received == result.gateway_ip_bytes_sent);
    CHECK(result.framing_errors == 0u);
    CHECK(result.checksum_errors == 0u);
}

static void test_checksum_valid_packets_recover_near_codec_cliff(void)
{
    um_light_network_simulation_config config =
        um_light_network_simulation_default_config();
    um_light_network_simulation_result result;
    ++tests_run;

    config.mtu = 300u;
    config.client_packet_count = 12u;
    config.gateway_packet_count = 12u;
    config.session.client_to_gateway_drop_period = 0u;
    config.session.gateway_to_client_drop_period = 0u;
    config.session.client_to_gateway.noise_stddev = 0.43f;
    config.session.gateway_to_client.noise_stddev = 0.43f;
    config.session.retransmit_after_frames = 3u;
    config.session.link_timeout_frames = 24u;
    config.session.max_frames = 1000u;
    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) == UM_OK);
    CHECK(result.session.decode_failures > 0u);
    CHECK(result.session.retransmissions > 0u);
    CHECK(result.gateway_packets_received == config.client_packet_count);
    CHECK(result.client_packets_received == config.gateway_packet_count);
    CHECK(result.framing_errors == 0u);
    CHECK(result.checksum_errors == 0u);
}

static void test_terminal_failure_and_invalid_arguments(void)
{
    um_light_network_simulation_config config =
        um_light_network_simulation_default_config();
    um_light_network_simulation_result result;
    ++tests_run;

    config.client_packet_count = 2u;
    config.gateway_packet_count = 2u;
    config.session.max_frames = 40u;
    config.session.client_to_gateway_drop_period = 1u;
    config.session.gateway_to_client_drop_period = 0u;
    config.session.blackout_frame_count = 0u;
    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) ==
          UM_ERR_TIMEOUT);
    CHECK(result.session.final_connected == 0);
    CHECK(result.session.frames_elapsed == config.session.max_frames);
    CHECK(result.gateway_packets_received == 0u);
    CHECK(result.client_packets_received == 0u);

    config = um_light_network_simulation_default_config();
    config.mtu = 67u;
    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) ==
          UM_ERR_ARGUMENT);
    config = um_light_network_simulation_default_config();
    config.mtu = 1501u;
    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) ==
          UM_ERR_ARGUMENT);
    config = um_light_network_simulation_default_config();
    config.client_packet_count = 4097u;
    CHECK(um_simulate_light_network(&config, &result, NULL, NULL) ==
          UM_ERR_ARGUMENT);
    CHECK(um_simulate_light_network(NULL, &result, NULL, NULL) ==
          UM_ERR_ARGUMENT);
    CHECK(um_simulate_light_network(&config, NULL, NULL, NULL) ==
          UM_ERR_ARGUMENT);
}

int main(void)
{
    test_default_mixed_ipv4_traffic();
    test_full_mtu_packets_cross_many_frames();
    test_packet_queues_survive_rediscovery();
    test_checksum_valid_packets_recover_near_codec_cliff();
    test_terminal_failure_and_invalid_arguments();

    if (failures != 0) {
        fprintf(stderr, "%d of %d light network tests failed\n", failures,
                tests_run);
        return 1;
    }
    printf("all %d light network tests passed\n", tests_run);
    return 0;
}
