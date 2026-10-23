#include "Game/SH/SHMainMenu.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/Audio/WorldAudio.h"
#include "Game/FE/FEAudio.h"
#include "Game/FE/feFinder.h"
#include "Game/FE/feMusic.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/FE/feOptionsSubMenus.h"
#include "Game/FE/feScene.h"
#include "Game/FE/tlImageInstance.h"
#include "Game/SH/SHBackground.h"
#include "Game/main.h"
#include "port/net_pad.h"
#include "NL/gl/glStruct.h"
#include "NL/nlLocalization.h"
#include "NL/nlPrint.h"

#include "NL/nlBind.h"

typedef Detail::MemFunImpl<void, void (SHMainMenu::*)(TLComponentInstance*)> MainMenuMemFun_t;
typedef BindExp2<void, MainMenuMemFun_t, SHMainMenu*, Placeholder<0> > MainMenuBind_t;

extern nlColour MenuHighliteColour;


bool SHMainMenu::mSnapMenuIntoPosition = false;
int SHMainMenu::mLastMenuItem = 0;

static char sSlideIn[] = "in";
static char sSlideOut[] = "out";

static unsigned long sUnlockedTickerMessages[SHMainMenu::NUM_ITEMS] = {
    0x10B8D08F,
    0xDB24E3FA,
    0x0755A109,
    0x35AABB0D,
    0x1B176F3B,
    0x268EF6F5,
    0x7B1F3B7E,
};

static unsigned long sLockedTickerMessages[SHMainMenu::NUM_ITEMS] = {
    0x10B8D08F,
    0xDB24E3FA,
    0x0F4F37A4,
    0x35AABB0D,
    0x1B176F3B,
    0x268EF6F5,
    0x7B1F3B7E,
};

/**
 * Offset/Address/Size: 0x1B14 | 0x800AB570 | size: 0x78
 */
static void onSelectFriendly(TLComponentInstance*)
{
    GameInfoManager::Instance()->SetMode(GameInfoManager::GM_FRIENDLY);
    GameInfoManager::Instance()->SetTeam(0, (eTeamID)3);
    GameInfoManager::Instance()->SetTeam(1, (eTeamID)3);
    GameSceneManager::Instance()->PopEntireStack();
    GameInfoManager::Instance()->ResetPlayingSides();
    GameSceneManager::Instance()->Push(SCENE_CHOOSE_CAPTAINS, SCREEN_FORWARD, false);
}

/**
 * Offset/Address/Size: 0x1AD0 | 0x800AB52C | size: 0x44
 */
static void onSelectCup(TLComponentInstance*)
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_CUP_CHOOSE_CUP, SCREEN_FORWARD, false);
}

/**
 * Offset/Address/Size: 0x1A50 | 0x800AB4AC | size: 0x80
 */
static void onSelectSuperCup(TLComponentInstance*)
{
    if (!GameInfoManager::Instance()->IsSuperCupModeUnlocked())
    {
        FEPopupMenu* menu = (FEPopupMenu*)GameSceneManager::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
        menu->Create(POPUP_SUPER_CUPS_LOCKED);
    }
    else
    {
        GameSceneManager::Instance()->PopEntireStack();
        GameSceneManager::Instance()->Push(SCENE_SUPER_CUP_CHOOSE_CUP, SCREEN_FORWARD, false);
    }
}

/**
 * Offset/Address/Size: 0x19C4 | 0x800AB420 | size: 0x8C
 */
typedef BasicString<unsigned short, Detail::TempStringAllocator> WStr;

static WStr* MakeWideString(const char* ascii) {
    static unsigned short s_wbufs[16][256];
    static int s_wbufIdx = 0;
    int idx = s_wbufIdx;
    s_wbufIdx = (s_wbufIdx + 1) % 16;

    size_t len = 0;
    while (ascii && ascii[len] != '\0' && len < 255) {
        s_wbufs[idx][len] = (unsigned char)ascii[len];
        len++;
    }
    s_wbufs[idx][len] = 0;

    return new (8, false) WStr(s_wbufs[idx], s_wbufs[idx] + len);
}

enum OnlineMenuState {
    OM_NONE,
    OM_HOST_WAITING_ROOM,
    OM_HOST_WAITING_LAN,
    OM_CLIENT_SEARCHING_LAN,
    OM_HANDSHAKING
};

enum OnlineMenuAction {
    ACTION_NONE,
    ACTION_CREATE_ROOM,
    ACTION_LIST_ROOMS,
    ACTION_JOIN_ROOM,
    ACTION_LAN_MENU,
    ACTION_LAN_HOST,
    ACTION_LAN_SEARCH,
    ACTION_SHOW_LAN_RESULTS,
    ACTION_CONNECT_LAN_HOST,
    ACTION_SHOW_TIMEOUT_ERROR
};

static OnlineMenuState s_onlineState = OM_NONE;
static uint32_t s_onlineStateCounter = 0;
static OnlineMenuAction s_pendingAction = ACTION_NONE;
static char s_pendingJoinCode[16] = { 0 };
static char s_lanIps[4][32];
static uint16_t s_lanPorts[4];
static int s_selectedLanIdx = 0;

static TLComponentInstance* s_onlineCompInstance = NULL;

static void ApplyOnlineLabel(TLComponentInstance* compinstance) {
    if (!compinstance) return;
    static unsigned short s_wOnlineLabel[32];
    static bool s_init = false;
    if (!s_init) {
        nlStrToWcs("ONLINE", s_wOnlineLabel, 32);
        s_init = true;
    }
    SetAllTextInComponent(compinstance, s_wOnlineLabel);
    if (compinstance->GetActiveSlide()) {
        TLTextInstance* text = FEFinder<TLTextInstance, 3>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("R JUST")));
        if (text) {
            text->SetString(s_wOnlineLabel);
        }
    }
}

static void launchOnlineMatch()
{
    GameInfoManager::Instance()->SetMode(GameInfoManager::GM_FRIENDLY);
    GameInfoManager::Instance()->SetTeam(0, (eTeamID)3);
    GameInfoManager::Instance()->SetTeam(1, (eTeamID)3);
    GameSceneManager::Instance()->PopEntireStack();
    GameInfoManager::Instance()->ResetPlayingSides();
    GameSceneManager::Instance()->Push(SCENE_CHOOSE_CAPTAINS, SCREEN_FORWARD, false);
    FEMusic::StartStreamIfDifferent(0);
}

static void pushHandshakeWaitingPopup() {
    FEPopupMenu* handMenu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
    handMenu->mPopup.pMessage = MakeWideString("Conectando con el rival...");
    handMenu->mPopup.pOptionLabels[0] = MakeWideString("Cancelar");
    for (int i = 1; i < 4; i++) handMenu->mPopup.pOptionLabels[i] = NULL;
    handMenu->mPopup.numOptions = 1;
    handMenu->mMenuCreated = true;
    handMenu->mType = POPUP_ABOUTTOSAVE;
    handMenu->mHighlightedOption = 0;
    handMenu->callBacks[0] = Function<FnVoidVoid>([]() {
        NetPad_Disconnect();
        s_onlineState = OM_NONE;
    });
    handMenu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
        NetPad_Disconnect();
        s_onlineState = OM_NONE;
    }));
}

static void onActionCreateOnlineRoom()
{
    char roomCode[16] = { 0 };
    if (NetPad_CreateOnlineRoom("SMS-Player", roomCode, sizeof(roomCode))) {
        s_onlineState = OM_HOST_WAITING_ROOM;
        s_onlineStateCounter = 0;

        FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
        if (!menu) return;

        char msgBuf[64];
        snprintf(msgBuf, sizeof(msgBuf), "SALA CREADA - CODIGO: [ %s ]", roomCode);
        menu->mPopup.pMessage = MakeWideString(msgBuf);
        menu->mPopup.pOptionLabels[0] = MakeWideString("Cancelar Sala");
        for (int i = 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
        menu->mPopup.numOptions = 1;
        menu->mMenuCreated = true;
        menu->mType = POPUP_ABOUTTOSAVE;
        menu->mHighlightedOption = 0;
        menu->mUnknownAA4 = false;

        menu->callBacks[0] = Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        });
        menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        }));
    } else {
        FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
        if (!menu) return;
        menu->mPopup.pMessage = MakeWideString("Error al conectar con el servidor.");
        menu->mPopup.pOptionLabels[0] = MakeWideString("Aceptar");
        for (int i = 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
        menu->mPopup.numOptions = 1;
        menu->mMenuCreated = true;
        menu->mType = POPUP_ABOUTTOSAVE;
        menu->mHighlightedOption = 0;
        menu->mUnknownAA4 = false;
        menu->callBacks[0] = Function<FnVoidVoid>(FEPopupMenu::Nothing);
        menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        }));
    }
}

static void onActionListOnlineRooms()
{
    NetPad_StartClient(24680);
    NetDiscoveredHost hosts[3];
    int count = NetPad_GetOnlineRooms(hosts, 3);

    FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
    if (!menu) return;

    if (count > 0) {
        if (count > 3) count = 3; // Reservar 1 slot para "Cancelar"
        menu->mPopup.pMessage = MakeWideString("SELECCIONA UNA SALA ONLINE:");
        menu->mPopup.numOptions = count + 1;

        static char s_roomCodes[3][16];
        for (int i = 0; i < count; i++) {
            strncpy(s_roomCodes[i], hosts[i].ip, sizeof(s_roomCodes[i]) - 1);
            s_roomCodes[i][sizeof(s_roomCodes[i]) - 1] = '\0';
            char optLabel[48];
            snprintf(optLabel, sizeof(optLabel), "Sala [ %s ] (%s)", hosts[i].ip, hosts[i].hostname);
            menu->mPopup.pOptionLabels[i] = MakeWideString(optLabel);
        }
        menu->mPopup.pOptionLabels[count] = MakeWideString("Cancelar");
        for (int i = count + 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;

        menu->mMenuCreated = true;
        menu->mType = POPUP_ABOUTTOSAVE;
        menu->mHighlightedOption = 0;

        menu->callBacks[0] = Function<FnVoidVoid>([]() {
            strncpy(s_pendingJoinCode, s_roomCodes[0], sizeof(s_pendingJoinCode) - 1);
            s_pendingAction = ACTION_JOIN_ROOM;
        });
        if (count > 1) {
            menu->callBacks[1] = Function<FnVoidVoid>([]() {
                strncpy(s_pendingJoinCode, s_roomCodes[1], sizeof(s_pendingJoinCode) - 1);
                s_pendingAction = ACTION_JOIN_ROOM;
            });
        }
        if (count > 2) {
            menu->callBacks[2] = Function<FnVoidVoid>([]() {
                strncpy(s_pendingJoinCode, s_roomCodes[2], sizeof(s_pendingJoinCode) - 1);
                s_pendingAction = ACTION_JOIN_ROOM;
            });
        }
        menu->callBacks[count] = Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        });
        menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        }));
    } else {
        menu->mPopup.pMessage = MakeWideString("No hay salas online disponibles.");
        menu->mPopup.pOptionLabels[0] = MakeWideString("Aceptar");
        for (int i = 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
        menu->mPopup.numOptions = 1;
        menu->mMenuCreated = true;
        menu->mType = POPUP_ABOUTTOSAVE;
        menu->mHighlightedOption = 0;
        menu->callBacks[0] = Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        });
        menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        }));
    }
}

static void onActionLANMode()
{
    FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
    if (!menu) return;

    menu->mPopup.pMessage = MakeWideString("MODO DE RED LOCAL (LAN / WI-FI):");
    menu->mPopup.pOptionLabels[0] = MakeWideString("Iniciar Servidor LAN (Host)");
    menu->mPopup.pOptionLabels[1] = MakeWideString("Buscar Partidas LAN (Cliente)");
    menu->mPopup.pOptionLabels[2] = MakeWideString("Volver");
    menu->mPopup.pOptionLabels[3] = NULL;
    menu->mPopup.numOptions = 3;
    menu->mMenuCreated = true;
    menu->mType = POPUP_ABOUTTOSAVE;
    menu->mHighlightedOption = 0;

    menu->callBacks[0] = Function<FnVoidVoid>([]() { s_pendingAction = ACTION_LAN_HOST; });
    menu->callBacks[1] = Function<FnVoidVoid>([]() { s_pendingAction = ACTION_LAN_SEARCH; });
    menu->callBacks[2] = Function<FnVoidVoid>(FEPopupMenu::Nothing);
    menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
        NetPad_Disconnect();
        s_onlineState = OM_NONE;
    }));
}

static void onActionShowLANResults()
{
    NetDiscoveredHost hosts[3];
    int count = NetPad_GetDiscoveredHosts(hosts, 3);

    FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
    if (!menu) return;

    if (count > 0) {
        if (count > 3) count = 3; // Reservar 1 slot para "Cancelar"
        menu->mPopup.pMessage = MakeWideString("PARTIDAS LAN ENCONTRADAS:");
        menu->mPopup.numOptions = count + 1;
        for (int i = 0; i < count; i++) {
            strncpy(s_lanIps[i], hosts[i].ip, 31);
            s_lanIps[i][31] = '\0';
            s_lanPorts[i] = hosts[i].port;
            char optLabel[48];
            snprintf(optLabel, sizeof(optLabel), "%s (%s)", hosts[i].hostname, hosts[i].ip);
            menu->mPopup.pOptionLabels[i] = MakeWideString(optLabel);
        }
        menu->mPopup.pOptionLabels[count] = MakeWideString("Cancelar");
        for (int i = count + 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
        menu->mMenuCreated = true;
        menu->mType = POPUP_ABOUTTOSAVE;
        menu->mHighlightedOption = 0;

        menu->callBacks[0] = Function<FnVoidVoid>([]() {
            s_selectedLanIdx = 0;
            s_pendingAction = ACTION_CONNECT_LAN_HOST;
        });
        if (count > 1) {
            menu->callBacks[1] = Function<FnVoidVoid>([]() {
                s_selectedLanIdx = 1;
                s_pendingAction = ACTION_CONNECT_LAN_HOST;
            });
        }
        if (count > 2) {
            menu->callBacks[2] = Function<FnVoidVoid>([]() {
                s_selectedLanIdx = 2;
                s_pendingAction = ACTION_CONNECT_LAN_HOST;
            });
        }
        menu->callBacks[count] = Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        });
        menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        }));
    } else {
        menu->mPopup.pMessage = MakeWideString("No se encontraron partidas en LAN.");
        menu->mPopup.pOptionLabels[0] = MakeWideString("Aceptar");
        for (int i = 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
        menu->mPopup.numOptions = 1;
        menu->mMenuCreated = true;
        menu->mType = POPUP_ABOUTTOSAVE;
        menu->mHighlightedOption = 0;
        menu->callBacks[0] = Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        });
        menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
            NetPad_Disconnect();
            s_onlineState = OM_NONE;
        }));
    }
}

static void onSelectOnline(TLComponentInstance*)
{
    FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);

    menu->mPopup.pMessage = MakeWideString("SUPER MARIO STRIKERS - MULTIJUGADOR ONLINE");
    menu->mPopup.pOptionLabels[0] = MakeWideString("Crear Sala Online (Host)");
    menu->mPopup.pOptionLabels[1] = MakeWideString("Buscar Salas Activas");
    menu->mPopup.pOptionLabels[2] = MakeWideString("Multijugador Red Local (LAN)");
    menu->mPopup.pOptionLabels[3] = MakeWideString("Cancelar");
    menu->mPopup.numOptions = 4;
    menu->mMenuCreated = true;
    menu->mType = POPUP_ABOUTTOSAVE;
    menu->mHighlightedOption = 0;

    menu->callBacks[0] = Function<FnVoidVoid>([]() { s_pendingAction = ACTION_CREATE_ROOM; });
    menu->callBacks[1] = Function<FnVoidVoid>([]() { s_pendingAction = ACTION_LIST_ROOMS; });
    menu->callBacks[2] = Function<FnVoidVoid>([]() { s_pendingAction = ACTION_LAN_MENU; });
    menu->callBacks[3] = Function<FnVoidVoid>(FEPopupMenu::Nothing);
    menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
        NetPad_Disconnect();
        s_onlineState = OM_NONE;
    }));
}

extern "C" void NetMenu_Tick() {
    static int s_actionDelayFrames = 0;
    if (s_pendingAction != ACTION_NONE) {
        if (++s_actionDelayFrames < 2) {
            return; // Esperar a que GameSceneManager termine el Pop() del popup anterior
        }
        s_actionDelayFrames = 0;
        OnlineMenuAction act = s_pendingAction;
        s_pendingAction = ACTION_NONE;

        switch (act) {
            case ACTION_CREATE_ROOM:
                onActionCreateOnlineRoom();
                break;
            case ACTION_LIST_ROOMS:
                onActionListOnlineRooms();
                break;
            case ACTION_JOIN_ROOM: {
                if (NetPad_JoinOnlineRoom(s_pendingJoinCode)) {
                    s_onlineState = OM_HANDSHAKING;
                    s_onlineStateCounter = 0;
                    pushHandshakeWaitingPopup();
                }
                break;
            }
            case ACTION_LAN_MENU:
                onActionLANMode();
                break;
            case ACTION_LAN_HOST: {
                NetPad_StartHost("SMS-Host", 24680);
                s_onlineState = OM_HOST_WAITING_LAN;
                s_onlineStateCounter = 0;

                FEPopupMenu* hostMenu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
                if (hostMenu) {
                    hostMenu->mPopup.pMessage = MakeWideString("SERVIDOR LAN ACTIVO - Esperando rival...");
                    hostMenu->mPopup.pOptionLabels[0] = MakeWideString("Cancelar Servidor");
                    for (int i = 1; i < 4; i++) hostMenu->mPopup.pOptionLabels[i] = NULL;
                    hostMenu->mPopup.numOptions = 1;
                    hostMenu->mMenuCreated = true;
                    hostMenu->mType = POPUP_ABOUTTOSAVE;
                    hostMenu->mHighlightedOption = 0;
                    hostMenu->mUnknownAA4 = false;
                    hostMenu->callBacks[0] = Function<FnVoidVoid>([]() {
                        NetPad_Disconnect();
                        s_onlineState = OM_NONE;
                    });
                    hostMenu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
                        NetPad_Disconnect();
                        s_onlineState = OM_NONE;
                    }));
                }
                break;
            }
            case ACTION_LAN_SEARCH: {
                NetPad_StartClient(24680);
                s_onlineState = OM_CLIENT_SEARCHING_LAN;
                s_onlineStateCounter = 0;

                FEPopupMenu* searchMenu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
                if (searchMenu) {
                    searchMenu->mPopup.pMessage = MakeWideString("Buscando partidas en red local...");
                    searchMenu->mPopup.pOptionLabels[0] = MakeWideString("Cancelar");
                    for (int i = 1; i < 4; i++) searchMenu->mPopup.pOptionLabels[i] = NULL;
                    searchMenu->mPopup.numOptions = 1;
                    searchMenu->mMenuCreated = true;
                    searchMenu->mType = POPUP_ABOUTTOSAVE;
                    searchMenu->mHighlightedOption = 0;
                    searchMenu->mUnknownAA4 = false;
                    searchMenu->callBacks[0] = Function<FnVoidVoid>([]() {
                        NetPad_Disconnect();
                        s_onlineState = OM_NONE;
                    });
                    searchMenu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
                        NetPad_Disconnect();
                        s_onlineState = OM_NONE;
                    }));
                }
                break;
            }
            default:
                break;
        }
        return;
    }

    if (s_onlineState == OM_NONE) return;

    switch (s_onlineState) {
        case OM_NONE:
            break;

        case OM_HOST_WAITING_ROOM: {
            const char* onlineCode = NetPad_GetOnlineRoomCode();
            if (onlineCode && onlineCode[0] != '\0' && NetPad_GetRole() == NET_ROLE_HOST) {
                static uint32_t s_pollCounter = 0;
                if (++s_pollCounter % 30 == 0) {
                    char peerIp[64];
                    uint16_t peerPort;
                    if (NetPad_PollOnlineRoom(peerIp, &peerPort)) {
                        s_onlineState = OM_HANDSHAKING;
                        s_onlineStateCounter = 0;
                    }
                }
            } else {
                s_onlineState = OM_NONE;
            }
            break;
        }

        case OM_HOST_WAITING_LAN: {
            if (NetPad_GetState() == NET_STATE_CONNECTED) {
                s_onlineState = OM_NONE;
                nlSingleton<GameSceneManager>::Instance()->PopEntireStack();
                launchOnlineMatch();
            }
            break;
        }

        case OM_CLIENT_SEARCHING_LAN: {
            if (++s_onlineStateCounter >= 90) { // ~1.5s search window for UDP broadcasts
                s_onlineState = OM_NONE;
                NetDiscoveredHost hosts[4];
                int count = NetPad_GetDiscoveredHosts(hosts, 4);

                // Pop searching popup first
                nlSingleton<GameSceneManager>::Instance()->Pop();

                // Push new popup from scratch
                FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
                if (count > 0) {
                    menu->mPopup.pMessage = MakeWideString("PARTIDAS LAN / WI-FI ENCONTRADAS:");
                    menu->mPopup.numOptions = count;
                    static char s_lanIps[4][32];
                    static uint16_t s_lanPorts[4];
                    for (int i = 0; i < count && i < 4; i++) {
                        strncpy(s_lanIps[i], hosts[i].ip, 31);
                        s_lanIps[i][31] = '\0';
                        s_lanPorts[i] = hosts[i].port;
                        char optLabel[64];
                        snprintf(optLabel, sizeof(optLabel), "%s (%s:%d)", hosts[i].hostname, hosts[i].ip, hosts[i].port);
                        menu->mPopup.pOptionLabels[i] = MakeWideString(optLabel);
                    }
                    for (int i = count; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
                    menu->mMenuCreated = true;
                    menu->mType = POPUP_ABOUTTOSAVE;
                    menu->mHighlightedOption = 0;

                    menu->callBacks[0] = Function<FnVoidVoid>([]() {
                        NetPad_ConnectTo(s_lanIps[0], s_lanPorts[0]);
                        s_onlineState = OM_HANDSHAKING;
                        s_onlineStateCounter = 0;
                        pushHandshakeWaitingPopup();
                    });
                    if (count > 1) {
                        menu->callBacks[1] = Function<FnVoidVoid>([]() {
                            NetPad_ConnectTo(s_lanIps[1], s_lanPorts[1]);
                            s_onlineState = OM_HANDSHAKING;
                            s_onlineStateCounter = 0;
                            pushHandshakeWaitingPopup();
                        });
                    }
                    if (count > 2) {
                        menu->callBacks[2] = Function<FnVoidVoid>([]() {
                            NetPad_ConnectTo(s_lanIps[2], s_lanPorts[2]);
                            s_onlineState = OM_HANDSHAKING;
                            s_onlineStateCounter = 0;
                            pushHandshakeWaitingPopup();
                        });
                    }
                    if (count > 3) {
                        menu->callBacks[3] = Function<FnVoidVoid>([]() {
                            NetPad_ConnectTo(s_lanIps[3], s_lanPorts[3]);
                            s_onlineState = OM_HANDSHAKING;
                            s_onlineStateCounter = 0;
                            pushHandshakeWaitingPopup();
                        });
                    }
                    menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
                        NetPad_Disconnect();
                        s_onlineState = OM_NONE;
                    }));
                } else {
                    menu->mPopup.pMessage = MakeWideString("No se encontraron partidas en la red local Wi-Fi.");
                    menu->mPopup.pOptionLabels[0] = MakeWideString("Aceptar");
                    for (int i = 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
                    menu->mPopup.numOptions = 1;
                    menu->mMenuCreated = true;
                    menu->mType = POPUP_ABOUTTOSAVE;
                    menu->mHighlightedOption = 0;
                    menu->callBacks[0] = Function<FnVoidVoid>(FEPopupMenu::Nothing);
                    menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
                        NetPad_Disconnect();
                        s_onlineState = OM_NONE;
                    }));
                }
            }
            break;
        }

        case OM_HANDSHAKING: {
            if (NetPad_GetState() == NET_STATE_CONNECTED) {
                s_onlineState = OM_NONE;
                nlSingleton<GameSceneManager>::Instance()->PopEntireStack();
                launchOnlineMatch();
            } else if (NetPad_GetState() == NET_STATE_DISCONNECTED) {
                s_onlineState = OM_NONE;
                nlSingleton<GameSceneManager>::Instance()->Pop();
                FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
                menu->mPopup.pMessage = MakeWideString("Error: Tiempo de conexion agotado con el rival.");
                menu->mPopup.pOptionLabels[0] = MakeWideString("Aceptar");
                for (int i = 1; i < 4; i++) menu->mPopup.pOptionLabels[i] = NULL;
                menu->mPopup.numOptions = 1;
                menu->mMenuCreated = true;
                menu->mType = POPUP_ABOUTTOSAVE;
                menu->mHighlightedOption = 0;
                menu->callBacks[0] = Function<FnVoidVoid>(FEPopupMenu::Nothing);
                menu->SetBackButtonCallback(Function<FnVoidVoid>([]() {
                    NetPad_Disconnect();
                    s_onlineState = OM_NONE;
                }));
            }
            break;
        }
    }
}

/**
 * Offset/Address/Size: 0x1964 | 0x800AB3C0 | size: 0x60
 */
static void newTourn()
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_TOURN_SETPARAMS, SCREEN_FORWARD, false);
    if (GameInfoManager::Instance()->mCustomTournamentInfo.m_cupConstructed)
    {
        GameInfoManager::Instance()->mCustomTournamentInfo.m_cup->mCupStarted = false;
    }
}

/**
 * Offset/Address/Size: 0x1914 | 0x800AB370 | size: 0x50
 */
static void continueTourn()
{
    GameInfoManager::Instance()->SetMode(GameInfoManager::GM_TOURNAMENT);
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_TOURNAMENT_STANDINGS, SCREEN_FORWARD, false);
}

/**
 * Offset/Address/Size: 0x182C | 0x800AB288 | size: 0xE8
 */
static void confirmNewTourn()
{
    FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
    menu->Create(
        POPUP_REALLY_OVERWRITE,
        Function<FnVoidVoid>(newTourn),
        Function<FnVoidVoid>(FEPopupMenu::Nothing));
    menu->mUnknownAA4 = false;
}

/**
 * Offset/Address/Size: 0x1680 | 0x800AB0DC | size: 0x1AC
 */
static void onSelectTournament(TLComponentInstance*)
{
    if (GameInfoManager::Instance()->mCustomTournamentInfo.m_cupConstructed
        && GameInfoManager::Instance()->mCustomTournamentInfo.m_cup->mCupStarted)
    {
        FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
        menu->Create(
            POPUP_START_NEW_TOURNAMENT,
            Function<FnVoidVoid>(continueTourn),
            Function<FnVoidVoid>(confirmNewTourn));
        menu->SetBackButtonCallback(Function<FnVoidVoid>(FEPopupMenu::Nothing));
        GameInfoManager::Instance()->SetMode(GameInfoManager::GM_TOURNAMENT);
    }
    else
    {
        GameSceneManager::Instance()->PopEntireStack();
        GameSceneManager::Instance()->Push(SCENE_TOURN_SETPARAMS, SCREEN_FORWARD, false);

        if (GameInfoManager::Instance()->mCustomTournamentInfo.m_cupConstructed)
        {
            GameInfoManager::Instance()->mCustomTournamentInfo.m_cup->mCupStarted = false;
        }
    }
}

/**
 * Offset/Address/Size: 0x1634 | 0x800AB090 | size: 0x4C
 */
static void onSelectTrophies(TLComponentInstance*)
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_TROPHY_ROOM, SCREEN_FORWARD, false);
    FEMusic::StartStreamIfDifferent(6);
}

/**
 * Offset/Address/Size: 0x15E8 | 0x800AB044 | size: 0x4C
 */
static void onSelectOptions(TLComponentInstance*)
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_OPTIONS, SCREEN_FORWARD, false);
    FEMusic::StartStreamIfDifferent(7);
}

/**
 * Offset/Address/Size: 0x1538 | 0x800AAF94 | size: 0xB0
 */
SHMainMenu::SHMainMenu()
    : BaseSceneHandler()
    , m_itemDescriptions(NULL)
    , mMenuItems()
    , mButtons()
{
    mHighlightColour = MenuHighliteColour;
}

/**
 * Offset/Address/Size: 0x1380 | 0x800AADDC | size: 0x158
 */
SHMainMenu::~SHMainMenu()
{
    if (m_itemDescriptions != NULL)
    {
        delete m_itemDescriptions;
    }
}

static BackgroundScene* GetBackgroundScene()
{
    BackgroundScene* bgscene = (BackgroundScene*)GameSceneManager::Instance()->GetScene(SCENE_MARIO_BACKGROUND);
    return bgscene;
}

/**
 * Offset/Address/Size: 0xA3C | 0x800AA498 | size: 0x8C0
 */
void SHMainMenu::SceneCreated()
{
    typedef MenuItem<TLComponentInstance>::Callback MenuCallback;
    static const char* MenuNameTable[] = { "MENU ITEM1", "MENU ITEM2", "MENU ITEM3", "MENU ITEM4", "MENU ITEM7", "MENU ITEM5", "MENU ITEM6" };
    static void (*const ApplyFuncTable[])(TLComponentInstance*) = { onSelectFriendly, onSelectCup, onSelectSuperCup, onSelectTournament, onSelectOnline, onSelectTrophies, onSelectOptions };
    FEMusic::StartStreamIfDifferent(0);
    FEPresentation* presentation = m_pFEScene->m_pFEPackage->GetPresentation();
    TLTextInstance* scrollText;
    const gl_ScreenInfo* screenInfo;
    char menuname[64];
    TLComponentInstance* buttons;
    MenuItem<TLComponentInstance>* item;
    BackgroundScene* scene;
    scrollText = FEFinder<TLTextInstance, 3>::Find<TLSlide>(
        presentation->m_currentSlide,
        InlineHasher(nlStringLowerHash("Layer")),
        InlineHasher(nlStringLowerHash("TickerText")));

    screenInfo = glGetScreenInfo();
    FEScrollText* scrollTextDisplay = new (nlMalloc(sizeof(FEScrollText), 8, false)) FEScrollText(scrollText, 0, screenInfo->ScreenWidth + 50);
    m_itemDescriptions = scrollTextDisplay;
    scene = GetBackgroundScene();
    scene->SetVisible(false);
    for (int i = 0; i < NUM_ITEMS; i++)
    {
        nlSNPrintf(menuname, 64, "MENU ITEM%d", i + 1);
        TLInstance* instance = FEFinder<TLInstance, 4>::Find<TLSlide>(
            presentation->m_currentSlide,
            InlineHasher(nlStringLowerHash("Layer")),
            InlineHasher(nlStringLowerHash(MenuNameTable[i])));
        TLComponentInstance* compinstance = (TLComponentInstance*)instance;

        if (i == 4)
        {
            s_onlineCompInstance = compinstance;
            ApplyOnlineLabel(compinstance);
        }

        item = mMenuItems.AddItem(compinstance);

        {
            MenuCallback openFunc(Bind<void>(MemFun<SHMainMenu, void, TLComponentInstance*>(&SHMainMenu::OpenItem), this, placeholder0));
            item->SetCallback(ON_HIGHLIGHT, openFunc);
        }

        {
            MenuCallback closeFunc(Bind<void>(MemFun<SHMainMenu, void, TLComponentInstance*>(&SHMainMenu::CloseItem), this, placeholder0));
            item->SetCallback(ON_UNHIGHLIGHT, closeFunc);
        }

        {
            MenuItem<TLComponentInstance>::Callback applyFunc(ApplyFuncTable[i]);
            item->SetCallback(ON_APPLY, applyFunc);
        }

        item->SetLockedFlag(false);
        if (i == mLastMenuItem)
        {
            OpenItem(compinstance);
            item->SetDisabledFlag(false);
        }
        else if (i == 2 && !g_e3_Build && !nlSingleton<GameInfoManager>::Instance()->IsSuperCupModeUnlocked())
        {
            CloseItem(compinstance);
            item->SetLockedFlag(true);
        }
        else
        {
            CloseItem(compinstance);
            if (g_e3_Build)
            {
                item->SetDisabledFlag(true);
            }
        }
        if (i == 2)
        {
            TLComponentInstance* lockedType = item->GetType();
            TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
                lockedType->GetActiveSlide(),
                InlineHasher(nlStringLowerHash("locked")),
                InlineHasher(0));
            u8 locked = item->IsLocked();
            lockedComp->m_bVisible = (bool)locked;
        }
    }
    mMenuItems.SetItem(mLastMenuItem);
    mMenuItems.SetFlag(1);
    scene->SetVisible(true);
    scene->mDesiredPlayMode = PM_STOP_AT_END;
    buttons = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        presentation->m_currentSlide,
        InlineHasher(nlStringLowerHash("Layer")),
        InlineHasher(nlStringLowerHash("buttons")));
    mButtons.mButtonInstance = buttons;
    mButtons.SetState(ButtonComponent::BS_A_AND_B);
    if (mSnapMenuIntoPosition)
    {
        FEAudio::EnableSounds(false);
        TLSlide* slide = presentation->m_currentSlide;
        BaseSceneHandler::Update(slide->m_start + slide->m_duration);
        FEAudio::EnableSounds(true);
    }
    mMenuItems.RunCallbackOnCurrent(ON_HIGHLIGHT);
    mSnapMenuIntoPosition = true;
}

/**
 * Offset/Address/Size: 0x60C | 0x800AA068 | size: 0x430
 */
void SHMainMenu::OpenItem(TLComponentInstance* compinstance)
{

    compinstance->SetActiveSlide(sSlideIn);
    compinstance->Update(0.0f);

    if (compinstance == s_onlineCompInstance || mMenuItems.GetActiveItemIndex() == 4)
    {
        ApplyOnlineLabel(compinstance);
    }

    TLComponentInstance* highlight = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("high")));

    highlight->SetActiveSlide(sSlideIn);
    highlight->Update(0.0f);

    TLComponentInstance* flash = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("flasher")));

    flash->SetActiveSlide("Slide1");
    flash->Update(0.0f);

    FEFinder<TLImageInstance, 2>::Find<TLSlide>(
        highlight->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("may_highlite")))
        ->SetAssetColour(mHighlightColour);

    if (mMenuItems.GetMenuItem()->IsDisabled())
    {

        TLTextInstance* text = FEFinder<TLTextInstance, 3>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("R JUST")));

        text->m_LocStrId = 0x38202C30;
        text->m_OverloadFlags |= 8;
    }

    if (mMenuItems.GetMenuItem()->IsLocked())
    {

        TLTextInstance* text = FEFinder<TLTextInstance, 3>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("R JUST")));

        if (g_pLocalization->m_CurrentLanguage == nlLocalization::LangJapanese)
        {
            text->m_LocStrId = 0x67452206;
            text->m_OverloadFlags |= 8;
        }
        else
        {
            text->m_LocStrId = 0x2A68AC55;
            text->m_OverloadFlags |= 8;
        }

        TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("locked")));

        if (lockedComp != NULL)
        {
            lockedComp->m_bVisible = true;
        }
    }
    else
    {

        TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("locked")));

        if (lockedComp != NULL)
        {
            lockedComp->m_bVisible = false;
        }
    }

    if (mMenuItems.GetActiveItemIndex() == 4)
    {
        m_itemDescriptions->SetDisplayMessage("Crear y buscar salas online para jugar por internet o red local.");
    }
    else if (mMenuItems.GetMenuItem()->IsLocked())
    {
        m_itemDescriptions->SetDisplayMessage(sLockedTickerMessages[mMenuItems.GetActiveItemIndex()]);
    }
    else
    {
        m_itemDescriptions->SetDisplayMessage(sUnlockedTickerMessages[mMenuItems.GetActiveItemIndex()]);
    }

    BaseSceneHandler* scene = nlSingleton<GameSceneManager>::Instance()->GetScene(SCENE_MARIO_BACKGROUND);
    if (scene->m_bVisible)
    {
        FEAudio::PlayAnimAudioEvent("sfx_main_menu_highlight_open", false);
    }
}

/**
 * Offset/Address/Size: 0x404 | 0x800A9E60 | size: 0x208
 */
void SHMainMenu::CloseItem(TLComponentInstance* compinstance)
{

    compinstance->SetActiveSlide(sSlideOut);
    compinstance->Update(0.0f);

    TLComponentInstance* highlight = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("high")));

    highlight->SetActiveSlide(sSlideOut);
    highlight->Update(0.0f);

    FEFinder<TLImageInstance, 2>::Find<TLSlide>(
        highlight->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("may_highlite")))
        ->SetAssetColour(mHighlightColour);

    BaseSceneHandler* scene = nlSingleton<GameSceneManager>::Instance()->GetScene(SCENE_MARIO_BACKGROUND);
    if (scene->m_bVisible)
    {
        Audio::gWorldSFX.Stop((Audio::eWorldSFX)0xC, cGameSFX::SFX_STOP_FIRST);
        FEAudio::PlayAnimAudioEvent("sfx_main_menu_highlight_close", false);
    }

    if (compinstance == s_onlineCompInstance)
    {
        ApplyOnlineLabel(compinstance);
    }

    TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("locked")));

    if (lockedComp != NULL)
    {
        if (mMenuItems.GetMenuItem()->IsLocked())
        {
            lockedComp->m_bVisible = true;
        }
        else
        {
            lockedComp->m_bVisible = false;
        }
    }
}

/**
 * Offset/Address/Size: 0x0 | 0x800A9A5C | size: 0x404
 */
void SHMainMenu::Update(float fDeltaT)
{
    NetMenu_Tick();
    BaseSceneHandler::Update(fDeltaT);
    mButtons.CentreButtons();

    FEPresentation* presentation = m_pFEScene->m_pFEPackage->GetPresentation();
    TLSlide* slide = presentation->m_currentSlide;

    if (presentation->m_fadeDuration >= slide->m_start + slide->m_duration)
    {
        m_itemDescriptions->Update(fDeltaT);
    }
    else
    {
        return;
    }

    if (g_pFEInput->IsAutoPressed(FE_ALL_PADS, 0xE, true, NULL))
    {
        mMenuItems.NextItem();
        return;
    }

    if (g_pFEInput->IsAutoPressed(FE_ALL_PADS, 0xD, true, NULL))
    {
        mMenuItems.PreviousItem();
        return;
    }

    eFEINPUT_PAD padused;
    if (g_pFEInput->JustPressed(FE_ALL_PADS, 0x100, false, &padused))
    {
        if (mMenuItems.RunCallbackOnCurrent(ON_APPLY) == RES_OK)
        {
            GameInfoManager::Instance()->mMainUserPadNumber = padused;
            FEAudio::PlayAnimAudioEvent("sfx_accept", false);
            mLastMenuItem = mMenuItems.GetActiveItemIndex();
        }
        else
        {
            FEAudio::PlayAnimAudioEvent("sfx_deny", false);
        }

        return;
    }

    if (g_pFEInput->JustPressed(FE_ALL_PADS, 0x200, false, NULL))
    {
        GameSceneManager::Instance()->PopEntireStack();
        GameSceneManager::Instance()->Push(SCENE_TITLE, SCREEN_BACK, false);
    }
}
