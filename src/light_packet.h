#ifndef UM_LIGHT_PACKET_H
#define UM_LIGHT_PACKET_H

#include "um.h"

#include <stddef.h>
#include <stdint.h>

#define LIGHT_PACKET_MODE UINT8_C(1)
#define LIGHT_PACKET_ACK_BYTES 8u

typedef struct light_packet_transport light_packet_transport;

typedef struct {
    size_t outgoing_packets_accepted;
    size_t incoming_packets_received;
    size_t outgoing_packets_queued;
    size_t incoming_packets_queued;
    size_t outgoing_cells_in_flight;
    size_t serialized_bytes_acked;
    size_t packet_bytes_acked;
    size_t packet_bytes_received;
    size_t duplicate_cells;
    size_t retransmissions;
} light_packet_transport_status;

int light_packet_transport_create(light_packet_transport **transport,
                                  unsigned max_packet_bytes,
                                  size_t queue_packets,
                                  unsigned transmit_window,
                                  unsigned retransmit_after_frames,
                                  uint32_t random_seed);
void light_packet_transport_destroy(light_packet_transport *transport);
int light_packet_transport_enqueue(light_packet_transport *transport,
                                   const uint8_t *packet,
                                   size_t packet_length);
int light_packet_transport_dequeue(light_packet_transport *transport,
                                   uint8_t *packet, size_t capacity,
                                   size_t *packet_length);
void light_packet_transport_build(light_packet_transport *transport,
                                  size_t local_frame, uint32_t *sequence,
                                  uint8_t *payload, size_t *payload_length,
                                  int *has_data, int *retransmission);
int light_packet_transport_process(light_packet_transport *transport,
                                   uint32_t sequence,
                                   const uint8_t *payload,
                                   size_t payload_length, int has_data);
unsigned light_packet_transport_max_packet(
    const light_packet_transport *transport);
void light_packet_transport_get_status(
    const light_packet_transport *transport,
    light_packet_transport_status *status);

#endif
