#ifndef UM_H
#define UM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UM_SAMPLE_RATE 48000u
#define UM_FFT_SIZE 2048u
#define UM_LIGHT_GRID_SIZE 48u
#define UM_LIGHT_MAX_PAYLOAD 91u

typedef struct {
    float re;
    float im;
} um_complex;

typedef enum {
    UM_OK = 0,
    UM_ERR_ARGUMENT = -1,
    UM_ERR_MEMORY = -2,
    UM_ERR_CONFIG = -3,
    UM_ERR_SYNC = -4,
    UM_ERR_TRUNCATED = -5,
    UM_ERR_HEADER = -6,
    UM_ERR_CRC = -7,
    UM_ERR_CAPACITY = -8,
    UM_ERR_TIMEOUT = -9,
    UM_ERR_AUDIO = -10,
    UM_ERR_UNSUPPORTED = -11,
    UM_ERR_INTERRUPTED = -12,
    UM_ERR_RELIABILITY = -13,
    UM_ERR_NETWORK = -14,
    UM_ERR_VIDEO = -15
} um_status;

typedef enum {
    UM_FEC_RATE_1_2 = 0,
    UM_FEC_RATE_2_3 = 1,
    UM_FEC_RATE_3_4 = 2
} um_fec_rate;

typedef struct {
    unsigned fft_size;
    unsigned first_bin;
    unsigned last_bin;
    unsigned cyclic_prefix;
    unsigned window_samples;
    unsigned sync_samples;
    unsigned sync_gap;
    unsigned training_symbols;
    unsigned symbol_repetitions;
    unsigned qam_bits;
    um_fec_rate fec_rate;
} um_modem_config;

typedef struct {
    float sync_correlation;
    float signal_rms;
    float input_peak;
    float normalization_gain;
    float clipped_sample_fraction;
    float noise_rms;
    float evm_rms;
    float estimated_snr_db;
    size_t frame_start;
    size_t ofdm_symbols;
} um_rx_metrics;

typedef struct {
    unsigned leading_silence;
    float gain;
    float noise_stddev;
    unsigned echo_delay;
    float echo_gain;
    unsigned secondary_echo_delay;
    float secondary_echo_gain;
    float lowpass_hz;
    unsigned lowpass_stages;
    float interference_hz[2];
    float interference_amplitude[2];
    float sample_rate_offset_ppm;
    float clip_level;
    size_t dropout_start;
    size_t dropout_length;
    uint32_t random_seed;
} um_channel_config;

enum {
    UM_IMPAIR_LEVEL = 1u << 0,
    UM_IMPAIR_DELAY = 1u << 1,
    UM_IMPAIR_ECHO = 1u << 2,
    UM_IMPAIR_NOISE = 1u << 3,
    UM_IMPAIR_CLIPPING = 1u << 4,
    UM_IMPAIR_DROPOUT = 1u << 5,
    UM_IMPAIR_BLACKOUT = 1u << 6,
    UM_IMPAIR_ACOUSTIC_ALL = (1u << 6) - 1u,
    UM_IMPAIR_ALL = (1u << 7) - 1u
};

typedef struct {
    size_t level;
    const char *name;
    unsigned impairment_mask;
    um_channel_config client_to_gateway;
    um_channel_config gateway_to_client;
    float blackout_after_data_seconds;
    float blackout_duration_seconds;
} um_distortion_profile;

typedef void (*um_log_callback)(void *context, const char *message);

typedef struct {
    um_modem_config config;
    size_t candidates_tested;
    size_t candidates_viable;
    size_t candidates_verified;
    size_t verification_frames;
    float score;
    float estimated_seconds;
    float payload_bps;
    float success_rate;
    float evm_rms;
} um_calibration_result;

typedef struct {
    um_channel_config client_to_gateway;
    um_channel_config gateway_to_client;
    size_t client_payload_bytes;
    size_t gateway_payload_bytes;
    size_t frame_payload_bytes;
    float discovery_interval_seconds;
    float ack_timeout_seconds;
    float blackout_after_data_seconds;
    float blackout_duration_seconds;
    unsigned retry_limit;
    unsigned reconnect_limit;
    int calibrate_high_quality;
    uint32_t random_seed;
} um_session_simulation_config;

typedef struct {
    um_modem_config client_to_gateway_config;
    um_modem_config gateway_to_client_config;
    size_t gateway_received_bytes;
    size_t client_received_bytes;
    size_t discovery_requests;
    size_t offers;
    size_t confirmations;
    size_t data_frames;
    size_t acknowledgements;
    size_t retries;
    size_t reconnects;
    size_t decode_failures;
    size_t calibrations_completed;
    size_t calibration_candidates;
    size_t calibration_verification_frames;
    float elapsed_seconds;
    int final_connected;
} um_session_simulation_result;

typedef struct {
    size_t passes_attempted;
    size_t passes_passed;
    size_t first_failed_level;
    int first_failure_status;
    unsigned impairments_exercised;
    size_t calibrations_run;
    size_t candidates_tested;
    size_t verification_frames;
    size_t session_retries;
    size_t session_reconnects;
    float simulated_seconds;
} um_distortion_ladder_result;

typedef struct {
    float x;
    float y;
} um_light_point;

/* Pixel levels and noise are normalized to [0, 1].  Corners are pixel
 * coordinates in top-left, top-right, bottom-right, bottom-left order. */
typedef struct {
    size_t image_width;
    size_t image_height;
    um_light_point corners[4];
    float black_level;
    float white_level;
    float noise_stddev;
    unsigned blur_radius;
    float occlusion_x;
    float occlusion_y;
    float occlusion_width;
    float occlusion_height;
    uint32_t random_seed;
} um_light_channel_config;

typedef struct {
    um_light_point corners[4];
    float threshold;
    float contrast;
    float image_coverage;
    float corrected_bit_fraction;
    unsigned orientation;
} um_light_rx_metrics;

typedef struct {
    um_light_channel_config client_to_gateway;
    um_light_channel_config gateway_to_client;
    size_t client_payload_bytes;
    size_t gateway_payload_bytes;
    size_t max_frames;
    unsigned frames_per_second;
    unsigned transmit_window;
    unsigned retransmit_after_frames;
    unsigned link_timeout_frames;
    unsigned client_to_gateway_drop_period;
    unsigned gateway_to_client_drop_period;
    size_t blackout_start_frame;
    size_t blackout_frame_count;
    float corner_jitter_pixels;
    uint32_t random_seed;
} um_light_session_simulation_config;

typedef struct {
    size_t frames_elapsed;
    size_t handshake_frames;
    size_t data_frames;
    size_t acknowledgement_frames;
    size_t simultaneous_data_frames;
    size_t client_to_gateway_decoded_frames;
    size_t gateway_to_client_decoded_frames;
    size_t scheduled_frame_drops;
    size_t decode_failures;
    size_t protocol_rejections;
    size_t retransmissions;
    size_t duplicate_data_frames;
    size_t reconnects;
    size_t link_timeouts;
    size_t gateway_received_bytes;
    size_t client_received_bytes;
    float client_to_gateway_average_correction;
    float gateway_to_client_average_correction;
    float elapsed_seconds;
    float payload_goodput_bps;
    int final_connected;
} um_light_session_simulation_result;

typedef struct {
    um_light_session_simulation_config session;
    unsigned mtu;
    size_t client_packet_count;
    size_t gateway_packet_count;
    uint32_t random_seed;
} um_light_network_simulation_config;

typedef struct {
    um_light_session_simulation_result session;
    size_t client_packets_sent;
    size_t gateway_packets_sent;
    size_t gateway_packets_received;
    size_t client_packets_received;
    size_t client_ip_bytes_sent;
    size_t gateway_ip_bytes_sent;
    size_t gateway_ip_bytes_received;
    size_t client_ip_bytes_received;
    size_t udp_packets;
    size_t tcp_packets;
    size_t packets_spanning_optical_frames;
    size_t framing_overhead_bytes;
    size_t framing_errors;
    size_t checksum_errors;
    uint32_t client_stream_crc32;
    uint32_t gateway_stream_crc32;
    float ip_goodput_bps;
} um_light_network_simulation_result;

typedef enum {
    UM_LIVE_GATEWAY = 1,
    UM_LIVE_CLIENT = 2
} um_live_role;

typedef struct {
    um_live_role role;
    const char *input_device;
    const char *output_device;
    int link_test;
    size_t test_bytes;
    size_t chunk_bytes;
    unsigned retry_limit;
    float discovery_interval_seconds;
    int calibrate_high_quality;
    const char *calibration_path;
    size_t proxy_test_packets;
    int filter_background_traffic;
    int allow_messages_traffic;
} um_live_audio_options;

uint32_t um_crc32(const uint8_t *data, size_t length);
uint16_t um_crc16(const uint8_t *data, size_t length);

size_t um_fec_encoded_bits(size_t data_bits, um_fec_rate rate);
int um_fec_encode(const uint8_t *bits, size_t data_bits, um_fec_rate rate,
                  uint8_t *encoded, size_t capacity, size_t *encoded_bits);
int um_fec_decode(const float *soft_bits, size_t soft_count, size_t data_bits,
                  um_fec_rate rate, uint8_t *decoded, size_t capacity);

int um_interleave_bits(const uint8_t *input, uint8_t *output, size_t count);
int um_deinterleave_soft(const float *input, float *output, size_t count);

unsigned um_qam_bits_per_symbol(unsigned qam_order);
um_complex um_qam_map(const uint8_t *bits, unsigned bit_count);
int um_qam_soft_demod(um_complex sample, unsigned bit_count, float *soft_bits);

int um_fft(um_complex *values, size_t count, int inverse);

um_modem_config um_modem_default_config(void);
int um_modem_config_validate(const um_modem_config *config);
size_t um_modem_data_carriers(const um_modem_config *config);

int um_modulate_frame(const um_modem_config *config,
                      const uint8_t *payload, size_t payload_length,
                      uint16_t sequence, float **samples, size_t *sample_count);
int um_demodulate_frame(const um_modem_config *config,
                        const float *samples, size_t sample_count,
                        uint8_t *payload, size_t payload_capacity,
                        size_t *payload_length, uint16_t *sequence,
                        um_rx_metrics *metrics);

um_channel_config um_channel_default_config(void);
int um_channel_apply(const float *input, size_t input_count,
                     const um_channel_config *config,
                     float **output, size_t *output_count);

size_t um_distortion_profile_count(void);
int um_distortion_profile_get(size_t level, um_distortion_profile *profile);

int um_calibrate_simulated(const um_channel_config *channel, int high_quality,
                           um_calibration_result *result,
                           um_log_callback logger, void *logger_context);

um_session_simulation_config um_session_simulation_default_config(void);
int um_simulate_session(const um_session_simulation_config *config,
                        um_session_simulation_result *result,
                        um_log_callback logger, void *logger_context);

int um_run_calibration_distortion_ladder(
    int high_quality, um_distortion_ladder_result *result,
    um_log_callback logger, void *logger_context);
int um_run_session_distortion_ladder(
    int high_quality, um_distortion_ladder_result *result,
    um_log_callback logger, void *logger_context);

um_light_channel_config um_light_channel_default_config(void);
int um_light_encode_frame(uint8_t frame_type, uint32_t session_id,
                          uint32_t sequence, const uint8_t *payload,
                          size_t payload_length, uint8_t *modules,
                          size_t module_capacity);
int um_light_render_frame(const uint8_t *modules, size_t module_count,
                          const um_light_channel_config *config,
                          uint8_t **pixels, size_t *pixel_count);
int um_light_decode_frame(const uint8_t *pixels, size_t width, size_t height,
                          size_t stride, uint8_t *frame_type,
                          uint32_t *session_id, uint32_t *sequence,
                          uint8_t *payload, size_t payload_capacity,
                          size_t *payload_length,
                          um_light_rx_metrics *metrics);

um_light_session_simulation_config
um_light_session_simulation_default_config(void);
int um_simulate_light_session(
    const um_light_session_simulation_config *config,
    um_light_session_simulation_result *result, um_log_callback logger,
    void *logger_context);

um_light_network_simulation_config
um_light_network_simulation_default_config(void);
int um_simulate_light_network(
    const um_light_network_simulation_config *config,
    um_light_network_simulation_result *result, um_log_callback logger,
    void *logger_context);

int um_audio_list_devices(um_log_callback logger, void *logger_context);
um_live_audio_options um_live_audio_default_options(um_live_role role);
int um_run_live_audio(const um_live_audio_options *options,
                      um_log_callback logger, void *logger_context);

const char *um_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif
