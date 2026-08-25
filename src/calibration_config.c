#define _POSIX_C_SOURCE 200809L

#include "um_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALIBRATION_CONFIG_VERSION 1u
#define REQUIRED_FIELDS UINT32_C(0x7fff)

static const char *role_name(um_live_role role)
{
    return role == UM_LIVE_CLIENT ? "client" : "gateway";
}

static const char *direction_name(um_live_role role)
{
    return role == UM_LIVE_CLIENT ? "gateway-to-client"
                                  : "client-to-gateway";
}

static int parse_unsigned(const char *text, unsigned *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT_MAX) {
        return 0;
    }
    *value = (unsigned)parsed;
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

static char *trim(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    end = text + strlen(text);
    while (end != text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
            end[-1] == '\r')) {
        --end;
    }
    *end = '\0';
    return text;
}

static int set_field(const char *key, const char *value,
                     um_live_role expected_role, um_modem_config *config,
                     uint32_t *fields)
{
    unsigned parsed;
    uint32_t bit;
    if (strcmp(key, "format") == 0) {
        bit = UINT32_C(1) << 0u;
        if (!parse_unsigned(value, &parsed) ||
            parsed != CALIBRATION_CONFIG_VERSION) {
            return 0;
        }
    } else if (strcmp(key, "protocol") == 0) {
        bit = UINT32_C(1) << 1u;
        if (!parse_unsigned(value, &parsed) ||
            parsed != UM_LIVE_PROTOCOL_VERSION) {
            return 0;
        }
    } else if (strcmp(key, "role") == 0) {
        bit = UINT32_C(1) << 2u;
        if (strcmp(value, role_name(expected_role)) != 0) {
            return 0;
        }
    } else if (strcmp(key, "direction") == 0) {
        bit = UINT32_C(1) << 3u;
        if (strcmp(value, direction_name(expected_role)) != 0) {
            return 0;
        }
    } else if (strcmp(key, "fft_size") == 0) {
        bit = UINT32_C(1) << 4u;
        if (!parse_unsigned(value, &config->fft_size)) {
            return 0;
        }
    } else if (strcmp(key, "first_bin") == 0) {
        bit = UINT32_C(1) << 5u;
        if (!parse_unsigned(value, &config->first_bin)) {
            return 0;
        }
    } else if (strcmp(key, "last_bin") == 0) {
        bit = UINT32_C(1) << 6u;
        if (!parse_unsigned(value, &config->last_bin)) {
            return 0;
        }
    } else if (strcmp(key, "cyclic_prefix") == 0) {
        bit = UINT32_C(1) << 7u;
        if (!parse_unsigned(value, &config->cyclic_prefix)) {
            return 0;
        }
    } else if (strcmp(key, "window_samples") == 0) {
        bit = UINT32_C(1) << 8u;
        if (!parse_unsigned(value, &config->window_samples)) {
            return 0;
        }
    } else if (strcmp(key, "sync_samples") == 0) {
        bit = UINT32_C(1) << 9u;
        if (!parse_unsigned(value, &config->sync_samples)) {
            return 0;
        }
    } else if (strcmp(key, "sync_gap") == 0) {
        bit = UINT32_C(1) << 10u;
        if (!parse_unsigned(value, &config->sync_gap)) {
            return 0;
        }
    } else if (strcmp(key, "training_symbols") == 0) {
        bit = UINT32_C(1) << 11u;
        if (!parse_unsigned(value, &config->training_symbols)) {
            return 0;
        }
    } else if (strcmp(key, "symbol_repetitions") == 0) {
        bit = UINT32_C(1) << 12u;
        if (!parse_unsigned(value, &config->symbol_repetitions)) {
            return 0;
        }
    } else if (strcmp(key, "qam") == 0) {
        bit = UINT32_C(1) << 13u;
        if (!parse_unsigned(value, &parsed)) {
            return 0;
        }
        config->qam_bits = um_qam_bits_per_symbol(parsed);
        if (config->qam_bits == 0u) {
            return 0;
        }
    } else if (strcmp(key, "fec_rate") == 0) {
        bit = UINT32_C(1) << 14u;
        if (!parse_fec(value, &config->fec_rate)) {
            return 0;
        }
    } else {
        return 0;
    }
    if ((*fields & bit) != 0u) {
        return 0;
    }
    *fields |= bit;
    return 1;
}

int um_calibration_config_load(const char *path, um_live_role role,
                               um_modem_config *config, int *found)
{
    FILE *file;
    char line[256];
    uint32_t fields = 0u;
    int status = UM_OK;
    if (path == NULL || config == NULL || found == NULL ||
        (role != UM_LIVE_CLIENT && role != UM_LIVE_GATEWAY)) {
        return UM_ERR_ARGUMENT;
    }
    *found = 0;
    file = fopen(path, "r");
    if (file == NULL) {
        return errno == ENOENT ? UM_OK : UM_ERR_CONFIG;
    }
    memset(config, 0, sizeof(*config));
    while (fgets(line, sizeof(line), file) != NULL) {
        char *key = trim(line);
        char *separator;
        char *value;
        if (*key == '\0' || *key == '#') {
            continue;
        }
        separator = strchr(key, '=');
        if (separator == NULL) {
            status = UM_ERR_CONFIG;
            break;
        }
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);
        if (*key == '\0' || *value == '\0' ||
            set_field(key, value, role, config, &fields) == 0) {
            status = UM_ERR_CONFIG;
            break;
        }
    }
    if (status == UM_OK && ferror(file) != 0) {
        status = UM_ERR_CONFIG;
    }
    if (fclose(file) != 0 && status == UM_OK) {
        status = UM_ERR_CONFIG;
    }
    if (status == UM_OK &&
        (fields != REQUIRED_FIELDS ||
         um_modem_config_validate(config) != UM_OK)) {
        status = UM_ERR_CONFIG;
    }
    if (status == UM_OK) {
        *found = 1;
    }
    return status;
}

int um_calibration_config_save(const char *path, um_live_role role,
                               const um_modem_config *config)
{
    const char *fec;
    char *temporary;
    size_t path_length;
    FILE *file;
    int status = UM_OK;
    if (path == NULL || config == NULL ||
        (role != UM_LIVE_CLIENT && role != UM_LIVE_GATEWAY) ||
        um_modem_config_validate(config) != UM_OK) {
        return UM_ERR_ARGUMENT;
    }
    fec = config->fec_rate == UM_FEC_RATE_1_2
              ? "1/2"
              : config->fec_rate == UM_FEC_RATE_2_3 ? "2/3" : "3/4";
    path_length = strlen(path);
    if (path_length > SIZE_MAX - 5u) {
        return UM_ERR_CAPACITY;
    }
    temporary = (char *)malloc(path_length + 5u);
    if (temporary == NULL) {
        return UM_ERR_MEMORY;
    }
    (void)snprintf(temporary, path_length + 5u, "%s.tmp", path);
    file = fopen(temporary, "w");
    if (file == NULL) {
        free(temporary);
        return UM_ERR_CONFIG;
    }
    if (fprintf(file,
                "# Universal Modem receive calibration\n"
                "format=%u\n"
                "protocol=%u\n"
                "role=%s\n"
                "direction=%s\n"
                "fft_size=%u\n"
                "first_bin=%u\n"
                "last_bin=%u\n"
                "cyclic_prefix=%u\n"
                "window_samples=%u\n"
                "sync_samples=%u\n"
                "sync_gap=%u\n"
                "training_symbols=%u\n"
                "symbol_repetitions=%u\n"
                "qam=%u\n"
                "fec_rate=%s\n",
                CALIBRATION_CONFIG_VERSION, UM_LIVE_PROTOCOL_VERSION,
                role_name(role), direction_name(role), config->fft_size,
                config->first_bin, config->last_bin, config->cyclic_prefix,
                config->window_samples, config->sync_samples,
                config->sync_gap, config->training_symbols,
                config->symbol_repetitions, 1u << config->qam_bits, fec) < 0 ||
        fflush(file) != 0) {
        status = UM_ERR_CONFIG;
    }
    if (fclose(file) != 0) {
        status = UM_ERR_CONFIG;
    }
    if (status == UM_OK && rename(temporary, path) != 0) {
        status = UM_ERR_CONFIG;
    }
    if (status != UM_OK) {
        (void)remove(temporary);
    }
    free(temporary);
    return status;
}
