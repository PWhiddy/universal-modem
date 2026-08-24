#include "um_internal.h"

size_t um_interleave_stride(size_t count)
{
    size_t stride;
    if (count < 2u) {
        return 1u;
    }
    stride = (count * 5u) / 8u + 1u;
    if (stride >= count) {
        stride = count - 1u;
    }
    while (stride > 1u && um_gcd_size(stride, count) != 1u) {
        --stride;
    }
    if (um_gcd_size(stride, count) != 1u) {
        stride = 1u;
    }
    return stride;
}

int um_interleave_bits(const uint8_t *input, uint8_t *output, size_t count)
{
    size_t stride;
    size_t i;
    if ((count != 0u && (input == NULL || output == NULL)) || input == output) {
        return UM_ERR_ARGUMENT;
    }
    stride = um_interleave_stride(count);
    for (i = 0u; i < count; ++i) {
        output[(i * stride) % count] = input[i];
    }
    return UM_OK;
}

int um_deinterleave_soft(const float *input, float *output, size_t count)
{
    size_t stride;
    size_t i;
    if ((count != 0u && (input == NULL || output == NULL)) || input == output) {
        return UM_ERR_ARGUMENT;
    }
    stride = um_interleave_stride(count);
    for (i = 0u; i < count; ++i) {
        output[i] = input[(i * stride) % count];
    }
    return UM_OK;
}
