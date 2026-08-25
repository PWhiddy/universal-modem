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

static int normalize_capture(const float *samples, size_t sample_count,
                             float **normalized, float *input_rms,
                             float *input_peak, float *normalization_gain,
                             float *clipped_fraction)
{
    const size_t block_samples = 256u;
    const double target_rms = 0.20;
    double sum = 0.0;
    double maximum_block_energy = 0.0;
    double mean;
    double gain = 1.0;
    float peak = 0.0f;
    size_t clipped = 0u;
    float *output;
    size_t start;
    size_t i;

    output = (float *)malloc((sample_count == 0u ? 1u : sample_count) *
                             sizeof(*output));
    if (output == NULL) {
        return UM_ERR_MEMORY;
    }
    for (i = 0u; i < sample_count; ++i) {
        float absolute = fabsf(samples[i]);
        sum += samples[i];
        if (absolute > peak) {
            peak = absolute;
        }
        if (absolute >= 0.999f) {
            ++clipped;
        }
    }
    mean = sample_count != 0u ? sum / (double)sample_count : 0.0;
    for (start = 0u; start < sample_count; start += block_samples) {
        size_t count = sample_count - start;
        double block_energy = 0.0;
        if (count > block_samples) {
            count = block_samples;
        }
        for (i = 0u; i < count; ++i) {
            double value = (double)samples[start + i] - mean;
            block_energy += value * value;
        }
        if (count != 0u) {
            block_energy /= (double)count;
        }
        if (block_energy > maximum_block_energy) {
            maximum_block_energy = block_energy;
        }
    }
    if (maximum_block_energy > 1.0e-20) {
        gain = target_rms / sqrt(maximum_block_energy);
        if (gain < 1.0e-4) {
            gain = 1.0e-4;
        } else if (gain > 1.0e4) {
            gain = 1.0e4;
        }
    }
    for (i = 0u; i < sample_count; ++i) {
        output[i] = (float)(((double)samples[i] - mean) * gain);
    }
    *normalized = output;
    *input_rms = (float)sqrt(maximum_block_energy);
    *input_peak = peak;
    *normalization_gain = (float)gain;
    *clipped_fraction = sample_count != 0u
                            ? (float)clipped / (float)sample_count
                            : 0.0f;
    return UM_OK;
}

um_modem_config um_modem_default_config(void)
{
    um_modem_config config;
    config.fft_size = UM_FFT_SIZE;
    config.first_bin = 64u;
    config.last_bin = 362u;
    config.cyclic_prefix = 1024u;
    config.window_samples = 64u;
    config.sync_samples = 1536u;
    config.sync_gap = 2560u;
    config.training_symbols = UM_TRAINING_SYMBOLS;
    config.symbol_repetitions = 1u;
    config.qam_bits = 2u;
    config.fec_rate = UM_FEC_RATE_2_3;
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
        config->sync_samples < 512u ||
        config->sync_samples > UM_MAX_SYNC_SAMPLES ||
        config->sync_gap > UM_SAMPLE_RATE / 5u ||
        config->training_symbols < 2u ||
        config->training_symbols > UM_MAX_TRAINING_SYMBOLS ||
        config->symbol_repetitions < 1u ||
        config->symbol_repetitions > UM_MAX_SYMBOL_REPETITIONS ||
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

um_modem_config um_modem_robust_config(void)
{
    um_modem_config config = um_modem_default_config();
    config.first_bin = 64u;
    config.last_bin = 298u;
    config.cyclic_prefix = 1024u;
    config.window_samples = 96u;
    config.sync_samples = UM_MAX_SYNC_SAMPLES;
    config.sync_gap = 3072u;
    config.training_symbols = UM_MAX_TRAINING_SYMBOLS;
    config.symbol_repetitions = 2u;
    config.qam_bits = 2u;
    config.fec_rate = UM_FEC_RATE_1_2;
    return config;
}

int um_modem_metrics_have_baseline_margin(const um_rx_metrics *metrics)
{
    if (metrics == NULL) {
        return 0;
    }
    /*
     * A robust frame is ultimately proven by its CRC and the repeated
     * calibration verification frames.  These two checks prevent a lucky
     * decode from being called a working baseline while avoiding dependence
     * on decision-directed EVM, which is diagnostic rather than authoritative
     * for strongly frequency-selective QPSK channels.
     */
    return metrics->sync_correlation >= 0.30f &&
           metrics->estimated_snr_db >= 6.0f;
}

int um_modem_config_uses_robust_gate(const um_modem_config *config)
{
    um_modem_config robust;
    if (config == NULL) {
        return 0;
    }
    robust = um_modem_robust_config();
    return config->fft_size == robust.fft_size &&
           config->first_bin == robust.first_bin &&
           config->last_bin == robust.last_bin &&
           config->cyclic_prefix == robust.cyclic_prefix &&
           config->window_samples == robust.window_samples &&
           config->sync_samples == robust.sync_samples &&
           config->sync_gap == robust.sync_gap &&
           config->training_symbols == robust.training_symbols &&
           config->symbol_repetitions >= robust.symbol_repetitions &&
           config->qam_bits == robust.qam_bits &&
           config->fec_rate == robust.fec_rate;
}

int um_modem_metrics_have_margin(const um_modem_config *config,
                                 const um_rx_metrics *metrics)
{
    float minimum_snr;
    float maximum_evm;
    if (config == NULL || metrics == NULL) {
        return 0;
    }
    switch (config->qam_bits) {
    case 2u:
        minimum_snr = 10.0f;
        maximum_evm = 0.40f;
        break;
    case 4u:
        minimum_snr = 15.0f;
        maximum_evm = 0.18f;
        break;
    case 6u:
        minimum_snr = 21.0f;
        maximum_evm = 0.085f;
        break;
    default:
        return 0;
    }
    if (config->fec_rate == UM_FEC_RATE_1_2) {
        minimum_snr -= 2.0f;
        maximum_evm *= 1.05f;
    } else if (config->fec_rate == UM_FEC_RATE_3_4) {
        minimum_snr += 2.0f;
        maximum_evm *= 0.90f;
    }
    return metrics->sync_correlation >= 0.30f &&
           metrics->estimated_snr_db >= minimum_snr &&
           metrics->evm_rms <= maximum_evm;
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

static void make_sync(float *sync, size_t sync_samples)
{
    size_t i;
    float phase = 0.0f;
    const float start_frequency = 1500.0f;
    const float end_frequency = 11500.0f;
    const size_t taper_samples = sync_samples / 8u;
    for (i = 0u; i < sync_samples; ++i) {
        float position = (float)i / (float)(sync_samples - 1u);
        float frequency = start_frequency +
                          (end_frequency - start_frequency) * position;
        float window = 1.0f;
        if (i < taper_samples) {
            float angle = ((float)i + 0.5f) * UM_PI /
                          (2.0f * (float)taper_samples);
            float sine = sinf(angle);
            window = sine * sine;
        } else if (i + taper_samples >= sync_samples) {
            float angle = ((float)(sync_samples - i) - 0.5f) * UM_PI /
                          (2.0f * (float)taper_samples);
            float sine = sinf(angle);
            window = sine * sine;
        }
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
    double energy = 0.0;
    float scale = 1.0f;
    float peak = 0.0f;
    float rms;
    size_t i;

    memcpy(time, frequency, sizeof(time));
    if (um_fft(time, config->fft_size, 1) != UM_OK) {
        return UM_ERR_ARGUMENT;
    }
    for (i = 0u; i < config->fft_size; ++i) {
        float magnitude = fabsf(time[i].re);
        energy += (double)time[i].re * time[i].re;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    rms = (float)sqrt(energy / (double)config->fft_size);
    if (rms > 1.0e-12f) {
        /*
         * Normalize acoustic power, not the single largest sample.  Pure peak
         * normalization made short, high-crest control symbols more than
         * 10 dB quieter than the low-crest training symbols that preceded
         * them.  A fixed RMS target keeps training and data at comparable
         * levels; the peak ceiling remains as a final anti-clipping guard.
         */
        scale = 0.24f / rms;
        if (peak * scale > 0.88f) {
            scale = 0.88f / peak;
        }
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

static um_complex known_training_value(const um_modem_config *config,
                                       unsigned bin)
{
    float carrier = (float)(bin - config->first_bin);
    float count = (float)(config->last_bin - config->first_bin + 1u);
    float phase = UM_PI * carrier * carrier / count;
    um_complex value = {cosf(phase), sinf(phase)};
    return value;
}

static void make_training_frequency(const um_modem_config *config,
                                    um_complex *frequency)
{
    unsigned bin;
    memset(frequency, 0, sizeof(*frequency) * config->fft_size);
    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        um_complex value = known_training_value(config, bin);
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
    header[5] = (uint8_t)config->symbol_repetitions;
    write_u16(&header[6], (uint16_t)payload_length);
    write_u16(&header[8], sequence);
    write_u32(&header[10], payload_crc);
    checksum = um_crc16(header, UM_HEADER_BYTES - 2u);
    write_u16(&header[14], checksum);
}

static uint8_t whitening_bit(size_t index, size_t raw_bits, um_fec_rate rate)
{
    uint32_t value = (uint32_t)index * UINT32_C(0x9e3779b1) ^
                     (uint32_t)raw_bits * UINT32_C(0x85ebca6b) ^
                     (uint32_t)rate * UINT32_C(0xc2b2ae35) ^
                     UINT32_C(0x71d67fff);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    return (uint8_t)(value & 1u);
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
    size_t i;
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
    for (i = 0u; i < capacity; ++i) {
        uint8_t whitening = whitening_bit(i, raw_bits, rate);
        if (i < actual) {
            coded[i] ^= whitening;
        } else {
            coded[i] = whitening;
        }
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
    size_t logical_data_symbols;
    size_t total_symbols;
    size_t hop;
    size_t ofdm_start;
    size_t count;
    uint8_t header[UM_HEADER_BYTES];
    uint8_t *header_coded = NULL;
    uint8_t *payload_coded = NULL;
    size_t encoded_count;
    float *output = NULL;
    float sync[UM_MAX_SYNC_SAMPLES];
    um_complex frequency[UM_FFT_SIZE];
    size_t symbol;
    size_t repetition;
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
    logical_data_symbols = header_symbols + payload_symbols;
    if (logical_data_symbols >
        (SIZE_MAX - config->training_symbols) /
            config->symbol_repetitions) {
        return UM_ERR_ARGUMENT;
    }
    total_symbols = config->training_symbols +
                    logical_data_symbols * config->symbol_repetitions;
    hop = config->fft_size + config->cyclic_prefix;
    ofdm_start = UM_SYNC_LEAD + config->sync_samples + config->sync_gap;
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
    make_sync(sync, config->sync_samples);
    for (symbol = 0u; symbol < config->sync_samples; ++symbol) {
        output[UM_SYNC_LEAD + symbol] = sync[symbol];
    }

    make_training_frequency(config, frequency);
    for (symbol = 0u; symbol < config->training_symbols; ++symbol) {
        status = render_symbol(config, frequency, output,
                               ofdm_start + symbol * hop);
        if (status != UM_OK) {
            goto done;
        }
    }
    for (symbol = 0u; symbol < header_symbols; ++symbol) {
        size_t offset = symbol * header_symbol_bits;
        make_data_frequency(config, header_coded + offset, header_symbol_bits,
                            2u, config->training_symbols + symbol, frequency);
        for (repetition = 0u; repetition < config->symbol_repetitions;
             ++repetition) {
            size_t physical_symbol = config->training_symbols +
                                     symbol * config->symbol_repetitions +
                                     repetition;
            status = render_symbol(config, frequency, output,
                                   ofdm_start + physical_symbol * hop);
            if (status != UM_OK) {
                goto done;
            }
        }
    }
    for (symbol = 0u; symbol < payload_symbols; ++symbol) {
        size_t offset = symbol * payload_symbol_bits;
        make_data_frequency(config, payload_coded + offset,
                            payload_symbol_bits, config->qam_bits,
                            config->training_symbols + header_symbols + symbol,
                            frequency);
        for (repetition = 0u; repetition < config->symbol_repetitions;
             ++repetition) {
            size_t physical_symbol = config->training_symbols +
                                     (header_symbols + symbol) *
                                         config->symbol_repetitions +
                                     repetition;
            status = render_symbol(config, frequency, output,
                                   ofdm_start + physical_symbol * hop);
            if (status != UM_OK) {
                goto done;
            }
        }
    }
    {
        float peak = 0.0f;
        float gain = 1.0f;
        size_t sample;
        for (sample = ofdm_start; sample < count; ++sample) {
            float magnitude = fabsf(output[sample]);
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        if (peak > 0.90f) {
            gain = 0.90f / peak;
            for (sample = ofdm_start; sample < count; ++sample) {
                output[sample] *= gain;
            }
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

static int locate_sync(const um_modem_config *config, const float *samples,
                       size_t sample_count,
                       size_t *location, float *correlation)
{
    float sync[UM_MAX_SYNC_SAMPLES];
    um_complex *capture_spectrum = NULL;
    um_complex *filtered_capture = NULL;
    um_complex *sync_spectrum = NULL;
    size_t convolution_count;
    size_t fft_count = 1u;
    double sync_sum = 0.0;
    double sync_energy = 0.0;
    double window_sum = 0.0;
    double window_energy = 0.0;
    float best = 0.0f;
    size_t best_location = 0u;
    size_t start;
    size_t i;
    int status = UM_OK;

    if (sample_count < config->sync_samples) {
        return UM_ERR_TRUNCATED;
    }
    if (sample_count > SIZE_MAX - config->sync_samples + 1u) {
        return UM_ERR_ARGUMENT;
    }
    convolution_count = sample_count + config->sync_samples - 1u;
    while (fft_count < convolution_count) {
        if (fft_count > SIZE_MAX / 2u) {
            return UM_ERR_ARGUMENT;
        }
        fft_count <<= 1u;
    }
    capture_spectrum = (um_complex *)calloc(fft_count,
                                             sizeof(*capture_spectrum));
    filtered_capture = (um_complex *)calloc(fft_count,
                                            sizeof(*filtered_capture));
    sync_spectrum = (um_complex *)calloc(fft_count, sizeof(*sync_spectrum));
    if (capture_spectrum == NULL || filtered_capture == NULL ||
        sync_spectrum == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    make_sync(sync, config->sync_samples);
    for (i = 0u; i < config->sync_samples; ++i) {
        sync_sum += sync[i];
    }
    for (i = 0u; i < sample_count; ++i) {
        capture_spectrum[i].re = samples[i];
    }
    for (i = 0u; i < config->sync_samples; ++i) {
        double centered = (double)sync[i] -
                          sync_sum / (double)config->sync_samples;
        sync_spectrum[config->sync_samples - 1u - i].re = (float)centered;
    }
    status = um_fft(capture_spectrum, fft_count, 0);
    if (status != UM_OK) {
        goto done;
    }
    status = um_fft(sync_spectrum, fft_count, 0);
    if (status != UM_OK) {
        goto done;
    }
    for (i = 0u; i < fft_count; ++i) {
        size_t folded_bin = i <= fft_count / 2u ? i : fft_count - i;
        float frequency = (float)folded_bin * (float)UM_SAMPLE_RATE /
                          (float)fft_count;
        float weight = 1.0f;
        /*
         * The synchronization chirp occupies 1.5--11.5 kHz.  Rumble, fan
         * noise, and microphone DC below that band used to count against the
         * normalized matched-filter score even though they could never
         * correlate with the chirp.  Apply a zero-phase spectral gate with
         * smooth guard bands before both the dot product and its normalization.
         * Acquisition deliberately emphasizes the 1.25--8 kHz portion that
         * survived the measured laptop-to-room path with the largest margin;
         * the transmitted chirp can retain its wider occupied band.
         */
        if (frequency <= 750.0f || frequency >= 9000.0f) {
            weight = 0.0f;
        } else if (frequency < 1250.0f) {
            float position = (frequency - 750.0f) / 500.0f;
            float sine = sinf(0.5f * UM_PI * position);
            weight = sine * sine;
        } else if (frequency > 8000.0f) {
            float position = (9000.0f - frequency) / 1000.0f;
            float sine = sinf(0.5f * UM_PI * position);
            weight = sine * sine;
        }
        filtered_capture[i] = um_cscale(capture_spectrum[i], weight);
        sync_spectrum[i] = um_cscale(sync_spectrum[i], weight);
        sync_energy += um_cabs2(sync_spectrum[i]) / (double)fft_count;
        capture_spectrum[i] =
            um_cmul(filtered_capture[i], sync_spectrum[i]);
    }
    status = um_fft(filtered_capture, fft_count, 1);
    if (status != UM_OK) {
        goto done;
    }
    status = um_fft(capture_spectrum, fft_count, 1);
    if (status != UM_OK) {
        goto done;
    }
    for (i = 0u; i < config->sync_samples; ++i) {
        double value = filtered_capture[i].re;
        window_sum += value;
        window_energy += value * value;
    }
    for (start = 0u; start + config->sync_samples <= sample_count; ++start) {
        double variance = window_energy -
                          window_sum * window_sum /
                              (double)config->sync_samples;
        double dot = capture_spectrum[start + config->sync_samples - 1u].re;
        float score;
        /*
         * The FFT convolution has a small absolute round-off floor.  Capture
         * normalization makes this threshold independent of microphone level;
         * without it, numerically tiny silent windows can report impossible
         * correlations and outrank the real chirp.
         */
        if (variance > (double)config->sync_samples * 1.0e-6) {
            score = (float)(fabs(dot) / sqrt(variance * sync_energy));
            if (score > best) {
                best = score;
                best_location = start;
            }
        }
        if (start + config->sync_samples < sample_count) {
            double departing = filtered_capture[start].re;
            double arriving = filtered_capture[start + config->sync_samples].re;
            window_sum += arriving - departing;
            window_energy += arriving * arriving - departing * departing;
        }
    }
    *location = best_location;
    *correlation = best;
    {
        float threshold = 0.10f + 4.3f / sqrtf((float)config->sync_samples);
        if (threshold < 0.18f) {
            threshold = 0.18f;
        }
        status = best >= threshold ? UM_OK : UM_ERR_SYNC;
        if (status == UM_OK) {
            size_t first = best_location > config->cyclic_prefix
                               ? best_location - config->cyclic_prefix
                               : 0u;
            double local_sum = 0.0;
            double local_energy = 0.0;
            float path_threshold = best * 0.55f;
            if (path_threshold < threshold) {
                path_threshold = threshold;
            }
            for (i = 0u; i < config->sync_samples; ++i) {
                double value = filtered_capture[first + i].re;
                local_sum += value;
                local_energy += value * value;
            }
            for (start = first; start <= best_location; ++start) {
                double variance = local_energy -
                                  local_sum * local_sum /
                                      (double)config->sync_samples;
                if (variance > (double)config->sync_samples * 1.0e-6) {
                    double dot =
                        capture_spectrum[start + config->sync_samples - 1u].re;
                    float score =
                        (float)(fabs(dot) / sqrt(variance * sync_energy));
                    if (score >= path_threshold) {
                        *location = start;
                        break;
                    }
                }
                if (start < best_location) {
                    double departing = filtered_capture[start].re;
                    double arriving =
                        filtered_capture[start + config->sync_samples].re;
                    local_sum += arriving - departing;
                    local_energy += arriving * arriving -
                                    departing * departing;
                }
            }
        }
    }

done:
    free(sync_spectrum);
    free(filtered_capture);
    free(capture_spectrum);
    return status;
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

typedef struct {
    float intercept;
    float slope;
    float confidence;
    float amplitude;
} phase_model;

static phase_model estimate_phase_model(const um_modem_config *config,
                                        const um_complex *frequency,
                                        const um_complex *channel,
                                        const float *reliability,
                                        size_t symbol_number)
{
    float pilot_x[UM_FFT_SIZE];
    um_complex pilot_value[UM_FFT_SIZE];
    float pilot_magnitude[UM_FFT_SIZE];
    float pilot_weight[UM_FFT_SIZE];
    const float reference =
        0.5f * (float)(config->first_bin + config->last_bin);
    phase_model model = {0.0f, 0.0f, 0.0f, 1.0f};
    size_t pilots = 0u;
    float weight_sum = 0.0f;
    float baseline_coherence = 0.0f;
    float best_coherence = 0.0f;
    um_complex best_sum = {0.0f, 0.0f};
    unsigned bin;
    int timing_step;

    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        if (is_pilot(config, bin) && reliability[bin] > 1.0e-4f) {
            um_complex equalized = um_cdiv(frequency[bin], channel[bin]);
            float pilot = known_sign(bin, symbol_number);
            float magnitude = sqrtf(um_cabs2(equalized));
            if (magnitude <= 1.0e-8f) {
                continue;
            }
            pilot_x[pilots] = (float)bin - reference;
            pilot_value[pilots].re = equalized.re * pilot / magnitude;
            pilot_value[pilots].im = equalized.im * pilot / magnitude;
            pilot_magnitude[pilots] = magnitude;
            pilot_weight[pilots] = reliability[bin];
            weight_sum += reliability[bin];
            ++pilots;
        }
    }
    if (pilots == 0u || weight_sum <= 1.0e-8f) {
        return model;
    }
    /*
     * Search a physically bounded fractional timing displacement rather than
     * unwrapping noisy pilot phases.  A slope is accepted only when it makes
     * the pilots materially more coherent than a common-phase-only model.
     */
    for (timing_step = pilots >= 8u ? -64 : 0;
         timing_step <= (pilots >= 8u ? 64 : 0); ++timing_step) {
        float timing_samples = 0.5f * (float)timing_step;
        float slope = -2.0f * UM_PI * timing_samples /
                      (float)config->fft_size;
        um_complex sum = {0.0f, 0.0f};
        float coherence;
        size_t pilot;
        for (pilot = 0u; pilot < pilots; ++pilot) {
            float phase = -slope * pilot_x[pilot];
            um_complex correction = {cosf(phase), sinf(phase)};
            sum = um_cadd(sum,
                          um_cscale(um_cmul(pilot_value[pilot], correction),
                                   pilot_weight[pilot]));
        }
        coherence = sqrtf(um_cabs2(sum)) / weight_sum;
        if (timing_step == 0) {
            baseline_coherence = coherence;
        }
        if (coherence > best_coherence) {
            best_coherence = coherence;
            best_sum = sum;
            model.slope = slope;
        }
    }
    if (pilots < 8u || best_coherence < baseline_coherence + 0.05f) {
        um_complex sum = {0.0f, 0.0f};
        size_t pilot;
        for (pilot = 0u; pilot < pilots; ++pilot) {
            sum = um_cadd(sum, um_cscale(pilot_value[pilot],
                                         pilot_weight[pilot]));
        }
        model.slope = 0.0f;
        best_sum = sum;
        best_coherence = baseline_coherence;
    }
    model.intercept = atan2f(best_sum.im, best_sum.re);
    model.confidence = best_coherence * best_coherence;
    {
        double amplitude_sum = 0.0;
        size_t pilot;
        for (pilot = 0u; pilot < pilots; ++pilot) {
            amplitude_sum += (double)pilot_weight[pilot] *
                             pilot_magnitude[pilot];
        }
        model.amplitude = (float)(amplitude_sum / weight_sum);
        if (model.amplitude < 0.05f) {
            model.amplitude = 0.05f;
        } else if (model.amplitude > 20.0f) {
            model.amplitude = 20.0f;
        }
    }
    return model;
}

static um_complex phase_correction(const phase_model *model, unsigned bin,
                                   const um_modem_config *config)
{
    float reference =
        0.5f * (float)(config->first_bin + config->last_bin);
    float phase = model->intercept + model->slope * ((float)bin - reference);
    float gain = 1.0f / model->amplitude;
    um_complex correction = {cosf(phase) * gain, -sinf(phase) * gain};
    return correction;
}

static int demodulate_symbols(const um_modem_config *config,
                              const float *samples, size_t sample_count,
                              size_t first_start, size_t first_symbol_number,
                              size_t symbol_count, unsigned qam_bits,
                              const um_complex *channel,
                              const float *reliability, float *soft,
                              double *error_energy, double *ideal_energy)
{
    size_t hop = config->fft_size + config->cyclic_prefix;
    size_t bits_per_symbol = um_modem_data_carriers(config) * qam_bits;
    size_t output = 0u;
    size_t symbol;
    for (symbol = 0u; symbol < symbol_count; ++symbol) {
        um_complex combined[UM_FFT_SIZE];
        float combined_weight[UM_FFT_SIZE];
        size_t repetition;
        memset(combined, 0, sizeof(combined));
        memset(combined_weight, 0, sizeof(combined_weight));
        memset(soft + output, 0, bits_per_symbol * sizeof(*soft));
        for (repetition = 0u;
             repetition < config->symbol_repetitions; ++repetition) {
            um_complex frequency[UM_FFT_SIZE];
            phase_model phase;
            size_t carrier_output = 0u;
            unsigned bin;
            int status = extract_fft(
                config, samples, sample_count,
                first_start +
                    (symbol * config->symbol_repetitions + repetition) * hop,
                frequency);
            if (status != UM_OK) {
                return status;
            }
            phase = estimate_phase_model(config, frequency, channel,
                                         reliability,
                                         first_symbol_number + symbol);
            for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
                if (!is_pilot(config, bin)) {
                    float symbol_soft[6];
                    float symbol_reliability =
                        phase.amplitude * phase.amplitude;
                    float decision_weight;
                    um_complex equalized = um_cmul(
                        um_cdiv(frequency[bin], channel[bin]),
                        phase_correction(&phase, bin, config));
                    unsigned bit;
                    status =
                        um_qam_soft_demod(equalized, qam_bits, symbol_soft);
                    if (status != UM_OK) {
                        return status;
                    }
                    if (symbol_reliability > 4.0f) {
                        symbol_reliability = 4.0f;
                    }
                    decision_weight = reliability[bin] * phase.confidence *
                                      symbol_reliability;
                    for (bit = 0u; bit < qam_bits; ++bit) {
                        soft[output + carrier_output + bit] +=
                            symbol_soft[bit] * decision_weight;
                    }
                    combined[bin] =
                        um_cadd(combined[bin],
                                um_cscale(equalized, decision_weight));
                    combined_weight[bin] += decision_weight;
                    carrier_output += qam_bits;
                }
            }
        }
        {
            size_t carrier_output = 0u;
            unsigned bin;
            for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
                if (!is_pilot(config, bin)) {
                    uint8_t hard[6];
                    um_complex nearest;
                    unsigned bit;
                    for (bit = 0u; bit < qam_bits; ++bit) {
                        hard[bit] =
                            soft[output + carrier_output + bit] >= 0.0f
                                ? 1u
                                : 0u;
                    }
                    nearest = um_qam_map(hard, qam_bits);
                    if (combined_weight[bin] > 1.0e-8f) {
                        um_complex effective = um_cscale(
                            combined[bin], 1.0f / combined_weight[bin]);
                        double weight = combined_weight[bin];
                        *error_energy +=
                            weight *
                            ((double)(effective.re - nearest.re) *
                                 (effective.re - nearest.re) +
                             (double)(effective.im - nearest.im) *
                                 (effective.im - nearest.im));
                        *ideal_energy += weight * um_cabs2(nearest);
                    }
                    carrier_output += qam_bits;
                }
            }
        }
        output += bits_per_symbol;
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
    size_t i;
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
    for (i = 0u; i < capacity; ++i) {
        if (whitening_bit(i, raw_bits, rate) != 0u) {
            ordered[i] = -ordered[i];
        }
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
    size_t sync_location = 0u;
    float sync_correlation = 0.0f;
    size_t ofdm_start;
    size_t hop;
    size_t carriers;
    um_complex channel[UM_FFT_SIZE];
    um_complex training[UM_MAX_TRAINING_SYMBOLS][UM_FFT_SIZE];
    float training_noise[UM_FFT_SIZE];
    float carrier_reliability[UM_FFT_SIZE];
    double signal_energy = 0.0;
    double noise_energy = 0.0;
    size_t training_values = 0u;
    size_t training_symbol;
    unsigned bin;
    size_t header_fec_bits;
    size_t header_symbol_bits;
    size_t header_symbols = 0u;
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
    float *normalized_samples = NULL;
    float input_rms = 0.0f;
    float input_peak = 0.0f;
    float normalization_gain = 1.0f;
    float clipped_fraction = 0.0f;
    int status;

    if (um_modem_config_validate(config) != UM_OK || samples == NULL ||
        payload == NULL || payload_length == NULL || sequence == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    status = normalize_capture(samples, sample_count, &normalized_samples,
                               &input_rms, &input_peak,
                               &normalization_gain, &clipped_fraction);
    if (status != UM_OK) {
        return status;
    }
    samples = normalized_samples;
    status = locate_sync(config, samples, sample_count, &sync_location,
                         &sync_correlation);
    if (status != UM_OK) {
        goto done;
    }
    hop = config->fft_size + config->cyclic_prefix;
    ofdm_start = sync_location + config->sync_samples + config->sync_gap;
    carriers = um_modem_data_carriers(config);

    memset(channel, 0, sizeof(channel));
    memset(training_noise, 0, sizeof(training_noise));
    memset(carrier_reliability, 0, sizeof(carrier_reliability));
    for (training_symbol = 0u;
         training_symbol < config->training_symbols;
         ++training_symbol) {
        status = extract_fft(config, samples, sample_count,
                             ofdm_start + training_symbol * hop,
                             training[training_symbol]);
        if (status != UM_OK) {
            goto done;
        }
    }
    for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
        um_complex known = known_training_value(config, bin);
        um_complex mean = {0.0f, 0.0f};
        double residual_energy = 0.0;
        for (training_symbol = 0u;
             training_symbol < config->training_symbols; ++training_symbol) {
            mean = um_cadd(mean, training[training_symbol][bin]);
        }
        mean = um_cscale(mean, 1.0f / (float)config->training_symbols);
        channel[bin] = um_cmul(mean, um_cconj(known));
        for (training_symbol = 0u;
             training_symbol < config->training_symbols; ++training_symbol) {
            um_complex expected = um_cmul(channel[bin], known);
            um_complex residual =
                um_csub(training[training_symbol][bin], expected);
            residual_energy += um_cabs2(residual);
        }
        training_noise[bin] =
            (float)(residual_energy /
                    (double)(config->training_symbols - 1u));
        signal_energy += um_cabs2(mean);
        noise_energy += training_noise[bin];
        ++training_values;
    }
    {
        double average_noise = training_values != 0u
                                   ? noise_energy / (double)training_values
                                   : 0.0;
        double maximum_signal = 0.0;
        double floor_power;
        for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
            double power = um_cabs2(channel[bin]);
            if (power > maximum_signal) {
                maximum_signal = power;
            }
        }
        floor_power = average_noise * 0.25 + maximum_signal * 1.0e-8;
        for (bin = config->first_bin; bin <= config->last_bin; ++bin) {
            unsigned first = bin > config->first_bin + 3u
                                 ? bin - 3u
                                 : config->first_bin;
            unsigned last = bin + 3u < config->last_bin
                                ? bin + 3u
                                : config->last_bin;
            double local_noise = 0.0;
            unsigned neighbor;
            for (neighbor = first; neighbor <= last; ++neighbor) {
                local_noise += training_noise[neighbor];
            }
            local_noise /= (double)(last - first + 1u);
            {
                double snr = um_cabs2(channel[bin]) /
                             (local_noise + floor_power + 1.0e-20);
                carrier_reliability[bin] =
                    (float)(snr / (snr + 4.0));
            }
        }
    }

    header_fec_bits = um_fec_encoded_bits(UM_HEADER_BITS, UM_FEC_RATE_1_2);
    header_symbol_bits = carriers * 2u;
    header_symbols = divide_round_up(header_fec_bits, header_symbol_bits);
    header_capacity = header_symbols * header_symbol_bits;
    header_soft = (float *)malloc(header_capacity * sizeof(*header_soft));
    if (header_soft == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    status = demodulate_symbols(
        config, samples, sample_count,
        ofdm_start + config->training_symbols * hop,
        config->training_symbols,
        header_symbols, 2u, channel, carrier_reliability, header_soft,
        &error_energy,
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
        header[4] != (uint8_t)config->fec_rate ||
        header[5] != (uint8_t)config->symbol_repetitions ||
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

    /* Successful frames report payload-mode EVM, not a QPSK-header average. */
    error_energy = 0.0;
    ideal_energy = 0.0;

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
                    (config->training_symbols +
                     header_symbols * config->symbol_repetitions) * hop;
    status = demodulate_symbols(
        config, samples, sample_count, payload_start,
        config->training_symbols + header_symbols, payload_symbols,
        config->qam_bits, channel, carrier_reliability, payload_soft,
        &error_energy,
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
        metrics->sync_correlation = sync_correlation;
        metrics->frame_start = sync_location;
        metrics->signal_rms = input_rms;
        metrics->input_peak = input_peak;
        metrics->normalization_gain = normalization_gain;
        metrics->clipped_sample_fraction = clipped_fraction;
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
        metrics->ofdm_symbols =
            config->training_symbols +
            (header_symbols +
             (status == UM_OK ? decoded_payload_symbols : 0u)) *
                config->symbol_repetitions;
    }
    free(payload_soft);
    free(header_soft);
    free(normalized_samples);
    return status;
}
