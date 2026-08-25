#include "um_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    um_log_callback logger;
    void *context;
    char prefix[80];
} prefixed_logger;

static void emit_log(um_log_callback logger, void *context,
                     const char *message)
{
    if (logger != NULL) {
        logger(context, message);
    }
}

static void forward_prefixed_log(void *context, const char *message)
{
    prefixed_logger *prefixed = (prefixed_logger *)context;
    char line[480];
    (void)snprintf(line, sizeof(line), "%s%s", prefixed->prefix, message);
    prefixed->logger(prefixed->context, line);
}

static int expected_signal_failure(int status)
{
    return status == UM_ERR_SYNC || status == UM_ERR_TRUNCATED ||
           status == UM_ERR_HEADER || status == UM_ERR_CRC ||
           status == UM_ERR_RELIABILITY;
}

static void begin_ladder(um_distortion_ladder_result *result)
{
    memset(result, 0, sizeof(*result));
    result->first_failed_level = SIZE_MAX;
    result->first_failure_status = UM_OK;
}

static void describe_profile(const char *kind,
                             const um_distortion_profile *profile,
                             um_log_callback logger, void *logger_context)
{
    char line[512];
    (void)snprintf(
        line, sizeof(line),
        "%s pass=%zu profile=%s mask=0x%02x "
        "c2g{delay=%u gain=%.3f noise=%.4f echo=%u/%.2f clip=%.3f "
        "drop=%zu+%zu} g2c{delay=%u gain=%.3f noise=%.4f "
        "echo=%u/%.2f clip=%.3f drop=%zu+%zu} blackout=%.2fs",
        kind, profile->level, profile->name, profile->impairment_mask,
        profile->client_to_gateway.leading_silence,
        profile->client_to_gateway.gain,
        profile->client_to_gateway.noise_stddev,
        profile->client_to_gateway.echo_delay,
        profile->client_to_gateway.echo_gain,
        profile->client_to_gateway.clip_level,
        profile->client_to_gateway.dropout_start,
        profile->client_to_gateway.dropout_length,
        profile->gateway_to_client.leading_silence,
        profile->gateway_to_client.gain,
        profile->gateway_to_client.noise_stddev,
        profile->gateway_to_client.echo_delay,
        profile->gateway_to_client.echo_gain,
        profile->gateway_to_client.clip_level,
        profile->gateway_to_client.dropout_start,
        profile->gateway_to_client.dropout_length,
        profile->blackout_duration_seconds);
    emit_log(logger, logger_context, line);
}

static int finish_expected_failure(um_distortion_ladder_result *result,
                                   size_t level, int status,
                                   um_log_callback logger,
                                   void *logger_context, const char *kind)
{
    char line[192];
    result->first_failed_level = level;
    result->first_failure_status = status;
    (void)snprintf(line, sizeof(line),
                   "%s threshold reached at pass=%zu: %s", kind, level,
                   um_status_string(status));
    emit_log(logger, logger_context, line);
    if (level == 0u || !expected_signal_failure(status)) {
        return status;
    }
    return UM_OK;
}

int um_run_calibration_distortion_ladder(
    int high_quality, um_distortion_ladder_result *result,
    um_log_callback logger, void *logger_context)
{
    size_t level;
    if (result == NULL) {
        return UM_ERR_ARGUMENT;
    }
    begin_ladder(result);
    for (level = 0u; level < um_distortion_profile_count(); ++level) {
        um_distortion_profile profile;
        um_calibration_result forward;
        um_calibration_result reverse;
        prefixed_logger prefixed;
        char line[256];
        int status;

        if (um_distortion_profile_get(level, &profile) != UM_OK) {
            return UM_ERR_CONFIG;
        }
        ++result->passes_attempted;
        result->impairments_exercised |=
            profile.impairment_mask & UM_IMPAIR_ACOUSTIC_ALL;
        describe_profile("calibration", &profile, logger, logger_context);

        prefixed.logger = logger;
        prefixed.context = logger_context;
        (void)snprintf(prefixed.prefix, sizeof(prefixed.prefix),
                       "[pass=%zu c2g] ", level);
        status = um_calibrate_simulated(
            &profile.client_to_gateway, high_quality, &forward,
            logger != NULL ? forward_prefixed_log : NULL, &prefixed);
        ++result->calibrations_run;
        result->candidates_tested += forward.candidates_tested;
        result->verification_frames += forward.verification_frames;
        result->simulated_seconds += forward.estimated_seconds;
        if (status != UM_OK) {
            return finish_expected_failure(result, level, status, logger,
                                           logger_context, "calibration");
        }

        (void)snprintf(prefixed.prefix, sizeof(prefixed.prefix),
                       "[pass=%zu g2c] ", level);
        status = um_calibrate_simulated(
            &profile.gateway_to_client, high_quality, &reverse,
            logger != NULL ? forward_prefixed_log : NULL, &prefixed);
        ++result->calibrations_run;
        result->candidates_tested += reverse.candidates_tested;
        result->verification_frames += reverse.verification_frames;
        result->simulated_seconds += reverse.estimated_seconds;
        if (status != UM_OK) {
            return finish_expected_failure(result, level, status, logger,
                                           logger_context, "calibration");
        }

        ++result->passes_passed;
        (void)snprintf(
            line, sizeof(line),
            "calibration pass=%zu PASS viable=%zu/%zu c2g=%ubps g2c=%ubps "
            "seconds=%.2f",
            level, forward.candidates_viable + reverse.candidates_viable,
            forward.candidates_tested + reverse.candidates_tested,
            (unsigned)forward.payload_bps, (unsigned)reverse.payload_bps,
            forward.estimated_seconds + reverse.estimated_seconds);
        emit_log(logger, logger_context, line);
    }
    return UM_ERR_CONFIG;
}

int um_run_session_distortion_ladder(
    int high_quality, um_distortion_ladder_result *result,
    um_log_callback logger, void *logger_context)
{
    size_t level;
    if (result == NULL) {
        return UM_ERR_ARGUMENT;
    }
    begin_ladder(result);
    for (level = 0u; level < um_distortion_profile_count(); ++level) {
        um_distortion_profile profile;
        um_session_simulation_config config =
            um_session_simulation_default_config();
        um_session_simulation_result session_result;
        prefixed_logger prefixed;
        char line[256];
        int status;

        if (um_distortion_profile_get(level, &profile) != UM_OK) {
            return UM_ERR_CONFIG;
        }
        ++result->passes_attempted;
        result->impairments_exercised |= profile.impairment_mask;
        describe_profile("session", &profile, logger, logger_context);
        config.client_to_gateway = profile.client_to_gateway;
        config.gateway_to_client = profile.gateway_to_client;
        config.blackout_after_data_seconds =
            profile.blackout_after_data_seconds;
        config.blackout_duration_seconds = profile.blackout_duration_seconds;
        config.calibrate_high_quality = high_quality;
        config.frame_payload_bytes = 64u;
        config.random_seed ^= (uint32_t)level * UINT32_C(0x9e3779b9);

        prefixed.logger = logger;
        prefixed.context = logger_context;
        (void)snprintf(prefixed.prefix, sizeof(prefixed.prefix),
                       "[pass=%zu] ", level);
        status = um_simulate_session(
            &config, &session_result,
            logger != NULL ? forward_prefixed_log : NULL, &prefixed);
        result->simulated_seconds += session_result.elapsed_seconds;
        result->session_retries += session_result.retries;
        result->session_reconnects += session_result.reconnects;
        if (status == UM_OK &&
            (session_result.final_connected == 0 ||
             session_result.gateway_received_bytes !=
                 config.client_payload_bytes ||
             session_result.client_received_bytes !=
                 config.gateway_payload_bytes)) {
            status = UM_ERR_CRC;
        }
        if (status != UM_OK) {
            return finish_expected_failure(result, level, status, logger,
                                           logger_context, "session");
        }
        ++result->passes_passed;
        (void)snprintf(
            line, sizeof(line),
            "session pass=%zu PASS elapsed=%.2fs bytes=%zu/%zu retries=%zu "
            "reconnects=%zu",
            level, session_result.elapsed_seconds,
            session_result.gateway_received_bytes,
            session_result.client_received_bytes, session_result.retries,
            session_result.reconnects);
        emit_log(logger, logger_context, line);
    }
    return UM_ERR_CONFIG;
}
