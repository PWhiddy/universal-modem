#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "network.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/sysctl.h>
#endif

#define UM_TUN_SUBNET "10.77.0.0/30"
#define UM_TUN_GATEWAY_ADDRESS "10.77.0.1"
#define UM_TUN_CLIENT_ADDRESS "10.77.0.2"
#define UM_DNS_PRIMARY "1.1.1.1"
#define UM_DNS_SECONDARY "8.8.8.8"

struct um_network {
    int fd;
    um_live_role role;
    char interface_name[IFNAMSIZ];
    um_log_callback logger;
    void *logger_context;
#if defined(__linux__)
    const char *ip_path;
    const char *iptables_path;
    const char *nft_path;
    const char *resolvectl_path;
    char egress_name[IFNAMSIZ];
    char firewall_tag[64];
    char nft_table[32];
    unsigned iptables_rules;
    int nft_table_created;
    int route_low_added;
    int route_high_added;
    int dns_configured;
    int forwarding_changed;
    int forwarding_original;
#elif defined(__APPLE__)
    const char *ifconfig_path;
    const char *route_path;
    const char *pfctl_path;
    char egress_name[IFNAMSIZ];
    char pf_anchor[64];
    unsigned long long pf_token;
    int pf_anchor_loaded;
    int pf_reference_held;
    int route_low_added;
    int route_high_added;
    int forwarding_changed;
    int forwarding_original;
    SCDynamicStoreRef dynamic_store;
    CFStringRef dns_key;
#endif
};

static void network_log(um_network *network, const char *format, ...)
{
    char line[512];
    va_list arguments;
    if (network == NULL || network->logger == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    network->logger(network->logger_context, line);
}

#if defined(__linux__) || defined(__APPLE__)
static const char *find_executable(const char *const *paths)
{
    size_t index;
    for (index = 0u; paths[index] != NULL; ++index) {
        if (access(paths[index], X_OK) == 0) {
            return paths[index];
        }
    }
    return NULL;
}

static int run_command(um_network *network, const char *const argv[],
                       char *output, size_t output_capacity)
{
    int descriptors[2];
    pid_t child;
    int wait_status = 0;
    size_t used = 0u;
    char discard[256];
    if (argv == NULL || argv[0] == NULL || pipe(descriptors) != 0) {
        return UM_ERR_NETWORK;
    }
    child = fork();
    if (child < 0) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        return UM_ERR_NETWORK;
    }
    if (child == 0) {
        (void)close(descriptors[0]);
        (void)dup2(descriptors[1], STDOUT_FILENO);
        (void)dup2(descriptors[1], STDERR_FILENO);
        (void)close(descriptors[1]);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    (void)close(descriptors[1]);
    while (1) {
        char *destination = discard;
        size_t available = sizeof(discard);
        ssize_t count;
        if (output != NULL && output_capacity > 0u &&
            used + 1u < output_capacity) {
            destination = output + used;
            available = output_capacity - used - 1u;
        }
        count = read(descriptors[0], destination, available);
        if (count > 0) {
            if (destination != discard) {
                used += (size_t)count;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    (void)close(descriptors[0]);
    while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
    }
    if (output != NULL && output_capacity > 0u) {
        output[used] = '\0';
    }
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        network_log(network, "System command failed: %s%s%s", argv[0],
                    used != 0u ? ": " : "", used != 0u ? output : "");
        return UM_ERR_NETWORK;
    }
    return UM_OK;
}

static int set_nonblocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL, 0);
    int descriptor_flags = fcntl(descriptor, F_GETFD, 0);
    if (flags < 0 || descriptor_flags < 0 ||
        fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0 ||
        fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        return UM_ERR_NETWORK;
    }
    return UM_OK;
}

static int wait_descriptor(int descriptor, short events, unsigned timeout_ms)
{
    struct pollfd item;
    int status;
    item.fd = descriptor;
    item.events = events;
    item.revents = 0;
    do {
        status = poll(&item, 1u, (int)timeout_ms);
    } while (status < 0 && errno == EINTR);
    if (status == 0) {
        return UM_ERR_TIMEOUT;
    }
    if (status < 0 || (item.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return UM_ERR_NETWORK;
    }
    return (item.revents & events) != 0 ? UM_OK : UM_ERR_TIMEOUT;
}

static int write_all(int descriptor, const uint8_t *bytes, size_t length,
                     unsigned timeout_ms)
{
    int status = wait_descriptor(descriptor, POLLOUT, timeout_ms);
    ssize_t written;
    if (status != UM_OK) {
        return status;
    }
    do {
        written = write(descriptor, bytes, length);
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)length ? UM_OK : UM_ERR_NETWORK;
}
#endif

#if defined(__linux__)
static const char *const linux_ip_paths[] = {
    "/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", NULL
};
static const char *const linux_iptables_paths[] = {
    "/usr/sbin/iptables", "/sbin/iptables", NULL
};
static const char *const linux_nft_paths[] = {
    "/usr/sbin/nft", "/sbin/nft", NULL
};
static const char *const linux_resolvectl_paths[] = {
    "/usr/bin/resolvectl", "/usr/sbin/resolvectl", NULL
};

static int linux_run(um_network *network, const char *const argv[])
{
    char output[768];
    return run_command(network, argv, output, sizeof(output));
}

static int linux_detect_egress(um_network *network)
{
    FILE *routes = fopen("/proc/net/route", "r");
    char line[512];
    unsigned best_metric = UINT32_MAX;
    int found = 0;
    if (routes == NULL) {
        return UM_ERR_NETWORK;
    }
    while (fgets(line, sizeof(line), routes) != NULL) {
        char interface_name[IFNAMSIZ];
        unsigned long destination;
        unsigned long gateway;
        unsigned flags;
        unsigned reference_count;
        unsigned use_count;
        unsigned metric;
        unsigned long mask;
        int fields = sscanf(line, "%15s %lx %lx %x %u %u %u %lx",
                            interface_name, &destination, &gateway, &flags,
                            &reference_count, &use_count, &metric, &mask);
        (void)gateway;
        (void)reference_count;
        (void)use_count;
        (void)mask;
        if (fields == 8 && destination == 0ul && (flags & 1u) != 0u &&
            strcmp(interface_name, network->interface_name) != 0 &&
            (!found || metric < best_metric)) {
            (void)snprintf(network->egress_name,
                           sizeof(network->egress_name), "%s",
                           interface_name);
            best_metric = metric;
            found = 1;
        }
    }
    (void)fclose(routes);
    return found ? UM_OK : UM_ERR_NETWORK;
}

static int linux_read_forwarding(int *value)
{
    int descriptor;
    char byte = '\0';
    ssize_t count;
    descriptor = open("/proc/sys/net/ipv4/ip_forward", O_RDONLY);
    if (descriptor < 0) {
        return UM_ERR_NETWORK;
    }
    do {
        count = read(descriptor, &byte, 1u);
    } while (count < 0 && errno == EINTR);
    (void)close(descriptor);
    if (count != 1 || (byte != '0' && byte != '1')) {
        return UM_ERR_NETWORK;
    }
    *value = byte == '1' ? 1 : 0;
    return UM_OK;
}

static int linux_write_forwarding(int value)
{
    int descriptor;
    const char bytes[2] = {value != 0 ? '1' : '0', '\n'};
    ssize_t count;
    descriptor = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
    if (descriptor < 0) {
        return UM_ERR_NETWORK;
    }
    do {
        count = write(descriptor, bytes, sizeof(bytes));
    } while (count < 0 && errno == EINTR);
    (void)close(descriptor);
    return count == (ssize_t)sizeof(bytes) ? UM_OK : UM_ERR_NETWORK;
}

static int linux_set_forwarding(um_network *network)
{
    int status = linux_read_forwarding(&network->forwarding_original);
    if (status != UM_OK) {
        return status;
    }
    if (network->forwarding_original == 0) {
        status = linux_write_forwarding(1);
        if (status != UM_OK) {
            return status;
        }
        network->forwarding_changed = 1;
    }
    network_log(network, "IPv4 forwarding enabled (previous=%d)",
                network->forwarding_original);
    return UM_OK;
}

static int linux_add_iptables(um_network *network)
{
    const char *nat[] = {
        network->iptables_path, "-w", "5", "-t", "nat", "-A",
        "POSTROUTING", "-s", UM_TUN_SUBNET, "-o", network->egress_name,
        "-m", "comment", "--comment", network->firewall_tag, "-j",
        "MASQUERADE", NULL
    };
    const char *forward_out[] = {
        network->iptables_path, "-w", "5", "-I", "FORWARD", "1", "-i",
        network->interface_name, "-o", network->egress_name, "-s",
        UM_TUN_SUBNET, "-m", "comment", "--comment",
        network->firewall_tag, "-j", "ACCEPT", NULL
    };
    const char *forward_in[] = {
        network->iptables_path, "-w", "5", "-I", "FORWARD", "1", "-i",
        network->egress_name, "-o", network->interface_name, "-d",
        UM_TUN_SUBNET, "-m", "conntrack", "--ctstate",
        "RELATED,ESTABLISHED", "-m", "comment", "--comment",
        network->firewall_tag, "-j", "ACCEPT", NULL
    };
    int status = linux_run(network, nat);
    if (status == UM_OK) {
        network->iptables_rules = 1u;
        status = linux_run(network, forward_out);
    }
    if (status == UM_OK) {
        network->iptables_rules = 2u;
        status = linux_run(network, forward_in);
    }
    if (status == UM_OK) {
        network->iptables_rules = 3u;
    }
    return status;
}

static void linux_remove_iptables(um_network *network)
{
    const char *forward_in[] = {
        network->iptables_path, "-w", "5", "-D", "FORWARD", "-i",
        network->egress_name, "-o", network->interface_name, "-d",
        UM_TUN_SUBNET, "-m", "conntrack", "--ctstate",
        "RELATED,ESTABLISHED", "-m", "comment", "--comment",
        network->firewall_tag, "-j", "ACCEPT", NULL
    };
    const char *forward_out[] = {
        network->iptables_path, "-w", "5", "-D", "FORWARD", "-i",
        network->interface_name, "-o", network->egress_name, "-s",
        UM_TUN_SUBNET, "-m", "comment", "--comment",
        network->firewall_tag, "-j", "ACCEPT", NULL
    };
    const char *nat[] = {
        network->iptables_path, "-w", "5", "-t", "nat", "-D",
        "POSTROUTING", "-s", UM_TUN_SUBNET, "-o", network->egress_name,
        "-m", "comment", "--comment", network->firewall_tag, "-j",
        "MASQUERADE", NULL
    };
    if (network->iptables_rules >= 3u) {
        (void)linux_run(network, forward_in);
    }
    if (network->iptables_rules >= 2u) {
        (void)linux_run(network, forward_out);
    }
    if (network->iptables_rules >= 1u) {
        (void)linux_run(network, nat);
    }
    network->iptables_rules = 0u;
}

static int linux_add_nft(um_network *network)
{
    const char *add_table[] = {
        network->nft_path, "add", "table", "ip", network->nft_table, NULL
    };
    const char *add_nat_chain[] = {
        network->nft_path, "add", "chain", "ip", network->nft_table,
        "postrouting", "{", "type", "nat", "hook", "postrouting",
        "priority", "srcnat", ";", "policy", "accept", ";", "}", NULL
    };
    const char *add_forward_chain[] = {
        network->nft_path, "add", "chain", "ip", network->nft_table,
        "forward", "{", "type", "filter", "hook", "forward", "priority",
        "-10", ";", "policy", "accept", ";", "}", NULL
    };
    const char *add_nat[] = {
        network->nft_path, "add", "rule", "ip", network->nft_table,
        "postrouting", "oifname", network->egress_name, "ip", "saddr",
        UM_TUN_SUBNET, "masquerade", NULL
    };
    const char *add_out[] = {
        network->nft_path, "add", "rule", "ip", network->nft_table,
        "forward", "iifname", network->interface_name, "oifname",
        network->egress_name, "ip", "saddr", UM_TUN_SUBNET, "accept", NULL
    };
    const char *add_in[] = {
        network->nft_path, "add", "rule", "ip", network->nft_table,
        "forward", "iifname", network->egress_name, "oifname",
        network->interface_name, "ip", "daddr", UM_TUN_SUBNET, "ct",
        "state", "related,established", "accept", NULL
    };
    int status = linux_run(network, add_table);
    if (status == UM_OK) {
        network->nft_table_created = 1;
        status = linux_run(network, add_nat_chain);
    }
    if (status == UM_OK) {
        status = linux_run(network, add_forward_chain);
    }
    if (status == UM_OK) {
        status = linux_run(network, add_nat);
    }
    if (status == UM_OK) {
        status = linux_run(network, add_out);
    }
    if (status == UM_OK) {
        status = linux_run(network, add_in);
    }
    return status;
}

static void linux_remove_nft(um_network *network)
{
    const char *remove_table[] = {
        network->nft_path, "delete", "table", "ip", network->nft_table, NULL
    };
    if (network->nft_table_created != 0) {
        (void)linux_run(network, remove_table);
        network->nft_table_created = 0;
    }
}

static int linux_configure_dns(um_network *network)
{
    const char *dns[] = {
        network->resolvectl_path, "dns", network->interface_name,
        UM_DNS_PRIMARY, UM_DNS_SECONDARY, NULL
    };
    const char *domain[] = {
        network->resolvectl_path, "domain", network->interface_name, "~.",
        NULL
    };
    const char *default_route[] = {
        network->resolvectl_path, "default-route", network->interface_name,
        "yes", NULL
    };
    int status;
    if (network->resolvectl_path == NULL) {
        network_log(network,
                    "resolvectl not found; retaining the system's existing "
                    "DNS servers (their IPv4 traffic is still tunneled)");
        return UM_OK;
    }
    network->dns_configured = 1;
    status = linux_run(network, dns);
    if (status == UM_OK) {
        status = linux_run(network, domain);
    }
    if (status == UM_OK) {
        status = linux_run(network, default_route);
    }
    if (status != UM_OK) {
        network_log(network,
                    "Could not install per-link DNS; retaining existing DNS "
                    "configuration");
        return UM_OK;
    }
    network_log(network, "DNS routed through %s using %s and %s",
                network->interface_name, UM_DNS_PRIMARY, UM_DNS_SECONDARY);
    return UM_OK;
}

static void linux_revert_dns(um_network *network)
{
    const char *revert[] = {
        network->resolvectl_path, "revert", network->interface_name, NULL
    };
    if (network->dns_configured != 0 && network->resolvectl_path != NULL) {
        (void)linux_run(network, revert);
        network->dns_configured = 0;
    }
}

static int linux_open_tun(um_network *network)
{
    struct ifreq request;
    int descriptor = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (descriptor < 0) {
        network_log(network,
                    "Could not open /dev/net/tun: %s (run as root and ensure "
                    "the tun kernel module is available)",
                    strerror(errno));
        return UM_ERR_NETWORK;
    }
    memset(&request, 0, sizeof(request));
    request.ifr_flags = IFF_TUN | IFF_NO_PI;
    (void)snprintf(request.ifr_name, sizeof(request.ifr_name), "um%%d");
    if (ioctl(descriptor, TUNSETIFF, &request) != 0) {
        network_log(network, "TUNSETIFF failed: %s", strerror(errno));
        (void)close(descriptor);
        return UM_ERR_NETWORK;
    }
    if (set_nonblocking(descriptor) != UM_OK) {
        (void)close(descriptor);
        return UM_ERR_NETWORK;
    }
    network->fd = descriptor;
    (void)snprintf(network->interface_name,
                   sizeof(network->interface_name), "%s", request.ifr_name);
    return UM_OK;
}

static int linux_configure_interface(um_network *network)
{
    char mtu[16];
    const char *local_address = network->role == UM_LIVE_CLIENT
                                    ? UM_TUN_CLIENT_ADDRESS
                                    : UM_TUN_GATEWAY_ADDRESS;
    const char *peer_address = network->role == UM_LIVE_CLIENT
                                   ? UM_TUN_GATEWAY_ADDRESS
                                   : UM_TUN_CLIENT_ADDRESS;
    const char *address[] = {
        network->ip_path, "-4", "address", "add", local_address, "peer",
        peer_address, "dev", network->interface_name, NULL
    };
    const char *link[] = {
        network->ip_path, "link", "set", "dev", network->interface_name,
        "mtu", mtu, "up", NULL
    };
    int status;
    (void)snprintf(mtu, sizeof(mtu), "%u", UM_NETWORK_MTU);
    status = linux_run(network, address);
    if (status == UM_OK) {
        status = linux_run(network, link);
    }
    return status;
}

static int linux_configure_client(um_network *network)
{
    const char *route_low[] = {
        network->ip_path, "-4", "route", "add", "0.0.0.0/1", "via",
        UM_TUN_GATEWAY_ADDRESS, "dev", network->interface_name, NULL
    };
    const char *route_high[] = {
        network->ip_path, "-4", "route", "add", "128.0.0.0/1", "via",
        UM_TUN_GATEWAY_ADDRESS, "dev", network->interface_name, NULL
    };
    int status = linux_run(network, route_low);
    if (status == UM_OK) {
        network->route_low_added = 1;
        status = linux_run(network, route_high);
    }
    if (status == UM_OK) {
        network->route_high_added = 1;
        status = linux_configure_dns(network);
    }
    return status;
}

static int linux_configure_gateway(um_network *network)
{
    int status = linux_detect_egress(network);
    if (status != UM_OK) {
        network_log(network, "Could not identify the gateway's IPv4 default "
                             "route interface");
        return status;
    }
    status = linux_set_forwarding(network);
    if (status != UM_OK) {
        return status;
    }
    if (network->iptables_path != NULL) {
        status = linux_add_iptables(network);
        if (status != UM_OK) {
            linux_remove_iptables(network);
            if (network->nft_path != NULL) {
                network_log(network,
                            "iptables setup failed; trying nftables");
                status = linux_add_nft(network);
                if (status != UM_OK) {
                    linux_remove_nft(network);
                }
            }
        }
    } else if (network->nft_path != NULL) {
        status = linux_add_nft(network);
        if (status != UM_OK) {
            linux_remove_nft(network);
        }
    } else {
        network_log(network, "Neither iptables nor nft was found; NAT cannot "
                             "be configured");
        status = UM_ERR_NETWORK;
    }
    if (status == UM_OK) {
        network_log(network, "Gateway NAT active subnet=%s egress=%s",
                    UM_TUN_SUBNET, network->egress_name);
    }
    return status;
}

static void linux_cleanup(um_network *network)
{
    if (network->role == UM_LIVE_CLIENT && network->ip_path != NULL) {
        const char *route_high[] = {
            network->ip_path, "-4", "route", "del", "128.0.0.0/1", "via",
            UM_TUN_GATEWAY_ADDRESS, "dev", network->interface_name, NULL
        };
        const char *route_low[] = {
            network->ip_path, "-4", "route", "del", "0.0.0.0/1", "via",
            UM_TUN_GATEWAY_ADDRESS, "dev", network->interface_name, NULL
        };
        linux_revert_dns(network);
        if (network->route_high_added != 0) {
            (void)linux_run(network, route_high);
            network->route_high_added = 0;
        }
        if (network->route_low_added != 0) {
            (void)linux_run(network, route_low);
            network->route_low_added = 0;
        }
    }
    if (network->role == UM_LIVE_GATEWAY) {
        linux_remove_iptables(network);
        linux_remove_nft(network);
        if (network->forwarding_changed != 0) {
            if (linux_write_forwarding(network->forwarding_original) !=
                UM_OK) {
                network_log(network, "Could not restore IPv4 forwarding");
            }
            network->forwarding_changed = 0;
        }
    }
}
#elif defined(__APPLE__)
static const char *const mac_ifconfig_paths[] = {
    "/sbin/ifconfig", NULL
};
static const char *const mac_route_paths[] = {
    "/sbin/route", NULL
};
static const char *const mac_pfctl_paths[] = {
    "/sbin/pfctl", NULL
};

static int mac_run(um_network *network, const char *const argv[],
                   char *output, size_t output_capacity)
{
    return run_command(network, argv, output, output_capacity);
}

static int mac_open_utun(um_network *network)
{
    struct ctl_info info;
    struct sockaddr_ctl address;
    socklen_t name_length;
    int descriptor = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (descriptor < 0) {
        network_log(network, "Could not create utun control socket: %s",
                    strerror(errno));
        return UM_ERR_NETWORK;
    }
    memset(&info, 0, sizeof(info));
    (void)snprintf(info.ctl_name, sizeof(info.ctl_name), "%s",
                   UTUN_CONTROL_NAME);
    if (ioctl(descriptor, CTLIOCGINFO, &info) != 0) {
        network_log(network, "CTLIOCGINFO for utun failed: %s",
                    strerror(errno));
        (void)close(descriptor);
        return UM_ERR_NETWORK;
    }
    memset(&address, 0, sizeof(address));
    address.sc_len = sizeof(address);
    address.sc_family = AF_SYSTEM;
    address.ss_sysaddr = AF_SYS_CONTROL;
    address.sc_id = info.ctl_id;
    address.sc_unit = 0u;
    if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
        0) {
        network_log(network, "Could not connect utun control: %s",
                    strerror(errno));
        (void)close(descriptor);
        return UM_ERR_NETWORK;
    }
    name_length = (socklen_t)sizeof(network->interface_name);
    if (getsockopt(descriptor, SYSPROTO_CONTROL, UTUN_OPT_IFNAME,
                   network->interface_name, &name_length) != 0 ||
        name_length == 0u) {
        network_log(network, "Could not read assigned utun name: %s",
                    strerror(errno));
        (void)close(descriptor);
        return UM_ERR_NETWORK;
    }
    network->interface_name[sizeof(network->interface_name) - 1u] = '\0';
    if (set_nonblocking(descriptor) != UM_OK) {
        (void)close(descriptor);
        return UM_ERR_NETWORK;
    }
    network->fd = descriptor;
    return UM_OK;
}

static int mac_configure_interface(um_network *network)
{
    char mtu[16];
    const char *local_address = network->role == UM_LIVE_CLIENT
                                    ? UM_TUN_CLIENT_ADDRESS
                                    : UM_TUN_GATEWAY_ADDRESS;
    const char *peer_address = network->role == UM_LIVE_CLIENT
                                   ? UM_TUN_GATEWAY_ADDRESS
                                   : UM_TUN_CLIENT_ADDRESS;
    const char *command[] = {
        network->ifconfig_path, network->interface_name, "inet",
        local_address, peer_address, "netmask", "255.255.255.252", "mtu",
        mtu, "up", NULL
    };
    char output[768];
    (void)snprintf(mtu, sizeof(mtu), "%u", UM_NETWORK_MTU);
    return mac_run(network, command, output, sizeof(output));
}

static int mac_detect_egress(um_network *network)
{
    const char *command[] = {
        network->route_path, "-n", "get", "default", NULL
    };
    char output[2048];
    char *line;
    int status = mac_run(network, command, output, sizeof(output));
    if (status != UM_OK) {
        return status;
    }
    line = strstr(output, "interface:");
    if (line == NULL ||
        sscanf(line + strlen("interface:"), "%15s",
               network->egress_name) != 1) {
        return UM_ERR_NETWORK;
    }
    return UM_OK;
}

static int mac_set_forwarding(um_network *network)
{
    size_t size = sizeof(network->forwarding_original);
    int enabled = 1;
    if (sysctlbyname("net.inet.ip.forwarding",
                     &network->forwarding_original, &size, NULL, 0u) != 0) {
        return UM_ERR_NETWORK;
    }
    if (network->forwarding_original == 0) {
        if (sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &enabled,
                         sizeof(enabled)) != 0) {
            return UM_ERR_NETWORK;
        }
        network->forwarding_changed = 1;
    }
    network_log(network, "IPv4 forwarding enabled (previous=%d)",
                network->forwarding_original);
    return UM_OK;
}

static int mac_write_rules_file(um_network *network, char path[64])
{
    char rules[512];
    int descriptor;
    int length;
    ssize_t written;
    (void)snprintf(path, 64u, "/tmp/universal-modem-pf.XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0) {
        return UM_ERR_NETWORK;
    }
    length = snprintf(rules, sizeof(rules),
                      "nat on %s inet from %s to any -> (%s)\n"
                      "pass quick on %s inet from %s to any keep state\n"
                      "pass quick on %s inet from %s to any keep state\n",
                      network->egress_name, UM_TUN_SUBNET,
                      network->egress_name, network->interface_name,
                      UM_TUN_SUBNET, network->egress_name, UM_TUN_SUBNET);
    if (length < 0 || (size_t)length >= sizeof(rules)) {
        (void)close(descriptor);
        (void)unlink(path);
        return UM_ERR_CAPACITY;
    }
    do {
        written = write(descriptor, rules, (size_t)length);
    } while (written < 0 && errno == EINTR);
    (void)close(descriptor);
    if (written != (ssize_t)length) {
        (void)unlink(path);
        return UM_ERR_NETWORK;
    }
    return UM_OK;
}

static int mac_configure_pf(um_network *network)
{
    char rules_path[64];
    char output[1024];
    char *token_text;
    char *end = NULL;
    const char *load[] = {
        network->pfctl_path, "-a", network->pf_anchor, "-f", rules_path,
        NULL
    };
    const char *enable[] = {network->pfctl_path, "-E", NULL};
    int status = mac_write_rules_file(network, rules_path);
    if (status != UM_OK) {
        return status;
    }
    status = mac_run(network, load, output, sizeof(output));
    (void)unlink(rules_path);
    if (status != UM_OK) {
        return status;
    }
    network->pf_anchor_loaded = 1;
    status = mac_run(network, enable, output, sizeof(output));
    if (status != UM_OK) {
        return status;
    }
    token_text = strstr(output, "Token");
    if (token_text == NULL || (token_text = strchr(token_text, ':')) == NULL) {
        network_log(network, "pfctl enabled PF but did not return a reference "
                             "token; refusing unsafe ownership");
        return UM_ERR_NETWORK;
    }
    errno = 0;
    network->pf_token = strtoull(token_text + 1, &end, 10);
    if (errno != 0 || end == token_text + 1 || network->pf_token == 0u) {
        return UM_ERR_NETWORK;
    }
    network->pf_reference_held = 1;
    return UM_OK;
}

static int mac_configure_dns(um_network *network)
{
    CFMutableDictionaryRef dictionary = NULL;
    CFStringRef interface = NULL;
    CFStringRef service_id = NULL;
    CFStringRef servers[1] = {CFSTR("1.1.1.1")};
    CFStringRef domains[1] = {CFSTR("")};
    int order_value = 1;
    int timeout_value = 60;
    CFNumberRef order = NULL;
    CFNumberRef timeout = NULL;
    CFArrayRef server_array = NULL;
    CFArrayRef domain_array = NULL;
    CFArrayRef order_array = NULL;
    CFPropertyListRef published = NULL;
    Boolean set = 0;
    network->dynamic_store =
        SCDynamicStoreCreate(NULL, CFSTR("UniversalModem"), NULL, NULL);
    if (network->dynamic_store == NULL) {
        return UM_ERR_NETWORK;
    }
    service_id = CFStringCreateWithFormat(
        NULL, NULL, CFSTR("UniversalModem-%d"), (int)getpid());
    if (service_id != NULL) {
        network->dns_key = SCDynamicStoreKeyCreateNetworkServiceEntity(
            NULL, kSCDynamicStoreDomainState, service_id, kSCEntNetDNS);
    }
    interface = CFStringCreateWithCString(NULL, network->interface_name,
                                          kCFStringEncodingUTF8);
    dictionary = CFDictionaryCreateMutable(
        NULL, 0u, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    server_array = CFArrayCreate(NULL, (const void **)servers, 1u,
                                 &kCFTypeArrayCallBacks);
    domain_array = CFArrayCreate(NULL, (const void **)domains, 1u,
                                 &kCFTypeArrayCallBacks);
    order = CFNumberCreate(NULL, kCFNumberIntType, &order_value);
    timeout = CFNumberCreate(NULL, kCFNumberIntType, &timeout_value);
    if (order != NULL) {
        const void *orders[1] = {order};
        order_array = CFArrayCreate(NULL, orders, 1u,
                                    &kCFTypeArrayCallBacks);
    }
    if (network->dns_key != NULL && interface != NULL && dictionary != NULL &&
        server_array != NULL && domain_array != NULL && order_array != NULL &&
        timeout != NULL) {
        CFDictionarySetValue(dictionary, kSCPropNetDNSServerAddresses,
                             server_array);
        CFDictionarySetValue(dictionary,
                             kSCPropNetDNSSupplementalMatchDomains,
                             domain_array);
        CFDictionarySetValue(dictionary,
                             kSCPropNetDNSSupplementalMatchOrders,
                             order_array);
        CFDictionarySetValue(dictionary, kSCPropInterfaceName, interface);
        CFDictionarySetValue(dictionary, kSCPropNetDNSServerTimeout,
                             timeout);
        set = SCDynamicStoreAddTemporaryValue(network->dynamic_store,
                                              network->dns_key, dictionary);
        if (set) {
            published = SCDynamicStoreCopyValue(network->dynamic_store,
                                                network->dns_key);
            set = published != NULL && CFEqual(published, dictionary);
        }
    }
    if (published != NULL) {
        CFRelease(published);
    }
    if (timeout != NULL) {
        CFRelease(timeout);
    }
    if (order_array != NULL) {
        CFRelease(order_array);
    }
    if (order != NULL) {
        CFRelease(order);
    }
    if (domain_array != NULL) {
        CFRelease(domain_array);
    }
    if (server_array != NULL) {
        CFRelease(server_array);
    }
    if (dictionary != NULL) {
        CFRelease(dictionary);
    }
    if (interface != NULL) {
        CFRelease(interface);
    }
    if (service_id != NULL) {
        CFRelease(service_id);
    }
    if (!set) {
        network_log(network, "Could not publish scoped DNS through "
                             "SystemConfiguration");
        return UM_ERR_NETWORK;
    }
    network_log(network,
                "Published catch-all DNS on %s using %s timeout=%ds; "
                "verify resolver adoption with 'scutil --dns'",
                network->interface_name, UM_DNS_PRIMARY, timeout_value);
    return UM_OK;
}

static int mac_configure_client(um_network *network)
{
    const char *low[] = {
        network->route_path, "-n", "add", "-net", "0.0.0.0/1",
        "-interface", network->interface_name, NULL
    };
    const char *high[] = {
        network->route_path, "-n", "add", "-net", "128.0.0.0/1",
        "-interface", network->interface_name, NULL
    };
    char output[768];
    int status = mac_run(network, low, output, sizeof(output));
    if (status == UM_OK) {
        network->route_low_added = 1;
        status = mac_run(network, high, output, sizeof(output));
    }
    if (status == UM_OK) {
        network->route_high_added = 1;
        status = mac_configure_dns(network);
    }
    return status;
}

static int mac_configure_gateway(um_network *network)
{
    int status = mac_detect_egress(network);
    if (status != UM_OK) {
        network_log(network, "Could not identify the gateway's IPv4 default "
                             "route interface");
        return status;
    }
    status = mac_set_forwarding(network);
    if (status == UM_OK) {
        status = mac_configure_pf(network);
    }
    if (status == UM_OK) {
        network_log(network, "Gateway PF NAT active subnet=%s egress=%s",
                    UM_TUN_SUBNET, network->egress_name);
    }
    return status;
}

static void mac_cleanup(um_network *network)
{
    char output[768];
    if (network->dns_key != NULL && network->dynamic_store != NULL) {
        (void)SCDynamicStoreRemoveValue(network->dynamic_store,
                                        network->dns_key);
    }
    if (network->dns_key != NULL) {
        CFRelease(network->dns_key);
        network->dns_key = NULL;
    }
    if (network->dynamic_store != NULL) {
        CFRelease(network->dynamic_store);
        network->dynamic_store = NULL;
    }
    if (network->role == UM_LIVE_CLIENT && network->route_path != NULL) {
        const char *high[] = {
            network->route_path, "-n", "delete", "-net", "128.0.0.0/1",
            "-interface", network->interface_name, NULL
        };
        const char *low[] = {
            network->route_path, "-n", "delete", "-net", "0.0.0.0/1",
            "-interface", network->interface_name, NULL
        };
        if (network->route_high_added != 0) {
            (void)mac_run(network, high, output, sizeof(output));
            network->route_high_added = 0;
        }
        if (network->route_low_added != 0) {
            (void)mac_run(network, low, output, sizeof(output));
            network->route_low_added = 0;
        }
    }
    if (network->pf_anchor_loaded != 0 && network->pfctl_path != NULL) {
        const char *flush[] = {
            network->pfctl_path, "-a", network->pf_anchor, "-F", "all", NULL
        };
        (void)mac_run(network, flush, output, sizeof(output));
        network->pf_anchor_loaded = 0;
    }
    if (network->pf_reference_held != 0 && network->pfctl_path != NULL) {
        char token[32];
        const char *release[] = {
            network->pfctl_path, "-X", token, NULL
        };
        (void)snprintf(token, sizeof(token), "%llu", network->pf_token);
        (void)mac_run(network, release, output, sizeof(output));
        network->pf_reference_held = 0;
    }
    if (network->forwarding_changed != 0) {
        int original = network->forwarding_original;
        if (sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &original,
                         sizeof(original)) != 0) {
            network_log(network, "Could not restore IPv4 forwarding");
        }
        network->forwarding_changed = 0;
    }
}
#endif

int um_network_open(um_network **network, um_live_role role,
                    um_log_callback logger, void *logger_context)
{
    um_network *opened;
    int status;
    if (network == NULL || logger == NULL ||
        (role != UM_LIVE_CLIENT && role != UM_LIVE_GATEWAY)) {
        return UM_ERR_ARGUMENT;
    }
    *network = NULL;
    opened = (um_network *)calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return UM_ERR_MEMORY;
    }
    opened->fd = -1;
    opened->role = role;
    opened->logger = logger;
    opened->logger_context = logger_context;
#if defined(__linux__)
    if (geteuid() != 0) {
        network_log(opened, "Network proxy mode requires root; restart with "
                            "sudo or use --link-test");
        free(opened);
        return UM_ERR_NETWORK;
    }
    opened->ip_path = find_executable(linux_ip_paths);
    opened->iptables_path = find_executable(linux_iptables_paths);
    opened->nft_path = find_executable(linux_nft_paths);
    opened->resolvectl_path = find_executable(linux_resolvectl_paths);
    if (opened->ip_path == NULL) {
        network_log(opened, "The system ip utility is required");
        free(opened);
        return UM_ERR_NETWORK;
    }
    (void)snprintf(opened->firewall_tag, sizeof(opened->firewall_tag),
                   "universal-modem-%ld", (long)getpid());
    (void)snprintf(opened->nft_table, sizeof(opened->nft_table), "um_%ld",
                   (long)getpid());
    status = linux_open_tun(opened);
    if (status == UM_OK) {
        status = linux_configure_interface(opened);
    }
    if (status == UM_OK) {
        status = role == UM_LIVE_CLIENT ? linux_configure_client(opened)
                                        : linux_configure_gateway(opened);
    }
#elif defined(__APPLE__)
    if (geteuid() != 0) {
        network_log(opened, "Network proxy mode requires root; restart with "
                            "sudo or use --link-test");
        free(opened);
        return UM_ERR_NETWORK;
    }
    opened->ifconfig_path = find_executable(mac_ifconfig_paths);
    opened->route_path = find_executable(mac_route_paths);
    opened->pfctl_path = find_executable(mac_pfctl_paths);
    if (opened->ifconfig_path == NULL || opened->route_path == NULL ||
        (role == UM_LIVE_GATEWAY && opened->pfctl_path == NULL)) {
        network_log(opened, "Required macOS network utilities are missing");
        free(opened);
        return UM_ERR_NETWORK;
    }
    (void)snprintf(opened->pf_anchor, sizeof(opened->pf_anchor),
                   "com.apple/universal-modem-%ld", (long)getpid());
    status = mac_open_utun(opened);
    if (status == UM_OK) {
        status = mac_configure_interface(opened);
    }
    if (status == UM_OK) {
        status = role == UM_LIVE_CLIENT ? mac_configure_client(opened)
                                        : mac_configure_gateway(opened);
    }
#else
    (void)role;
    status = UM_ERR_UNSUPPORTED;
#endif
    if (status != UM_OK) {
        um_network_close(opened);
        return status;
    }
    network_log(opened, "Network interface ready: %s address=%s peer=%s "
                        "mtu=%u",
                opened->interface_name,
                role == UM_LIVE_CLIENT ? UM_TUN_CLIENT_ADDRESS
                                       : UM_TUN_GATEWAY_ADDRESS,
                role == UM_LIVE_CLIENT ? UM_TUN_GATEWAY_ADDRESS
                                       : UM_TUN_CLIENT_ADDRESS,
                UM_NETWORK_MTU);
    *network = opened;
    return UM_OK;
}

void um_network_close(um_network *network)
{
    if (network == NULL) {
        return;
    }
#if defined(__linux__)
    linux_cleanup(network);
#elif defined(__APPLE__)
    mac_cleanup(network);
#endif
    if (network->fd >= 0) {
        (void)close(network->fd);
        network->fd = -1;
    }
    network_log(network, "Network proxy configuration removed");
    free(network);
}

int um_network_read(um_network *network, uint8_t *packet, size_t capacity,
                    unsigned timeout_ms, size_t *packet_length)
{
#if defined(__linux__) || defined(__APPLE__)
    int status;
    ssize_t count;
    if (network == NULL || packet == NULL || capacity == 0u ||
        packet_length == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *packet_length = 0u;
    status = wait_descriptor(network->fd, POLLIN, timeout_ms);
    if (status != UM_OK) {
        return status;
    }
#if defined(__APPLE__)
    {
        uint8_t framed[UM_NETWORK_MAX_PACKET + 4u];
        uint32_t family;
        size_t payload_length;
        do {
            count = read(network->fd, framed, sizeof(framed));
        } while (count < 0 && errno == EINTR);
        if (count < 5) {
            return UM_ERR_NETWORK;
        }
        memcpy(&family, framed, sizeof(family));
        family = ntohl(family);
        payload_length = (size_t)count - sizeof(family);
        if (payload_length > capacity ||
            (family != AF_INET && family != AF_INET6)) {
            return UM_ERR_CAPACITY;
        }
        memcpy(packet, framed + sizeof(family), payload_length);
        *packet_length = payload_length;
    }
#else
    do {
        count = read(network->fd, packet, capacity);
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
        return UM_ERR_NETWORK;
    }
    *packet_length = (size_t)count;
#endif
    return UM_OK;
#else
    (void)network;
    (void)packet;
    (void)capacity;
    (void)timeout_ms;
    (void)packet_length;
    return UM_ERR_UNSUPPORTED;
#endif
}

int um_network_write(um_network *network, const uint8_t *packet,
                     size_t packet_length, unsigned timeout_ms)
{
#if defined(__linux__) || defined(__APPLE__)
    if (network == NULL || packet == NULL || packet_length == 0u ||
        packet_length > UM_NETWORK_MAX_PACKET) {
        return UM_ERR_ARGUMENT;
    }
#if defined(__APPLE__)
    {
        uint8_t framed[UM_NETWORK_MAX_PACKET + 4u];
        uint32_t family;
        unsigned version = packet[0] >> 4u;
        if (version == 4u) {
            family = htonl(AF_INET);
        } else if (version == 6u) {
            family = htonl(AF_INET6);
        } else {
            return UM_ERR_HEADER;
        }
        memcpy(framed, &family, sizeof(family));
        memcpy(framed + sizeof(family), packet, packet_length);
        return write_all(network->fd, framed,
                         packet_length + sizeof(family), timeout_ms);
    }
#else
    return write_all(network->fd, packet, packet_length, timeout_ms);
#endif
#else
    (void)network;
    (void)packet;
    (void)packet_length;
    (void)timeout_ms;
    return UM_ERR_UNSUPPORTED;
#endif
}

const char *um_network_interface_name(const um_network *network)
{
    return network != NULL ? network->interface_name : NULL;
}
