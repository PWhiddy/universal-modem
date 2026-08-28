#include "traffic_policy.h"

#include "network.h"

#include <ctype.h>
#include <string.h>

/*
 * Quiet-link firewall policy
 * --------------------------
 * This is the single list of service-specific traffic suppressed before it
 * reaches the acoustic queue. Rules match a domain itself and its subdomains.
 * An explicit iMessage/APNs allow list is checked first.
 *
 * Apple service descriptions and APNs requirements:
 *   https://support.apple.com/en-us/101555
 *   https://support.apple.com/en-us/102266
 * Firefox automatic-connection behavior and current endpoint defaults:
 *   https://support.mozilla.org/en-US/kb/how-stop-firefox-making-automatic-connections
 *   https://searchfox.org/firefox-main/source/browser/app/profile/firefox.js
 *   https://searchfox.org/firefox-main/source/modules/libpref/init/all.js
 *
 * ESS names are not published in Apple's enterprise table, but are retained
 * because the supplied trace shows them immediately preceding and accompanying
 * the APNs/iMessage connection. Generic websites and CDNs are deliberately not
 * listed: a TUN packet has no marker distinguishing a Firefox preload from an
 * intentional navigation to the same site.
 */
typedef struct {
    const char *domain;
    const char *reason;
} quiet_domain_rule;

static const quiet_domain_rule imessage_allow_rules[] = {

/*
    {"push.apple.com", "Apple Push Notification service"},
    {"push-apple.com.akadns.net", "APNs load-balancing alias"},
    {"ess.apple.com", "observed iMessage identity/query service"},
    {"ess-apple.com.akadns.net", "observed iMessage service alias"},
    {"ess.g.aaplimg.com", "observed iMessage service CDN alias"},
*/
    /* Apple lists these as certificate-validation dependencies for its
     * services.  They also support ordinary foreground HTTPS validation. */
    {"certs.apple.com", "Apple certificate validation"},
    {"crl.apple.com", "Apple certificate revocation"},
    {"ocsp.apple.com", "Apple certificate validation"},
    {"ocsp2.apple.com", "Apple certificate validation"},
    {"valid.apple.com", "Apple certificate validation"}
};

static const quiet_domain_rule quiet_block_rules[] = {
    /* macOS service discovery, connection checks, and encrypted-DNS probes. */
    {"_dns.resolver.arpa", "encrypted DNS discovery"},
    {"captive.apple.com", "Apple captive-portal check"},
    {"doh.dns.apple.com", "Apple DNS-over-HTTPS discovery"},
    {"apple-dns.net", "iCloud DNS service"},
    {"probe.icloud.com", "iCloud connection test"},
    {"pong.icloud.com", "iCloud connection test"},
    {"metrics.icloud.com", "iCloud diagnostics"},
    {"time.apple.com", "background time synchronization"},
    {"time-macos.apple.com", "background time synchronization"},
    
    // blocking these temp
    {"push.apple.com", "Apple Push Notification service"},
    {"push-apple.com.akadns.net", "APNs load-balancing alias"},
    {"ess.apple.com", "observed iMessage identity/query service"},
    {"ess-apple.com.akadns.net", "observed iMessage service alias"},
    {"ess.g.aaplimg.com", "observed iMessage service CDN alias"},

    /* Apple services outside the requested APNs/iMessage text path. */
    {"news-edge.apple.com", "Apple News"},
    {"news.apple.com", "Apple News"},
    {"itunes.apple.com", "Apple Store/media content"},
    {"apps.apple.com", "Apple Store content"},
    {"mzstatic.com", "Apple Store/media content"},
    {"apple-cloudkit.com", "CloudKit background service"},
    {"gateway.icloud.com", "CloudKit content and asset updates"},
    {"mask.icloud.com", "iCloud Private Relay"},
    {"mask-h2.icloud.com", "iCloud Private Relay"},
    {"mask-api.icloud.com", "iCloud Private Relay"},
    {"guzzoni.apple.com", "Siri and dictation"},
    {"smoot.apple.com", "Siri and system search"},
    {"diagassets.apple.com", "Apple diagnostics"},
    {"iadsdk.apple.com", "Apple advertising service"},
    {"lcdn-locator.apple.com", "Apple content-cache discovery"},
    {"serverstatus.apple.com", "Apple content-cache address check"},

    /* Apple software and component update checks/downloads. */
    {"appldnld.apple.com", "Apple software update"},
    {"configuration.apple.com", "Apple component update"},
    {"gdmf.apple.com", "Apple software update catalog"},
    {"gdmf-ados.apple.com", "Apple component update catalog"},
    {"gg.apple.com", "Apple software update"},
    {"gs.apple.com", "Apple software update"},
    {"gsra.apple.com", "Apple component update"},
    {"ig.apple.com", "macOS software update"},
    {"mesu.apple.com", "Apple software update catalog"},
    {"oscdn.apple.com", "macOS recovery content"},
    {"osrecovery.apple.com", "macOS recovery content"},
    {"skl.apple.com", "macOS software update"},
    {"swcdn.apple.com", "macOS software update"},
    {"swdist.apple.com", "macOS software update"},
    {"swdownload.apple.com", "macOS software update"},
    {"swscan.apple.com", "macOS software update scan"},
    {"updates-http.cdn-apple.com", "Apple software update download"},
    {"updates.cdn-apple.com", "Apple software update download"},
    {"xp.apple.com", "Apple update/reporting service"},

    /* Firefox-owned automatic services. */
    {"detectportal.firefox.com", "Firefox connectivity check"},
    {"services.mozilla.com", "Firefox background service"},
    {"telemetry.mozilla.org", "Firefox telemetry"},
    {"cdn.mozilla.net", "Firefox remote content/experiments"},
    {"aus5.mozilla.org", "Firefox update check"},
    {"download.mozilla.org", "Firefox update download"},
    {"product-details.mozilla.org", "Firefox product metadata"},
    {"addons.mozilla.org", "Firefox add-on catalog/update"},
    {"accounts.firefox.com", "Firefox Sync account"},
    {"mozilla.cloudflare-dns.com", "Firefox DNS-over-HTTPS provider"},
    {"firefox.dns.nextdns.io", "Firefox DNS-over-HTTPS provider"},
    {"model-hub.mozilla.org", "Firefox ML model download"},
    {"spocs.getpocket.com", "Firefox sponsored Pocket content"},
    {"safebrowsing.google.com", "Firefox Safe Browsing refresh"},
    {"safebrowsing.googleapis.com", "Firefox Safe Browsing refresh"},
    {"webservices.mozgcp.net", "firefox nonsense"},

    /* Catch all remaining Apple-controlled service zones.  The APNs, ESS,
     * and certificate-validation exceptions above are checked first.  This
     * intentionally rejects an explicit visit to an Apple property in quiet
     * mode too: DNS alone cannot distinguish it from a system daemon. */
    {"apple.com", "non-iMessage Apple service"},
    {"icloud.com", "non-iMessage iCloud service"},
    {"aaplimg.com", "non-iMessage Apple content"},
    {"apple-cloudkit.com", "non-iMessage CloudKit service"},
    {"apple-dns.net", "Apple encrypted DNS service"},
    {"cdn-apple.com", "Apple content delivery"},
    {"icloud-content.com", "iCloud content delivery"},
    {"itunes.com", "Apple Store/media service"},
    {"mzstatic.com", "Apple Store/media content"},
    {"apple.news", "Apple News"},
    {"apple-mapkit.com", "Apple Maps content"},
    {"me.com", "non-iMessage iCloud service"},
    {"mac.com", "non-iMessage Apple service"},

    /* Resolver identity probes observed after encrypted-DNS discovery. */
    {"one.one.one.one", "resolver identity discovery"}
};

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static int validate_ip_packet(const uint8_t *packet, size_t length)
{
    unsigned version;
    if (packet == NULL || length == 0u || length > UM_NETWORK_MAX_PACKET) {
        return 0;
    }
    version = packet[0] >> 4u;
    if (version == 4u) {
        size_t header_length;
        if (length < 20u) {
            return 0;
        }
        header_length = (size_t)(packet[0] & 0x0fu) * 4u;
        return header_length >= 20u && header_length <= length &&
               read_u16(&packet[2]) == length;
    }
    return version == 6u && length >= 40u &&
           (size_t)read_u16(&packet[4]) + 40u == length;
}

static int domain_matches(const char *name, const char *domain)
{
    size_t name_length = strlen(name);
    size_t domain_length = strlen(domain);
    size_t index;
    if (domain_length > name_length) {
        return 0;
    }
    if (domain_length < name_length &&
        name[name_length - domain_length - 1u] != '.') {
        return 0;
    }
    name += name_length - domain_length;
    for (index = 0u; index < domain_length; ++index) {
        if (tolower((unsigned char)name[index]) !=
            tolower((unsigned char)domain[index])) {
            return 0;
        }
    }
    return 1;
}

static int read_dns_name(const uint8_t *dns, size_t dns_length,
                         size_t start, char *name, size_t name_capacity,
                         size_t *next)
{
    size_t position = start;
    size_t output = 0u;
    size_t jumps = 0u;
    int jumped = 0;
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
            name[output++] = (char)tolower(character);
        }
        if (jumped == 0) {
            *next = position;
        }
    }
    return 0;
}

static int parse_dns(const uint8_t *packet, size_t length, int query_only,
                     char *name, size_t name_capacity, uint16_t *type,
                     size_t *header_length_out)
{
    const uint8_t *dns;
    size_t header_length;
    size_t dns_length;
    size_t question_end = 0u;
    uint16_t udp_length;
    uint16_t flags;
    if (validate_ip_packet(packet, length) == 0 ||
        (packet[0] >> 4u) != 4u || packet[9] != 17u ||
        (read_u16(&packet[6]) & UINT16_C(0x3fff)) != 0u) {
        return 0;
    }
    header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    if (header_length + 20u > length) {
        return 0;
    }
    udp_length = read_u16(&packet[header_length + 4u]);
    if (udp_length != length - header_length || udp_length < 20u) {
        return 0;
    }
    dns = &packet[header_length + 8u];
    dns_length = udp_length - 8u;
    flags = read_u16(&dns[2]);
    if (read_u16(&dns[4]) != 1u || read_u16(&dns[6]) != 0u ||
        read_u16(&dns[8]) != 0u ||
        (query_only != 0 &&
         ((flags & UINT16_C(0x8000)) != 0u ||
          read_u16(&packet[header_length + 2u]) != 53u)) ||
        read_dns_name(dns, dns_length, 12u, name, name_capacity,
                      &question_end) == 0 ||
        question_end + 4u > dns_length) {
        return 0;
    }
    *type = read_u16(&dns[question_end]);
    if (header_length_out != NULL) {
        *header_length_out = header_length;
    }
    return 1;
}

static const quiet_domain_rule *blocked_dns_rule(const char *name)
{
    size_t index;
    for (index = 0u;
         index < sizeof(imessage_allow_rules) / sizeof(imessage_allow_rules[0]);
         ++index) {
        if (domain_matches(name, imessage_allow_rules[index].domain) != 0) {
            return NULL;
        }
    }
    if (domain_matches(name, "0.77.10.in-addr.arpa") != 0) {
        static const quiet_domain_rule tunnel_discovery = {
            "0.77.10.in-addr.arpa", "tunnel DNS-service discovery"
        };
        return &tunnel_discovery;
    }
    for (index = 0u;
         index < sizeof(quiet_block_rules) / sizeof(quiet_block_rules[0]);
         ++index) {
        if (domain_matches(name, quiet_block_rules[index].domain) != 0) {
            return &quiet_block_rules[index];
        }
    }
    return NULL;
}

static int packet_is_multicast(const uint8_t *packet, size_t length)
{
    if (validate_ip_packet(packet, length) == 0) {
        return 0;
    }
    if ((packet[0] >> 4u) == 4u) {
        return (packet[16] & UINT8_C(0xf0)) == UINT8_C(0xe0);
    }
    return packet[24] == UINT8_C(0xff);
}

static int packet_is_tunnel_broadcast(const uint8_t *packet, size_t length)
{
    static const uint8_t subnet_broadcast[4] = {10u, 77u, 0u, 3u};
    static const uint8_t limited_broadcast[4] = {255u, 255u, 255u, 255u};
    return validate_ip_packet(packet, length) != 0 &&
           (packet[0] >> 4u) == 4u &&
           (memcmp(&packet[16], subnet_broadcast,
                   sizeof(subnet_broadcast)) == 0 ||
            memcmp(&packet[16], limited_broadcast,
                   sizeof(limited_broadcast)) == 0);
}

static int packet_is_stale_dns_icmp(const uint8_t *packet, size_t length)
{
    size_t header_length;
    size_t quoted_offset;
    size_t quoted_header_length;
    if (validate_ip_packet(packet, length) == 0 ||
        (packet[0] >> 4u) != 4u || packet[9] != 1u) {
        return 0;
    }
    header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    quoted_offset = header_length + 8u;
    if (quoted_offset + 28u > length || packet[header_length] != 3u ||
        packet[header_length + 1u] != 3u ||
        (packet[quoted_offset] >> 4u) != 4u ||
        packet[quoted_offset + 9u] != 17u) {
        return 0;
    }
    quoted_header_length =
        (size_t)(packet[quoted_offset] & 0x0fu) * 4u;
    return quoted_header_length >= 20u &&
           quoted_offset + quoted_header_length + 8u <= length &&
           read_u16(&packet[quoted_offset + quoted_header_length]) == 53u;
}

static int destination_port(const uint8_t *packet, size_t length,
                            unsigned *protocol, uint16_t *port)
{
    size_t offset;
    if (validate_ip_packet(packet, length) == 0) {
        return 0;
    }
    if ((packet[0] >> 4u) == 4u) {
        offset = (size_t)(packet[0] & 0x0fu) * 4u;
        *protocol = packet[9];
        if ((read_u16(&packet[6]) & UINT16_C(0x1fff)) != 0u) {
            return 0;
        }
    } else {
        offset = 40u;
        *protocol = packet[6];
    }
    if ((*protocol != 6u && *protocol != 17u) || offset + 4u > length) {
        return 0;
    }
    *port = read_u16(&packet[offset + 2u]);
    return 1;
}

int um_traffic_policy_decide(const uint8_t *packet, size_t packet_length,
                             int client_outbound, int quiet_background,
                             um_traffic_policy_decision *decision)
{
    const quiet_domain_rule *rule;
    unsigned protocol;
    uint16_t port;
    if (decision == NULL || validate_ip_packet(packet, packet_length) == 0) {
        return -1;
    }
    memset(decision, 0, sizeof(*decision));
    if (packet_is_multicast(packet, packet_length) != 0) {
        decision->action = UM_TRAFFIC_POLICY_DROP_MULTICAST;
        decision->rule = "link-local multicast has no Internet path";
        return 0;
    }
    if (packet_is_tunnel_broadcast(packet, packet_length) != 0) {
        decision->action = UM_TRAFFIC_POLICY_DROP_BROADCAST;
        decision->rule = "tunnel or limited broadcast";
        return 0;
    }
    if (packet_is_stale_dns_icmp(packet, packet_length) != 0) {
        decision->action = UM_TRAFFIC_POLICY_DROP_STALE_DNS_ICMP;
        decision->rule = "stale DNS UDP port-unreachable";
        return 0;
    }
    if (client_outbound == 0 || quiet_background == 0) {
        return 0;
    }
    if (parse_dns(packet, packet_length, 1, decision->dns_name,
                  sizeof(decision->dns_name), &decision->dns_type,
                  NULL) != 0) {
        rule = blocked_dns_rule(decision->dns_name);
        if (rule != NULL) {
            decision->action = UM_TRAFFIC_POLICY_REJECT_BACKGROUND_DNS;
            decision->rule = rule->reason;
            return 0;
        }
    }
    if (destination_port(packet, packet_length, &protocol, &port) != 0) {
        if (protocol == 17u && port == 123u) {
            decision->action = UM_TRAFFIC_POLICY_DROP_BACKGROUND;
            decision->rule = "background NTP";
        } else if ((protocol == 6u || protocol == 17u) && port == 853u) {
            decision->action = UM_TRAFFIC_POLICY_DROP_BACKGROUND;
            decision->rule = "background encrypted DNS";
        }
    }
    return 0;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *bytes,
                             size_t length)
{
    while (length >= 2u) {
        sum += ((uint32_t)bytes[0] << 8u) | bytes[1];
        bytes += 2u;
        length -= 2u;
    }
    if (length != 0u) {
        sum += (uint32_t)bytes[0] << 8u;
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while ((sum >> 16u) != 0u) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16u);
    }
    return (uint16_t)~sum;
}

int um_traffic_policy_build_dns_rejection(
    const uint8_t *query, size_t query_length, uint8_t *response,
    size_t response_capacity, size_t *response_length)
{
    char name[UM_TRAFFIC_POLICY_DNS_NAME_MAX];
    size_t header_length;
    size_t udp_length;
    uint16_t type;
    uint16_t flags;
    uint16_t checksum;
    uint32_t sum;
    uint8_t address[4];
    uint8_t port[2];
    uint8_t *dns;
    if (response == NULL || response_length == NULL ||
        query_length > response_capacity ||
        parse_dns(query, query_length, 1, name, sizeof(name), &type,
                  &header_length) == 0) {
        return -1;
    }
    (void)type;
    memcpy(response, query, query_length);
    memcpy(address, &response[12], sizeof(address));
    memcpy(&response[12], &response[16], sizeof(address));
    memcpy(&response[16], address, sizeof(address));
    memcpy(port, &response[header_length], sizeof(port));
    memcpy(&response[header_length], &response[header_length + 2u],
           sizeof(port));
    memcpy(&response[header_length + 2u], port, sizeof(port));
    dns = &response[header_length + 8u];
    flags = read_u16(&dns[2]);
    flags &= UINT16_C(0x7910); /* opcode, RD, and CD */
    flags |= UINT16_C(0x8083); /* response, recursion available, NXDOMAIN */
    write_u16(&dns[2], flags);
    write_u16(&dns[6], 0u);
    write_u16(&dns[8], 0u);

    response[10] = 0u;
    response[11] = 0u;
    checksum = checksum_finish(checksum_add(0u, response, header_length));
    write_u16(&response[10], checksum);

    udp_length = query_length - header_length;
    response[header_length + 6u] = 0u;
    response[header_length + 7u] = 0u;
    sum = checksum_add(0u, &response[12], 8u);
    sum += 17u;
    sum += (uint32_t)udp_length;
    sum = checksum_add(sum, &response[header_length], udp_length);
    checksum = checksum_finish(sum);
    write_u16(&response[header_length + 6u],
              checksum == 0u ? UINT16_C(0xffff) : checksum);
    *response_length = query_length;
    return 0;
}

int um_traffic_policy_is_tunnel_discovery_dns(const uint8_t *packet,
                                              size_t packet_length)
{
    char name[UM_TRAFFIC_POLICY_DNS_NAME_MAX];
    uint16_t type;
    return parse_dns(packet, packet_length, 0, name, sizeof(name), &type,
                     NULL) != 0 &&
           type == 12u &&
           domain_matches(name, "0.77.10.in-addr.arpa") != 0;
}
