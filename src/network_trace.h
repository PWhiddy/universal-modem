#ifndef UM_NETWORK_TRACE_H
#define UM_NETWORK_TRACE_H

#include <stddef.h>
#include <stdint.h>

#define UM_NETWORK_TRACE_ADDRESSES 128u
#define UM_NETWORK_TRACE_NAME_BYTES 96u

typedef struct {
    uint8_t address[16];
    uint8_t address_length;
    char name[UM_NETWORK_TRACE_NAME_BYTES];
    uint64_t expires_at;
} um_network_trace_address;

typedef struct {
    um_network_trace_address addresses[UM_NETWORK_TRACE_ADDRESSES];
    size_t next_address;
} um_network_trace;

void um_network_trace_init(um_network_trace *trace);
void um_network_trace_observe(um_network_trace *trace,
                              const uint8_t *packet,
                              size_t packet_length);
int um_network_trace_read_dns_name(const uint8_t *dns, size_t dns_length,
                                   size_t start, char *name,
                                   size_t name_capacity, size_t *next);

/* Describes one IP packet and returns nonzero for application-visible
 * activity worth logging.  Pure TCP acknowledgements are described but
 * return zero so live links need not spend bandwidth-time printing them. */
int um_network_trace_describe(const um_network_trace *trace,
                              const uint8_t *packet,
                              size_t packet_length, char *description,
                              size_t description_capacity);

#endif
