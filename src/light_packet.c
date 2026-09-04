#include "light_packet.h"

#include <stdlib.h>
#include <string.h>

#define LIGHT_PACKET_RECORD_BYTES 2u
#define LIGHT_PACKET_DATA_BYTES (UM_LIGHT_MAX_PAYLOAD - LIGHT_PACKET_ACK_BYTES)
#define LIGHT_PACKET_ACK_BITS 16u
#define LIGHT_PACKET_MAX_WINDOW LIGHT_PACKET_ACK_BITS
#define LIGHT_PACKET_MAX_QUEUE 256u

typedef struct {
    uint8_t *bytes;
    uint16_t *lengths;
    size_t capacity;
    size_t head;
    size_t count;
    unsigned max_packet;
} light_packet_queue;

typedef struct {
    uint32_t sequence;
    size_t last_sent_frame;
    uint8_t bytes[LIGHT_PACKET_DATA_BYTES];
    uint8_t length;
    uint8_t payload_length;
    int occupied;
} light_packet_tx_cell;

typedef struct {
    uint32_t sequence;
    uint8_t bytes[LIGHT_PACKET_DATA_BYTES];
    uint8_t length;
    uint8_t offset;
    int occupied;
} light_packet_rx_cell;

struct light_packet_transport {
    light_packet_queue outgoing;
    light_packet_queue incoming;
    light_packet_tx_cell transmit[LIGHT_PACKET_MAX_WINDOW];
    light_packet_rx_cell receive[LIGHT_PACKET_MAX_WINDOW];
    uint8_t *receive_packet;
    size_t outgoing_record_offset;
    size_t receive_header_length;
    size_t receive_packet_length;
    size_t receive_packet_expected;
    uint32_t transmit_next;
    uint32_t remote_receive_base;
    uint32_t receive_base;
    uint32_t random_state;
    unsigned max_packet;
    unsigned transmit_window;
    unsigned retransmit_after_frames;
    light_packet_transport_status status;
};

_Static_assert(LIGHT_PACKET_DATA_BYTES == 83u,
               "unexpected optical packet data capacity");

static void light_packet_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void light_packet_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static uint16_t light_packet_read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t light_packet_read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) |
           ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

static uint32_t light_packet_random(uint32_t *state)
{
    uint32_t value = *state;
    if (value == 0u) {
        value = UINT32_C(0x7061636b);
    }
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static int light_packet_queue_init(light_packet_queue *queue,
                                   size_t capacity, unsigned max_packet)
{
    if (capacity > SIZE_MAX / max_packet) {
        return UM_ERR_ARGUMENT;
    }
    memset(queue, 0, sizeof(*queue));
    queue->bytes = (uint8_t *)malloc(capacity * max_packet);
    queue->lengths = (uint16_t *)calloc(capacity, sizeof(uint16_t));
    if (queue->bytes == NULL || queue->lengths == NULL) {
        free(queue->lengths);
        free(queue->bytes);
        memset(queue, 0, sizeof(*queue));
        return UM_ERR_MEMORY;
    }
    queue->capacity = capacity;
    queue->max_packet = max_packet;
    return UM_OK;
}

static void light_packet_queue_destroy(light_packet_queue *queue)
{
    free(queue->lengths);
    free(queue->bytes);
    memset(queue, 0, sizeof(*queue));
}

static size_t light_packet_queue_index(const light_packet_queue *queue,
                                       size_t relative)
{
    return (queue->head + relative) % queue->capacity;
}

static int light_packet_queue_push(light_packet_queue *queue,
                                   const uint8_t *packet, size_t length)
{
    size_t index;
    if (queue->count == queue->capacity) {
        return UM_ERR_CAPACITY;
    }
    index = light_packet_queue_index(queue, queue->count);
    memcpy(queue->bytes + index * queue->max_packet, packet, length);
    queue->lengths[index] = (uint16_t)length;
    ++queue->count;
    return UM_OK;
}

static const uint8_t *light_packet_queue_head(
    const light_packet_queue *queue, size_t *length)
{
    if (queue->count == 0u) {
        *length = 0u;
        return NULL;
    }
    *length = queue->lengths[queue->head];
    return queue->bytes + queue->head * queue->max_packet;
}

static void light_packet_queue_pop(light_packet_queue *queue)
{
    if (queue->count == 0u) {
        return;
    }
    queue->head = (queue->head + 1u) % queue->capacity;
    --queue->count;
}

static size_t light_packet_in_flight(const light_packet_transport *transport)
{
    size_t count = 0u;
    unsigned index;
    for (index = 0u; index < LIGHT_PACKET_MAX_WINDOW; ++index) {
        if (transport->transmit[index].occupied != 0) {
            ++count;
        }
    }
    return count;
}

static size_t light_packet_retry_delay(
    const light_packet_transport *transport,
    const light_packet_tx_cell *cell)
{
    size_t base = transport->retransmit_after_frames;
    size_t spread = base < 4u ? 4u : base;
    uint32_t value = transport->random_state ^
                     cell->sequence * UINT32_C(0x9e3779b9) ^
                     (uint32_t)cell->last_sent_frame *
                         UINT32_C(0x85ebca6b);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    return base + value % spread;
}

static light_packet_tx_cell *light_packet_select_retry(
    light_packet_transport *transport, size_t local_frame)
{
    size_t eligible = 0u;
    size_t choice;
    unsigned index;
    for (index = 0u; index < LIGHT_PACKET_MAX_WINDOW; ++index) {
        const light_packet_tx_cell *cell = &transport->transmit[index];
        if (cell->occupied != 0 &&
            local_frame - cell->last_sent_frame >=
                light_packet_retry_delay(transport, cell)) {
            ++eligible;
        }
    }
    if (eligible == 0u) {
        return NULL;
    }
    choice = light_packet_random(&transport->random_state) % eligible;
    for (index = 0u; index < LIGHT_PACKET_MAX_WINDOW; ++index) {
        light_packet_tx_cell *cell = &transport->transmit[index];
        if (cell->occupied != 0 &&
            local_frame - cell->last_sent_frame >=
                light_packet_retry_delay(transport, cell)) {
            if (choice == 0u) {
                return cell;
            }
            --choice;
        }
    }
    return NULL;
}

static size_t light_packet_serialize(light_packet_transport *transport,
                                     uint8_t *bytes, size_t capacity,
                                     size_t *payload_bytes)
{
    size_t written = 0u;
    *payload_bytes = 0u;
    while (written < capacity && transport->outgoing.count != 0u) {
        size_t packet_length;
        size_t record_length;
        size_t remaining;
        size_t take;
        const uint8_t *packet = light_packet_queue_head(
            &transport->outgoing, &packet_length);
        record_length = LIGHT_PACKET_RECORD_BYTES + packet_length;
        remaining = record_length - transport->outgoing_record_offset;
        take = capacity - written;
        if (take > remaining) {
            take = remaining;
        }
        while (take != 0u) {
            size_t offset = transport->outgoing_record_offset;
            if (offset < LIGHT_PACKET_RECORD_BYTES) {
                bytes[written] =
                    offset == 0u ? (uint8_t)(packet_length >> 8u)
                                 : (uint8_t)packet_length;
            } else {
                bytes[written] = packet[offset - LIGHT_PACKET_RECORD_BYTES];
                ++*payload_bytes;
            }
            ++written;
            ++transport->outgoing_record_offset;
            --take;
        }
        if (transport->outgoing_record_offset == record_length) {
            light_packet_queue_pop(&transport->outgoing);
            transport->outgoing_record_offset = 0u;
        }
    }
    return written;
}

static light_packet_tx_cell *light_packet_create_cell(
    light_packet_transport *transport)
{
    light_packet_tx_cell *cell;
    size_t length;
    size_t payload_length;
    if (transport->outgoing.count == 0u ||
        light_packet_in_flight(transport) >= transport->transmit_window ||
        (uint64_t)transport->transmit_next >=
            (uint64_t)transport->remote_receive_base +
                LIGHT_PACKET_ACK_BITS ||
        transport->transmit_next == UINT32_MAX) {
        return NULL;
    }
    cell = &transport->transmit[
        transport->transmit_next % LIGHT_PACKET_MAX_WINDOW];
    if (cell->occupied != 0) {
        return NULL;
    }
    memset(cell, 0, sizeof(*cell));
    length = light_packet_serialize(transport, cell->bytes,
                                    sizeof(cell->bytes), &payload_length);
    if (length == 0u) {
        return NULL;
    }
    cell->sequence = transport->transmit_next++;
    cell->length = (uint8_t)length;
    cell->payload_length = (uint8_t)payload_length;
    cell->occupied = 1;
    return cell;
}

static uint16_t light_packet_receive_mask(
    const light_packet_transport *transport)
{
    uint16_t mask = 0u;
    unsigned bit;
    for (bit = 0u; bit < LIGHT_PACKET_ACK_BITS; ++bit) {
        uint32_t sequence = transport->receive_base + bit;
        const light_packet_rx_cell *cell =
            &transport->receive[sequence % LIGHT_PACKET_MAX_WINDOW];
        if (cell->occupied != 0 && cell->sequence == sequence) {
            mask |= (uint16_t)(UINT16_C(1) << bit);
        }
    }
    return mask;
}

static void light_packet_write_ack(const light_packet_transport *transport,
                                   uint8_t *payload)
{
    payload[0] = 1u;
    payload[1] = LIGHT_PACKET_MODE;
    light_packet_write_u32(&payload[2], transport->receive_base);
    light_packet_write_u16(&payload[6],
                           light_packet_receive_mask(transport));
}

static void light_packet_ack_cell(light_packet_transport *transport,
                                  uint32_t sequence)
{
    light_packet_tx_cell *cell =
        &transport->transmit[sequence % LIGHT_PACKET_MAX_WINDOW];
    if (cell->occupied != 0 && cell->sequence == sequence) {
        transport->status.serialized_bytes_acked += cell->length;
        transport->status.packet_bytes_acked += cell->payload_length;
        memset(cell, 0, sizeof(*cell));
    }
}

static int light_packet_apply_ack(light_packet_transport *transport,
                                  const uint8_t *payload,
                                  size_t payload_length)
{
    uint32_t base;
    uint16_t mask;
    unsigned index;
    unsigned bit;
    if (payload_length < LIGHT_PACKET_ACK_BYTES || payload[0] != 1u ||
        payload[1] != LIGHT_PACKET_MODE) {
        return UM_ERR_HEADER;
    }
    base = light_packet_read_u32(&payload[2]);
    mask = light_packet_read_u16(&payload[6]);
    if (base > transport->transmit_next) {
        return UM_ERR_HEADER;
    }
    if (base > transport->remote_receive_base) {
        transport->remote_receive_base = base;
    }
    for (index = 0u; index < LIGHT_PACKET_MAX_WINDOW; ++index) {
        light_packet_tx_cell *cell = &transport->transmit[index];
        if (cell->occupied != 0 && cell->sequence < base) {
            light_packet_ack_cell(transport, cell->sequence);
        }
    }
    for (bit = 0u; bit < LIGHT_PACKET_ACK_BITS; ++bit) {
        uint64_t sequence = (uint64_t)base + bit;
        if (sequence < transport->transmit_next &&
            (mask & (uint16_t)(UINT16_C(1) << bit)) != 0u) {
            light_packet_ack_cell(transport, (uint32_t)sequence);
        }
    }
    return UM_OK;
}

static int light_packet_drain_receive(light_packet_transport *transport)
{
    for (;;) {
        light_packet_rx_cell *cell = &transport->receive[
            transport->receive_base % LIGHT_PACKET_MAX_WINDOW];
        if (cell->occupied == 0 ||
            cell->sequence != transport->receive_base) {
            return UM_OK;
        }
        while (cell->offset < cell->length) {
            uint8_t byte = cell->bytes[cell->offset];
            if (transport->receive_header_length <
                LIGHT_PACKET_RECORD_BYTES) {
                transport->receive_header_length++;
                if (transport->receive_header_length == 1u) {
                    transport->receive_packet_expected =
                        (size_t)byte << 8u;
                } else {
                    transport->receive_packet_expected |= byte;
                    if (transport->receive_packet_expected == 0u ||
                        transport->receive_packet_expected >
                            transport->max_packet) {
                        return UM_ERR_HEADER;
                    }
                }
                ++cell->offset;
                continue;
            }
            if (transport->receive_packet_length + 1u ==
                    transport->receive_packet_expected &&
                transport->incoming.count ==
                    transport->incoming.capacity) {
                return UM_OK;
            }
            transport->receive_packet[
                transport->receive_packet_length++] = byte;
            ++cell->offset;
            if (transport->receive_packet_length ==
                transport->receive_packet_expected) {
                int status = light_packet_queue_push(
                    &transport->incoming, transport->receive_packet,
                    transport->receive_packet_length);
                if (status != UM_OK) {
                    return status;
                }
                ++transport->status.incoming_packets_received;
                transport->status.packet_bytes_received +=
                    transport->receive_packet_length;
                transport->receive_header_length = 0u;
                transport->receive_packet_length = 0u;
                transport->receive_packet_expected = 0u;
            }
        }
        memset(cell, 0, sizeof(*cell));
        ++transport->receive_base;
    }
}

int light_packet_transport_create(light_packet_transport **transport,
                                  unsigned max_packet_bytes,
                                  size_t queue_packets,
                                  unsigned transmit_window,
                                  unsigned retransmit_after_frames,
                                  uint32_t random_seed)
{
    light_packet_transport *created;
    int status;
    if (transport == NULL || max_packet_bytes == 0u ||
        max_packet_bytes > UINT16_MAX || queue_packets == 0u ||
        queue_packets > LIGHT_PACKET_MAX_QUEUE || transmit_window == 0u ||
        transmit_window > LIGHT_PACKET_MAX_WINDOW ||
        retransmit_after_frames == 0u) {
        return UM_ERR_ARGUMENT;
    }
    *transport = NULL;
    created = (light_packet_transport *)calloc(1u, sizeof(*created));
    if (created == NULL) {
        return UM_ERR_MEMORY;
    }
    created->receive_packet = (uint8_t *)malloc(max_packet_bytes);
    if (created->receive_packet == NULL) {
        free(created);
        return UM_ERR_MEMORY;
    }
    status = light_packet_queue_init(&created->outgoing, queue_packets,
                                     max_packet_bytes);
    if (status == UM_OK) {
        status = light_packet_queue_init(&created->incoming, queue_packets,
                                         max_packet_bytes);
    }
    if (status != UM_OK) {
        light_packet_transport_destroy(created);
        return status;
    }
    created->max_packet = max_packet_bytes;
    created->transmit_window = transmit_window;
    created->retransmit_after_frames = retransmit_after_frames;
    created->random_state = random_seed;
    *transport = created;
    return UM_OK;
}

void light_packet_transport_destroy(light_packet_transport *transport)
{
    if (transport == NULL) {
        return;
    }
    light_packet_queue_destroy(&transport->incoming);
    light_packet_queue_destroy(&transport->outgoing);
    free(transport->receive_packet);
    free(transport);
}

int light_packet_transport_enqueue(light_packet_transport *transport,
                                   const uint8_t *packet,
                                   size_t packet_length)
{
    int status;
    if (transport == NULL || packet == NULL || packet_length == 0u ||
        packet_length > transport->max_packet) {
        return UM_ERR_ARGUMENT;
    }
    status = light_packet_queue_push(&transport->outgoing, packet,
                                     packet_length);
    if (status == UM_OK) {
        ++transport->status.outgoing_packets_accepted;
    }
    return status;
}

int light_packet_transport_dequeue(light_packet_transport *transport,
                                   uint8_t *packet, size_t capacity,
                                   size_t *packet_length)
{
    const uint8_t *queued;
    size_t length;
    int status;
    if (transport == NULL || packet == NULL || capacity == 0u ||
        packet_length == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *packet_length = 0u;
    status = light_packet_drain_receive(transport);
    if (status != UM_OK) {
        return status;
    }
    queued = light_packet_queue_head(&transport->incoming, &length);
    if (queued == NULL) {
        return UM_ERR_TIMEOUT;
    }
    *packet_length = length;
    if (length > capacity) {
        return UM_ERR_CAPACITY;
    }
    memcpy(packet, queued, length);
    light_packet_queue_pop(&transport->incoming);
    return light_packet_drain_receive(transport);
}

void light_packet_transport_build(light_packet_transport *transport,
                                  size_t local_frame, uint32_t *sequence,
                                  uint8_t *payload, size_t *payload_length,
                                  int *has_data, int *retransmission)
{
    light_packet_tx_cell *cell;
    light_packet_write_ack(transport, payload);
    *sequence = UINT32_MAX;
    *payload_length = LIGHT_PACKET_ACK_BYTES;
    *has_data = 0;
    *retransmission = 0;

    cell = light_packet_select_retry(transport, local_frame);
    if (cell != NULL) {
        *retransmission = 1;
        ++transport->status.retransmissions;
    } else {
        cell = light_packet_create_cell(transport);
    }
    if (cell == NULL) {
        return;
    }
    memcpy(payload + LIGHT_PACKET_ACK_BYTES, cell->bytes, cell->length);
    *sequence = cell->sequence;
    *payload_length = LIGHT_PACKET_ACK_BYTES + cell->length;
    *has_data = 1;
    cell->last_sent_frame = local_frame;
}

int light_packet_transport_process(light_packet_transport *transport,
                                   uint32_t sequence,
                                   const uint8_t *payload,
                                   size_t payload_length, int has_data)
{
    light_packet_rx_cell *cell;
    uint64_t receive_limit;
    int status;
    if (transport == NULL || payload == NULL) {
        return UM_ERR_ARGUMENT;
    }
    status = light_packet_apply_ack(transport, payload, payload_length);
    if (status != UM_OK) {
        return status;
    }
    if (has_data == 0) {
        return payload_length == LIGHT_PACKET_ACK_BYTES ? UM_OK
                                                        : UM_ERR_HEADER;
    }
    if (payload_length <= LIGHT_PACKET_ACK_BYTES ||
        payload_length > UM_LIGHT_MAX_PAYLOAD) {
        return UM_ERR_HEADER;
    }
    if (sequence < transport->receive_base) {
        ++transport->status.duplicate_cells;
        return UM_OK;
    }
    receive_limit = (uint64_t)transport->receive_base +
                    LIGHT_PACKET_ACK_BITS;
    if ((uint64_t)sequence >= receive_limit) {
        return UM_ERR_HEADER;
    }
    cell = &transport->receive[sequence % LIGHT_PACKET_MAX_WINDOW];
    if (cell->occupied != 0) {
        if (cell->sequence != sequence) {
            return UM_ERR_HEADER;
        }
        ++transport->status.duplicate_cells;
        return UM_OK;
    }
    cell->sequence = sequence;
    cell->length = (uint8_t)(payload_length - LIGHT_PACKET_ACK_BYTES);
    memcpy(cell->bytes, payload + LIGHT_PACKET_ACK_BYTES, cell->length);
    cell->occupied = 1;
    return light_packet_drain_receive(transport);
}

unsigned light_packet_transport_max_packet(
    const light_packet_transport *transport)
{
    return transport != NULL ? transport->max_packet : 0u;
}

void light_packet_transport_get_status(
    const light_packet_transport *transport,
    light_packet_transport_status *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (transport == NULL) {
        return;
    }
    *status = transport->status;
    status->outgoing_packets_queued = transport->outgoing.count;
    status->incoming_packets_queued = transport->incoming.count;
    status->outgoing_cells_in_flight = light_packet_in_flight(transport);
}
