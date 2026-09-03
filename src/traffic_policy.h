#ifndef UM_TRAFFIC_POLICY_H
#define UM_TRAFFIC_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define UM_TRAFFIC_POLICY_DNS_NAME_MAX 256u

typedef enum {
    UM_TRAFFIC_POLICY_PASS = 0,
    UM_TRAFFIC_POLICY_DROP_MULTICAST,
    UM_TRAFFIC_POLICY_DROP_BROADCAST,
    UM_TRAFFIC_POLICY_DROP_STALE_DNS_ICMP,
    UM_TRAFFIC_POLICY_DROP_BACKGROUND,
    UM_TRAFFIC_POLICY_REJECT_BACKGROUND_DNS,
    UM_TRAFFIC_POLICY_REJECT_QUIC
} um_traffic_policy_action;

typedef struct {
    um_traffic_policy_action action;
    const char *rule;
    char dns_name[UM_TRAFFIC_POLICY_DNS_NAME_MAX];
    uint16_t dns_type;
} um_traffic_policy_decision;

int um_traffic_policy_decide(const uint8_t *packet, size_t packet_length,
                             int client_outbound, int quiet_background,
                             int allow_messages,
                             um_traffic_policy_decision *decision);
int um_traffic_policy_build_dns_rejection(
    const uint8_t *query, size_t query_length, uint8_t *response,
    size_t response_capacity, size_t *response_length);
int um_traffic_policy_build_port_unreachable(
    const uint8_t *packet, size_t packet_length, uint8_t *response,
    size_t response_capacity, size_t *response_length);
int um_traffic_policy_is_tunnel_discovery_dns(const uint8_t *packet,
                                              size_t packet_length);

#endif
