#include "network_trace.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    unsigned version;
    unsigned protocol;
    const uint8_t *source;
    const uint8_t *destination;
    size_t address_length;
    size_t transport_offset;
    size_t packet_length;
} trace_packet_view;

typedef struct {
    const uint8_t *bytes;
    size_t length;
    uint16_t source_port;
    uint16_t destination_port;
    int response;
} trace_dns_view;

static uint16_t trace_read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t trace_read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) |
           ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

static uint64_t trace_now(void)
{
    time_t now = time(NULL);
    return now > 0 ? (uint64_t)now : 0u;
}

static int trace_packet_open(const uint8_t *packet, size_t packet_length,
                             trace_packet_view *view)
{
    size_t header_length;
    size_t wire_length;
    if (packet == NULL || view == NULL || packet_length == 0u) {
        return 0;
    }
    memset(view, 0, sizeof(*view));
    view->version = packet[0] >> 4u;
    if (view->version == 4u) {
        if (packet_length < 20u) {
            return 0;
        }
        header_length = (size_t)(packet[0] & 0x0fu) * 4u;
        wire_length = trace_read_u16(&packet[2]);
        if (header_length < 20u || wire_length < header_length ||
            wire_length > packet_length) {
            return 0;
        }
        view->protocol = packet[9];
        view->source = &packet[12];
        view->destination = &packet[16];
        view->address_length = 4u;
        view->transport_offset = header_length;
        view->packet_length = wire_length;
        if ((trace_read_u16(&packet[6]) & UINT16_C(0x1fff)) != 0u) {
            view->transport_offset = wire_length;
        }
        return 1;
    }
    if (view->version == 6u) {
        if (packet_length < 40u) {
            return 0;
        }
        wire_length = 40u + trace_read_u16(&packet[4]);
        if (wire_length > packet_length) {
            return 0;
        }
        view->protocol = packet[6];
        view->source = &packet[8];
        view->destination = &packet[24];
        view->address_length = 16u;
        view->transport_offset = 40u;
        view->packet_length = wire_length;
        return 1;
    }
    return 0;
}

static int trace_dns_open(const uint8_t *packet, size_t packet_length,
                          trace_packet_view *packet_view,
                          trace_dns_view *dns_view)
{
    uint16_t udp_length;
    if (dns_view == NULL ||
        trace_packet_open(packet, packet_length, packet_view) == 0 ||
        packet_view->protocol != 17u ||
        packet_view->transport_offset + 8u > packet_view->packet_length) {
        return 0;
    }
    dns_view->source_port =
        trace_read_u16(&packet[packet_view->transport_offset]);
    dns_view->destination_port =
        trace_read_u16(&packet[packet_view->transport_offset + 2u]);
    if (dns_view->source_port != 53u &&
        dns_view->destination_port != 53u) {
        return 0;
    }
    udp_length =
        trace_read_u16(&packet[packet_view->transport_offset + 4u]);
    if (udp_length < 20u ||
        packet_view->transport_offset + udp_length >
            packet_view->packet_length) {
        return 0;
    }
    dns_view->bytes = &packet[packet_view->transport_offset + 8u];
    dns_view->length = udp_length - 8u;
    dns_view->response =
        (trace_read_u16(&dns_view->bytes[2]) & UINT16_C(0x8000)) != 0u;
    if ((dns_view->response != 0 && dns_view->source_port != 53u) ||
        (dns_view->response == 0 && dns_view->destination_port != 53u)) {
        return 0;
    }
    return 1;
}

int um_network_trace_read_dns_name(const uint8_t *dns, size_t dns_length,
                                   size_t start, char *name,
                                   size_t name_capacity, size_t *next)
{
    size_t position = start;
    size_t output = 0u;
    size_t jumps = 0u;
    int jumped = 0;
    if (dns == NULL || name == NULL || name_capacity == 0u ||
        next == NULL) {
        return 0;
    }
    while (position < dns_length && jumps <= 16u) {
        size_t label_length = dns[position];
        if ((label_length & 0xc0u) == 0xc0u) {
            size_t target;
            if (position + 1u >= dns_length) {
                return 0;
            }
            target = ((label_length & 0x3fu) << 8u) | dns[position + 1u];
            if (target >= dns_length) {
                return 0;
            }
            if (jumped == 0) {
                *next = position + 2u;
            }
            position = target;
            jumped = 1;
            ++jumps;
            continue;
        }
        if (label_length == 0u) {
            if (jumped == 0) {
                *next = position + 1u;
            }
            if (output == 0u) {
                if (name_capacity < 2u) {
                    return 0;
                }
                name[output++] = '.';
            }
            name[output] = '\0';
            return 1;
        }
        if (label_length > 63u ||
            position + 1u + label_length > dns_length) {
            return 0;
        }
        if (output != 0u) {
            if (output + 1u >= name_capacity) {
                return 0;
            }
            name[output++] = '.';
        }
        ++position;
        while (label_length-- != 0u) {
            unsigned char character = dns[position++];
            if (output + 1u >= name_capacity) {
                return 0;
            }
            name[output++] =
                (char)(isalnum(character) != 0 || character == '-' ||
                               character == '_'
                           ? tolower(character)
                           : '?');
        }
        if (jumped == 0) {
            *next = position;
        }
    }
    return 0;
}

static const char *trace_dns_type(uint16_t type, char text[16])
{
    switch (type) {
    case 1u:
        return "A";
    case 5u:
        return "CNAME";
    case 12u:
        return "PTR";
    case 16u:
        return "TXT";
    case 28u:
        return "AAAA";
    case 64u:
        return "SVCB";
    case 65u:
        return "HTTPS";
    default:
        (void)snprintf(text, 16u, "TYPE%u", (unsigned)type);
        return text;
    }
}

static int trace_describe_dns(const trace_dns_view *view, char *text,
                              size_t capacity)
{
    char name[UM_NETWORK_TRACE_NAME_BYTES];
    char type_text[16];
    size_t question_end = 0u;
    uint16_t type;
    uint16_t flags;
    if (view == NULL || trace_read_u16(&view->bytes[4]) == 0u ||
        um_network_trace_read_dns_name(view->bytes, view->length, 12u,
                                       name, sizeof(name),
                                       &question_end) == 0 ||
        question_end + 4u > view->length) {
        return 0;
    }
    type = trace_read_u16(&view->bytes[question_end]);
    flags = trace_read_u16(&view->bytes[2]);
    if (view->response == 0) {
        (void)snprintf(text, capacity, "DNS query %s %s id=%u", name,
                       trace_dns_type(type, type_text),
                       (unsigned)trace_read_u16(view->bytes));
    } else {
        (void)snprintf(text, capacity,
                       "DNS response %s %s id=%u rcode=%u answers=%u",
                       name, trace_dns_type(type, type_text),
                       (unsigned)trace_read_u16(view->bytes),
                       (unsigned)(flags & 0x0fu),
                       (unsigned)trace_read_u16(&view->bytes[6]));
    }
    return 1;
}

static void trace_remember_address(um_network_trace *trace,
                                   const uint8_t *address,
                                   size_t address_length, const char *name,
                                   uint32_t ttl)
{
    uint64_t now = trace_now();
    uint64_t ttl_seconds = ttl == 0u ? 1u : ttl;
    size_t selected = SIZE_MAX;
    size_t index;
    if (trace == NULL || address == NULL || name == NULL ||
        (address_length != 4u && address_length != 16u)) {
        return;
    }
    if (ttl_seconds > 86400u) {
        ttl_seconds = 86400u;
    }
    for (index = 0u; index < UM_NETWORK_TRACE_ADDRESSES; ++index) {
        um_network_trace_address *entry = &trace->addresses[index];
        if (entry->address_length == address_length &&
            memcmp(entry->address, address, address_length) == 0) {
            selected = index;
            break;
        }
        if (selected == SIZE_MAX &&
            (entry->address_length == 0u ||
             (now != 0u && entry->expires_at <= now))) {
            selected = index;
        }
    }
    if (selected == SIZE_MAX) {
        selected = trace->next_address++ % UM_NETWORK_TRACE_ADDRESSES;
    }
    trace->addresses[selected].address_length = (uint8_t)address_length;
    memcpy(trace->addresses[selected].address, address, address_length);
    (void)snprintf(trace->addresses[selected].name,
                   sizeof(trace->addresses[selected].name), "%s", name);
    trace->addresses[selected].expires_at =
        now != 0u ? now + ttl_seconds : UINT64_MAX;
}

void um_network_trace_init(um_network_trace *trace)
{
    if (trace != NULL) {
        memset(trace, 0, sizeof(*trace));
    }
}

void um_network_trace_observe(um_network_trace *trace,
                              const uint8_t *packet,
                              size_t packet_length)
{
    trace_packet_view packet_view;
    trace_dns_view dns_view;
    char question[UM_NETWORK_TRACE_NAME_BYTES];
    size_t offset = 12u;
    size_t index;
    uint16_t question_count;
    uint16_t answer_count;
    if (trace == NULL ||
        trace_dns_open(packet, packet_length, &packet_view, &dns_view) == 0 ||
        dns_view.response == 0) {
        return;
    }
    question_count = trace_read_u16(&dns_view.bytes[4]);
    answer_count = trace_read_u16(&dns_view.bytes[6]);
    if (question_count == 0u || answer_count == 0u) {
        return;
    }
    for (index = 0u; index < question_count; ++index) {
        char name[UM_NETWORK_TRACE_NAME_BYTES];
        size_t next = 0u;
        if (um_network_trace_read_dns_name(
                dns_view.bytes, dns_view.length, offset, name,
                sizeof(name), &next) == 0 ||
            next + 4u > dns_view.length) {
            return;
        }
        if (index == 0u) {
            (void)snprintf(question, sizeof(question), "%s", name);
        }
        offset = next + 4u;
    }
    for (index = 0u; index < answer_count; ++index) {
        char owner[UM_NETWORK_TRACE_NAME_BYTES];
        size_t next = 0u;
        uint16_t type;
        uint16_t record_class;
        uint32_t ttl;
        uint16_t data_length;
        if (um_network_trace_read_dns_name(
                dns_view.bytes, dns_view.length, offset, owner,
                sizeof(owner), &next) == 0 ||
            next + 10u > dns_view.length) {
            return;
        }
        type = trace_read_u16(&dns_view.bytes[next]);
        record_class = trace_read_u16(&dns_view.bytes[next + 2u]);
        ttl = trace_read_u32(&dns_view.bytes[next + 4u]);
        data_length = trace_read_u16(&dns_view.bytes[next + 8u]);
        offset = next + 10u;
        if ((size_t)data_length > dns_view.length - offset) {
            return;
        }
        if (record_class == 1u && type == 1u && data_length == 4u) {
            trace_remember_address(trace, &dns_view.bytes[offset], 4u,
                                   question, ttl);
        } else if (record_class == 1u && type == 28u &&
                   data_length == 16u) {
            trace_remember_address(trace, &dns_view.bytes[offset], 16u,
                                   question, ttl);
        }
        offset += data_length;
    }
}

static const char *trace_name_for_address(const um_network_trace *trace,
                                          const uint8_t *address,
                                          size_t address_length)
{
    uint64_t now = trace_now();
    size_t index;
    if (trace == NULL || address == NULL) {
        return NULL;
    }
    for (index = 0u; index < UM_NETWORK_TRACE_ADDRESSES; ++index) {
        const um_network_trace_address *entry = &trace->addresses[index];
        if (entry->address_length == address_length &&
            (now == 0u || entry->expires_at > now) &&
            memcmp(entry->address, address, address_length) == 0) {
            return entry->name;
        }
    }
    return NULL;
}

static void trace_describe_address(const um_network_trace *trace,
                                   const uint8_t *address,
                                   size_t address_length, char *text,
                                   size_t capacity)
{
    const char *name =
        trace_name_for_address(trace, address, address_length);
    char numeric[64];
    if (address_length == 4u) {
        (void)snprintf(numeric, sizeof(numeric), "%u.%u.%u.%u",
                       address[0], address[1], address[2], address[3]);
    } else {
        (void)snprintf(
            numeric, sizeof(numeric),
            "%x:%x:%x:%x:%x:%x:%x:%x",
            (unsigned)trace_read_u16(&address[0]),
            (unsigned)trace_read_u16(&address[2]),
            (unsigned)trace_read_u16(&address[4]),
            (unsigned)trace_read_u16(&address[6]),
            (unsigned)trace_read_u16(&address[8]),
            (unsigned)trace_read_u16(&address[10]),
            (unsigned)trace_read_u16(&address[12]),
            (unsigned)trace_read_u16(&address[14]));
    }
    if (name != NULL) {
        (void)snprintf(text, capacity, "%s(%s)", numeric, name);
    } else {
        (void)snprintf(text, capacity, "%s", numeric);
    }
}

static void trace_describe_icmp(const um_network_trace *trace,
                                const uint8_t *packet,
                                const trace_packet_view *view,
                                const char *family, const char *source,
                                const char *destination, char *description,
                                size_t description_capacity)
{
    unsigned type = packet[view->transport_offset];
    unsigned code = packet[view->transport_offset + 1u];
    if (view->version == 4u && type == 3u &&
        view->transport_offset + 8u + 20u <= view->packet_length) {
        const uint8_t *quoted = &packet[view->transport_offset + 8u];
        size_t quoted_length =
            view->packet_length - view->transport_offset - 8u;
        size_t quoted_header = (size_t)(quoted[0] & 0x0fu) * 4u;
        unsigned quoted_protocol = quoted[9];
        if ((quoted[0] >> 4u) == 4u && quoted_header >= 20u &&
            quoted_header + 4u <= quoted_length &&
            (quoted_protocol == 6u || quoted_protocol == 17u)) {
            char quoted_source[176];
            char quoted_destination[176];
            trace_describe_address(trace, &quoted[12], 4u,
                                   quoted_source,
                                   sizeof(quoted_source));
            trace_describe_address(trace, &quoted[16], 4u,
                                   quoted_destination,
                                   sizeof(quoted_destination));
            (void)snprintf(
                description, description_capacity,
                "%s/ICMP %s->%s type=%u code=%u quoted=%s %s:%u->%s:%u",
                family, source, destination, type, code,
                quoted_protocol == 6u ? "TCP" : "UDP", quoted_source,
                (unsigned)trace_read_u16(&quoted[quoted_header]),
                quoted_destination,
                (unsigned)trace_read_u16(&quoted[quoted_header + 2u]));
            return;
        }
    }
    (void)snprintf(description, description_capacity,
                   "%s/ICMP %s->%s type=%u code=%u", family, source,
                   destination, type, code);
}

int um_network_trace_describe(const um_network_trace *trace,
                              const uint8_t *packet,
                              size_t packet_length, char *description,
                              size_t description_capacity)
{
    trace_packet_view view;
    trace_dns_view dns_view;
    const char *family;
    char source[176];
    char destination[176];
    char dns[180];
    if (description == NULL || description_capacity == 0u) {
        return 0;
    }
    if (trace_packet_open(packet, packet_length, &view) == 0) {
        (void)snprintf(description, description_capacity, "invalid IP");
        return 0;
    }
    family = view.version == 4u ? "IPv4" : "IPv6";
    trace_describe_address(trace, view.source, view.address_length,
                           source, sizeof(source));
    trace_describe_address(trace, view.destination, view.address_length,
                           destination, sizeof(destination));
    if ((view.protocol == 6u || view.protocol == 17u) &&
        view.transport_offset + 4u <= view.packet_length) {
        uint16_t source_port =
            trace_read_u16(&packet[view.transport_offset]);
        uint16_t destination_port =
            trace_read_u16(&packet[view.transport_offset + 2u]);
        int have_dns =
            view.protocol == 17u &&
            trace_dns_open(packet, packet_length, &view, &dns_view) != 0 &&
            trace_describe_dns(&dns_view, dns, sizeof(dns)) != 0;
        if (view.protocol == 17u) {
            size_t payload =
                view.transport_offset + 8u <= view.packet_length
                    ? view.packet_length - view.transport_offset - 8u
                    : 0u;
            (void)snprintf(
                description, description_capacity,
                "%s/UDP %s%s%s:%u->%s%s%s:%u payload=%zu%s%s",
                family, view.version == 6u ? "[" : "", source,
                view.version == 6u ? "]" : "", (unsigned)source_port,
                view.version == 6u ? "[" : "", destination,
                view.version == 6u ? "]" : "",
                (unsigned)destination_port, payload,
                have_dns != 0 ? " " : "", have_dns != 0 ? dns : "");
            return 1;
        }
        if (view.transport_offset + 20u <= view.packet_length) {
            size_t header_length =
                (size_t)(packet[view.transport_offset + 12u] >> 4u) * 4u;
            unsigned flags = packet[view.transport_offset + 13u];
            size_t payload =
                header_length >= 20u &&
                        view.transport_offset + header_length <=
                            view.packet_length
                    ? view.packet_length - view.transport_offset -
                          header_length
                    : 0u;
            (void)snprintf(
                description, description_capacity,
                "%s/TCP %s%s%s:%u->%s%s%s:%u flags=0x%02x seq=%u "
                "ack=%u payload=%zu",
                family, view.version == 6u ? "[" : "", source,
                view.version == 6u ? "]" : "", (unsigned)source_port,
                view.version == 6u ? "[" : "", destination,
                view.version == 6u ? "]" : "",
                (unsigned)destination_port, flags,
                (unsigned)trace_read_u32(
                    &packet[view.transport_offset + 4u]),
                (unsigned)trace_read_u32(
                    &packet[view.transport_offset + 8u]),
                payload);
            return payload != 0u ||
                   (flags & (UINT8_C(0x01) | UINT8_C(0x02) |
                             UINT8_C(0x04))) != 0u;
        }
    }
    if ((view.protocol == 1u || view.protocol == 58u) &&
        view.transport_offset + 2u <= view.packet_length) {
        trace_describe_icmp(trace, packet, &view, family, source,
                            destination, description,
                            description_capacity);
        return 1;
    }
    (void)snprintf(description, description_capacity,
                   "%s/proto-%u %s->%s", family, view.protocol, source,
                   destination);
    return 1;
}
