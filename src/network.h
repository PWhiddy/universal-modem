#ifndef UM_NETWORK_H
#define UM_NETWORK_H

#include "um.h"

#include <stddef.h>
#include <stdint.h>

#define UM_NETWORK_MIN_MTU 576u
#define UM_NETWORK_MAX_MTU 1500u
#define UM_NETWORK_MAX_PACKET 2048u

typedef struct um_network um_network;

int um_network_prepare_audio_user(um_log_callback logger,
                                  void *logger_context);
int um_network_open(um_network **network, um_live_role role, unsigned mtu,
                    um_log_callback logger, void *logger_context);
void um_network_close(um_network *network);
int um_network_read(um_network *network, uint8_t *packet, size_t capacity,
                    unsigned timeout_ms, size_t *packet_length);
int um_network_write(um_network *network, const uint8_t *packet,
                     size_t packet_length, unsigned timeout_ms);
const char *um_network_interface_name(const um_network *network);
unsigned um_network_mtu(const um_network *network);

#endif
