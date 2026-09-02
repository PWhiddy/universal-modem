#ifndef UM_TCP_RELAY_H
#define UM_TCP_RELAY_H

#include "um.h"

#include <stdint.h>

typedef struct um_tcp_relay um_tcp_relay;

int um_tcp_relay_open_transparent(um_tcp_relay **relay,
                                  const char *listen_address,
                                  um_log_callback logger,
                                  void *logger_context,
                                  uint16_t *listen_port);
int um_tcp_relay_open_fixed(um_tcp_relay **relay,
                            const char *destination_address,
                            uint16_t destination_port,
                            int loopback_only,
                            um_log_callback logger,
                            void *logger_context,
                            uint16_t *listen_port);
void um_tcp_relay_close(um_tcp_relay *relay);

#endif
