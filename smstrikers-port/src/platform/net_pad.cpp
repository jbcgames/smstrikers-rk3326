#include "port/net_pad.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>

#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <winhttp.h>
    typedef int socklen_t;
    #define CLOSE_SOCKET(s) closesocket(s)
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "winhttp.lib")
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    typedef int SOCKET;
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR   (-1)
    #define CLOSE_SOCKET(s) close(s)
#endif

// Packet protocol magic "SMS1"
static const uint32_t SMS_NET_MAGIC = 0x534D5331;

enum PacketType : uint8_t {
    PKT_DISCOVERY_PING = 1,
    PKT_DISCOVERY_PONG = 2,
    PKT_CONNECT_REQ    = 3,
    PKT_CONNECT_ACK    = 4,
    PKT_INPUT_FRAME    = 5,
    PKT_HEARTBEAT_CRC  = 6,
    PKT_DISCONNECT     = 7,
    PKT_WORLD_STATE    = 8
};

#pragma pack(push, 1)
struct NetPadEntry {
    uint32_t frame_index;
    uint16_t button;
    int8_t   stickX;
    int8_t   stickY;
    int8_t   substickX;
    int8_t   substickY;
    uint8_t  triggerLeft;
    uint8_t  triggerRight;
    uint8_t  err;
};

struct NetHeader {
    uint32_t magic;
    uint8_t  type;
    uint8_t  role;
    uint16_t seq;
};

struct DiscoveryPacket {
    NetHeader header;
    char hostname[32];
    uint16_t game_port;
};

struct ConnectAckPacket {
    NetHeader header;
    uint32_t seed;
    uint8_t  delay_frames;
    char hostname[32];
};

struct InputBundlePacket {
    NetHeader header;
    uint8_t count; // Number of bundled frames (up to 3)
    NetPadEntry frames[3];
};

struct HeartbeatPacket {
    NetHeader header;
    uint32_t frame_index;
    uint32_t crc;
};

struct WorldStatePacket {
    NetHeader header;
    NetWorldState state;
};
#pragma pack(pop)

// Internal state
static std::recursive_mutex g_netMutex;
static SOCKET g_socket = INVALID_SOCKET;
static SOCKET g_wsSocket = INVALID_SOCKET;
static sockaddr_in g_remoteAddr;
static bool g_hasRemoteAddr = false;

static char g_onlineRoomCode[16] = { 0 };
static char g_peerWanIp[64] = { 0 };
static char g_peerLanIp[64] = { 0 };
static uint16_t g_peerPort = NET_DEFAULT_PORT;
static uint32_t g_relayIpAddr = 0;
static bool g_udpPeerLocked = false;
static bool g_udpIsDirect = false;
static std::chrono::steady_clock::time_point g_lastUdpPacketTime;

static NetRole  g_role = NET_ROLE_NONE;
static NetState g_state = NET_STATE_DISCONNECTED;
static char     g_localHostname[32] = "SMS-Console";
static uint32_t g_inputDelay = 2; // Default 2 frames input buffer
static uint32_t g_syncSeed = 0x12345678;

static uint32_t g_remoteLastFrame = 0;
static uint32_t g_lastPingMs = 0;
static std::chrono::steady_clock::time_point g_lastPacketTime;

// Ring buffer for local input delay and remote inputs
static const int RING_BUFFER_SIZE = 256;
static NetPadEntry g_localRing[RING_BUFFER_SIZE];
static NetPadEntry g_remoteRing[RING_BUFFER_SIZE];
static bool g_remoteRingValid[RING_BUFFER_SIZE];

static std::vector<NetDiscoveredHost> g_discoveredHosts;
static NetWorldState g_latestWorldState;
static bool g_hasNewWorldState = false;
static bool g_hasReceivedWorldState = false;

static bool set_nonblocking(SOCKET s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static void pad_to_entry(uint32_t frame, const PADStatus* pad, NetPadEntry* out) {
    out->frame_index = frame;
    if (pad == nullptr) {
        out->button = 0;
        out->stickX = 0;
        out->stickY = 0;
        out->substickX = 0;
        out->substickY = 0;
        out->triggerLeft = 0;
        out->triggerRight = 0;
        out->err = (uint8_t)PAD_ERR_NONE;
    } else {
        out->button = pad->button;
        out->stickX = pad->stickX;
        out->stickY = pad->stickY;
        out->substickX = pad->substickX;
        out->substickY = pad->substickY;
        out->triggerLeft = pad->triggerLeft;
        out->triggerRight = pad->triggerRight;
        out->err = (uint8_t)PAD_ERR_NONE;
    }
}

static void entry_to_pad(const NetPadEntry* in, PADStatus* out) {
    if (out == nullptr) return;
    memset(out, 0, sizeof(PADStatus));
    out->err = PAD_ERR_NONE;
    if (in != nullptr) {
        out->button = in->button;
        out->stickX = in->stickX;
        out->stickY = in->stickY;
        out->substickX = in->substickX;
        out->substickY = in->substickY;
        out->triggerLeft = in->triggerLeft;
        out->triggerRight = in->triggerRight;
    }
}

static void reset_session_buffers() {
    memset(g_localRing, 0, sizeof(g_localRing));
    memset(g_remoteRing, 0, sizeof(g_remoteRing));
    memset(g_remoteRingValid, 0, sizeof(g_remoteRingValid));
    g_remoteLastFrame = 0;
    g_udpPeerLocked = false;
    g_udpIsDirect = false;
    g_hasReceivedWorldState = false;
    g_hasNewWorldState = false;
}

static void send_lan_discovery_sweep(const void* pkt, size_t pktLen, uint16_t targetPort, bool fullSubnetSweep);

bool NetPad_Init(uint16_t port) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
#endif

    if (g_socket != INVALID_SOCKET) {
        CLOSE_SOCKET(g_socket);
        g_socket = INVALID_SOCKET;
    }

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) {
        return false;
    }

#ifdef _WIN32
    // Evitar que ICMP Port Unreachable cierre/bloquee el socket UDP en Windows (Error 10054)
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    #ifndef SIO_UDP_CONNRESET
        #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    WSAIoctl(g_socket, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior),
             NULL, 0, &dwBytesReturned, NULL, NULL);
#endif

    int broadcast = 1;
    setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    int reuse = 1;
#ifdef SO_REUSEADDR
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#endif
#ifdef SO_REUSEPORT
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse));
#endif

    sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(port);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_socket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        // Try ephemeral port if default is occupied
        bindAddr.sin_port = 0;
        if (bind(g_socket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
            CLOSE_SOCKET(g_socket);
            g_socket = INVALID_SOCKET;
            return false;
        }
    }

    set_nonblocking(g_socket);

    memset(g_localRing, 0, sizeof(g_localRing));
    memset(g_remoteRing, 0, sizeof(g_remoteRing));
    memset(g_remoteRingValid, 0, sizeof(g_remoteRingValid));
    g_discoveredHosts.clear();

    return true;
}

void NetPad_Shutdown(void) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (g_socket != INVALID_SOCKET) {
        CLOSE_SOCKET(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (g_wsSocket != INVALID_SOCKET) {
        CLOSE_SOCKET(g_wsSocket);
        g_wsSocket = INVALID_SOCKET;
    }
    g_state = NET_STATE_DISCONNECTED;
    g_role = NET_ROLE_NONE;
    g_hasRemoteAddr = false;
    g_udpPeerLocked = false;
    g_udpIsDirect = false;
#ifdef _WIN32
    WSACleanup();
#endif
}

void NetPad_StartHost(const char* hostName, uint16_t port) {
    NetPad_Init(port);
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    g_role = NET_ROLE_HOST;
    g_state = NET_STATE_BROADCASTING;
    if (hostName && hostName[0] != '\0') {
        strncpy(g_localHostname, hostName, sizeof(g_localHostname) - 1);
        g_localHostname[sizeof(g_localHostname) - 1] = '\0';
    }
    g_syncSeed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    g_hasRemoteAddr = false;
    g_onlineRoomCode[0] = '\0';
    g_peerWanIp[0] = '\0';
    g_peerLanIp[0] = '\0';
    reset_session_buffers();

    DiscoveryPacket pong;
    memset(&pong, 0, sizeof(pong));
    pong.header.magic = SMS_NET_MAGIC;
    pong.header.type = PKT_DISCOVERY_PONG;
    pong.header.role = (uint8_t)NET_ROLE_HOST;
    strncpy(pong.hostname, g_localHostname, sizeof(pong.hostname) - 1);
    pong.game_port = port;
    send_lan_discovery_sweep(&pong, sizeof(pong), port, true);
}

void NetPad_StartClient(uint16_t port) {
    NetPad_Init(port);
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    g_role = NET_ROLE_CLIENT;
    g_state = NET_STATE_SEARCHING;
    g_discoveredHosts.clear();
    g_hasRemoteAddr = false;
    g_onlineRoomCode[0] = '\0';
    g_peerWanIp[0] = '\0';
    g_peerLanIp[0] = '\0';
    reset_session_buffers();

    NetHeader ping;
    ping.magic = SMS_NET_MAGIC;
    ping.type = PKT_DISCOVERY_PING;
    ping.role = (uint8_t)NET_ROLE_CLIENT;
    ping.seq = 0;
    send_lan_discovery_sweep(&ping, sizeof(ping), NET_DEFAULT_PORT, true);
}

void NetPad_ConnectTo(const char* ip, uint16_t port) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (g_socket == INVALID_SOCKET) return;

    memset(&g_remoteAddr, 0, sizeof(g_remoteAddr));
    g_remoteAddr.sin_family = AF_INET;
    g_remoteAddr.sin_port = htons(port);
    g_remoteAddr.sin_addr.s_addr = inet_addr(ip);
    g_hasRemoteAddr = true;

    // Send connection request
    NetHeader req;
    req.magic = SMS_NET_MAGIC;
    req.type = PKT_CONNECT_REQ;
    req.role = (uint8_t)NET_ROLE_CLIENT;
    req.seq = 0;

    for (int i = 0; i < 3; ++i) {
        sendto(g_socket, (const char*)&req, sizeof(req), 0,
               (struct sockaddr*)&g_remoteAddr, sizeof(g_remoteAddr));
    }

    g_state = NET_STATE_CONNECTING;
    // CRÍTICO: Inicializar g_lastPacketTime para que NetPad_ProcessFrame no desconecte en el primer frame
    g_lastPacketTime = std::chrono::steady_clock::now();
}

static void send_ws_binary(const void* payload, size_t len);

void NetPad_Disconnect(void) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (g_socket != INVALID_SOCKET && g_hasRemoteAddr) {
        NetHeader disc;
        disc.magic = SMS_NET_MAGIC;
        disc.type = PKT_DISCONNECT;
        disc.role = (uint8_t)g_role;
        disc.seq = 0;
        sendto(g_socket, (const char*)&disc, sizeof(disc), 0,
               (struct sockaddr*)&g_remoteAddr, sizeof(g_remoteAddr));
    }
    if (g_wsSocket != INVALID_SOCKET) {
        NetHeader disc;
        disc.magic = SMS_NET_MAGIC;
        disc.type = PKT_DISCONNECT;
        disc.role = (uint8_t)g_role;
        disc.seq = 0;
        send_ws_binary(&disc, sizeof(disc));
        CLOSE_SOCKET(g_wsSocket);
        g_wsSocket = INVALID_SOCKET;
    }
    g_state = NET_STATE_DISCONNECTED;
    g_role = NET_ROLE_NONE;
    g_hasRemoteAddr = false;
    g_onlineRoomCode[0] = '\0';
    g_peerWanIp[0] = '\0';
    g_peerLanIp[0] = '\0';
    reset_session_buffers();
}

static void encode_ws_frame(const uint8_t* payload, size_t len, uint8_t* outFrame, size_t* outFrameLen)
{
    size_t headerLen = 2;
    outFrame[0] = 0x82;

    if (len <= 125) {
        outFrame[1] = (uint8_t)(len | 0x80);
    } else if (len <= 65535) {
        outFrame[1] = 126 | 0x80;
        outFrame[2] = (uint8_t)((len >> 8) & 0xFF);
        outFrame[3] = (uint8_t)(len & 0xFF);
        headerLen = 4;
    } else {
        outFrame[1] = 127 | 0x80;
        for (int i = 7; i >= 0; i--) {
            outFrame[2 + i] = (uint8_t)((len >> ((7 - i) * 8)) & 0xFF);
        }
        headerLen = 10;
    }

    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    for (int i = 0; i < 4; i++) {
        outFrame[headerLen++] = mask[i];
    }

    for (size_t i = 0; i < len; i++) {
        outFrame[headerLen++] = payload[i] ^ mask[i % 4];
    }

    *outFrameLen = headerLen;
}

static bool decode_ws_frame(const uint8_t* buf, size_t bufLen, uint8_t* outPayload,
                            size_t maxOutLen, size_t* outPayloadLen, size_t* consumedBytes, uint8_t* outOpcode)
{
    if (bufLen < 2) return false;

    uint8_t opcode = buf[0] & 0x0F;
    uint8_t b1 = buf[1];
    bool masked = (b1 & 0x80) != 0;
    uint64_t payloadLen = b1 & 0x7F;

    size_t headerLen = 2;
    if (payloadLen == 126) {
        if (bufLen < 4) return false;
        payloadLen = ((uint64_t)buf[2] << 8) | buf[3];
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (bufLen < 10) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) {
            payloadLen = (payloadLen << 8) | buf[2 + i];
        }
        headerLen = 10;
    }

    uint8_t maskKey[4] = { 0 };
    if (masked) {
        if (bufLen < headerLen + 4) return false;
        memcpy(maskKey, buf + headerLen, 4);
        headerLen += 4;
    }

    if (payloadLen > maxOutLen) {
        *consumedBytes = bufLen;
        *outPayloadLen = 0;
        if (outOpcode) *outOpcode = opcode;
        return false;
    }

    if (bufLen < headerLen + payloadLen) return false;

    if (outOpcode) *outOpcode = opcode;
    *outPayloadLen = (size_t)payloadLen;
    for (size_t i = 0; i < payloadLen; i++) {
        uint8_t byte = buf[headerLen + i];
        outPayload[i] = masked ? (byte ^ maskKey[i % 4]) : byte;
    }

    *consumedBytes = headerLen + (size_t)payloadLen;
    return true;
}

static bool send_ws_text(SOCKET sock, const char* text)
{
    if (sock == INVALID_SOCKET) return false;
    size_t len = strlen(text);

    uint8_t frame[2048];
    size_t headerLen = 2;
    frame[0] = 0x81;

    if (len <= 125) {
        frame[1] = (uint8_t)(len | 0x80);
    } else if (len <= 65535) {
        frame[1] = 126 | 0x80;
        frame[2] = (uint8_t)((len >> 8) & 0xFF);
        frame[3] = (uint8_t)(len & 0xFF);
        headerLen = 4;
    } else {
        frame[1] = 127 | 0x80;
        for (int i = 7; i >= 0; i--) {
            frame[2 + i] = (uint8_t)((len >> ((7 - i) * 8)) & 0xFF);
        }
        headerLen = 10;
    }

    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    for (int i = 0; i < 4; i++) {
        frame[headerLen++] = mask[i];
    }

    for (size_t i = 0; i < len; i++) {
        frame[headerLen++] = ((const uint8_t*)text)[i] ^ mask[i % 4];
    }

    int sent = send(sock, (const char*)frame, (int)headerLen, 0);
    return (sent > 0);
}

static bool connect_websocket_signaling(const char* host, int port)
{
    if (g_wsSocket != INVALID_SOCKET) {
        CLOSE_SOCKET(g_wsSocket);
        g_wsSocket = INVALID_SOCKET;
    }

    struct hostent* he = gethostbyname(host);
    if (!he) return false;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

#if defined(_WIN32)
    DWORD timeout = 4000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif

    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    memcpy(&serverAddr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) != 0) {
        CLOSE_SOCKET(sock);
        return false;
    }

    char req[512];
    snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", host);

    send(sock, req, (int)strlen(req), 0);

    char resp[512] = { 0 };
    int bytes = recv(sock, resp, sizeof(resp) - 1, 0);
    if (bytes <= 0 || strstr(resp, "101") == NULL) {
        CLOSE_SOCKET(sock);
        return false;
    }

#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
#endif

    g_wsSocket = sock;
    return true;
}

static void send_ws_binary(const void* payload, size_t len)
{
    if (g_wsSocket == INVALID_SOCKET) return;
    uint8_t frame[2048];
    if (len + 14 > sizeof(frame)) return;
    size_t frameLen = 0;
    encode_ws_frame((const uint8_t*)payload, len, frame, &frameLen);
    send(g_wsSocket, (const char*)frame, (int)frameLen, 0);
}

static void send_stun_pings(const char* wanIp, const char* lanIp, uint16_t port);
static void parse_json_field(const char* json, const char* key, char* outVal, size_t maxLen);
static void send_lan_discovery_sweep(const void* pkt, size_t pktLen, uint16_t targetPort, bool fullSubnetSweep);

static void process_incoming_packets() {
    if (g_wsSocket != INVALID_SOCKET) {
        uint8_t wsBuf[4096];
        while (true) {
            int wsBytes = recv(g_wsSocket, (char*)wsBuf, sizeof(wsBuf), 0);
            if (wsBytes <= 0) break;

            size_t offset = 0;
            while (offset < (size_t)wsBytes) {
                uint8_t payload[2048];
                size_t payloadLen = 0;
                size_t consumed = 0;
                uint8_t opcode = 0;
                if (!decode_ws_frame(wsBuf + offset, wsBytes - offset, payload, sizeof(payload), &payloadLen, &consumed, &opcode) || consumed == 0) {
                    break;
                }
                offset += consumed;

                // Handle text JSON messages (opcode 0x1)
                if (opcode == 0x1 && payloadLen > 0) {
                    payload[payloadLen < sizeof(payload) ? payloadLen : sizeof(payload) - 1] = '\0';
                    if (strstr((char*)payload, "PEER_CONNECTED") != NULL) {
                        parse_json_field((char*)payload, "peerWanIp", g_peerWanIp, sizeof(g_peerWanIp));
                        parse_json_field((char*)payload, "peerLanIp", g_peerLanIp, sizeof(g_peerLanIp));
                        const char* pPort = strstr((char*)payload, "\"peerPort\":");
                        if (pPort) g_peerPort = (uint16_t)atoi(pPort + 11);

                        const char* targetIp = (g_peerWanIp[0] != '\0') ? g_peerWanIp : g_peerLanIp;
                        if (targetIp[0] != '\0') {
                            g_remoteAddr.sin_family = AF_INET;
                            g_remoteAddr.sin_port = htons(g_peerPort);
                            g_remoteAddr.sin_addr.s_addr = inet_addr(targetIp);
                            g_hasRemoteAddr = true;
                        }
                        send_stun_pings(g_peerWanIp, g_peerLanIp, g_peerPort);
                    } else if (strstr((char*)payload, "PEER_DISCONNECTED") != NULL) {
                        g_state = NET_STATE_DISCONNECTED;
                        g_hasRemoteAddr = false;
                        g_udpPeerLocked = false;
                        g_udpIsDirect = false;
                    }
                }
                // Handle binary messages (opcode 0x2)
                else if (opcode == 0x2 && payloadLen >= sizeof(NetHeader)) {
                    const NetHeader* hdr = (const NetHeader*)payload;
                    if (hdr->magic == SMS_NET_MAGIC) {
                        if (hdr->type == PKT_HEARTBEAT_CRC) {
                            g_lastPacketTime = std::chrono::steady_clock::now();
                            g_state = NET_STATE_CONNECTED;
                        }
                        else if (hdr->type == PKT_WORLD_STATE && payloadLen >= sizeof(WorldStatePacket)) {
                            const WorldStatePacket* pkt = (const WorldStatePacket*)payload;
                            if (!g_hasReceivedWorldState || pkt->state.frame_index >= g_latestWorldState.frame_index) {
                                g_latestWorldState = pkt->state;
                                g_hasNewWorldState = true;
                                g_hasReceivedWorldState = true;
                            }
                            g_lastPacketTime = std::chrono::steady_clock::now();
                            g_state = NET_STATE_CONNECTED;
                        }
                        else if (hdr->type == PKT_INPUT_FRAME && payloadLen >= sizeof(NetHeader) + 1) {
                            const InputBundlePacket* pkt = (const InputBundlePacket*)payload;
                            int maxCount = (payloadLen - sizeof(NetHeader) - 1) / sizeof(NetPadEntry);
                            int count = std::min((int)pkt->count, std::min(maxCount, 3));
                            for (int i = 0; i < count; ++i) {
                                uint32_t f = pkt->frames[i].frame_index;
                                int idx = f % RING_BUFFER_SIZE;
                                g_remoteRing[idx] = pkt->frames[i];
                                g_remoteRingValid[idx] = true;
                                if (f > g_remoteLastFrame) {
                                    g_remoteLastFrame = f;
                                }
                            }
                            g_lastPacketTime = std::chrono::steady_clock::now();
                            g_state = NET_STATE_CONNECTED;
                        }
                        else if (hdr->type == PKT_DISCONNECT) {
                            g_state = NET_STATE_DISCONNECTED;
                            g_hasRemoteAddr = false;
                            g_udpPeerLocked = false;
                            g_udpIsDirect = false;
                        }
                    }
                }
            }
        }
    }

    if (g_socket == INVALID_SOCKET) return;

    uint8_t buf[2048];
    sockaddr_in sender;

    while (true) {
        socklen_t senderLen = sizeof(sender);
        int bytes = recvfrom(g_socket, (char*)buf, sizeof(buf), 0,
                             (struct sockaddr*)&sender, &senderLen);
        if (bytes <= 0) break;
        if (bytes < (int)sizeof(NetHeader)) continue;

        const NetHeader* hdr = (const NetHeader*)buf;
        if (hdr->magic != SMS_NET_MAGIC) continue;

        // Auto-lock remote peer address on receiving valid network packet
        if (hdr->type == PKT_HEARTBEAT_CRC || hdr->type == PKT_WORLD_STATE || hdr->type == PKT_INPUT_FRAME) {
            auto now = std::chrono::steady_clock::now();
            bool fromRelay = (g_relayIpAddr != 0 && sender.sin_addr.s_addr == g_relayIpAddr && ntohs(sender.sin_port) == 24680);
            bool directTimedOut = g_udpIsDirect && (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastUdpPacketTime).count() > 1500);

            if (!g_udpPeerLocked || (!fromRelay && !g_udpIsDirect) || (fromRelay && directTimedOut)) {
                g_remoteAddr = sender;
                g_hasRemoteAddr = true;
                g_udpPeerLocked = true;
                g_udpIsDirect = !fromRelay;
            }
            g_state = NET_STATE_CONNECTED;
            g_lastPacketTime = now;
            g_lastUdpPacketTime = now;
        }

        switch (hdr->type) {
            case PKT_DISCOVERY_PING: {
                if (g_role == NET_ROLE_HOST && g_state == NET_STATE_BROADCASTING) {
                    DiscoveryPacket pong;
                    pong.header.magic = SMS_NET_MAGIC;
                    pong.header.type = PKT_DISCOVERY_PONG;
                    pong.header.role = (uint8_t)NET_ROLE_HOST;
                    pong.header.seq = 0;
                    strncpy(pong.hostname, g_localHostname, sizeof(pong.hostname) - 1);
                    pong.game_port = NET_DEFAULT_PORT;
                    sendto(g_socket, (const char*)&pong, sizeof(pong), 0,
                           (struct sockaddr*)&sender, sizeof(sender));
                }
                break;
            }

            case PKT_DISCOVERY_PONG: {
                if (g_role == NET_ROLE_CLIENT && (g_state == NET_STATE_SEARCHING || g_state == NET_STATE_DISCONNECTED)) {
                    if (bytes >= (int)sizeof(DiscoveryPacket)) {
                        const DiscoveryPacket* pkt = (const DiscoveryPacket*)buf;
                        const char* senderIp = inet_ntoa(sender.sin_addr);

                        bool found = false;
                        for (auto& h : g_discoveredHosts) {
                            if (strcmp(h.ip, senderIp) == 0) {
                                found = true;
                                strncpy(h.hostname, pkt->hostname, sizeof(h.hostname) - 1);
                                break;
                            }
                        }
                        if (!found && g_discoveredHosts.size() < NET_MAX_DISCOVERED) {
                            NetDiscoveredHost host;
                            strncpy(host.ip, senderIp, sizeof(host.ip) - 1);
                            host.ip[sizeof(host.ip) - 1] = '\0';
                            host.port = pkt->game_port;
                            strncpy(host.hostname, pkt->hostname, sizeof(host.hostname) - 1);
                            host.ping_ms = 0;
                            g_discoveredHosts.push_back(host);
                        }
                    }
                }
                break;
            }

            case PKT_CONNECT_REQ: {
                if (g_role == NET_ROLE_HOST) {
                    g_remoteAddr = sender;
                    g_hasRemoteAddr = true;

                    // Reset ring buffers and frame counters on handshake
                    memset(g_localRing, 0, sizeof(g_localRing));
                    memset(g_remoteRing, 0, sizeof(g_remoteRing));
                    memset(g_remoteRingValid, 0, sizeof(g_remoteRingValid));
                    g_remoteLastFrame = 0;

                    ConnectAckPacket ack;
                    ack.header.magic = SMS_NET_MAGIC;
                    ack.header.type = PKT_CONNECT_ACK;
                    ack.header.role = (uint8_t)NET_ROLE_HOST;
                    ack.header.seq = 0;
                    ack.seed = g_syncSeed;
                    ack.delay_frames = (uint8_t)g_inputDelay;
                    strncpy(ack.hostname, g_localHostname, sizeof(ack.hostname) - 1);

                    sendto(g_socket, (const char*)&ack, sizeof(ack), 0,
                           (struct sockaddr*)&g_remoteAddr, sizeof(g_remoteAddr));

                    g_state = NET_STATE_CONNECTED;
                    g_lastPacketTime = std::chrono::steady_clock::now();
                }
                break;
            }

            case PKT_CONNECT_ACK: {
                if (g_role == NET_ROLE_CLIENT && g_state == NET_STATE_CONNECTING) {
                    if (bytes >= (int)sizeof(ConnectAckPacket)) {
                        const ConnectAckPacket* ack = (const ConnectAckPacket*)buf;
                        g_syncSeed = ack->seed;
                        g_inputDelay = ack->delay_frames;

                        // Reset ring buffers and frame counters on handshake
                        memset(g_localRing, 0, sizeof(g_localRing));
                        memset(g_remoteRing, 0, sizeof(g_remoteRing));
                        memset(g_remoteRingValid, 0, sizeof(g_remoteRingValid));
                        g_remoteLastFrame = 0;

                        g_state = NET_STATE_CONNECTED;
                        g_lastPacketTime = std::chrono::steady_clock::now();
                    }
                }
                break;
            }

            case PKT_INPUT_FRAME: {
                if (g_state == NET_STATE_CONNECTED && bytes >= (int)(sizeof(NetHeader) + 1)) {
                    const InputBundlePacket* pkt = (const InputBundlePacket*)buf;
                    int maxCount = (bytes - (int)sizeof(NetHeader) - 1) / (int)sizeof(NetPadEntry);
                    int count = std::min((int)pkt->count, std::min(maxCount, 3));
                    for (int i = 0; i < count; ++i) {
                        uint32_t f = pkt->frames[i].frame_index;
                        int idx = f % RING_BUFFER_SIZE;
                        g_remoteRing[idx] = pkt->frames[i];
                        g_remoteRingValid[idx] = true;
                        if (f > g_remoteLastFrame) {
                            g_remoteLastFrame = f;
                        }
                    }
                    g_lastPacketTime = std::chrono::steady_clock::now();
                }
                break;
            }

            case PKT_HEARTBEAT_CRC: {
                if (g_state == NET_STATE_CONNECTED && bytes >= (int)sizeof(HeartbeatPacket)) {
                    // Checksum verification can trigger warning if needed
                    g_lastPacketTime = std::chrono::steady_clock::now();
                }
                break;
            }

            case PKT_WORLD_STATE: {
                if (g_state == NET_STATE_CONNECTED && bytes >= (int)sizeof(WorldStatePacket)) {
                    const WorldStatePacket* pkt = (const WorldStatePacket*)buf;
                    if (!g_hasReceivedWorldState || pkt->state.frame_index >= g_latestWorldState.frame_index) {
                        g_latestWorldState = pkt->state;
                        g_hasNewWorldState = true;
                        g_hasReceivedWorldState = true;
                    }
                    g_lastPacketTime = std::chrono::steady_clock::now();

                    static uint32_t s_ballLogCounter = 0;
                    if (++s_ballLogCounter % 50 == 0) {
                        printf("[NETPLAY_BALL_LOG] Frame: %u | Owner: %d | Pos: (%.2f, %.2f, %.2f) | Vel: (%.2f, %.2f, %.2f)\n",
                               pkt->state.frame_index,
                               (int)pkt->state.ball_owner,
                               pkt->state.ball.posX, pkt->state.ball.posY, pkt->state.ball.posZ,
                               pkt->state.ball.velX, pkt->state.ball.velY, pkt->state.ball.velZ);
                        fflush(stdout);
                    }
                }
                break;
            }

            case PKT_DISCONNECT: {
                g_state = NET_STATE_DISCONNECTED;
                g_hasRemoteAddr = false;
                break;
            }
        }
    }
}

void NetPad_ProcessFrame(uint32_t frame_index, const PADStatus* localStatus, PADStatus* outRemoteStatus) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);

    static bool s_envChecked = false;
    if (!s_envChecked) {
        s_envChecked = true;
        const char* role = getenv("STRIKERS_NET_ROLE");
        if (role) {
            if (strcmp(role, "host") == 0) {
                const char* name = getenv("STRIKERS_NET_NAME");
                NetPad_StartHost(name ? name : "SMS-Host", NET_DEFAULT_PORT);
            } else if (strcmp(role, "client") == 0) {
                const char* hostIp = getenv("STRIKERS_NET_HOST");
                NetPad_StartClient(NET_DEFAULT_PORT);
                if (hostIp && hostIp[0] != '\0') {
                    NetPad_ConnectTo(hostIp, NET_DEFAULT_PORT);
                }
            }
        }
    }

    // Periodic discovery ping if searching (Cliente) o anunciando (Host)
    if (g_role == NET_ROLE_CLIENT && g_state == NET_STATE_SEARCHING) {
        static auto lastBroadcast = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBroadcast).count() > 350) {
            lastBroadcast = now;
            NetHeader ping;
            ping.magic = SMS_NET_MAGIC;
            ping.type = PKT_DISCOVERY_PING;
            ping.role = (uint8_t)NET_ROLE_CLIENT;
            ping.seq = 0;
            send_lan_discovery_sweep(&ping, sizeof(ping), NET_DEFAULT_PORT, false);
        }
    } else if (g_role == NET_ROLE_HOST && g_state == NET_STATE_BROADCASTING) {
        static auto lastHostAnnounce = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHostAnnounce).count() > 600) {
            lastHostAnnounce = now;
            DiscoveryPacket pong;
            memset(&pong, 0, sizeof(pong));
            pong.header.magic = SMS_NET_MAGIC;
            pong.header.type = PKT_DISCOVERY_PONG;
            pong.header.role = (uint8_t)NET_ROLE_HOST;
            strncpy(pong.hostname, g_localHostname, sizeof(pong.hostname) - 1);
            pong.game_port = NET_DEFAULT_PORT;
            send_lan_discovery_sweep(&pong, sizeof(pong), NET_DEFAULT_PORT, false);
        }
    }

    process_incoming_packets();

    // Connection timeout check (5 seconds)
    if (g_state == NET_STATE_CONNECTING) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPacketTime).count() > 5000) {
            NetPad_Disconnect(); // Reset state on timeout
            if (outRemoteStatus) {
                memset(outRemoteStatus, 0, sizeof(PADStatus));
                outRemoteStatus->err = PAD_ERR_NO_CONTROLLER;
            }
            return;
        }
    }

    // Reenviar petición de conexión en LAN o pings STUN en WAN cada 400ms mientras conecta
    if (g_state == NET_STATE_CONNECTING || (g_state == NET_STATE_CONNECTED && !g_udpPeerLocked)) {
        static auto lastStunTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStunTime).count() > 400) {
            lastStunTime = now;
            if (g_peerWanIp[0] != '\0' || g_peerLanIp[0] != '\0' || g_wsSocket != INVALID_SOCKET) {
                send_stun_pings(g_peerWanIp, g_peerLanIp, g_peerPort);
            } else if (g_role == NET_ROLE_CLIENT && g_state == NET_STATE_CONNECTING && g_hasRemoteAddr) {
                NetHeader req;
                req.magic = SMS_NET_MAGIC;
                req.type = PKT_CONNECT_REQ;
                req.role = (uint8_t)NET_ROLE_CLIENT;
                req.seq = 0;
                sendto(g_socket, (const char*)&req, sizeof(req), 0,
                       (struct sockaddr*)&g_remoteAddr, sizeof(g_remoteAddr));
            }
        }
    }

    if (g_state != NET_STATE_CONNECTED || (!g_hasRemoteAddr && g_wsSocket == INVALID_SOCKET)) {
        if (outRemoteStatus) {
            // Mientras el Cliente no haya entrado a la partida (sigue en menús eligiendo sala),
            // conservar su input local para que el Puerto 0 no quede muerto.
            if (g_role == NET_ROLE_CLIENT && localStatus != nullptr) {
                *outRemoteStatus = *localStatus;
            } else {
                memset(outRemoteStatus, 0, sizeof(PADStatus));
                outRemoteStatus->err = PAD_ERR_NO_CONTROLLER;
            }
        }
        return;
    }

    // 1. Store local input in local ring buffer
    int localIdx = frame_index % RING_BUFFER_SIZE;
    pad_to_entry(frame_index, localStatus, &g_localRing[localIdx]);

    // 2. Transmit bundled input packet (current frame + previous 2 frames for packet loss tolerance)
    InputBundlePacket pkt;
    pkt.header.magic = SMS_NET_MAGIC;
    pkt.header.type = PKT_INPUT_FRAME;
    pkt.header.role = (uint8_t)g_role;
    pkt.header.seq = (uint16_t)(frame_index & 0xFFFF);
    pkt.count = 0;

    for (int offset = 0; offset < 3; ++offset) {
        if (frame_index >= (uint32_t)offset) {
            uint32_t f = frame_index - offset;
            pkt.frames[pkt.count++] = g_localRing[f % RING_BUFFER_SIZE];
        }
    }

    if (g_hasRemoteAddr) {
        sendto(g_socket, (const char*)&pkt, sizeof(pkt), 0,
               (struct sockaddr*)&g_remoteAddr, sizeof(g_remoteAddr));
    }

    // 2.5 Only send through WS if UDP hasn't locked yet or timed out
    auto now = std::chrono::steady_clock::now();
    bool udpHealthy = g_udpPeerLocked && (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastUdpPacketTime).count() < 800);
    if (!udpHealthy) {
        send_ws_binary(&pkt, sizeof(pkt));
    }

    // 3. Retrieve remote input for the target execution frame
    uint32_t targetFrame = (frame_index >= g_inputDelay) ? (frame_index - g_inputDelay) : 0;
    int targetIdx = targetFrame % RING_BUFFER_SIZE;

    bool remoteApplied = false;
    // 1st preference: exact target frame matched in buffer
    if (g_remoteRingValid[targetIdx] && g_remoteRing[targetIdx].frame_index == targetFrame) {
        entry_to_pad(&g_remoteRing[targetIdx], outRemoteStatus);
        remoteApplied = true;
    }
    // 2nd preference: fallback to latest received frame from remote player
    if (!remoteApplied && g_remoteLastFrame > 0) {
        int lastIdx = g_remoteLastFrame % RING_BUFFER_SIZE;
        if (g_remoteRingValid[lastIdx]) {
            entry_to_pad(&g_remoteRing[lastIdx], outRemoteStatus);
            remoteApplied = true;
        }
    }
    // 3rd: fallback neutral pad with PAD_ERR_NONE so Controller 2 is kept alive
    if (!remoteApplied && outRemoteStatus) {
        memset(outRemoteStatus, 0, sizeof(PADStatus));
        outRemoteStatus->err = PAD_ERR_NONE;
    }
}

NetRole NetPad_GetRole(void) {
    // Mientras el cliente sigue en los menús buscando/seleccionando sala (antes de conectar),
    // devolver NET_ROLE_HOST para que input.cpp mantenga el mando físico en el Puerto 0 (Jugador 1)
    // pero siga llamando a NetPad_ProcessFrame() en cada cuadro.
    if (g_role == NET_ROLE_CLIENT && g_state != NET_STATE_CONNECTED) {
        return NET_ROLE_HOST;
    }
    return g_role;
}

NetState NetPad_GetState(void) {
    return g_state;
}

const char* NetPad_GetStateString(void) {
    switch (g_state) {
        case NET_STATE_DISCONNECTED: return "Desconectado";
        case NET_STATE_BROADCASTING: return "Esperando rival (Host)";
        case NET_STATE_SEARCHING:    return "Buscando partidas...";
        case NET_STATE_CONNECTING:   return "Conectando...";
        case NET_STATE_CONNECTED:    return "En Partida (Conectado)";
        case NET_STATE_DESYNC_ERROR: return "Error de Desincronización";
        default:                     return "Inactivo";
    }
}

int NetPad_GetDiscoveredHosts(NetDiscoveredHost* outHosts, int maxHosts) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    int count = (int)std::min((size_t)maxHosts, g_discoveredHosts.size());
    for (int i = 0; i < count; ++i) {
        outHosts[i] = g_discoveredHosts[i];
    }
    return count;
}

uint32_t NetPad_GetPingMs(void) {
    return g_lastPingMs;
}

uint32_t NetPad_GetRemoteFrame(void) {
    return g_remoteLastFrame;
}

bool NetPad_IsConnected(void) {
    return (g_state == NET_STATE_CONNECTED);
}

void NetPad_SetInputDelay(uint32_t delayFrames) {
    if (delayFrames >= 1 && delayFrames <= 6) {
        g_inputDelay = delayFrames;
    }
}

uint32_t NetPad_GetInputDelay(void) {
    return g_inputDelay;
}

uint32_t NetPad_GetSyncSeed(void) {
    return g_syncSeed;
}

void NetPad_SendChecksum(uint32_t frame_index, uint32_t crc) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (g_state != NET_STATE_CONNECTED) return;

    HeartbeatPacket pkt;
    pkt.header.magic = SMS_NET_MAGIC;
    pkt.header.type = PKT_HEARTBEAT_CRC;
    pkt.header.role = (uint8_t)g_role;
    pkt.header.seq = (uint16_t)(frame_index & 0xFFFF);
    pkt.frame_index = frame_index;
    pkt.crc = crc;

    if (g_hasRemoteAddr) {
        sendto(g_socket, (const char*)&pkt, sizeof(pkt), 0,
               (struct sockaddr*)&g_remoteAddr, sizeof(g_remoteAddr));
    }
}

void NetPad_SendWorldState(const NetWorldState* state) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (g_state != NET_STATE_CONNECTED || g_role != NET_ROLE_HOST || !state) return;

    WorldStatePacket pkt;
    pkt.header.magic = SMS_NET_MAGIC;
    pkt.header.type = PKT_WORLD_STATE;
    pkt.header.role = (uint8_t)g_role;
    pkt.header.seq = (uint16_t)(state->frame_index & 0xFFFF);
    pkt.state = *state;

    if (g_hasRemoteAddr) {
        sendto(g_socket, (const char*)&pkt, sizeof(pkt), 0,
               (struct sockaddr*)&g_remoteAddr, sizeof(g_remoteAddr));
    }

    auto now = std::chrono::steady_clock::now();
    bool udpHealthy = g_udpPeerLocked && (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastUdpPacketTime).count() < 800);
    if (!udpHealthy) {
        send_ws_binary(&pkt, sizeof(pkt));
    }
}

bool NetPad_PollWorldState(NetWorldState* outState) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (!g_hasNewWorldState || !outState) return false;
    *outState = g_latestWorldState;
    g_hasNewWorldState = false;
    return true;
}

bool NetPad_GetLatestWorldState(NetWorldState* outState) {
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (!g_hasReceivedWorldState || !outState) return false;
    *outState = g_latestWorldState;
    return true;
}

static bool https_get_request(const char* host, const char* path, char* response, size_t responseSize)
{
    memset(response, 0, responseSize);

#if defined(_WIN32)
    wchar_t wHost[128] = { 0 };
    wchar_t wPath[512] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wHost, 127);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, 511);

    HINTERNET hSession = WinHttpOpen(L"SMS-Netplay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, wHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD dwFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                    SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE |
                    SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwFlags, sizeof(dwFlags));

    WinHttpSetTimeouts(hRequest, 4000, 4000, 4000, 4000);

    BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    if (bResults) {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        size_t totalBytes = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;

            if (totalBytes + dwSize >= responseSize) dwSize = (DWORD)(responseSize - 1 - totalBytes);
            if (WinHttpReadData(hRequest, (LPVOID)(response + totalBytes), dwSize, &dwDownloaded)) {
                totalBytes += dwDownloaded;
            } else {
                break;
            }
        } while (dwSize > 0 && totalBytes < responseSize - 1);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return (strlen(response) > 0);
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -s -m 5 \"https://%s%s\"", host, path);

    FILE* fp = popen(cmd, "r");
    if (!fp) return false;

    size_t totalBytes = fread(response, 1, responseSize - 1, fp);
    pclose(fp);
    response[totalBytes] = '\0';

    return (totalBytes > 0);
#endif
}

static void get_local_ip(char* outIp, size_t maxLen)
{
    memset(outIp, 0, maxLen);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock != INVALID_SOCKET) {
        sockaddr_in loopback;
        memset(&loopback, 0, sizeof(loopback));
        loopback.sin_family = AF_INET;
        loopback.sin_addr.s_addr = inet_addr("8.8.8.8");
        loopback.sin_port = htons(53);

        if (connect(sock, (struct sockaddr*)&loopback, sizeof(loopback)) == 0) {
            sockaddr_in name;
            socklen_t namelen = sizeof(name);
            if (getsockname(sock, (struct sockaddr*)&name, &namelen) == 0) {
                const char* ipStr = inet_ntoa(name.sin_addr);
                if (ipStr) {
                    strncpy(outIp, ipStr, maxLen - 1);
                }
            }
        }
        CLOSE_SOCKET(sock);
    }
}

static void send_lan_discovery_sweep(const void* pkt, size_t pktLen, uint16_t targetPort, bool fullSubnetSweep)
{
    if (g_socket == INVALID_SOCKET) return;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(targetPort);

    // 1. Broadcast global 255.255.255.255 y loopback 127.0.0.1
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(g_socket, (const char*)pkt, (int)pktLen, 0, (struct sockaddr*)&addr, sizeof(addr));

    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    sendto(g_socket, (const char*)pkt, (int)pktLen, 0, (struct sockaddr*)&addr, sizeof(addr));

    // 2. Recopilar subredes locales activas
    std::vector<uint32_t> subnets;
    char primaryIp[64] = { 0 };
    get_local_ip(primaryIp, sizeof(primaryIp));
    if (primaryIp[0] != '\0') {
        uint32_t ipNet = ntohl(inet_addr(primaryIp)) & 0xFFFFFF00;
        if (ipNet != 0 && ipNet != 0x7F000000) {
            subnets.push_back(ipNet);
        }
    }

    char hostBuf[128] = { 0 };
    if (gethostname(hostBuf, sizeof(hostBuf) - 1) == 0) {
        struct hostent* he = gethostbyname(hostBuf);
        if (he && he->h_addrtype == AF_INET) {
            for (int i = 0; he->h_addr_list[i] != nullptr; ++i) {
                uint32_t ipHostOrder = ntohl(*(uint32_t*)he->h_addr_list[i]);
                uint32_t ipNet = ipHostOrder & 0xFFFFFF00;
                uint8_t b0 = (ipHostOrder >> 24) & 0xFF;
                uint8_t b1 = (ipHostOrder >> 16) & 0xFF;
                bool isPrivate = (b0 == 192 && b1 == 168) || (b0 == 10) || (b0 == 172 && b1 >= 16 && b1 <= 31);
                if (isPrivate && std::find(subnets.begin(), subnets.end(), ipNet) == subnets.end()) {
                    subnets.push_back(ipNet);
                }
            }
        }
    }

    // 3. Enviar a cada broadcast de subred (X.Y.Z.255) y barrido unicast /24 para abrir Firewall de Windows
    for (uint32_t subnet : subnets) {
        addr.sin_addr.s_addr = htonl(subnet | 0xFF);
        sendto(g_socket, (const char*)pkt, (int)pktLen, 0, (struct sockaddr*)&addr, sizeof(addr));

        if (fullSubnetSweep) {
            for (uint32_t host = 1; host < 255; ++host) {
                addr.sin_addr.s_addr = htonl(subnet | host);
                sendto(g_socket, (const char*)pkt, (int)pktLen, 0, (struct sockaddr*)&addr, sizeof(addr));
            }
        }
    }
}

bool NetPad_CreateOnlineRoom(const char* hostName, char* outRoomCode, size_t maxLen)
{
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    g_role = NET_ROLE_HOST;
    g_state = NET_STATE_SEARCHING;
    g_hasRemoteAddr = false;
    g_onlineRoomCode[0] = '\0';
    g_peerWanIp[0] = '\0';
    g_peerLanIp[0] = '\0';
    reset_session_buffers();

    if (g_socket == INVALID_SOCKET) {
        NetPad_Init(NET_DEFAULT_PORT);
    }

    char localIp[64] = { 0 };
    get_local_ip(localIp, sizeof(localIp));

    // Encode hostName to prevent URL issues (basic escaping)
    char safeHost[64] = { 0 };
    for (int i = 0, j = 0; hostName && hostName[i] && j < 63; i++) {
        if (hostName[i] == ' ' || hostName[i] == '&' || hostName[i] == '?') safeHost[j++] = '_';
        else safeHost[j++] = hostName[i];
    }
    if (safeHost[0] == '\0') strcpy(safeHost, "SMS-Host");

    char path[256];
    snprintf(path, sizeof(path), "/api/create_room?localIp=%s&port=%u&hostName=%s", localIp, NET_DEFAULT_PORT, safeHost);

    char resp[1024];
    if (!https_get_request("strikers.aprendizaje-guiado.com", path, resp, sizeof(resp))) {
        return false;
    }

    const char* pCode = strstr(resp, "\"roomCode\":\"");
    if (!pCode) return false;
    pCode += 12;

    char codeBuf[16] = { 0 };
    int i = 0;
    while (*pCode && *pCode != '"' && i < 15) {
        codeBuf[i++] = *pCode++;
    }
    codeBuf[i] = '\0';

    strncpy(g_onlineRoomCode, codeBuf, sizeof(g_onlineRoomCode) - 1);
    if (outRoomCode && maxLen > 0) {
        strncpy(outRoomCode, codeBuf, maxLen - 1);
        outRoomCode[maxLen - 1] = '\0';
    }

    if (connect_websocket_signaling("strikers.aprendizaje-guiado.com", 80)) {
        char bindMsg[128];
        snprintf(bindMsg, sizeof(bindMsg), "{\"type\":\"BIND_WS\",\"roomCode\":\"%s\",\"role\":\"HOST\"}", g_onlineRoomCode);
        send_ws_text(g_wsSocket, bindMsg);
    }

    send_stun_pings(nullptr, nullptr, 0); // Registrar UDP en el reflector anticipadamente

    return true;
}

int NetPad_GetOnlineRooms(NetDiscoveredHost* outHosts, int maxHosts)
{
    char resp[8192];
    if (!https_get_request("strikers.aprendizaje-guiado.com", "/api/list_rooms", resp, sizeof(resp))) {
        return 0;
    }

    // Basic JSON array parser for [{"code":"...", "host":"..."}, ...]
    int count = 0;
    const char* p = resp;
    while ((p = strstr(p, "\"code\":\"")) != NULL && count < maxHosts) {
        p += 8;
        char codeBuf[16] = { 0 };
        int i = 0;
        while (*p && *p != '"' && i < 15) {
            codeBuf[i++] = *p++;
        }
        codeBuf[i] = '\0';

        char hostBuf[32] = "SMS-Host";
        const char* pHost = strstr(p, "\"host\":\"");
        if (pHost) {
            pHost += 8;
            int j = 0;
            while (*pHost && *pHost != '"' && j < 31) {
                hostBuf[j++] = *pHost++;
            }
            hostBuf[j] = '\0';
        }

        strncpy(outHosts[count].ip, codeBuf, sizeof(outHosts[count].ip) - 1); // code -> ip
        outHosts[count].port = 0;
        strncpy(outHosts[count].hostname, hostBuf, sizeof(outHosts[count].hostname) - 1);
        outHosts[count].ping_ms = 0;
        count++;
    }

    return count;
}

static void send_stun_pings(const char* wanIp, const char* lanIp, uint16_t port)
{
    HeartbeatPacket pkt;
    pkt.header.magic = SMS_NET_MAGIC;
    pkt.header.type = PKT_HEARTBEAT_CRC;
    pkt.header.role = (uint8_t)g_role;
    pkt.header.seq = 0;
    pkt.frame_index = 0;
    pkt.crc = 0;

    // Send over WebSocket to unlock state if UDP is blocked by firewall
    send_ws_binary(&pkt, sizeof(pkt));

    if (g_socket == INVALID_SOCKET) return;

    if (wanIp && wanIp[0] != '\0') {
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(wanIp);
        for (int i = 0; i < 2; i++) {
            sendto(g_socket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&addr, sizeof(addr));
        }
    }

    if (lanIp && lanIp[0] != '\0') {
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(lanIp);
        for (int i = 0; i < 2; i++) {
            sendto(g_socket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&addr, sizeof(addr));
        }
    }

    // Resolver DNS una sola vez y guardar en g_relayIpAddr
    if (g_relayIpAddr == 0) {
        struct hostent* he = gethostbyname("strikers.aprendizaje-guiado.com");
        if (he && he->h_addr_list[0]) {
            memcpy(&g_relayIpAddr, he->h_addr_list[0], sizeof(uint32_t));
        }
    }

    if (g_relayIpAddr != 0) {
        sockaddr_in relayAddr;
        memset(&relayAddr, 0, sizeof(relayAddr));
        relayAddr.sin_family = AF_INET;
        relayAddr.sin_port = htons(24680);
        relayAddr.sin_addr.s_addr = g_relayIpAddr;

        for (int i = 0; i < 2; i++) {
            sendto(g_socket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&relayAddr, sizeof(relayAddr));
        }
    }
}

static void parse_json_field(const char* json, const char* key, char* outVal, size_t maxLen)
{
    outVal[0] = '\0';
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\":\"", key);
    const char* p = strstr(json, searchKey);
    if (p) {
        p += strlen(searchKey);
        size_t i = 0;
        while (*p && *p != '"' && i < maxLen - 1) {
            outVal[i++] = *p++;
        }
        outVal[i] = '\0';
    }
}

bool NetPad_PollOnlineRoom(char* outPeerIp, uint16_t* outPeerPort)
{
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (g_role != NET_ROLE_HOST || g_onlineRoomCode[0] == '\0') return false;

    if (g_state == NET_STATE_CONNECTED && (g_peerWanIp[0] != '\0' || g_peerLanIp[0] != '\0')) {
        const char* targetIp = (g_peerWanIp[0] != '\0') ? g_peerWanIp : g_peerLanIp;
        if (outPeerIp) {
            strncpy(outPeerIp, targetIp, 63);
            outPeerIp[63] = '\0';
        }
        if (outPeerPort) *outPeerPort = g_peerPort;
        return true;
    }

    // Rate-limit interno de seguridad (máximo 1 poll cada 1000ms)
    static auto s_lastPollTime = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPollTime).count() < 1000) {
        return false;
    }
    s_lastPollTime = now;

    char path[128];
    snprintf(path, sizeof(path), "/api/poll_room?code=%s", g_onlineRoomCode);

    char resp[1024];
    if (!https_get_request("strikers.aprendizaje-guiado.com", path, resp, sizeof(resp))) {
        return false;
    }

    if (strstr(resp, "\"hasClient\":true") != NULL) {
        parse_json_field(resp, "peerWanIp", g_peerWanIp, sizeof(g_peerWanIp));
        parse_json_field(resp, "peerLanIp", g_peerLanIp, sizeof(g_peerLanIp));

        const char* pPort = strstr(resp, "\"peerPort\":");
        if (pPort) {
            g_peerPort = (uint16_t)atoi(pPort + 11);
        } else {
            g_peerPort = NET_DEFAULT_PORT;
        }

        // Prioritize WAN IP as base target; if on same local LAN, auto-lock on recvfrom will switch to LAN IP
        const char* targetIp = (g_peerWanIp[0] != '\0') ? g_peerWanIp : g_peerLanIp;
        if (targetIp[0] != '\0') {
            g_remoteAddr.sin_family = AF_INET;
            g_remoteAddr.sin_port = htons(g_peerPort);
            g_remoteAddr.sin_addr.s_addr = inet_addr(targetIp);
            g_hasRemoteAddr = true;
        }

        if (outPeerIp) strncpy(outPeerIp, targetIp, 63);
        if (outPeerPort) *outPeerPort = g_peerPort;

        g_state = NET_STATE_CONNECTING;
        // CRÍTICO: Inicializar g_lastPacketTime para que el timeout de 5s en NetPad_ProcessFrame no desconecte al Host
        g_lastPacketTime = std::chrono::steady_clock::now();

        if (g_wsSocket != INVALID_SOCKET) {
            char bindMsg[128];
            snprintf(bindMsg, sizeof(bindMsg), "{\"type\":\"BIND_WS\",\"roomCode\":\"%s\",\"role\":\"HOST\"}", g_onlineRoomCode);
            send_ws_text(g_wsSocket, bindMsg);
        }

        send_stun_pings(g_peerWanIp, g_peerLanIp, g_peerPort);

        return true;
    }
    return false;
}

bool NetPad_JoinOnlineRoom(const char* roomCode)
{
    std::lock_guard<std::recursive_mutex> lock(g_netMutex);
    if (!roomCode || roomCode[0] == '\0') return false;

    g_role = NET_ROLE_CLIENT;
    g_state = NET_STATE_SEARCHING;
    g_hasRemoteAddr = false;
    g_udpPeerLocked = false;
    g_udpIsDirect = false;
    g_hasReceivedWorldState = false;

    if (g_socket == INVALID_SOCKET) {
        NetPad_Init(NET_DEFAULT_PORT);
    }

    char localIp[64] = { 0 };
    get_local_ip(localIp, sizeof(localIp));

    char path[256];
    snprintf(path, sizeof(path), "/api/join_room?code=%s&localIp=%s&port=%u", roomCode, localIp, NET_DEFAULT_PORT);

    char resp[1024];
    if (!https_get_request("strikers.aprendizaje-guiado.com", path, resp, sizeof(resp))) {
        return false;
    }

    if (strstr(resp, "\"type\":\"ROOM_JOINED\"") != NULL) {
        parse_json_field(resp, "peerWanIp", g_peerWanIp, sizeof(g_peerWanIp));
        parse_json_field(resp, "peerLanIp", g_peerLanIp, sizeof(g_peerLanIp));

        const char* pPort = strstr(resp, "\"peerPort\":");
        if (pPort) {
            g_peerPort = (uint16_t)atoi(pPort + 11);
        } else {
            g_peerPort = NET_DEFAULT_PORT;
        }

        const char* targetIp = (g_peerWanIp[0] != '\0') ? g_peerWanIp : g_peerLanIp;
        if (targetIp[0] != '\0') {
            g_remoteAddr.sin_family = AF_INET;
            g_remoteAddr.sin_port = htons(g_peerPort);
            g_remoteAddr.sin_addr.s_addr = inet_addr(targetIp);
            g_hasRemoteAddr = true;
        }

        strncpy(g_onlineRoomCode, roomCode, sizeof(g_onlineRoomCode) - 1);
        g_state = NET_STATE_CONNECTING;
        g_lastPacketTime = std::chrono::steady_clock::now(); // Start connection timeout timer

        if (connect_websocket_signaling("strikers.aprendizaje-guiado.com", 80)) {
            char bindMsg[128];
            snprintf(bindMsg, sizeof(bindMsg), "{\"type\":\"BIND_WS\",\"roomCode\":\"%s\",\"role\":\"CLIENT\"}", g_onlineRoomCode);
            send_ws_text(g_wsSocket, bindMsg);
        }

        send_stun_pings(g_peerWanIp, g_peerLanIp, g_peerPort);

        return true;
    }
    return false;
}

const char* NetPad_GetOnlineRoomCode()
{
    return g_onlineRoomCode;
}
