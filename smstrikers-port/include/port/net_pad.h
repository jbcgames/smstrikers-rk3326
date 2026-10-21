#ifndef PORT_NET_PAD_H
#define PORT_NET_PAD_H

#include <stdint.h>
#include <stdbool.h>
#include "dolphin/pad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_DEFAULT_PORT 24680
#define NET_MAX_DISCOVERED 8

typedef enum {
    NET_ROLE_NONE = 0,
    NET_ROLE_HOST = 1,
    NET_ROLE_CLIENT = 2
} NetRole;

typedef enum {
    NET_STATE_DISCONNECTED = 0,
    NET_STATE_BROADCASTING,  // Host announcing presence
    NET_STATE_SEARCHING,     // Client discovering lobbies
    NET_STATE_CONNECTING,    // Handshake in progress
    NET_STATE_CONNECTED,     // Active in-game netplay
    NET_STATE_DESYNC_ERROR   // Checksum mismatch
} NetState;

typedef struct {
    char ip[32];
    uint16_t port;
    char hostname[32];
    uint32_t ping_ms;
} NetDiscoveredHost;

// Initialize / shutdown socket subsystem
bool NetPad_Init(uint16_t port);
void NetPad_Shutdown(void);

// Role management
void NetPad_StartHost(const char* hostName, uint16_t port);
void NetPad_StartClient(uint16_t port);
void NetPad_ConnectTo(const char* ip, uint16_t port);
void NetPad_Disconnect(void);

// Frame processing - called per simulation tick
// localStatus: input from local device
// outRemoteStatus: populated with remote player's input
void NetPad_ProcessFrame(uint32_t frame_index, const PADStatus* localStatus, PADStatus* outRemoteStatus);

// Status queries for UI / Overlay
NetRole NetPad_GetRole(void);
NetState NetPad_GetState(void);
const char* NetPad_GetStateString(void);
int NetPad_GetDiscoveredHosts(NetDiscoveredHost* outHosts, int maxHosts);
uint32_t NetPad_GetPingMs(void);
uint32_t NetPad_GetRemoteFrame(void);
bool NetPad_IsConnected(void);

// Input delay configuration (1 to 5 frames, default 2)
void NetPad_SetInputDelay(uint32_t delayFrames);
uint32_t NetPad_GetInputDelay(void);

// Seed sync
uint32_t NetPad_GetSyncSeed(void);

// World state replication (Host -> Client)
typedef struct {
    float posX, posY, posZ;
    float velX, velY, velZ;
    uint16_t facing;
    int16_t animID;
} NetCharSync;

typedef struct {
    float posX, posY, posZ;
    float velX, velY, velZ;
} NetBallSync;

typedef struct {
    float posX, posY, posZ;
    uint16_t orientation;
    uint8_t type;
    uint8_t active;
} NetPowerupSync;

typedef struct {
    uint32_t frame_index;
    float game_time;
    uint8_t score[2];
    uint8_t game_state;
    int8_t ball_owner;
    int8_t char_pad[10];

    // ShootToScore & Special Shot Sync
    uint8_t sts_meter_visible;
    uint8_t sts_meter_type;
    float sts_white_bar_pos;
    float sts_green_angle;
    float sts_green_width;
    float sts_yellow_width;
    uint8_t captain_sts_on;

    // Team Powerups (HUD)
    int8_t team_powerups[2][2];
    uint8_t team_num_powerups[2][2];

    // Field 3D Powerups (Up to 8)
    uint8_t num_field_powerups;
    NetPowerupSync field_powerups[8];

    NetBallSync ball;
    NetCharSync characters[10];
} NetWorldState;

void NetPad_SendWorldState(const NetWorldState* state);
bool NetPad_PollWorldState(NetWorldState* outState);
bool NetPad_GetLatestWorldState(NetWorldState* outState);

// Online WAN Room Signaling & NAT Hole Punching
bool NetPad_CreateOnlineRoom(char* outRoomCode, size_t maxLen);
bool NetPad_JoinOnlineRoom(const char* roomCode);
bool NetPad_PollOnlineRoom(char* outPeerIp, uint16_t* outPeerPort);
const char* NetPad_GetOnlineRoomCode();

#ifdef __cplusplus
}
#endif

#endif // PORT_NET_PAD_H
