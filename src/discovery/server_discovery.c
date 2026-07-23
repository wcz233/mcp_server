#include "discovery/server_discovery.h"

#include "common/platform.h"
#include "core/server_internal.h"
#include "tools/system_status.h"
#include "tools/tool_result.h"
#include "transport/peer_transport.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif

#define MCP_DISCOVERY_PROTOCOL 1
#define MCP_DISCOVERY_ANNOUNCE_MS 10000u
#define MCP_DISCOVERY_HEARTBEAT_MS 1000u
#define MCP_DISCOVERY_WAIT_MS 300u
#define MCP_DISCOVERY_HEARTBEAT_TIMEOUT_MS 3000ull
#define MCP_DISCOVERY_HEARTBEAT_STALL_MS 2000ull
#define MCP_DISCOVERY_MAX_DATAGRAM 65536u
#define MCP_DISCOVERY_MAX_FRAME (1024u * 1024u)
#define MCP_DISCOVERY_WRITE_HIGH_WATERMARK (4u * 1024u * 1024u)
#define MCP_DISCOVERY_PROXY_TIMEOUT_MS_DEFAULT 5000u
#define MCP_DISCOVERY_PROXY_TIMEOUT_MS_MAX 300000u
#define MCP_DISCOVERY_INITIALIZE_ID "mcp_gateway_initialize"
#define MCP_DISCOVERY_MFT_DATA_CAPABILITY "mft.v1.data_channel"
#define MCP_DISCOVERY_MFT_MAGIC "MFT1"
#define MCP_DISCOVERY_MFT_DATA_FRAME 5u

enum discovery_peer_state {
    DISCOVERY_PEER_ONLINE = 1,
    DISCOVERY_PEER_TIMEOUT = 2,
    DISCOVERY_PEER_OFFLINE = 3,
};

enum discovery_tools_state {
    DISCOVERY_TOOLS_UNKNOWN = 0,
    DISCOVERY_TOOLS_REFRESHING = 1,
    DISCOVERY_TOOLS_READY = 2,
};

struct discovery_peer_conn;
struct discovery_pending_proxy;
struct discovery_tools_refresh;

struct discovery_peer {
    char *id;
    unsigned int server_id;
    char ip[64];
    unsigned int port;
    json_t *status;
    json_t *tools_list;
    unsigned long long last_seen_ms;
    unsigned long long last_heartbeat_ms;
    unsigned long seen_generation;
    unsigned long tools_generation;
    enum discovery_peer_state state;
    enum discovery_tools_state tools_state;
    struct discovery_peer_conn *conn;
    struct discovery_peer_conn *data_conn;
    struct discovery_tools_refresh *tools_refresh;
    struct discovery_peer *next;
};

struct discovery_udp_send {
    uv_udp_send_t req;
    uv_buf_t buf;
    struct sockaddr_storage target;
};

struct discovery_write_req {
    uv_write_t req;
    uv_buf_t buf;
    struct discovery_peer_conn *conn;
    size_t queued_bytes;
    bool close_after_write;
};

struct discovery_peer_conn {
    struct mcp_server_discovery *discovery;
    struct discovery_peer *peer;
    uv_tcp_t tcp;
    uv_connect_t connect_req;
    bool initialized;
    bool connected;
    bool connecting;
    bool closing;
    bool mcp_initialize_sent;
    bool mcp_initialized;
    bool peer_notified;
    bool data_channel;
    char *rx_buf;
    size_t rx_len;
    size_t rx_cap;
    size_t write_queue_bytes;
};

struct discovery_pending_list {
    struct mcp_server_discovery *discovery;
    uv_timer_t timer;
    bool timer_initialized;
    char *id_key;
    unsigned long generation;
    struct discovery_pending_list *next;
};

struct discovery_pending_proxy {
    struct mcp_server_discovery *discovery;
    struct discovery_peer *peer;
    uv_timer_t timer;
    bool timer_initialized;
    bool closing;
    bool sent;
    char *id_key;
    char *remote_id;
    char *tool_name;
    json_t *arguments;
    enum mcp_discovery_proxy_kind kind;
    unsigned int timeout_ms;
    struct discovery_pending_proxy *next;
};

struct discovery_tools_refresh {
    struct mcp_server_discovery *discovery;
    struct discovery_peer *peer;
    struct discovery_peer_conn *conn;
    uv_timer_t timer;
    bool timer_initialized;
    bool closing;
    unsigned long generation;
    char *remote_id;
};

struct mcp_server_discovery {
    struct mcp_server *server;
    uv_loop_t *loop;

    uv_udp_t udp;
    uv_timer_t announce_timer;
    uv_timer_t heartbeat_timer;
    bool udp_initialized;
    bool announce_timer_initialized;
    bool heartbeat_timer_initialized;
    bool opened;
    bool closing;
    bool destroy_on_close;
    unsigned int udp_sends_pending;
    unsigned int pending_proxy_closes;
    unsigned int tools_refresh_closes;
    bool close_udp_after_sends;

    char *bind_host;
    unsigned int discovery_port;
    unsigned int broadcast_port;
    char *tcp_host;
    unsigned int tcp_port;
    char *advertise_host;
    char *explicit_hosts;
    char *instance_id;
    unsigned long current_generation;
    unsigned long long heartbeat_id;
    unsigned long long last_heartbeat_tick_ms;
    unsigned int next_server_id;

    struct discovery_peer *peers;
    struct discovery_pending_list *pending_lists;
    struct discovery_pending_proxy *pending_proxies;
};

static void discovery_send_announce(struct mcp_server_discovery *discovery, bool reply);
static void discovery_send_offline(struct mcp_server_discovery *discovery);
static void discovery_maybe_release(struct mcp_server_discovery *discovery);
static void udp_close_cb(uv_handle_t *handle);
static int pending_proxy_send(struct discovery_pending_proxy *ctx);
static void peer_send_pending_proxies(struct discovery_peer_conn *conn);
static int peer_send_initialize(struct discovery_peer_conn *conn);
static void peer_invalidate_tools_cache(struct discovery_peer *peer);
static void pending_proxy_complete_for_peer(struct mcp_server_discovery *discovery,
                                            struct discovery_peer *peer,
                                            const char *message);
static bool tools_refresh_complete_response(struct discovery_peer_conn *conn, json_t *root);
static bool pending_proxy_complete_response(struct discovery_peer_conn *conn, json_t *root);
static bool peer_handle_initialize_response(struct discovery_peer_conn *conn, json_t *root);
static int discovery_external_send_frame(void *arg,
                                         unsigned int server_id,
                                         const void *payload,
                                         size_t len);

static uint32_t read_u32_be(const char *data)
{
    const unsigned char *bytes = (const unsigned char *)data;

    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void write_u32_be(char *data, uint32_t value)
{
    data[0] = (char)((value >> 24) & 0xffu);
    data[1] = (char)((value >> 16) & 0xffu);
    data[2] = (char)((value >> 8) & 0xffu);
    data[3] = (char)(value & 0xffu);
}

static int dup_string(char **dst, const char *value)
{
    char *copy = NULL;

    if (value && value[0] != '\0') {
        copy = mcp_strdup(value);
        if (!copy)
            return -1;
    }

    free(*dst);
    *dst = copy;
    return 0;
}

static bool make_peer_id(const char *ip, unsigned int port, char **out)
{
    int len;
    char *id;

    *out = NULL;
    len = snprintf(NULL, 0, "%s:%u", ip, port);
    if (len < 0)
        return false;

    id = malloc((size_t)len + 1);
    if (!id)
        return false;

    snprintf(id, (size_t)len + 1, "%s:%u", ip, port);
    *out = id;
    return true;
}

static bool copy_host(char *out, size_t out_len, const char *host)
{
    int len;

    if (!out || out_len == 0 || !host || host[0] == '\0')
        return false;

    len = snprintf(out, out_len, "%s", host);
    return len >= 0 && (size_t)len < out_len;
}

static bool host_is_unspecified(const char *host)
{
    return !host ||
           host[0] == '\0' ||
           strcmp(host, "0.0.0.0") == 0 ||
           strcmp(host, "::") == 0;
}

static bool select_local_interface_ip(char *out, size_t out_len)
{
    uv_interface_address_t *interfaces = NULL;
    char fallback[64] = {0};
    bool found = false;
    int count = 0;
    int i;

    if (!out || out_len == 0)
        return false;
    if (uv_interface_addresses(&interfaces, &count) != 0)
        return false;

    for (i = 0; i < count; i++) {
        char ip[64];

        if (interfaces[i].address.address4.sin_family != AF_INET)
            continue;
        if (uv_ip4_name(&interfaces[i].address.address4, ip, sizeof(ip)) != 0)
            continue;
        if (!fallback[0])
            snprintf(fallback, sizeof(fallback), "%s", ip);
        if (interfaces[i].is_internal || strcmp(ip, "0.0.0.0") == 0)
            continue;

        found = copy_host(out, out_len, ip);
        break;
    }

    if (!found && fallback[0])
        found = copy_host(out, out_len, fallback);

    uv_free_interface_addresses(interfaces, count);
    return found;
}

static bool discovery_local_ip(const struct mcp_server_discovery *discovery,
                               char *out,
                               size_t out_len)
{
    if (!discovery)
        return false;
    if (copy_host(out, out_len, discovery->advertise_host))
        return true;
    if (!host_is_unspecified(discovery->tcp_host) &&
        copy_host(out, out_len, discovery->tcp_host))
        return true;
    if (select_local_interface_ip(out, out_len))
        return true;
    return copy_host(out, out_len, "127.0.0.1");
}

static char *make_instance_id(void)
{
    unsigned long long now = mcp_now_ms();
    int len;
    char *id;

#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif

    len = snprintf(NULL, 0, "mcp-%lu-%llu-%p", pid, now, (void *)&now);
    if (len < 0)
        return NULL;

    id = malloc((size_t)len + 1);
    if (!id)
        return NULL;

    snprintf(id, (size_t)len + 1, "mcp-%lu-%llu-%p", pid, now, (void *)&now);
    return id;
}

static bool sockaddr_to_ip(const struct sockaddr *addr, char *out, size_t out_len)
{
    if (addr->sa_family == AF_INET)
        return uv_ip4_name((const struct sockaddr_in *)addr, out, out_len) == 0;
    if (addr->sa_family == AF_INET6)
        return uv_ip6_name((const struct sockaddr_in6 *)addr, out, out_len) == 0;
    return false;
}

static int sockaddr_from_host_port(const char *host,
                                   unsigned int port,
                                   struct sockaddr_storage *out)
{
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;

    if (!host || port > 65535)
        return -1;

    memset(out, 0, sizeof(*out));
    if (uv_ip4_addr(host, (int)port, &addr4) == 0) {
        memcpy(out, &addr4, sizeof(addr4));
        return 0;
    }
    if (uv_ip6_addr(host, (int)port, &addr6) == 0) {
        memcpy(out, &addr6, sizeof(addr6));
        return 0;
    }

    return -1;
}

static size_t discovery_sockaddr_size(const struct sockaddr_storage *addr)
{
    if (addr->ss_family == AF_INET)
        return sizeof(struct sockaddr_in);
    if (addr->ss_family == AF_INET6)
        return sizeof(struct sockaddr_in6);
    return 0;
}

static char *discovery_strtok(char *str, const char *delim, char **save)
{
#ifdef _WIN32
    return strtok_s(str, delim, save);
#else
    return strtok_r(str, delim, save);
#endif
}

static unsigned int env_uint_range(const char *name,
                                   unsigned int default_value,
                                   unsigned int min_value,
                                   unsigned int max_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (!value || value[0] == '\0')
        return default_value;

    parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed < min_value || parsed > max_value)
        return default_value;

    return (unsigned int)parsed;
}

static unsigned int discovery_proxy_timeout_default_ms(void)
{
    return env_uint_range("MCP_DISCOVERY_PROXY_TIMEOUT_MS",
                          MCP_DISCOVERY_PROXY_TIMEOUT_MS_DEFAULT,
                          1u,
                          MCP_DISCOVERY_PROXY_TIMEOUT_MS_MAX);
}

static void udp_send_cb(uv_udp_send_t *req, int status)
{
    struct discovery_udp_send *send_req = (struct discovery_udp_send *)req;
    struct mcp_server_discovery *discovery = req->handle->data;

    (void)status;
    free(send_req->buf.base);
    free(send_req);

    if (discovery && discovery->udp_sends_pending > 0)
        discovery->udp_sends_pending--;
    if (discovery &&
        discovery->close_udp_after_sends &&
        discovery->udp_sends_pending == 0 &&
        discovery->udp_initialized &&
        !uv_is_closing((uv_handle_t *)&discovery->udp)) {
        uv_udp_recv_stop(&discovery->udp);
        uv_close((uv_handle_t *)&discovery->udp, udp_close_cb);
    }
}

static int discovery_udp_send_json(struct mcp_server_discovery *discovery,
                                   json_t *packet,
                                   const struct sockaddr_storage *target)
{
    struct discovery_udp_send *send_req;
    char *dumped;
    size_t len;
    size_t target_len;
    int rc;

    if (!discovery || !packet || !target || !discovery->opened)
        return -1;

    target_len = discovery_sockaddr_size(target);
    if (target_len == 0)
        return -1;

    dumped = json_dumps(packet, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!dumped)
        return -1;

    len = strlen(dumped);
    send_req = calloc(1, sizeof(*send_req));
    if (!send_req) {
        free(dumped);
        return -1;
    }

    send_req->buf.base = dumped;
#ifdef _WIN32
    send_req->buf.len = (unsigned long)len;
#else
    send_req->buf.len = len;
#endif
    memcpy(&send_req->target, target, target_len);

    rc = uv_udp_send(&send_req->req,
                     &discovery->udp,
                     &send_req->buf,
                     1,
                     (const struct sockaddr *)&send_req->target,
                     udp_send_cb);
    if (rc != 0) {
        free(send_req->buf.base);
        free(send_req);
        return -1;
    }

    discovery->udp_sends_pending++;
    return 0;
}

static json_t *build_discovery_packet(struct mcp_server_discovery *discovery,
                                      bool reply,
                                      const char *event)
{
    json_t *packet = json_object();
    json_t *status;
    char ip[64];

    if (!packet)
        return NULL;

    status = mcp_system_status_json();
    if (!status)
        status = json_object();

    json_object_set_new(packet, "mcp_server_discovery", json_integer(MCP_DISCOVERY_PROTOCOL));
    json_object_set_new(packet, "instance_id", json_string(discovery->instance_id));
    json_object_set_new(packet, "tcp_port", json_integer((json_int_t)discovery->tcp_port));
    json_object_set_new(packet, "reply", json_boolean(reply));
    json_object_set_new(packet, "event", json_string(event ? event : "online"));
    if (discovery->advertise_host)
        json_object_set_new(packet, "advertise_host", json_string(discovery->advertise_host));
    else if (discovery_local_ip(discovery, ip, sizeof(ip)))
        json_object_set_new(packet, "advertise_host", json_string(ip));
    json_object_set_new(packet, "status", status);
    return packet;
}

static void send_packet_to_host(struct mcp_server_discovery *discovery,
                                json_t *packet,
                                const char *host,
                                unsigned int port)
{
    struct sockaddr_storage target;

    if (sockaddr_from_host_port(host, port, &target) != 0)
        return;

    discovery_udp_send_json(discovery, packet, &target);
}

static void send_packet_to_sockaddr(struct mcp_server_discovery *discovery,
                                    json_t *packet,
                                    const struct sockaddr *addr)
{
    struct sockaddr_storage target;
    size_t len;

    if (addr->sa_family == AF_INET)
        len = sizeof(struct sockaddr_in);
    else if (addr->sa_family == AF_INET6)
        len = sizeof(struct sockaddr_in6);
    else
        return;

    memset(&target, 0, sizeof(target));
    memcpy(&target, addr, len);
    discovery_udp_send_json(discovery, packet, &target);
}

static void send_explicit_hosts(struct mcp_server_discovery *discovery, json_t *packet)
{
    char *hosts;
    char *entry;
    char *save = NULL;

    if (!discovery->explicit_hosts || discovery->explicit_hosts[0] == '\0')
        return;

    hosts = mcp_strdup(discovery->explicit_hosts);
    if (!hosts)
        return;

    for (entry = discovery_strtok(hosts, ",", &save);
         entry;
         entry = discovery_strtok(NULL, ",", &save)) {
        char *colon;
        char *end = NULL;
        unsigned long port = discovery->broadcast_port;

        while (*entry == ' ' || *entry == '\t')
            entry++;
        if (*entry == '\0')
            continue;

        colon = strrchr(entry, ':');
        if (colon && colon[1] != '\0') {
            *colon = '\0';
            port = strtoul(colon + 1, &end, 10);
            if (!end || *end != '\0' || port > 65535ul)
                port = discovery->broadcast_port;
        }

        send_packet_to_host(discovery, packet, entry, (unsigned int)port);
    }

    free(hosts);
}

static void send_interface_broadcasts(struct mcp_server_discovery *discovery, json_t *packet)
{
    uv_interface_address_t *interfaces = NULL;
    int count = 0;
    int i;

    send_packet_to_host(discovery, packet, "255.255.255.255", discovery->broadcast_port);

    if (uv_interface_addresses(&interfaces, &count) != 0)
        return;

    for (i = 0; i < count; i++) {
        struct sockaddr_in *addr;
        struct sockaddr_in *mask;
        uint32_t ip;
        uint32_t netmask;
        uint32_t broadcast;
        struct sockaddr_storage target;
        struct sockaddr_in *target4 = (struct sockaddr_in *)&target;

        if (interfaces[i].is_internal)
            continue;
        if (interfaces[i].address.address4.sin_family != AF_INET ||
            interfaces[i].netmask.netmask4.sin_family != AF_INET)
            continue;

        addr = &interfaces[i].address.address4;
        mask = &interfaces[i].netmask.netmask4;
        ip = ntohl(addr->sin_addr.s_addr);
        netmask = ntohl(mask->sin_addr.s_addr);
        broadcast = (ip & netmask) | (~netmask);
        if (broadcast == ip || broadcast == UINT32_MAX)
            continue;

        memset(&target, 0, sizeof(target));
        target4->sin_family = AF_INET;
        target4->sin_addr.s_addr = htonl(broadcast);
        target4->sin_port = htons((uint16_t)discovery->broadcast_port);
        discovery_udp_send_json(discovery, packet, &target);
    }

    uv_free_interface_addresses(interfaces, count);
}

static void discovery_send_announce(struct mcp_server_discovery *discovery, bool reply)
{
    json_t *packet;

    if (!discovery || !discovery->opened || discovery->closing)
        return;

    packet = build_discovery_packet(discovery, reply, "online");
    if (!packet)
        return;

    send_interface_broadcasts(discovery, packet);
    send_explicit_hosts(discovery, packet);
    json_decref(packet);
}

static void discovery_send_offline(struct mcp_server_discovery *discovery)
{
    json_t *packet;

    if (!discovery || !discovery->udp_initialized)
        return;

    packet = build_discovery_packet(discovery, false, "offline");
    if (!packet)
        return;

    send_interface_broadcasts(discovery, packet);
    send_explicit_hosts(discovery, packet);
    json_decref(packet);
}

static void announce_timer_cb(uv_timer_t *timer)
{
    struct mcp_server_discovery *discovery = timer->data;

    discovery_send_announce(discovery, false);
}

static void peer_conn_close(struct discovery_peer_conn *conn);

static const char *peer_state_name(enum discovery_peer_state state)
{
    switch (state) {
    case DISCOVERY_PEER_ONLINE:
        return "online";
    case DISCOVERY_PEER_TIMEOUT:
        return "timeout";
    case DISCOVERY_PEER_OFFLINE:
        return "offline";
    }
    return "timeout";
}

static void peer_mark_timeout(struct discovery_peer *peer)
{
    if (!peer || peer->state == DISCOVERY_PEER_OFFLINE)
        return;

    peer->state = DISCOVERY_PEER_TIMEOUT;
}

static void peer_mark_heartbeat_ok(struct discovery_peer *peer)
{
    unsigned long long now;

    if (!peer || peer->state == DISCOVERY_PEER_OFFLINE)
        return;

    now = mcp_now_ms();
    peer->last_heartbeat_ms = now;
    peer->last_seen_ms = now;
    peer->state = DISCOVERY_PEER_ONLINE;
}

static void peer_mark_activity(struct discovery_peer_conn *conn)
{
    if (!conn || conn->data_channel)
        return;
    peer_mark_heartbeat_ok(conn->peer);
}

static void peer_conn_fail(struct discovery_peer_conn *conn)
{
    if (!conn)
        return;

    if (conn->data_channel) {
        peer_conn_close(conn);
        return;
    }

    peer_mark_timeout(conn->peer);
    pending_proxy_complete_for_peer(conn->discovery,
                                    conn->peer,
                                    "Remote server connection failed.");
    peer_conn_close(conn->peer ? conn->peer->data_conn : NULL);
    peer_conn_close(conn);
}

static void discovery_mark_stale_peers(struct mcp_server_discovery *discovery,
                                       unsigned long long now)
{
    struct discovery_peer *peer;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->state == DISCOVERY_PEER_ONLINE &&
            peer->last_heartbeat_ms > 0 &&
            now - peer->last_heartbeat_ms > MCP_DISCOVERY_HEARTBEAT_TIMEOUT_MS) {
            peer->state = DISCOVERY_PEER_TIMEOUT;
            peer_conn_close(peer->data_conn);
            peer_conn_close(peer->conn);
        }
    }
}

static void discovery_rebase_peer_heartbeats(struct mcp_server_discovery *discovery,
                                             unsigned long long now)
{
    struct discovery_peer *peer;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->state == DISCOVERY_PEER_ONLINE && peer->conn && peer->conn->connected)
            peer->last_heartbeat_ms = now;
    }
}

static void peer_conn_free(struct discovery_peer_conn *conn)
{
    if (!conn)
        return;

    if (conn->peer) {
        if (conn->data_channel && conn->peer->data_conn == conn)
            conn->peer->data_conn = NULL;
        else if (!conn->data_channel && conn->peer->conn == conn)
            conn->peer->conn = NULL;
    }
    free(conn->rx_buf);
    free(conn);
}

static void peer_conn_close_cb(uv_handle_t *handle)
{
    struct discovery_peer_conn *conn = handle->data;
    struct mcp_server_discovery *discovery = conn ? conn->discovery : NULL;

    if (conn && !conn->data_channel && conn->peer)
        peer_conn_close(conn->peer->data_conn);
    if (discovery && conn && conn->peer && !conn->data_channel)
        mcp_peer_transport_notify_closed(discovery->server->peer_transport,
                                         conn->peer->server_id);
    peer_conn_free(conn);
    discovery_maybe_release(discovery);
}

static void peer_conn_close(struct discovery_peer_conn *conn)
{
    if (!conn || conn->closing)
        return;

    if (!conn->data_channel) {
        peer_invalidate_tools_cache(conn->peer);
        pending_proxy_complete_for_peer(conn->discovery,
                                        conn->peer,
                                        "Remote server connection closed.");
    }
    conn->closing = true;
    conn->connected = false;
    conn->connecting = false;
    if (conn->initialized && !uv_is_closing((uv_handle_t *)&conn->tcp))
        uv_close((uv_handle_t *)&conn->tcp, peer_conn_close_cb);
    else
        peer_conn_free(conn);
}

static int peer_conn_rx_reserve(struct discovery_peer_conn *conn, size_t want)
{
    size_t need = conn->rx_len + want;
    size_t cap = conn->rx_cap;
    char *next;

    if (need <= cap)
        return 0;

    if (cap == 0)
        cap = 4096;
    while (cap < need)
        cap *= 2;

    next = realloc(conn->rx_buf, cap);
    if (!next)
        return -1;

    conn->rx_buf = next;
    conn->rx_cap = cap;
    return 0;
}

static void peer_conn_rx_consume(struct discovery_peer_conn *conn, size_t count)
{
    if (count >= conn->rx_len) {
        conn->rx_len = 0;
        return;
    }

    memmove(conn->rx_buf, conn->rx_buf + count, conn->rx_len - count);
    conn->rx_len -= count;
}

static void peer_conn_handle_frame(struct discovery_peer_conn *conn, const char *data, size_t len)
{
    json_error_t error;
    json_t *root;
    json_t *method;
    json_t *result;
    json_t *params;

    if (len == 0 || data[0] != '{') {
        if (mcp_peer_transport_dispatch_frame(conn->discovery->server->peer_transport,
                                              conn->peer->server_id,
                                              data,
                                              len) != 0)
            peer_conn_fail(conn);
        else
            peer_mark_activity(conn);
        return;
    }

    root = json_loadb(data, len, JSON_REJECT_DUPLICATES, &error);
    if (!root)
        return;

    method = json_object_get(root, "method");
    result = json_object_get(root, "result");
    params = json_object_get(root, "params");

    if (peer_handle_initialize_response(conn, root)) {
        /* Handled by gateway client session setup. */
    } else if (!conn->data_channel && json_is_string(method) &&
        strcmp(json_string_value(method), MCP_SERVER_DISCOVERY_OFFLINE_METHOD) == 0) {
        mcp_server_discovery_handle_offline_notification(conn->discovery, params);
    } else if (!conn->data_channel && tools_refresh_complete_response(conn, root)) {
        /* Handled by private remote tools refresh. */
    } else if (!conn->data_channel && pending_proxy_complete_response(conn, root)) {
        /* Handled by gateway proxy completion. */
    } else if (!conn->data_channel && (result || json_object_get(root, "error"))) {
        peer_mark_activity(conn);
    }

    json_decref(root);
}

static void peer_conn_process_rx(struct discovery_peer_conn *conn)
{
    while (conn->rx_len >= 4) {
        uint32_t frame_len = read_u32_be(conn->rx_buf);
        size_t total_len;

        if (frame_len == 0 || frame_len > MCP_DISCOVERY_MAX_FRAME) {
            peer_conn_fail(conn);
            return;
        }

        total_len = (size_t)frame_len + 4;
        if (conn->rx_len < total_len)
            return;

        peer_conn_handle_frame(conn, conn->rx_buf + 4, (size_t)frame_len);
        peer_conn_rx_consume(conn, total_len);
    }
}

static void peer_conn_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;

    buf->base = malloc(4096);
    buf->len = buf->base ? 4096 : 0;
}

static void peer_conn_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct discovery_peer_conn *conn = stream->data;

    if (nread < 0) {
        free(buf->base);
        peer_conn_fail(conn);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    if (peer_conn_rx_reserve(conn, (size_t)nread) != 0) {
        free(buf->base);
        peer_conn_fail(conn);
        return;
    }

    memcpy(conn->rx_buf + conn->rx_len, buf->base, (size_t)nread);
    conn->rx_len += (size_t)nread;
    free(buf->base);
    peer_conn_process_rx(conn);
}

static void peer_connect_cb(uv_connect_t *req, int status)
{
    struct discovery_peer_conn *conn = req->data;

    conn->connecting = false;
    if (status != 0 || conn->closing) {
        peer_conn_fail(conn);
        return;
    }

    conn->connected = true;
    if (!conn->data_channel)
        peer_mark_heartbeat_ok(conn->peer);
    if (uv_read_start((uv_stream_t *)&conn->tcp, peer_conn_alloc_cb, peer_conn_read_cb) != 0)
        peer_conn_fail(conn);
    else if (peer_send_initialize(conn) != 0)
        peer_conn_fail(conn);
}

static void peer_connect(struct mcp_server_discovery *discovery,
                         struct discovery_peer *peer,
                         bool data_channel)
{
    struct discovery_peer_conn *conn;
    struct discovery_peer_conn **slot;
    struct sockaddr_storage addr;

    slot = data_channel ? &peer->data_conn : &peer->conn;
    if (peer->state == DISCOVERY_PEER_OFFLINE || *slot || discovery->closing)
        return;
    if (sockaddr_from_host_port(peer->ip, peer->port, &addr) != 0)
        return;

    conn = calloc(1, sizeof(*conn));
    if (!conn)
        return;

    conn->discovery = discovery;
    conn->peer = peer;
    conn->data_channel = data_channel;
    if (uv_tcp_init(discovery->loop, &conn->tcp) != 0) {
        free(conn);
        return;
    }

    conn->initialized = true;
    conn->connecting = true;
    conn->tcp.data = conn;
    conn->connect_req.data = conn;
    *slot = conn;

    if (uv_tcp_connect(&conn->connect_req,
                       &conn->tcp,
                       (const struct sockaddr *)&addr,
                       peer_connect_cb) != 0)
        peer_conn_fail(conn);
}

static void peer_write_cb(uv_write_t *req, int status)
{
    struct discovery_write_req *write_req = (struct discovery_write_req *)req;
    struct discovery_peer_conn *conn = write_req->conn ? write_req->conn : req->handle->data;
    bool close_after_write = write_req->close_after_write;

    if (conn && conn->write_queue_bytes >= write_req->queued_bytes)
        conn->write_queue_bytes -= write_req->queued_bytes;
    free(write_req->buf.base);
    free(write_req);

    if (status < 0)
        peer_conn_fail(conn);
    else if (close_after_write)
        peer_conn_close(conn);
}

static int peer_send_frame(struct discovery_peer_conn *conn,
                           const char *data,
                           size_t len,
                           bool close_after_write)
{
    struct discovery_write_req *write_req;

    if (!conn || !conn->connected || conn->closing || !data || len == 0 || len > UINT32_MAX)
        return -1;
    if (conn->write_queue_bytes + len + 4 > MCP_DISCOVERY_WRITE_HIGH_WATERMARK)
        return -2;

    write_req = calloc(1, sizeof(*write_req));
    if (!write_req)
        return -1;

    write_req->buf.base = malloc(len + 4);
    if (!write_req->buf.base) {
        free(write_req);
        return -1;
    }
    write_req->conn = conn;
    write_req->queued_bytes = len + 4;
    write_req->close_after_write = close_after_write;

    write_u32_be(write_req->buf.base, (uint32_t)len);
    memcpy(write_req->buf.base + 4, data, len);
#ifdef _WIN32
    write_req->buf.len = (unsigned long)(len + 4);
#else
    write_req->buf.len = len + 4;
#endif

    conn->write_queue_bytes += write_req->queued_bytes;
    if (uv_write(&write_req->req,
                 (uv_stream_t *)&conn->tcp,
                 &write_req->buf,
                 1,
                 peer_write_cb) != 0) {
        conn->write_queue_bytes -= write_req->queued_bytes;
        free(write_req->buf.base);
        free(write_req);
        return -1;
    }

    return 0;
}

static int discovery_external_send_frame(void *arg,
                                         unsigned int server_id,
                                         const void *payload,
                                         size_t len)
{
    struct mcp_server_discovery *discovery = arg;
    struct discovery_peer *peer;

    if (!discovery || server_id == 0 || !payload || len == 0)
        return -1;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->server_id != server_id)
            continue;
        if (len >= 6 &&
            memcmp(payload, MCP_DISCOVERY_MFT_MAGIC, 4) == 0 &&
            ((const unsigned char *)payload)[5] == MCP_DISCOVERY_MFT_DATA_FRAME &&
            mcp_peer_transport_has_capability(discovery->server->peer_transport,
                                              server_id,
                                              MCP_DISCOVERY_MFT_DATA_CAPABILITY)) {
            if (!peer->data_conn) {
                peer_connect(discovery, peer, true);
                return -2;
            }
            if (!peer->data_conn->mcp_initialized)
                return -2;
            return peer_send_frame(peer->data_conn, payload, len, false);
        }
        return peer_send_frame(peer->conn, payload, len, false);
    }

    return -1;
}

static void pending_proxy_unlink(struct discovery_pending_proxy *ctx)
{
    struct discovery_pending_proxy **current = &ctx->discovery->pending_proxies;

    while (*current) {
        if (*current == ctx) {
            *current = ctx->next;
            ctx->next = NULL;
            return;
        }
        current = &(*current)->next;
    }
}

static void pending_proxy_free(struct discovery_pending_proxy *ctx)
{
    if (!ctx)
        return;

    free(ctx->id_key);
    free(ctx->remote_id);
    free(ctx->tool_name);
    json_decref(ctx->arguments);
    free(ctx);
}

static void pending_proxy_close_cb(uv_handle_t *handle)
{
    struct discovery_pending_proxy *ctx = handle->data;
    struct mcp_server_discovery *discovery = ctx->discovery;

    ctx->timer_initialized = false;
    if (discovery->pending_proxy_closes > 0)
        discovery->pending_proxy_closes--;
    pending_proxy_free(ctx);
    discovery_maybe_release(discovery);
}

static void pending_proxy_close(struct discovery_pending_proxy *ctx)
{
    if (!ctx)
        return;
    if (ctx->closing)
        return;

    if (ctx->timer_initialized && !uv_is_closing((uv_handle_t *)&ctx->timer)) {
        ctx->closing = true;
        uv_timer_stop(&ctx->timer);
        ctx->discovery->pending_proxy_closes++;
        uv_close((uv_handle_t *)&ctx->timer, pending_proxy_close_cb);
        return;
    }

    pending_proxy_free(ctx);
}

static void pending_proxy_finish_result(struct discovery_pending_proxy *ctx, json_t *result)
{
    pending_proxy_unlink(ctx);
    mcp_server_complete_async_ok(ctx->discovery->server, ctx->id_key, result);
    pending_proxy_close(ctx);
}

static json_t *proxy_error_result(const char *message)
{
    return mcp_tool_result_text(message ? message : "Remote proxy call failed.", true);
}

static void pending_proxy_timeout_cb(uv_timer_t *timer)
{
    struct discovery_pending_proxy *ctx = timer->data;
    char message[160];
    json_t *result;

    snprintf(message,
             sizeof(message),
             "gateway_proxy_timed_out: remote proxy call exceeded %u ms.",
             ctx ? ctx->timeout_ms : 0u);
    result = proxy_error_result(message);

    pending_proxy_finish_result(ctx, result);
    json_decref(result);
}

static bool peer_store_tools_list_from_result(struct discovery_peer *peer, json_t *result)
{
    json_t *tools_list;

    if (!peer ||
        !json_is_object(result) ||
        !json_is_array(json_object_get(result, "tools")))
        return false;

    tools_list = json_deep_copy(result);
    if (!tools_list)
        return false;

    json_decref(peer->tools_list);
    peer->tools_list = tools_list;
    return true;
}

static bool tools_list_contains_tool(json_t *tools_list, const char *tool_name)
{
    json_t *tools;
    json_t *tool;
    size_t index;

    if (!json_is_object(tools_list) || !tool_name)
        return false;

    tools = json_object_get(tools_list, "tools");
    if (!json_is_array(tools))
        return false;

    json_array_foreach(tools, index, tool) {
        json_t *name = json_object_get(tool, "name");
        if (json_is_string(name) && strcmp(json_string_value(name), tool_name) == 0)
            return true;
    }

    return false;
}

static int pending_proxy_send(struct discovery_pending_proxy *ctx)
{
    json_t *request;
    json_t *params;
    char *dumped;
    int rc;

    if (!ctx || !ctx->peer || !ctx->peer->conn || !ctx->peer->conn->connected)
        return -1;
    if (ctx->sent)
        return 0;

    request = json_object();
    if (!request)
        return -1;

    json_object_set_new(request, "jsonrpc", json_string("2.0"));
    json_object_set_new(request, "id", json_string(ctx->remote_id));
    if (ctx->kind == MCP_DISCOVERY_PROXY_TOOLS_LIST) {
        json_object_set_new(request, "method", json_string("tools/list"));
        json_object_set(request, "params", ctx->arguments);
    } else {
        params = json_object();
        if (!params) {
            json_decref(request);
            return -1;
        }
        json_object_set_new(request, "method", json_string("tools/call"));
        json_object_set_new(params, "name", json_string(ctx->tool_name));
        json_object_set(params, "arguments", ctx->arguments);
        json_object_set_new(request, "params", params);
    }

    dumped = json_dumps(request, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(request);
    if (!dumped)
        return -1;

    rc = peer_send_frame(ctx->peer->conn, dumped, strlen(dumped), false);
    free(dumped);
    if (rc == 0)
        ctx->sent = true;
    return rc;
}

static void pending_proxy_complete_waiting_for_tools(struct discovery_peer *peer,
                                                     const char *message)
{
    struct discovery_pending_proxy *ctx;

    if (!peer || !peer->conn)
        return;

    ctx = peer->conn->discovery->pending_proxies;
    while (ctx) {
        struct discovery_pending_proxy *next = ctx->next;

        if (ctx->peer == peer && !ctx->sent) {
            json_t *result = proxy_error_result(message);
            pending_proxy_finish_result(ctx, result);
            json_decref(result);
        }
        ctx = next;
    }
}

static void tools_refresh_free(struct discovery_tools_refresh *ctx)
{
    if (!ctx)
        return;

    free(ctx->remote_id);
    free(ctx);
}

static void tools_refresh_close_cb(uv_handle_t *handle)
{
    struct discovery_tools_refresh *ctx = handle->data;
    struct mcp_server_discovery *discovery = ctx->discovery;

    ctx->timer_initialized = false;
    if (discovery->tools_refresh_closes > 0)
        discovery->tools_refresh_closes--;
    tools_refresh_free(ctx);
    discovery_maybe_release(discovery);
}

static void tools_refresh_close(struct discovery_tools_refresh *ctx)
{
    if (!ctx || ctx->closing)
        return;

    if (ctx->timer_initialized && !uv_is_closing((uv_handle_t *)&ctx->timer)) {
        ctx->closing = true;
        uv_timer_stop(&ctx->timer);
        ctx->discovery->tools_refresh_closes++;
        uv_close((uv_handle_t *)&ctx->timer, tools_refresh_close_cb);
        return;
    }

    tools_refresh_free(ctx);
}

static void peer_invalidate_tools_cache(struct discovery_peer *peer)
{
    struct discovery_tools_refresh *refresh;

    if (!peer)
        return;

    refresh = peer->tools_refresh;
    peer->tools_refresh = NULL;
    peer->tools_state = DISCOVERY_TOOLS_UNKNOWN;
    peer->tools_generation++;
    json_decref(peer->tools_list);
    peer->tools_list = NULL;
    tools_refresh_close(refresh);
}

static void tools_refresh_fail(struct discovery_tools_refresh *ctx, const char *message)
{
    struct discovery_peer *peer;

    if (!ctx)
        return;

    peer = ctx->peer;
    if (peer && peer->tools_refresh == ctx) {
        peer->tools_refresh = NULL;
        peer->tools_state = DISCOVERY_TOOLS_UNKNOWN;
        peer->tools_generation++;
        json_decref(peer->tools_list);
        peer->tools_list = NULL;
        pending_proxy_complete_waiting_for_tools(peer, message);
    }
    tools_refresh_close(ctx);
}

static void tools_refresh_timeout_cb(uv_timer_t *timer)
{
    struct discovery_tools_refresh *ctx = timer->data;

    tools_refresh_fail(ctx, "Remote tools discovery timed out.");
}

static int tools_refresh_send(struct discovery_tools_refresh *ctx)
{
    json_t *request;
    char *dumped;
    int rc;

    if (!ctx || !ctx->conn || !ctx->conn->connected || ctx->conn->closing)
        return -1;

    request = json_object();
    if (!request)
        return -1;

    json_object_set_new(request, "jsonrpc", json_string("2.0"));
    json_object_set_new(request, "id", json_string(ctx->remote_id));
    json_object_set_new(request, "method", json_string("tools/list"));
    json_object_set_new(request, "params", json_object());

    dumped = json_dumps(request, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(request);
    if (!dumped)
        return -1;

    rc = peer_send_frame(ctx->conn, dumped, strlen(dumped), false);
    free(dumped);
    return rc;
}

static int peer_start_tools_refresh(struct discovery_peer_conn *conn)
{
    struct discovery_tools_refresh *ctx;
    struct discovery_peer *peer;
    int len;

    if (!conn || !conn->connected || !conn->mcp_initialized || conn->closing || conn->data_channel)
        return -1;

    peer = conn->peer;
    if (peer->tools_state == DISCOVERY_TOOLS_REFRESHING)
        return 0;
    if (peer->tools_state != DISCOVERY_TOOLS_UNKNOWN)
        return -1;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    peer->tools_generation++;
    len = snprintf(NULL,
                   0,
                   "gateway:tools:%u:%lu",
                   peer->server_id,
                   peer->tools_generation);
    if (len < 0) {
        tools_refresh_free(ctx);
        return -1;
    }

    ctx->remote_id = malloc((size_t)len + 1);
    if (!ctx->remote_id) {
        tools_refresh_free(ctx);
        return -1;
    }
    snprintf(ctx->remote_id,
             (size_t)len + 1,
             "gateway:tools:%u:%lu",
             peer->server_id,
             peer->tools_generation);
    ctx->discovery = conn->discovery;
    ctx->peer = peer;
    ctx->conn = conn;
    ctx->generation = peer->tools_generation;
    if (uv_timer_init(conn->discovery->loop, &ctx->timer) != 0) {
        tools_refresh_free(ctx);
        return -1;
    }

    ctx->timer_initialized = true;
    ctx->timer.data = ctx;
    peer->tools_refresh = ctx;
    peer->tools_state = DISCOVERY_TOOLS_REFRESHING;
    uv_timer_start(&ctx->timer,
                   tools_refresh_timeout_cb,
                   discovery_proxy_timeout_default_ms(),
                   0);

    if (tools_refresh_send(ctx) == 0)
        return 0;

    peer->tools_refresh = NULL;
    peer->tools_state = DISCOVERY_TOOLS_UNKNOWN;
    peer->tools_generation++;
    tools_refresh_close(ctx);
    return -1;
}

static bool tools_refresh_complete_response(struct discovery_peer_conn *conn, json_t *root)
{
    struct discovery_tools_refresh *ctx;
    json_t *id;
    json_t *result;
    json_t *error;

    if (!conn || !conn->peer)
        return false;

    ctx = conn->peer->tools_refresh;
    id = json_object_get(root, "id");
    if (!ctx || ctx->conn != conn ||
        !json_is_string(id) ||
        strcmp(json_string_value(id), ctx->remote_id) != 0)
        return false;

    result = json_object_get(root, "result");
    error = json_object_get(root, "error");
    if (!result || !peer_store_tools_list_from_result(ctx->peer, result)) {
        json_t *message = json_object_get(error, "message");
        char failure[256];

        snprintf(failure,
                 sizeof(failure),
                 "Remote tools discovery failed: %s",
                 json_is_string(message) ? json_string_value(message) : "invalid tools/list response.");
        tools_refresh_fail(ctx, failure);
        return true;
    }

    ctx->peer->tools_refresh = NULL;
    ctx->peer->tools_state = DISCOVERY_TOOLS_READY;
    tools_refresh_close(ctx);
    peer_mark_heartbeat_ok(conn->peer);
    peer_send_pending_proxies(conn);
    return true;
}

static void peer_send_initialized_notification(struct discovery_peer_conn *conn)
{
    json_t *notification;
    char *dumped;

    if (!conn || !conn->connected || conn->closing)
        return;

    notification = json_object();
    if (!notification)
        return;

    json_object_set_new(notification, "jsonrpc", json_string("2.0"));
    json_object_set_new(notification, "method", json_string("notifications/initialized"));
    json_object_set_new(notification, "params", json_object());

    dumped = json_dumps(notification, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(notification);
    if (!dumped)
        return;

    if (peer_send_frame(conn, dumped, strlen(dumped), false) != 0)
        peer_conn_fail(conn);
    free(dumped);
}

static int peer_send_initialize(struct discovery_peer_conn *conn)
{
    json_t *request;
    json_t *params;
    json_t *client_info;
    char *dumped;
    int rc;

    if (!conn || !conn->connected || conn->closing)
        return -1;
    if (conn->mcp_initialize_sent)
        return 0;

    request = json_object();
    params = json_object();
    client_info = json_object();
    if (!request || !params || !client_info) {
        json_decref(client_info);
        json_decref(params);
        json_decref(request);
        return -1;
    }

    json_object_set_new(request, "jsonrpc", json_string("2.0"));
    json_object_set_new(request, "id", json_string(MCP_DISCOVERY_INITIALIZE_ID));
    json_object_set_new(request, "method", json_string("initialize"));
    json_object_set_new(params, "protocolVersion", json_string("2024-11-05"));
    json_object_set_new(params, "capabilities", json_object());
    json_object_set_new(client_info, "name", json_string("mcp_gateway"));
    json_object_set_new(client_info, "version", json_string(MCP_SERVER_VERSION));
    json_object_set_new(params, "clientInfo", client_info);
    {
        json_t *identity = mcp_server_discovery_local_identity(conn->discovery);
        if (identity) {
            if (conn->data_channel)
                json_object_set_new(identity, "data_channel", json_true());
            json_object_set_new(params, "mcp_peer_identity", identity);
        }
    }
    json_object_set_new(request, "params", params);

    dumped = json_dumps(request, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(request);
    if (!dumped)
        return -1;

    rc = peer_send_frame(conn, dumped, strlen(dumped), false);
    free(dumped);
    if (rc == 0)
        conn->mcp_initialize_sent = true;
    return rc;
}

static void peer_send_pending_proxies(struct discovery_peer_conn *conn)
{
    struct discovery_pending_proxy *ctx;

    if (!conn || !conn->connected || !conn->mcp_initialized)
        return;

    if (conn->peer->tools_state == DISCOVERY_TOOLS_UNKNOWN) {
        if (peer_start_tools_refresh(conn) != 0)
            pending_proxy_complete_waiting_for_tools(
                conn->peer,
                "Failed to start remote tools discovery.");
        return;
    }
    if (conn->peer->tools_state == DISCOVERY_TOOLS_REFRESHING)
        return;

    ctx = conn->discovery->pending_proxies;
    while (ctx) {
        struct discovery_pending_proxy *next = ctx->next;

        if (ctx->peer == conn->peer && !ctx->sent) {
            if (ctx->kind == MCP_DISCOVERY_PROXY_TOOLS_LIST) {
                json_t *result = mcp_tool_result_json_text(conn->peer->tools_list, false);
                pending_proxy_finish_result(ctx, result);
                json_decref(result);
            } else if (!tools_list_contains_tool(conn->peer->tools_list, ctx->tool_name)) {
                json_t *result = proxy_error_result("Remote tool is not cached for this server.");
                pending_proxy_finish_result(ctx, result);
                json_decref(result);
            } else if (pending_proxy_send(ctx) != 0) {
                json_t *result = proxy_error_result("Failed to send remote proxy request.");
                pending_proxy_finish_result(ctx, result);
                json_decref(result);
                peer_conn_fail(conn);
                return;
            }
        }
        ctx = next;
    }
}

static void pending_proxy_complete_for_peer(struct mcp_server_discovery *discovery,
                                            struct discovery_peer *peer,
                                            const char *message)
{
    struct discovery_pending_proxy *ctx = discovery ? discovery->pending_proxies : NULL;

    while (ctx) {
        struct discovery_pending_proxy *next = ctx->next;

        if (!peer || ctx->peer == peer) {
            json_t *result = proxy_error_result(message);
            pending_proxy_finish_result(ctx, result);
            json_decref(result);
        }
        ctx = next;
    }
}

static bool pending_proxy_complete_response(struct discovery_peer_conn *conn, json_t *root)
{
    struct discovery_pending_proxy *ctx;
    json_t *id;
    const char *id_value;
    json_t *result;
    json_t *error;

    id = json_object_get(root, "id");
    if (!json_is_string(id))
        return false;

    id_value = json_string_value(id);
    result = json_object_get(root, "result");
    error = json_object_get(root, "error");

    for (ctx = conn->discovery->pending_proxies; ctx; ctx = ctx->next) {
        if (ctx->peer != conn->peer || strcmp(ctx->remote_id, id_value) != 0)
            continue;

        if (result) {
            if (ctx->kind == MCP_DISCOVERY_PROXY_TOOLS_LIST) {
                json_t *tool_result;

                peer_store_tools_list_from_result(ctx->peer, result);
                tool_result = mcp_tool_result_json_text(result, false);
                pending_proxy_finish_result(ctx, tool_result);
                json_decref(tool_result);
            } else {
                pending_proxy_finish_result(ctx, result);
            }
        } else {
            json_t *message = json_object_get(error, "message");
            json_t *tool_result = proxy_error_result(
                json_is_string(message) ? json_string_value(message) : "Remote proxy call failed.");
            pending_proxy_finish_result(ctx, tool_result);
            json_decref(tool_result);
        }
        peer_mark_heartbeat_ok(conn->peer);
        return true;
    }

    return false;
}

static bool peer_handle_initialize_response(struct discovery_peer_conn *conn, json_t *root)
{
    json_t *id = json_object_get(root, "id");

    if (!json_is_string(id) ||
        strcmp(json_string_value(id), MCP_DISCOVERY_INITIALIZE_ID) != 0)
        return false;

    if (json_object_get(root, "result")) {
        conn->mcp_initialized = true;
        peer_send_initialized_notification(conn);
        if (!conn->data_channel) {
            if (!conn->peer_notified) {
                conn->peer_notified = true;
                mcp_peer_transport_notify_connected(
                    conn->discovery->server->peer_transport,
                    conn->peer->server_id);
            }
            peer_mark_heartbeat_ok(conn->peer);
            peer_send_pending_proxies(conn);
        }
    } else {
        if (!conn->data_channel)
            pending_proxy_complete_for_peer(conn->discovery,
                                            conn->peer,
                                            "Remote server initialization failed.");
        peer_conn_fail(conn);
    }

    return true;
}

static json_t *build_offline_params(struct mcp_server_discovery *discovery)
{
    json_t *params = json_object();
    json_t *status;
    char ip[64];

    if (!params)
        return NULL;

    if (!discovery_local_ip(discovery, ip, sizeof(ip)))
        snprintf(ip, sizeof(ip), "127.0.0.1");

    status = mcp_system_status_json();
    if (!status)
        status = json_object();

    json_object_set_new(params, "instance_id", json_string(discovery->instance_id));
    json_object_set_new(params, "ip", json_string(ip));
    json_object_set_new(params, "port", json_integer((json_int_t)discovery->tcp_port));
    json_object_set_new(params, "status", status);
    return params;
}

static void peer_send_offline(struct discovery_peer_conn *conn)
{
    json_t *notification;
    json_t *params;
    char *dumped;

    if (!conn || !conn->connected || conn->closing)
        return;

    notification = json_object();
    if (!notification)
        return;

    params = build_offline_params(conn->discovery);
    if (!params) {
        json_decref(notification);
        return;
    }

    json_object_set_new(notification, "jsonrpc", json_string("2.0"));
    json_object_set_new(notification,
                        "method",
                        json_string(MCP_SERVER_DISCOVERY_OFFLINE_METHOD));
    json_object_set_new(notification, "params", params);

    dumped = json_dumps(notification, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(notification);
    if (!dumped)
        return;

    peer_send_frame(conn, dumped, strlen(dumped), true);
    free(dumped);
}

static void peer_send_heartbeat(struct mcp_server_discovery *discovery,
                                struct discovery_peer *peer)
{
    json_t *request;
    char *dumped;

    if (!peer->conn || !peer->conn->connected)
        return;

    request = json_object();
    if (!request)
        return;

    discovery->heartbeat_id++;
    json_object_set_new(request, "jsonrpc", json_string("2.0"));
    json_object_set_new(request, "id", json_integer((json_int_t)discovery->heartbeat_id));
    json_object_set_new(request, "method", json_string("ping"));
    json_object_set_new(request, "params", json_object());

    dumped = json_dumps(request, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(request);
    if (!dumped)
        return;

    if (peer_send_frame(peer->conn, dumped, strlen(dumped), false) != 0)
        peer_conn_fail(peer->conn);
    free(dumped);
}

static void heartbeat_timer_cb(uv_timer_t *timer)
{
    struct mcp_server_discovery *discovery = timer->data;
    struct discovery_peer *peer;
    unsigned long long now = mcp_now_ms();
    bool local_stall = discovery->last_heartbeat_tick_ms > 0 &&
                       now >= discovery->last_heartbeat_tick_ms &&
                       now - discovery->last_heartbeat_tick_ms >
                           MCP_DISCOVERY_HEARTBEAT_STALL_MS;

    discovery->last_heartbeat_tick_ms = now;
    if (local_stall)
        discovery_rebase_peer_heartbeats(discovery, now);
    else
        discovery_mark_stale_peers(discovery, now);
    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->state == DISCOVERY_PEER_OFFLINE)
            continue;
        if (!peer->conn)
            peer_connect(discovery, peer, false);
        else if (peer->conn->connected)
            peer_send_heartbeat(discovery, peer);
    }
}

static struct discovery_peer *find_peer(struct mcp_server_discovery *discovery,
                                        const char *ip,
                                        unsigned int port)
{
    struct discovery_peer *peer;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->port == port && strcmp(peer->ip, ip) == 0)
            return peer;
    }

    return NULL;
}

static struct discovery_peer *upsert_peer(struct mcp_server_discovery *discovery,
                                          const char *ip,
                                          unsigned int port,
                                          json_t *status,
                                          enum discovery_peer_state state)
{
    struct discovery_peer *peer = find_peer(discovery, ip, port);
    char *id = NULL;

    if (!peer) {
        if (!make_peer_id(ip, port, &id))
            return NULL;

        peer = calloc(1, sizeof(*peer));
        if (!peer) {
            free(id);
            return NULL;
        }

        peer->id = id;
        peer->server_id = discovery->next_server_id++;
        snprintf(peer->ip, sizeof(peer->ip), "%s", ip);
        peer->port = port;
        peer->next = discovery->peers;
        discovery->peers = peer;
    }

    json_decref(peer->status);
    peer->status = status ? json_deep_copy(status) : json_object();
    if (!peer->status)
        peer->status = json_object();
    peer->last_seen_ms = mcp_now_ms();
    peer->seen_generation = discovery->current_generation;
    if (state == DISCOVERY_PEER_OFFLINE) {
        peer->state = DISCOVERY_PEER_OFFLINE;
        peer_conn_close(peer->data_conn);
        peer_conn_close(peer->conn);
    } else if (state == DISCOVERY_PEER_ONLINE) {
        if (peer->conn && peer->conn->connected)
            peer->state = DISCOVERY_PEER_ONLINE;
        else
            peer->state = DISCOVERY_PEER_TIMEOUT;
    }
    return peer;
}

static bool mark_peer_offline(struct mcp_server_discovery *discovery,
                              const char *ip,
                              unsigned int port,
                              json_t *status)
{
    struct discovery_peer *peer;

    if (!discovery || !ip || ip[0] == '\0' || port > 65535)
        return false;

    peer = upsert_peer(discovery, ip, port, status, DISCOVERY_PEER_OFFLINE);
    if (!peer)
        return false;

    peer->last_seen_ms = mcp_now_ms();
    peer->state = DISCOVERY_PEER_OFFLINE;
    peer_conn_close(peer->data_conn);
    peer_conn_close(peer->conn);
    return true;
}

bool mcp_server_discovery_handle_offline_notification(struct mcp_server_discovery *discovery,
                                                      json_t *params)
{
    json_t *ip;
    json_t *port;
    json_t *status;

    if (!discovery || !json_is_object(params))
        return false;

    ip = json_object_get(params, "ip");
    port = json_object_get(params, "port");
    status = json_object_get(params, "status");
    if (!json_is_string(ip) ||
        !json_is_integer(port) ||
        json_integer_value(port) < 0 ||
        json_integer_value(port) > 65535) {
        return false;
    }

    if (status && !json_is_object(status))
        status = NULL;

    return mark_peer_offline(discovery,
                             json_string_value(ip),
                             (unsigned int)json_integer_value(port),
                             status);
}

json_t *mcp_server_discovery_local_identity(struct mcp_server_discovery *discovery)
{
    json_t *identity;
    json_t *status;
    char ip[64];

    if (!discovery)
        return NULL;

    identity = json_object();
    if (!identity)
        return NULL;

    if (!discovery_local_ip(discovery, ip, sizeof(ip)))
        snprintf(ip, sizeof(ip), "127.0.0.1");
    status = mcp_system_status_json();
    if (!status)
        status = json_object();

    json_object_set_new(identity, "instance_id", json_string(discovery->instance_id));
    json_object_set_new(identity, "ip", json_string(ip));
    json_object_set_new(identity, "port", json_integer((json_int_t)discovery->tcp_port));
    json_object_set_new(identity, "status", status);
    return identity;
}

unsigned int mcp_server_discovery_note_peer_identity(struct mcp_server_discovery *discovery,
                                                     json_t *identity,
                                                     bool data_channel)
{
    json_t *ip;
    json_t *port;
    json_t *status;
    struct discovery_peer *peer;

    if (!discovery || !json_is_object(identity))
        return 0;

    ip = json_object_get(identity, "ip");
    port = json_object_get(identity, "port");
    status = json_object_get(identity, "status");
    if (!json_is_string(ip) ||
        !json_is_integer(port) ||
        json_integer_value(port) < 0 ||
        json_integer_value(port) > 65535)
        return 0;
    if (status && !json_is_object(status))
        status = NULL;

    if (data_channel) {
        peer = find_peer(discovery,
                         json_string_value(ip),
                         (unsigned int)json_integer_value(port));
        if (!peer || peer->state == DISCOVERY_PEER_OFFLINE)
            return 0;
        return peer->server_id;
    }

    peer = upsert_peer(discovery,
                       json_string_value(ip),
                       (unsigned int)json_integer_value(port),
                       status,
                       DISCOVERY_PEER_ONLINE);
    if (!peer)
        return 0;

    return peer->server_id;
}

static void discovery_on_datagram(uv_udp_t *handle,
                                  ssize_t nread,
                                  const uv_buf_t *buf,
                                  const struct sockaddr *addr,
                                  unsigned int flags)
{
    struct mcp_server_discovery *discovery = handle->data;
    json_error_t error;
    json_t *root;
    json_t *protocol;
    json_t *instance_id;
    json_t *tcp_port;
    json_t *status;
    json_t *reply;
    json_t *event;
    json_t *advertise_host;
    const char *peer_ip_value = NULL;
    char peer_ip[64];
    bool offline;

    (void)flags;

    if (nread < 0 || !addr) {
        free(buf->base);
        return;
    }
    if (nread == 0) {
        free(buf->base);
        return;
    }

    root = json_loadb(buf->base, (size_t)nread, JSON_REJECT_DUPLICATES, &error);
    free(buf->base);
    if (!root)
        return;

    protocol = json_object_get(root, "mcp_server_discovery");
    instance_id = json_object_get(root, "instance_id");
    tcp_port = json_object_get(root, "tcp_port");
    status = json_object_get(root, "status");
    reply = json_object_get(root, "reply");
    event = json_object_get(root, "event");
    advertise_host = json_object_get(root, "advertise_host");

    if (!json_is_integer(protocol) ||
        json_integer_value(protocol) != MCP_DISCOVERY_PROTOCOL ||
        !json_is_string(instance_id) ||
        !json_is_integer(tcp_port) ||
        json_integer_value(tcp_port) < 0 ||
        json_integer_value(tcp_port) > 65535 ||
        !json_is_object(status)) {
        json_decref(root);
        return;
    }

    if (strcmp(json_string_value(instance_id), discovery->instance_id) == 0) {
        json_decref(root);
        return;
    }

    if (json_is_string(advertise_host) && json_string_value(advertise_host)[0] != '\0')
        peer_ip_value = json_string_value(advertise_host);
    else if (sockaddr_to_ip(addr, peer_ip, sizeof(peer_ip)))
        peer_ip_value = peer_ip;

    offline = json_is_string(event) && strcmp(json_string_value(event), "offline") == 0;
    if (peer_ip_value) {
        unsigned int port = (unsigned int)json_integer_value(tcp_port);

        if (offline) {
            struct discovery_peer *peer = find_peer(discovery, peer_ip_value, port);
            if (!peer || peer->state != DISCOVERY_PEER_OFFLINE)
                mark_peer_offline(discovery, peer_ip_value, port, status);
        } else {
            upsert_peer(discovery, peer_ip_value, port, status, DISCOVERY_PEER_ONLINE);
        }
    }

    if (!offline && !json_is_true(reply)) {
        json_t *response = build_discovery_packet(discovery, true, "online");
        if (response) {
            send_packet_to_sockaddr(discovery, response, addr);
            json_decref(response);
        }
    }

    json_decref(root);
}

static void discovery_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;

    buf->base = malloc(MCP_DISCOVERY_MAX_DATAGRAM);
    buf->len = buf->base ? MCP_DISCOVERY_MAX_DATAGRAM : 0;
}

static void udp_close_cb(uv_handle_t *handle)
{
    struct mcp_server_discovery *discovery = handle->data;

    discovery->udp_initialized = false;
    discovery->opened = false;
    discovery_maybe_release(discovery);
}

static void timer_close_cb(uv_handle_t *handle)
{
    struct mcp_server_discovery *discovery = handle->data;

    if (handle == (uv_handle_t *)&discovery->announce_timer)
        discovery->announce_timer_initialized = false;
    else if (handle == (uv_handle_t *)&discovery->heartbeat_timer)
        discovery->heartbeat_timer_initialized = false;
    discovery_maybe_release(discovery);
}

static void pending_unlink(struct discovery_pending_list *ctx)
{
    struct discovery_pending_list **current = &ctx->discovery->pending_lists;

    while (*current) {
        if (*current == ctx) {
            *current = ctx->next;
            ctx->next = NULL;
            return;
        }
        current = &(*current)->next;
    }
}

static void pending_close_cb(uv_handle_t *handle)
{
    struct discovery_pending_list *ctx = handle->data;
    struct mcp_server_discovery *discovery = ctx->discovery;

    free(ctx->id_key);
    free(ctx);
    discovery_maybe_release(discovery);
}

static void apply_generation(struct mcp_server_discovery *discovery, unsigned long generation)
{
    struct discovery_peer *peer;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->seen_generation != generation &&
            peer->state != DISCOVERY_PEER_OFFLINE &&
            (!peer->conn || !peer->conn->connected)) {
            peer_mark_timeout(peer);
        }
    }
}

static void pending_list_timer_cb(uv_timer_t *timer)
{
    struct discovery_pending_list *ctx = timer->data;
    json_t *snapshot;
    json_t *result;

    pending_unlink(ctx);
    apply_generation(ctx->discovery, ctx->generation);

    snapshot = mcp_server_discovery_snapshot_json(ctx->discovery);
    result = mcp_tool_result_json_text(snapshot, false);
    json_decref(snapshot);

    mcp_server_complete_async_ok(ctx->discovery->server, ctx->id_key, result);
    json_decref(result);

    uv_timer_stop(&ctx->timer);
    uv_close((uv_handle_t *)&ctx->timer, pending_close_cb);
}

static bool discovery_has_pending(const struct mcp_server_discovery *discovery)
{
    return discovery && discovery->pending_lists;
}

static bool discovery_has_pending_proxies(const struct mcp_server_discovery *discovery)
{
    return discovery &&
           (discovery->pending_proxies ||
            discovery->pending_proxy_closes > 0 ||
            discovery->tools_refresh_closes > 0);
}

static bool discovery_has_peer_connections(const struct mcp_server_discovery *discovery)
{
    const struct discovery_peer *peer;

    if (!discovery)
        return false;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->conn || peer->data_conn)
            return true;
    }

    return false;
}

static void discovery_maybe_release(struct mcp_server_discovery *discovery)
{
    if (!discovery || !discovery->destroy_on_close)
        return;
    if (discovery->udp_initialized ||
        discovery->announce_timer_initialized ||
        discovery->heartbeat_timer_initialized ||
        discovery_has_pending_proxies(discovery) ||
        discovery_has_pending(discovery) ||
        discovery_has_peer_connections(discovery))
        return;

    mcp_server_discovery_destroy(discovery);
}

int mcp_server_discovery_create(struct mcp_server_discovery **out,
                                struct mcp_server *server,
                                uv_loop_t *loop)
{
    struct mcp_server_discovery *discovery;

    *out = NULL;
    if (!server || !loop)
        return -1;

    discovery = calloc(1, sizeof(*discovery));
    if (!discovery)
        return -1;

    discovery->server = server;
    discovery->loop = loop;
    discovery->next_server_id = 1;
    discovery->instance_id = make_instance_id();
    if (!discovery->instance_id) {
        free(discovery);
        return -1;
    }

    mcp_peer_transport_set_external_sender(server->peer_transport,
                                           discovery_external_send_frame,
                                           discovery);

    *out = discovery;
    return 0;
}

void mcp_server_discovery_destroy(struct mcp_server_discovery *discovery)
{
    struct discovery_peer *peer;
    struct discovery_pending_list *pending;
    struct discovery_pending_proxy *proxy;

    if (!discovery)
        return;

    if (discovery->server && discovery->server->peer_transport)
        mcp_peer_transport_set_external_sender(discovery->server->peer_transport, NULL, NULL);

    if (discovery->udp_initialized ||
        discovery->announce_timer_initialized ||
        discovery->heartbeat_timer_initialized ||
        discovery_has_pending_proxies(discovery) ||
        discovery_has_pending(discovery) ||
        discovery_has_peer_connections(discovery)) {
        discovery->destroy_on_close = true;
        mcp_server_discovery_close(discovery);
        return;
    }

    pending = discovery->pending_lists;
    while (pending) {
        struct discovery_pending_list *next = pending->next;
        free(pending->id_key);
        free(pending);
        pending = next;
    }

    proxy = discovery->pending_proxies;
    while (proxy) {
        struct discovery_pending_proxy *next = proxy->next;
        pending_proxy_free(proxy);
        proxy = next;
    }

    peer = discovery->peers;
    while (peer) {
        struct discovery_peer *next = peer->next;
        json_decref(peer->status);
        json_decref(peer->tools_list);
        free(peer->id);
        free(peer);
        peer = next;
    }

    free(discovery->bind_host);
    free(discovery->tcp_host);
    free(discovery->advertise_host);
    free(discovery->explicit_hosts);
    free(discovery->instance_id);
    free(discovery);
}

int mcp_server_discovery_start(struct mcp_server_discovery *discovery,
                               const struct mcp_server_discovery_config *config)
{
    struct sockaddr_storage bind_addr;
    int rc;

    if (!discovery || !config || discovery->opened || !config->tcp_port)
        return -1;
    if (config->discovery_port > 65535 ||
        config->broadcast_port > 65535 ||
        config->tcp_port > 65535)
        return -1;
    if (sockaddr_from_host_port(config->bind_host ? config->bind_host : "0.0.0.0",
                                config->discovery_port,
                                &bind_addr) != 0)
        return -1;

    if (dup_string(&discovery->bind_host, config->bind_host ? config->bind_host : "0.0.0.0") != 0 ||
        dup_string(&discovery->tcp_host, config->tcp_host) != 0 ||
        dup_string(&discovery->advertise_host, config->advertise_host) != 0 ||
        dup_string(&discovery->explicit_hosts, config->explicit_hosts) != 0)
        return -1;
    discovery->discovery_port = config->discovery_port;
    discovery->broadcast_port = config->broadcast_port ? config->broadcast_port : config->tcp_port;
    discovery->tcp_port = config->tcp_port;

    rc = uv_udp_init(discovery->loop, &discovery->udp);
    if (rc != 0)
        return -1;
    discovery->udp_initialized = true;
    discovery->udp.data = discovery;

    rc = uv_udp_bind(&discovery->udp, (const struct sockaddr *)&bind_addr, UV_UDP_REUSEADDR);
    if (rc != 0)
        return -1;

    rc = uv_udp_set_broadcast(&discovery->udp, 1);
    if (rc != 0)
        return -1;

    rc = uv_udp_recv_start(&discovery->udp, discovery_alloc_cb, discovery_on_datagram);
    if (rc != 0)
        return -1;

    if (uv_timer_init(discovery->loop, &discovery->announce_timer) != 0)
        return -1;
    discovery->announce_timer_initialized = true;
    discovery->announce_timer.data = discovery;

    if (uv_timer_init(discovery->loop, &discovery->heartbeat_timer) != 0)
        return -1;
    discovery->heartbeat_timer_initialized = true;
    discovery->heartbeat_timer.data = discovery;

    discovery->opened = true;
    discovery->last_heartbeat_tick_ms = mcp_now_ms();
    uv_timer_start(&discovery->announce_timer,
                   announce_timer_cb,
                   MCP_DISCOVERY_ANNOUNCE_MS,
                   MCP_DISCOVERY_ANNOUNCE_MS);
    uv_timer_start(&discovery->heartbeat_timer,
                   heartbeat_timer_cb,
                   MCP_DISCOVERY_HEARTBEAT_MS,
                   MCP_DISCOVERY_HEARTBEAT_MS);
    discovery_send_announce(discovery, false);
    return 0;
}

void mcp_server_discovery_close(struct mcp_server_discovery *discovery)
{
    struct discovery_peer *peer;
    struct discovery_pending_list *pending;

    if (!discovery || discovery->closing)
        return;

    discovery->closing = true;

    for (peer = discovery->peers; peer; peer = peer->next) {
        peer_send_offline(peer->conn);
        peer_conn_close(peer->data_conn);
    }
    discovery_send_offline(discovery);
    pending_proxy_complete_for_peer(discovery,
                                    NULL,
                                    "Remote proxy call cancelled because discovery is closing.");

    discovery->opened = false;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->conn && !peer->conn->connected)
            peer_conn_close(peer->conn);
        peer_conn_close(peer->data_conn);
    }

    pending = discovery->pending_lists;
    while (pending) {
        struct discovery_pending_list *next = pending->next;
        pending->next = NULL;
        uv_timer_stop(&pending->timer);
        if (!uv_is_closing((uv_handle_t *)&pending->timer))
            uv_close((uv_handle_t *)&pending->timer, pending_close_cb);
        pending = next;
    }
    discovery->pending_lists = NULL;

    if (discovery->announce_timer_initialized &&
        !uv_is_closing((uv_handle_t *)&discovery->announce_timer)) {
        uv_timer_stop(&discovery->announce_timer);
        uv_close((uv_handle_t *)&discovery->announce_timer, timer_close_cb);
    }
    if (discovery->heartbeat_timer_initialized &&
        !uv_is_closing((uv_handle_t *)&discovery->heartbeat_timer)) {
        uv_timer_stop(&discovery->heartbeat_timer);
        uv_close((uv_handle_t *)&discovery->heartbeat_timer, timer_close_cb);
    }
    if (discovery->udp_initialized && !uv_is_closing((uv_handle_t *)&discovery->udp)) {
        if (discovery->udp_sends_pending == 0) {
            uv_udp_recv_stop(&discovery->udp);
            uv_close((uv_handle_t *)&discovery->udp, udp_close_cb);
        } else {
            discovery->close_udp_after_sends = true;
            uv_udp_recv_stop(&discovery->udp);
        }
    }

    discovery_maybe_release(discovery);
}

bool mcp_server_discovery_is_open(const struct mcp_server_discovery *discovery)
{
    return discovery && discovery->opened && !discovery->closing;
}

static struct discovery_peer *find_peer_by_server_id(struct mcp_server_discovery *discovery,
                                                     unsigned int server_id)
{
    struct discovery_peer *peer;

    if (!discovery || server_id == 0)
        return NULL;

    for (peer = discovery->peers; peer; peer = peer->next) {
        if (peer->server_id == server_id)
            return peer;
    }

    return NULL;
}

void mcp_server_discovery_mark_peer_active(struct mcp_server_discovery *discovery,
                                           unsigned int server_id)
{
    struct discovery_peer *peer = find_peer_by_server_id(discovery, server_id);

    if (peer)
        peer_mark_heartbeat_ok(peer);
}

int mcp_server_discovery_call_remote_tool(struct mcp_server_discovery *discovery,
                                          const char *id_key,
                                          unsigned int server_id,
                                          enum mcp_discovery_proxy_kind kind,
                                          const char *tool_name,
                                          json_t *arguments,
                                          unsigned int proxy_timeout_ms)
{
    struct discovery_pending_proxy *ctx;
    struct discovery_peer *peer;
    int len;

    if (!discovery || !id_key || !tool_name || !json_is_object(arguments))
        return -1;
    if (!discovery->opened || discovery->closing)
        return -1;

    peer = find_peer_by_server_id(discovery, server_id);
    if (!peer || peer->state == DISCOVERY_PEER_OFFLINE)
        return -1;
    if (!peer->conn || !peer->conn->connected)
        return -1;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    ctx->discovery = discovery;
    ctx->peer = peer;
    ctx->id_key = mcp_strdup(id_key);
    ctx->tool_name = mcp_strdup(tool_name);
    ctx->arguments = json_incref(arguments);
    ctx->kind = kind;
    ctx->timeout_ms = proxy_timeout_ms ? proxy_timeout_ms : discovery_proxy_timeout_default_ms();
    len = snprintf(NULL, 0, "gateway:%s", id_key);
    if (!ctx->id_key || !ctx->tool_name || len < 0) {
        pending_proxy_free(ctx);
        return -1;
    }
    ctx->remote_id = malloc((size_t)len + 1);
    if (!ctx->remote_id) {
        pending_proxy_free(ctx);
        return -1;
    }
    snprintf(ctx->remote_id, (size_t)len + 1, "gateway:%s", id_key);

    if (uv_timer_init(discovery->loop, &ctx->timer) != 0) {
        pending_proxy_free(ctx);
        return -1;
    }
    ctx->timer_initialized = true;
    ctx->timer.data = ctx;
    uv_timer_start(&ctx->timer, pending_proxy_timeout_cb, ctx->timeout_ms, 0);

    ctx->next = discovery->pending_proxies;
    discovery->pending_proxies = ctx;

    if (!peer->conn->mcp_initialized) {
        if (peer_send_initialize(peer->conn) == 0)
            return 0;

        pending_proxy_unlink(ctx);
        pending_proxy_close(ctx);
        return -1;
    }

    if (kind == MCP_DISCOVERY_PROXY_TOOLS_LIST &&
        peer->tools_state == DISCOVERY_TOOLS_READY)
        peer_invalidate_tools_cache(peer);

    peer_send_pending_proxies(peer->conn);
    return 0;
}

int mcp_server_discovery_list_async(struct mcp_server_discovery *discovery,
                                    const char *id_key,
                                    unsigned int wait_ms)
{
    struct discovery_pending_list *ctx;

    if (!discovery || !id_key || !discovery->opened)
        return -1;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    ctx->discovery = discovery;
    ctx->id_key = mcp_strdup(id_key);
    if (!ctx->id_key) {
        free(ctx);
        return -1;
    }

    discovery->current_generation++;
    ctx->generation = discovery->current_generation;

    if (uv_timer_init(discovery->loop, &ctx->timer) != 0) {
        free(ctx->id_key);
        free(ctx);
        return -1;
    }

    ctx->timer_initialized = true;
    ctx->timer.data = ctx;
    ctx->next = discovery->pending_lists;
    discovery->pending_lists = ctx;

    discovery_send_announce(discovery, false);
    uv_timer_start(&ctx->timer,
                   pending_list_timer_cb,
                   wait_ms ? wait_ms : MCP_DISCOVERY_WAIT_MS,
                   0);
    return 0;
}

static json_t *build_system_status_summary(json_t *status)
{
    static const char *fields[] = {
        "hostname",
        "os",
        "machine",
        "memory_total_bytes",
        "memory_available_bytes",
        "commands",
    };
    json_t *summary = json_object();
    size_t i;

    if (!summary)
        return NULL;
    if (!json_is_object(status))
        return summary;

    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        json_t *value = json_object_get(status, fields[i]);

        if (value)
            json_object_set(summary, fields[i], value);
    }

    return summary;
}

static json_t *build_server_entry(const char *scope,
                                  unsigned int server_id,
                                  const char *address,
                                  unsigned int port,
                                  const char *state,
                                  bool tcp_connected,
                                  unsigned long long last_seen_ms,
                                  json_t *status)
{
    json_t *entry = json_object();
    json_t *status_summary;

    if (!entry)
        return NULL;

    status_summary = build_system_status_summary(status);
    if (!status_summary) {
        json_decref(entry);
        return NULL;
    }

    json_object_set_new(entry, "server_id", json_integer((json_int_t)server_id));
    json_object_set_new(entry, "address", json_string(address));
    json_object_set_new(entry, "port", json_integer((json_int_t)port));
    json_object_set_new(entry, "scope", json_string(scope));
    json_object_set_new(entry, "state", json_string(state));
    json_object_set_new(entry, "tcp_connected", json_boolean(tcp_connected));
    json_object_set_new(entry, "last_seen_ms", json_integer((json_int_t)last_seen_ms));
    json_object_set_new(entry, "system_status", status_summary);

    return entry;
}

static json_t *build_local_server_entry(struct mcp_server_discovery *discovery)
{
    json_t *entry;
    json_t *status;
    char ip[64];
    char *address = NULL;

    if (!discovery || !discovery_local_ip(discovery, ip, sizeof(ip)))
        return NULL;
    if (!make_peer_id(ip, discovery->tcp_port, &address))
        return NULL;

    status = mcp_system_status_json();
    entry = build_server_entry("local",
                               0,
                               address,
                               discovery->tcp_port,
                               "online",
                               true,
                               mcp_now_ms(),
                               status);
    json_decref(status);
    free(address);
    return entry;
}

json_t *mcp_server_discovery_snapshot_json(struct mcp_server_discovery *discovery)
{
    json_t *root = json_object();
    json_t *servers = json_array();
    struct discovery_peer *peer;
    size_t total = 0;

    if (!root || !servers)
        goto fail;

    if (discovery) {
        json_t *entry = build_local_server_entry(discovery);
        if (!entry)
            goto fail;
        json_array_append_new(servers, entry);
        total++;
    }

    for (peer = discovery ? discovery->peers : NULL; peer; peer = peer->next) {
        json_t *entry;

        entry = build_server_entry("remote",
                                   peer->server_id,
                                   peer->id,
                                   peer->port,
                                   peer_state_name(peer->state),
                                   peer->conn && peer->conn->connected,
                                   peer->last_seen_ms,
                                   peer->status);
        if (!entry)
            goto fail;

        json_array_append_new(servers, entry);
        total++;
    }

    json_object_set_new(root, "total", json_integer((json_int_t)total));
    json_object_set_new(root, "servers", servers);
    return root;

fail:
    json_decref(servers);
    json_decref(root);
    return json_pack("{s:i,s:[]}", "total", 0, "servers");
}
