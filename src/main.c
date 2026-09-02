#include "um.h"
#include "live_wire.h"
#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  universal-modem --simulate [--qam 4|16|64] "
            "[--fec 1/2|2/3|3/4] [--noise LEVEL]\n"
            "  universal-modem --calibrate [--calib-high]\n"
            "  universal-modem --session-sim [--calib-high]\n"
            "  universal-modem --list-audio\n"
            "  universal-modem --gateway [audio options]\n"
            "  universal-modem --client [audio options]\n"
            "\nAudio options:\n"
            "  --audio              Explicitly select the default audio medium\n"
            "  --input-device ID    Capture device ID shown at startup\n"
            "  --output-device ID   Playback device ID shown at startup\n"
            "  --link-test          Finite bidirectional data test; no TUN/routes\n"
            "  --test-bytes N       Link-test bytes each direction (default 1024)\n"
            "  --chunk-bytes N      Maximum calibrated frame body (default 6144)\n"
            "  --retries N          Attempts per acknowledged frame (default 4)\n"
            "  --calib-high         Use the extended real-audio calibration\n"
            "  --allow-background   Disable the default quiet-link firewall\n"
            "  calibration.config  Auto-loaded/saved; delete it to recalibrate\n");
}

static void print_log(void *context, const char *message)
{
    FILE *stream = (FILE *)context;
    fprintf(stream, "%s\n", message);
    fflush(stream);
}

static int run_calibration(int high_quality)
{
    um_distortion_ladder_result result;
    int status;

    status = um_run_calibration_distortion_ladder(
        high_quality, &result, print_log, stdout);
    if (status != UM_OK) {
        fprintf(stderr, "calibration ladder failed unexpectedly: %s\n",
                um_status_string(status));
        return 1;
    }
    printf("calibration ladder complete passes=%zu/%zu first-failure=%zu "
           "reason=%s calibrations=%zu candidates=%zu verification=%zu "
           "simulated=%.1fs\n",
           result.passes_passed, result.passes_attempted,
           result.first_failed_level,
           um_status_string(result.first_failure_status),
           result.calibrations_run, result.candidates_tested,
           result.verification_frames,
           result.simulated_seconds);
    return 0;
}

static int run_session_simulation(int high_quality)
{
    um_distortion_ladder_result result;
    int status;
    status = um_run_session_distortion_ladder(
        high_quality, &result, print_log, stdout);
    if (status != UM_OK) {
        fprintf(stderr, "session distortion ladder failed unexpectedly: %s\n",
                um_status_string(status));
        return 1;
    }
    printf("session ladder complete passes=%zu/%zu first-failure=%zu "
           "reason=%s retries=%zu reconnects=%zu simulated=%.1fs\n",
           result.passes_passed, result.passes_attempted,
           result.first_failed_level,
           um_status_string(result.first_failure_status),
           result.session_retries, result.session_reconnects,
           result.simulated_seconds);
    return 0;
}

static int parse_qam(const char *text, unsigned *bits)
{
    char *end = NULL;
    unsigned long order = strtoul(text, &end, 10);
    unsigned parsed = um_qam_bits_per_symbol((unsigned)order);
    if (end == text || *end != '\0' || parsed == 0u) {
        return 0;
    }
    *bits = parsed;
    return 1;
}

static int parse_fec(const char *text, um_fec_rate *rate)
{
    if (strcmp(text, "1/2") == 0) {
        *rate = UM_FEC_RATE_1_2;
    } else if (strcmp(text, "2/3") == 0) {
        *rate = UM_FEC_RATE_2_3;
    } else if (strcmp(text, "3/4") == 0) {
        *rate = UM_FEC_RATE_3_4;
    } else {
        return 0;
    }
    return 1;
}

static int run_simulation(um_modem_config config, float noise)
{
    static const uint8_t message[] =
        "Universal Modem: coded OFDM loopback is operational.";
    uint8_t decoded[sizeof(message)];
    float *transmitted = NULL;
    float *received = NULL;
    size_t transmitted_count = 0u;
    size_t received_count = 0u;
    size_t decoded_length = 0u;
    uint16_t sequence = 0u;
    um_rx_metrics metrics;
    um_channel_config channel = um_channel_default_config();
    int status;

    channel.leading_silence = 337u;
    channel.gain = 0.35f;
    channel.noise_stddev = noise;
    channel.echo_delay = 17u;
    channel.echo_gain = 0.28f;

    status = um_modulate_frame(&config, message, sizeof(message) - 1u, 42u,
                               &transmitted, &transmitted_count);
    if (status != UM_OK) {
        fprintf(stderr, "modulation failed: %s\n", um_status_string(status));
        goto done;
    }
    status = um_channel_apply(transmitted, transmitted_count, &channel,
                              &received, &received_count);
    if (status != UM_OK) {
        fprintf(stderr, "channel simulation failed: %s\n",
                um_status_string(status));
        goto done;
    }
    status = um_demodulate_frame(&config, received, received_count, decoded,
                                 sizeof(decoded), &decoded_length, &sequence,
                                 &metrics);
    if (status != UM_OK) {
        fprintf(stderr, "decoding failed: %s\n", um_status_string(status));
        goto done;
    }

    printf("decoded sequence=%u bytes=%zu qam=%u fec=%u/%u\n",
           (unsigned)sequence, decoded_length, 1u << config.qam_bits,
           config.fec_rate == UM_FEC_RATE_1_2 ? 1u :
           config.fec_rate == UM_FEC_RATE_2_3 ? 2u : 3u,
           config.fec_rate == UM_FEC_RATE_1_2 ? 2u :
           config.fec_rate == UM_FEC_RATE_2_3 ? 3u : 4u);
    printf("sync=%.3f snr=%.1f dB evm=%.3f duration=%.3f s\n",
           metrics.sync_correlation, metrics.estimated_snr_db,
           metrics.evm_rms,
           (double)transmitted_count / (double)UM_SAMPLE_RATE);
    printf("payload: %.*s\n", (int)decoded_length, (const char *)decoded);

done:
    free(received);
    free(transmitted);
    return status == UM_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    um_modem_config config = um_modem_default_config();
    float noise = 0.002f;
    int simulate = 0;
    int calibrate = 0;
    int session_simulation = 0;
    int high_quality = 0;
    int audio = 0;
    int link_test = 0;
    int allow_background = 0;
    int endpoint = 0;
    int list_audio = 0;
    int noise_was_set = 0;
    const char *input_device = "default";
    const char *output_device = "default";
    size_t test_bytes = 1024u;
    size_t chunk_bytes = UM_LIVE_MAX_BODY;
    unsigned retries = 4u;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--simulate") == 0) {
            simulate = 1;
        } else if (strcmp(argv[i], "--calibrate") == 0) {
            calibrate = 1;
        } else if (strcmp(argv[i], "--session-sim") == 0) {
            session_simulation = 1;
        } else if (strcmp(argv[i], "--calib-high") == 0) {
            high_quality = 1;
        } else if (strcmp(argv[i], "--audio") == 0) {
            audio = 1;
        } else if (strcmp(argv[i], "--link-test") == 0) {
            link_test = 1;
        } else if (strcmp(argv[i], "--allow-background") == 0) {
            allow_background = 1;
        } else if (strcmp(argv[i], "--list-audio") == 0) {
            list_audio = 1;
        } else if (strcmp(argv[i], "--gateway") == 0) {
            if (endpoint != 0) {
                fprintf(stderr, "choose exactly one of --gateway or --client\n");
                return 2;
            }
            endpoint = 1;
        } else if (strcmp(argv[i], "--client") == 0) {
            if (endpoint != 0) {
                fprintf(stderr, "choose exactly one of --gateway or --client\n");
                return 2;
            }
            endpoint = 2;
        } else if (strcmp(argv[i], "--input-device") == 0 && i + 1 < argc) {
            input_device = argv[++i];
        } else if (strcmp(argv[i], "--output-device") == 0 && i + 1 < argc) {
            output_device = argv[++i];
        } else if (strcmp(argv[i], "--test-bytes") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value == 0ul ||
                value > UINT32_MAX) {
                fprintf(stderr, "invalid test byte count\n");
                return 2;
            }
            test_bytes = (size_t)value;
        } else if (strcmp(argv[i], "--chunk-bytes") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value == 0ul ||
                value > UM_LIVE_MAX_BODY) {
                fprintf(stderr,
                        "chunk bytes must be between 1 and %u\n",
                        UM_LIVE_MAX_BODY);
                return 2;
            }
            chunk_bytes = (size_t)value;
        } else if (strcmp(argv[i], "--retries") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value == 0ul ||
                value > 100ul) {
                fprintf(stderr, "retries must be between 1 and 100\n");
                return 2;
            }
            retries = (unsigned)value;
        } else if (strcmp(argv[i], "--qam") == 0 && i + 1 < argc) {
            if (!parse_qam(argv[++i], &config.qam_bits)) {
                fprintf(stderr, "invalid QAM order\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--fec") == 0 && i + 1 < argc) {
            if (!parse_fec(argv[++i], &config.fec_rate)) {
                fprintf(stderr, "invalid FEC rate\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--noise") == 0 && i + 1 < argc) {
            char *end = NULL;
            noise = strtof(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || noise < 0.0f) {
                fprintf(stderr, "invalid noise level\n");
                return 2;
            }
            noise_was_set = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    if (list_audio != 0) {
        int status = um_network_prepare_audio_user(print_log, stdout);
        if (status != UM_OK) {
            fprintf(stderr, "could not select the invoking user for audio\n");
            return 1;
        }
        status = um_audio_list_devices(print_log, stdout);
        return status == UM_OK ? 0 : 1;
    }

    if (simulate != 0 && audio == 0 && endpoint == 0) {
        return run_simulation(config, noise);
    }
    if (calibrate != 0 && simulate == 0 && audio == 0 && endpoint == 0) {
        if (noise_was_set != 0) {
            fprintf(stderr,
                    "--noise applies to --simulate; calibration uses the "
                    "progressive distortion ladder\n");
            return 2;
        }
        return run_calibration(high_quality);
    }
    if (session_simulation != 0 && simulate == 0 && calibrate == 0 &&
        audio == 0 && endpoint == 0) {
        if (noise_was_set != 0) {
            fprintf(stderr,
                    "--noise applies to --simulate; session simulation uses "
                    "the progressive distortion ladder\n");
            return 2;
        }
        return run_session_simulation(high_quality);
    }
    if (endpoint != 0 && simulate == 0 && calibrate == 0 &&
        session_simulation == 0) {
        um_live_audio_options options = um_live_audio_default_options(
            endpoint == 1 ? UM_LIVE_GATEWAY : UM_LIVE_CLIENT);
        int status = um_network_prepare_audio_user(print_log, stdout);
        if (status != UM_OK) {
            fprintf(stderr, "could not select the invoking user for audio\n");
            return 1;
        }
        if (noise_was_set != 0) {
            fprintf(stderr, "--noise only applies to --simulate\n");
            return 2;
        }
        options.input_device = input_device;
        options.output_device = output_device;
        options.link_test = link_test;
        options.test_bytes = test_bytes;
        options.chunk_bytes = chunk_bytes;
        options.retry_limit = retries;
        options.calibrate_high_quality = high_quality;
        options.filter_background_traffic = allow_background == 0;
        status = um_run_live_audio(&options, print_log, stdout);
        if (status == UM_ERR_INTERRUPTED) {
            return 130;
        }
        if (status != UM_OK) {
            fprintf(stderr, "real-audio session failed: %s\n",
                    um_status_string(status));
            return 1;
        }
        return 0;
    }
    usage(stderr);
    return 2;
}
