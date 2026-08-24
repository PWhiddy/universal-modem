#ifndef UM_H
#define UM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UM_SAMPLE_RATE 48000u
#define UM_FFT_SIZE 256u

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
    UM_ERR_CAPACITY = -8
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
    unsigned qam_bits;
    um_fec_rate fec_rate;
} um_modem_config;

typedef struct {
    float sync_correlation;
    float signal_rms;
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

const char *um_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif
