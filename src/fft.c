#include "um_internal.h"

int um_fft(um_complex *values, size_t count, int inverse)
{
    size_t i;
    size_t j;
    size_t length;

    if (values == NULL || count < 2u || (count & (count - 1u)) != 0u) {
        return UM_ERR_ARGUMENT;
    }

    j = 0u;
    for (i = 1u; i < count; ++i) {
        size_t bit = count >> 1u;
        while ((j & bit) != 0u) {
            j ^= bit;
            bit >>= 1u;
        }
        j ^= bit;
        if (i < j) {
            um_complex temporary = values[i];
            values[i] = values[j];
            values[j] = temporary;
        }
    }

    for (length = 2u; length <= count; length <<= 1u) {
        float angle = (inverse != 0 ? 2.0f : -2.0f) * UM_PI / (float)length;
        um_complex step = {cosf(angle), sinf(angle)};
        size_t start;
        for (start = 0u; start < count; start += length) {
            um_complex twiddle = {1.0f, 0.0f};
            size_t offset;
            for (offset = 0u; offset < length / 2u; ++offset) {
                um_complex even = values[start + offset];
                um_complex odd = um_cmul(values[start + offset + length / 2u],
                                         twiddle);
                values[start + offset] = um_cadd(even, odd);
                values[start + offset + length / 2u] = um_csub(even, odd);
                twiddle = um_cmul(twiddle, step);
            }
        }
        if (length == count) {
            break;
        }
    }

    if (inverse != 0) {
        float scale = 1.0f / (float)count;
        for (i = 0u; i < count; ++i) {
            values[i] = um_cscale(values[i], scale);
        }
    }
    return UM_OK;
}
