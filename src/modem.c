#include "um_internal.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

static int is_pilot(const um_modem_config *config, unsigned bin)
{
    return ((bin - config->first_bin) % 12u) == 0u;
}

static float known_sign(unsigned bin, size_t symbol)
{
    uint32_t value = (uint32_t)bin * UINT32_C(0x9e3779b1) ^
                     (uint32_t)symbol * UINT32_C(0x85ebca6b) ^
                     UINT32_C(0xa5c31e27);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    return (value & 1u) != 0u ? 1.0f : -1.0f;
}

static size_t divide_round_up(size_t numerator, size_t denominator)
{
    return numerator / denominator + (numerator % denominator != 0u ? 1u : 0u);
}

um_modem_config um_modem_default_config(void)
{
    um_modem_config config;
    config.fft_size = UM_FFT_SIZE;
    config.first_bin = 16u;
    config.last_bin = 72u;
    config.cyclic_prefix = 32u;
    config.window_samples = 8u;
    config.qam_bits = 4u;
    config.fec_rate = UM_FEC_RATE_1_2;
    return config;
}

int um_modem_config_validate(const um_modem_config *config)
{
    size_t carriers;
    if (config == NULL || config->fft_size != UM_FFT_SIZE ||
        config->first_bin < 2u || config->last_bin < config->first_bin ||
        config->last_bin >= config->fft_size / 2u ||
        config->cyclic_prefix < 8u ||
        config->cyclic_prefix > config->fft_size / 2u ||
        config->window_samples > config->cyclic_prefix ||
        (config->qam_bits != 2u && config->qam_bits != 4u &&
         config->qam_bits != 6u) ||
        (config->fec_rate != UM_FEC_RATE_1_2 &&
         config->fec_rate != UM_FEC_RATE_2_3 &&
         config->fec_rate != UM_FEC_RATE_3_4)) {
        return UM_ERR_CONFIG;
    }
    carriers = um_modem_data_carriers(config);
    return carriers >= 8u ? UM_OK : UM_ERR_CONFIG;
}

size_t um_modem_data_carriers(const um_modem_config *config)
{
    size_t count = 0u;
    unsigned bin;
    if (config == NULL || config->last_bin < config->first_bin) {
        return 0u;
    }
    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        if (!is_pilot(config, bin)) {
            ++count;
        }
    }
    return count;
}

static void make_sync(float *sync)
{
    size_t i;
    float phase = 0.0f;
    const float start_frequency = 1800.0f;
    const float end_frequency = 18200.0f;
    for (i = 0u; i < UM_SYNC_SAMPLES; ++i) {
        float position = (float)i / (float)(UM_SYNC_SAMPLES - 1u);
        float frequency = start_frequency +
                          (end_frequency - start_frequency) * position;
        float window = 0.5f - 0.5f * cosf(2.0f * UM_PI * position);
        sync[i] = 0.55f * window * sinf(phase);
        phase += 2.0f * UM_PI * frequency / (float)UM_SAMPLE_RATE;
        if (phase > 2.0f * UM_PI) {
            phase -= 2.0f * UM_PI;
        }
    }
}

static int render_symbol(const um_modem_config *config,
                         const um_complex *frequency,
                         float *samples, size_t offset)
{
    um_complex time[UM_FFT_SIZE];
    size_t hop = config->fft_size + config->cyclic_prefix;
    size_t extended = hop + config->window_samples;
    size_t active = config->last_bin - config->first_bin + 1u;
    float scale = 0.30f * (float)config->fft_size /
                  sqrtf(2.0f * (float)active);
    size_t i;

    memcpy(time, frequency, sizeof(time));
    if (um_fft(time, config->fft_size, 1) != UM_OK) {
        return UM_ERR_ARGUMENT;
    }
    for (i = 0u; i < extended; ++i) {
        size_t source;
        float window = 1.0f;
        if (i < config->cyclic_prefix) {
            source = config->fft_size - config->cyclic_prefix + i;
        } else if (i < hop) {
            source = i - config->cyclic_prefix;
        } else {
            source = i - hop;
        }
        if (config->window_samples != 0u && i < config->window_samples) {
            float angle = ((float)i + 0.5f) * UM_PI /
                          (2.0f * (float)config->window_samples);
            float sine = sinf(angle);
            window = sine * sine;
        } else if (config->window_samples != 0u && i >= hop) {
            float angle = ((float)(i - hop) + 0.5f) * UM_PI /
                          (2.0f * (float)config->window_samples);
            float cosine = cosf(angle);
            window = cosine * cosine;
        }
        samples[offset + i] += time[source].re * scale * window;
    }
    return UM_OK;
}

static void set_conjugate_bin(um_complex *frequency, size_t fft_size,
                              unsigned bin, um_complex value)
{
    frequency[bin] = value;
    frequency[fft_size - bin] = um_cconj(value);
}

static void make_training_frequency(const um_modem_config *config,
                                    um_complex *frequency)
{
    unsigned bin;
    memset(frequency, 0, sizeof(*frequency) * config->fft_size);
    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        um_complex value = {known_sign(bin, 0u), 0.0f};
        set_conjugate_bin(frequency, config->fft_size, bin, value);
    }
}

static void make_data_frequency(const um_modem_config *config,
                                const uint8_t *bits, size_t bit_capacity,
                                unsigned qam_bits, size_t symbol_number,
                                um_complex *frequency)
{
    size_t bit_offset = 0u;
    unsigned bin;
    memset(frequency, 0, sizeof(*frequency) * config->fft_size);
    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        um_complex value;
        if (is_pilot(config, bin)) {
            value.re = known_sign(bin, symbol_number);
            value.im = 0.0f;
        } else {
            uint8_t symbol_bits[6] = {0u, 0u, 0u, 0u, 0u, 0u};
            unsigned bit;
            for (bit = 0u; bit < qam_bits; ++bit) {
                if (bit_offset < bit_capacity) {
                    symbol_bits[bit] = bits[bit_offset];
                }
                ++bit_offset;
            }
            value = um_qam_map(symbol_bits, qam_bits);
        }
        set_conjugate_bin(frequency, config->fft_size, bin, value);
    }
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

static void build_header(const um_modem_config *config, size_t payload_length,
                         uint16_t sequence, uint32_t payload_crc,
                         uint8_t header[UM_HEADER_BYTES])
{
    uint16_t checksum;
    memset(header, 0, UM_HEADER_BYTES);
    header[0] = UINT8_C(0x55);
    header[1] = UINT8_C(0x4d);
    header[2] = 1u;
    header[3] = (uint8_t)config->qam_bits;
    header[4] = (uint8_t)config->fec_rate;
    header[5] = 0u;
    write_u16(&header[6], (uint16_t)payload_length);
    write_u16(&header[8], sequence);
    write_u32(&header[10], payload_crc);
    checksum = um_crc16(header, UM_HEADER_BYTES - 2u);
    write_u16(&header[14], checksum);
}

static int encode_block(const uint8_t *bytes, size_t byte_count,
                        um_fec_rate rate, size_t capacity,
                        uint8_t **interleaved, size_t *encoded_count)
{
    size_t raw_bits = byte_count * 8u;
    size_t fec_bits = um_fec_encoded_bits(raw_bits, rate);
    uint8_t *bits = NULL;
    uint8_t *coded = NULL;
    uint8_t *output = NULL;
    size_t actual = 0u;
    int status;

    if (capacity < fec_bits) {
        return UM_ERR_CAPACITY;
    }
    bits = (uint8_t *)malloc(raw_bits == 0u ? 1u : raw_bits);
    coded = (uint8_t *)calloc(capacity == 0u ? 1u : capacity, 1u);
    output = (uint8_t *)malloc(capacity == 0u ? 1u : capacity);
    if (bits == NULL || coded == NULL || output == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    um_bytes_to_bits(bytes, byte_count, bits);
    status = um_fec_encode(bits, raw_bits, rate, coded, capacity, &actual);
    if (status != UM_OK) {
        goto done;
    }
    status = um_interleave_bits(coded, output, capacity);
    if (status != UM_OK) {
        goto done;
    }
    *interleaved = output;
    *encoded_count = actual;
    output = NULL;

done:
    free(output);
    free(coded);
    free(bits);
    return status;
}

int um_modulate_frame(const um_modem_config *config,
                      const uint8_t *payload, size_t payload_length,
                      uint16_t sequence, float **samples, size_t *sample_count)
{
    size_t carriers;
    size_t header_fec_bits;
    size_t header_symbol_bits;
    size_t header_symbols;
    size_t header_capacity;
    size_t payload_fec_bits;
    size_t payload_symbol_bits;
    size_t payload_symbols = 0u;
    size_t payload_capacity;
    size_t total_symbols;
    size_t hop;
    size_t ofdm_start;
    size_t count;
    uint8_t header[UM_HEADER_BYTES];
    uint8_t *header_coded = NULL;
    uint8_t *payload_coded = NULL;
    size_t encoded_count;
    float *output = NULL;
    float sync[UM_SYNC_SAMPLES];
    um_complex frequency[UM_FFT_SIZE];
    size_t symbol;
    int status;

    if (um_modem_config_validate(config) != UM_OK ||
        (payload_length != 0u && payload == NULL) ||
        payload_length > UM_MAX_PAYLOAD || samples == NULL ||
        sample_count == NULL) {
        return UM_ERR_ARGUMENT;
    }
    carriers = um_modem_data_carriers(config);
    header_fec_bits = um_fec_encoded_bits(UM_HEADER_BITS, UM_FEC_RATE_1_2);
    header_symbol_bits = carriers * 2u;
    header_symbols = divide_round_up(header_fec_bits, header_symbol_bits);
    header_capacity = header_symbols * header_symbol_bits;
    payload_fec_bits = um_fec_encoded_bits(payload_length * 8u,
                                           config->fec_rate);
    payload_symbol_bits = carriers * config->qam_bits;
    payload_symbols = divide_round_up(payload_fec_bits, payload_symbol_bits);
    payload_capacity = payload_symbols * payload_symbol_bits;
    total_symbols = UM_TRAINING_SYMBOLS + header_symbols + payload_symbols;
    hop = config->fft_size + config->cyclic_prefix;
    ofdm_start = UM_SYNC_LEAD + UM_SYNC_SAMPLES + UM_SYNC_GAP;
    if (total_symbols > (SIZE_MAX - ofdm_start - config->window_samples - 64u) /
                            hop) {
        return UM_ERR_ARGUMENT;
    }
    count = ofdm_start + total_symbols * hop + config->window_samples + 64u;

    build_header(config, payload_length, sequence,
                 um_crc32(payload, payload_length), header);
    status = encode_block(header, sizeof(header), UM_FEC_RATE_1_2,
                          header_capacity, &header_coded, &encoded_count);
    if (status != UM_OK || encoded_count != header_fec_bits) {
        goto done;
    }
    status = encode_block(payload, payload_length, config->fec_rate,
                          payload_capacity, &payload_coded, &encoded_count);
    if (status != UM_OK || encoded_count != payload_fec_bits) {
        goto done;
    }

    output = (float *)calloc(count, sizeof(*output));
    if (output == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    make_sync(sync);
    for (symbol = 0u; symbol < UM_SYNC_SAMPLES; ++symbol) {
        output[UM_SYNC_LEAD + symbol] = sync[symbol];
    }

    make_training_frequency(config, frequency);
    for (symbol = 0u; symbol < UM_TRAINING_SYMBOLS; ++symbol) {
        status = render_symbol(config, frequency, output,
                               ofdm_start + symbol * hop);
        if (status != UM_OK) {
            goto done;
        }
    }
    for (symbol = 0u; symbol < header_symbols; ++symbol) {
        size_t offset = symbol * header_symbol_bits;
        make_data_frequency(config, header_coded + offset, header_symbol_bits,
                            2u, UM_TRAINING_SYMBOLS + symbol, frequency);
        status = render_symbol(config, frequency, output,
                               ofdm_start +
                                   (UM_TRAINING_SYMBOLS + symbol) * hop);
        if (status != UM_OK) {
            goto done;
        }
    }
    for (symbol = 0u; symbol < payload_symbols; ++symbol) {
        size_t offset = symbol * payload_symbol_bits;
        make_data_frequency(config, payload_coded + offset,
                            payload_symbol_bits, config->qam_bits,
                            UM_TRAINING_SYMBOLS + header_symbols + symbol,
                            frequency);
        status = render_symbol(config, frequency, output,
                               ofdm_start +
                                   (UM_TRAINING_SYMBOLS + header_symbols +
                                    symbol) * hop);
        if (status != UM_OK) {
            goto done;
        }
    }

    *samples = output;
    *sample_count = count;
    output = NULL;
    status = UM_OK;

done:
    free(output);
    free(payload_coded);
    free(header_coded);
    return status;
}

static int locate_sync(const float *samples, size_t sample_count,
                       size_t *location, float *correlation)
{
    float sync[UM_SYNC_SAMPLES];
    double sync_energy = 0.0;
    float best = 0.0f;
    size_t best_location = 0u;
    size_t start;
    size_t i;

    if (sample_count < UM_SYNC_SAMPLES) {
        return UM_ERR_TRUNCATED;
    }
    make_sync(sync);
    for (i = 0u; i < UM_SYNC_SAMPLES; ++i) {
        sync_energy += (double)sync[i] * sync[i];
    }
    for (start = 0u; start + UM_SYNC_SAMPLES <= sample_count; ++start) {
        double dot = 0.0;
        double energy = 0.0;
        float score;
        for (i = 0u; i < UM_SYNC_SAMPLES; ++i) {
            float value = samples[start + i];
            dot += (double)value * sync[i];
            energy += (double)value * value;
        }
        if (energy < 1.0e-18) {
            continue;
        }
        score = (float)(fabs(dot) / sqrt(energy * sync_energy));
        if (score > best) {
            best = score;
            best_location = start;
        }
    }
    *location = best_location;
    *correlation = best;
    return best >= 0.32f ? UM_OK : UM_ERR_SYNC;
}

static int extract_fft(const um_modem_config *config, const float *samples,
                       size_t sample_count, size_t symbol_start,
                       um_complex *frequency)
{
    size_t i;
    size_t core = symbol_start + config->cyclic_prefix;
    if (core > sample_count || config->fft_size > sample_count - core) {
        return UM_ERR_TRUNCATED;
    }
    for (i = 0u; i < config->fft_size; ++i) {
        frequency[i].re = samples[core + i];
        frequency[i].im = 0.0f;
    }
    return um_fft(frequency, config->fft_size, 0);
}

static um_complex phase_correction(const um_modem_config *config,
                                   const um_complex *frequency,
                                   const um_complex *channel,
                                   size_t symbol_number)
{
    um_complex sum = {0.0f, 0.0f};
    unsigned bin;
    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        if (is_pilot(config, bin)) {
            um_complex equalized = um_cdiv(frequency[bin], channel[bin]);
            float pilot = known_sign(bin, symbol_number);
            sum.re += equalized.re * pilot;
            sum.im += equalized.im * pilot;
        }
    }
    {
        float magnitude = sqrtf(um_cabs2(sum));
        um_complex correction = {1.0f, 0.0f};
        if (magnitude > 1.0e-12f) {
            correction.re = sum.re / magnitude;
            correction.im = -sum.im / magnitude;
        }
        return correction;
    }
}

static int demodulate_symbols(const um_modem_config *config,
                              const float *samples, size_t sample_count,
                              size_t first_start, size_t first_symbol_number,
                              size_t symbol_count, unsigned qam_bits,
                              const um_complex *channel, float *soft,
                              double *error_energy, double *ideal_energy)
{
    size_t hop = config->fft_size + config->cyclic_prefix;
    size_t output = 0u;
    size_t symbol;
    for (symbol = 0u; symbol < symbol_count; ++symbol) {
        um_complex frequency[UM_FFT_SIZE];
        um_complex correction;
        unsigned bin;
        int status = extract_fft(config, samples, sample_count,
                                 first_start + symbol * hop, frequency);
        if (status != UM_OK) {
            return status;
        }
        correction = phase_correction(config, frequency, channel,
                                      first_symbol_number + symbol);
        for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
            if (!is_pilot(config, bin)) {
                float symbol_soft[6];
                uint8_t hard[6];
                um_complex equalized = um_cmul(
                    um_cdiv(frequency[bin], channel[bin]), correction);
                um_complex nearest;
                unsigned bit;
                status = um_qam_soft_demod(equalized, qam_bits, symbol_soft);
                if (status != UM_OK) {
                    return status;
                }
                for (bit = 0u; bit < qam_bits; ++bit) {
                    soft[output++] = symbol_soft[bit];
                    hard[bit] = symbol_soft[bit] >= 0.0f ? 1u : 0u;
                }
                nearest = um_qam_map(hard, qam_bits);
                *error_energy += (double)(equalized.re - nearest.re) *
                                     (equalized.re - nearest.re) +
                                 (double)(equalized.im - nearest.im) *
                                     (equalized.im - nearest.im);
                *ideal_energy += um_cabs2(nearest);
            }
        }
    }
    return UM_OK;
}

static int decode_block(const float *interleaved, size_t capacity,
                        size_t byte_count, um_fec_rate rate, uint8_t *bytes)
{
    size_t raw_bits = byte_count * 8u;
    size_t fec_bits = um_fec_encoded_bits(raw_bits, rate);
    float *ordered = NULL;
    uint8_t *decoded = NULL;
    int status;

    if (capacity < fec_bits) {
        return UM_ERR_TRUNCATED;
    }
    ordered = (float *)malloc(capacity * sizeof(*ordered));
    decoded = (uint8_t *)malloc(raw_bits == 0u ? 1u : raw_bits);
    if (ordered == NULL || decoded == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    status = um_deinterleave_soft(interleaved, ordered, capacity);
    if (status != UM_OK) {
        goto done;
    }
    status = um_fec_decode(ordered, fec_bits, raw_bits, rate, decoded,
                           raw_bits);
    if (status != UM_OK) {
        goto done;
    }
    um_bits_to_bytes(decoded, raw_bits, bytes);

done:
    free(decoded);
    free(ordered);
    return status;
}

int um_demodulate_frame(const um_modem_config *config,
                        const float *samples, size_t sample_count,
                        uint8_t *payload, size_t payload_capacity,
                        size_t *payload_length, uint16_t *sequence,
                        um_rx_metrics *metrics)
{
    size_t sync_location;
    float sync_correlation;
    size_t ofdm_start;
    size_t hop;
    size_t carriers;
    um_complex channel[UM_FFT_SIZE];
    um_complex training[UM_TRAINING_SYMBOLS][UM_FFT_SIZE];
    double signal_energy = 0.0;
    double noise_energy = 0.0;
    size_t training_values = 0u;
    size_t training_symbol;
    unsigned bin;
    size_t header_fec_bits;
    size_t header_symbol_bits;
    size_t header_symbols;
    size_t header_capacity;
    float *header_soft = NULL;
    uint8_t header[UM_HEADER_BYTES];
    size_t decoded_length;
    uint32_t expected_crc;
    size_t payload_fec_bits;
    size_t payload_symbol_bits;
    size_t payload_symbols = 0u;
    size_t payload_soft_count;
    size_t decoded_payload_symbols = 0u;
    float *payload_soft = NULL;
    size_t payload_start;
    double error_energy = 0.0;
    double ideal_energy = 0.0;
    double total_sample_energy = 0.0;
    size_t i;
    int status;

    if (um_modem_config_validate(config) != UM_OK || samples == NULL ||
        payload == NULL || payload_length == NULL || sequence == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    status = locate_sync(samples, sample_count, &sync_location,
                         &sync_correlation);
    if (status != UM_OK) {
        return status;
    }
    hop = config->fft_size + config->cyclic_prefix;
    ofdm_start = sync_location + UM_SYNC_SAMPLES + UM_SYNC_GAP;
    carriers = um_modem_data_carriers(config);

    memset(channel, 0, sizeof(channel));
    for (training_symbol = 0u; training_symbol < UM_TRAINING_SYMBOLS;
         ++training_symbol) {
        status = extract_fft(config, samples, sample_count,
                             ofdm_start + training_symbol * hop,
                             training[training_symbol]);
        if (status != UM_OK) {
            return status;
        }
    }
    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        float sign = known_sign(bin, 0u);
        um_complex mean = um_cscale(
            um_cadd(training[0][bin], training[1][bin]), 0.5f * sign);
        um_complex difference = um_csub(training[0][bin], training[1][bin]);
        channel[bin] = mean;
        signal_energy += um_cabs2(mean);
        noise_energy += 0.5 * um_cabs2(difference);
        ++training_values;
    }

    header_fec_bits = um_fec_encoded_bits(UM_HEADER_BITS, UM_FEC_RATE_1_2);
    header_symbol_bits = carriers * 2u;
    header_symbols = divide_round_up(header_fec_bits, header_symbol_bits);
    header_capacity = header_symbols * header_symbol_bits;
    header_soft = (float *)malloc(header_capacity * sizeof(*header_soft));
    if (header_soft == NULL) {
        return UM_ERR_MEMORY;
    }
    status = demodulate_symbols(
        config, samples, sample_count,
        ofdm_start + UM_TRAINING_SYMBOLS * hop, UM_TRAINING_SYMBOLS,
        header_symbols, 2u, channel, header_soft, &error_energy,
        &ideal_energy);
    if (status != UM_OK) {
        goto done;
    }
    status = decode_block(header_soft, header_capacity, sizeof(header),
                          UM_FEC_RATE_1_2, header);
    if (status != UM_OK) {
        goto done;
    }
    if (header[0] != UINT8_C(0x55) || header[1] != UINT8_C(0x4d) ||
        header[2] != 1u || header[3] != config->qam_bits ||
        header[4] != (uint8_t)config->fec_rate || header[5] != 0u ||
        read_u16(&header[14]) != um_crc16(header, UM_HEADER_BYTES - 2u)) {
        status = UM_ERR_HEADER;
        goto done;
    }
    decoded_length = read_u16(&header[6]);
    *sequence = read_u16(&header[8]);
    expected_crc = read_u32(&header[10]);
    if (payload_capacity < decoded_length) {
        status = UM_ERR_CAPACITY;
        goto done;
    }

    payload_fec_bits = um_fec_encoded_bits(decoded_length * 8u,
                                           config->fec_rate);
    payload_symbol_bits = carriers * config->qam_bits;
    payload_symbols = divide_round_up(payload_fec_bits, payload_symbol_bits);
    decoded_payload_symbols = payload_symbols;
    payload_soft_count = payload_symbols * payload_symbol_bits;
    payload_soft =
        (float *)malloc(payload_soft_count * sizeof(*payload_soft));
    if (payload_soft == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    payload_start = ofdm_start +
                    (UM_TRAINING_SYMBOLS + header_symbols) * hop;
    status = demodulate_symbols(
        config, samples, sample_count, payload_start,
        UM_TRAINING_SYMBOLS + header_symbols, payload_symbols,
        config->qam_bits, channel, payload_soft, &error_energy,
        &ideal_energy);
    if (status != UM_OK) {
        goto done;
    }
    status = decode_block(payload_soft, payload_soft_count, decoded_length,
                          config->fec_rate, payload);
    if (status != UM_OK) {
        goto done;
    }
    if (um_crc32(payload, decoded_length) != expected_crc) {
        status = UM_ERR_CRC;
        goto done;
    }
    *payload_length = decoded_length;
    status = UM_OK;

done:
    if (metrics != NULL) {
        for (i = 0u; i < sample_count; ++i) {
            total_sample_energy += (double)samples[i] * samples[i];
        }
        metrics->sync_correlation = sync_correlation;
        metrics->frame_start = sync_location;
        metrics->signal_rms = sample_count != 0u
                                  ? (float)sqrt(total_sample_energy /
                                                (double)sample_count)
                                  : 0.0f;
        metrics->noise_rms = training_values != 0u
                                 ? (float)sqrt(noise_energy /
                                               (double)training_values)
                                 : 0.0f;
        metrics->estimated_snr_db =
            noise_energy > 1.0e-20 && signal_energy > 1.0e-20
                ? (float)(10.0 * log10(signal_energy / noise_energy))
                : 120.0f;
        metrics->evm_rms = ideal_energy > 0.0
                               ? (float)sqrt(error_energy / ideal_energy)
                               : 0.0f;
        metrics->ofdm_symbols = UM_TRAINING_SYMBOLS + header_symbols +
                                (status == UM_OK ? decoded_payload_symbols : 0u);
    }
    free(payload_soft);
    free(header_soft);
    return status;
}
