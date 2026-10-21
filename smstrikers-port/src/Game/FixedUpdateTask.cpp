#include "Game/FixedUpdateTask.h"

#include "Game/AI/AIPad.h"
#include "Game/AI/Powerups.h"
#include "Game/Ball.h"
#include "Game/BasicStadium.h"
#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/CharacterTriggers.h"
#include "Game/AnimInventory.h"
#include "Game/Player.h"
#include "Game/OverlayHandlerHUD.h"
#include "Game/OverlayManager.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/Render/ElectricFence.h"
#include "Game/Render/Wiper.h"
#include "Game/Render/ShootToScoreArrow.h"
#include "Game/Render/ShootToScoreMeter.h"
#include "Game/Field.h"
#include "Game/FlickDetection.h"
#include "Game/Game.h"
#include "Game/Physics/Physics.h"
#include "Game/Physics/PhysicsAIBall.h"
#include "Game/Render/NetMesh.h"
#include "Game/Render/SidelineExplodable.h"
#include "Game/ReplayManager.h"
#include "Game/Sys/clock.h"
#include "Game/Team.h"
#include "NL/platpad.h"
#include "port/net_pad.h"
#include <cstring>

float g_fFixedUpdateTick = 0.02f;
extern PhysicsWorld* g_PhysicsWorld;
extern PowerupBase* g_pPowerups[25];

bool g_bRunSimAndRenderInLockStep;
float g_fSimulationTick = g_fFixedUpdateTick;

float FixedUpdateTask::mAccumulatedDeltaT;
float FixedUpdateTask::mSimulationTime;
float FixedUpdateTask::mfFrameLockTime;
float FixedUpdateTask::mTimeScale = 1.0f;

extern "C" float* PortSimTimeScalePtr(void)
{
    return &FixedUpdateTask::mTimeScale;
}

/**
 * Offset/Address/Size: 0x2D8 | 0x8016E608 | size: 0x30
 */
FixedUpdateTask::FixedUpdateTask()
{
    mAccumulatedDeltaT = g_fFixedUpdateTick;
    mSimulationTime = 0.f;
    mfFrameLockTime = 0.f;
}

/**
 * Offset/Address/Size: 0x2CC | 0x8016E5FC | size: 0xC
 */
const char* FixedUpdateTask::GetName()
{
    return "Game Fixed Update";
}

/**
 * Offset/Address/Size: 0x2C4 | 0x8016E5F4 | size: 0x8
 */
float FixedUpdateTask::GetPhysicsUpdateTick()
{
    return g_fSimulationTick;
}

/**
 * Offset/Address/Size: 0x280 | 0x8016E5B0 | size: 0x44
 */
void FixedUpdateTask::DecrementFrameLock(float fDeltaT)
{
    mfFrameLockTime -= fDeltaT;
    if (mfFrameLockTime < 0.f)
    {
        nlTaskManager::SetNextState(2);
        mfFrameLockTime = 0.f;
    }
}

/**
 * Offset/Address/Size: 0x0 | 0x8016E330 | size: 0x280
 */
void FixedUpdateTask::Run(float dt)
{
    if (nlTaskManager::m_pInstance->m_CurrState == 2)
    {
        float simulationTick;

        mAccumulatedDeltaT += dt * mTimeScale;

        while (g_bRunSimAndRenderInLockStep || mAccumulatedDeltaT >= g_fFixedUpdateTick)
        {
            UseFixedUpdatePad();

            UpdatePlatPad(simulationTick = g_fFixedUpdateTick);
            cPadManager::Update(simulationTick);
            FlickDetection::Update();

            mAccumulatedDeltaT -= g_fFixedUpdateTick;
            if (g_bRunSimAndRenderInLockStep)
            {
                mAccumulatedDeltaT = 0.0f;
            }

            CallFixedUpdateTasks();

            if (g_bRunSimAndRenderInLockStep)
            {
                break;
            }
        }
    }

    UseDefaultPad();
    UpdatePlatPad(dt);
    cPadManager::Update(dt);
    FlickDetection::Update();
}

void FixedUpdateTask::AIUpdateTask(float fDeltaT)
{
    g_pGame->PreUpdate(fDeltaT);
    g_pGame->Update(fDeltaT);
}

void FixedUpdateTask::PrePhysicsAITask(float fDeltaT)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        g_pCharacters[i]->PrePhysicsUpdate(fDeltaT);
    }
}

void FixedUpdateTask::PostPhysicsAITask(float fDeltaT)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        g_pCharacters[i]->PostPhysicsUpdate();
    }
    g_pBall->PostPhysicsUpdate(fDeltaT);
}

static inline nlVector3 MakeVector3(float x, float y, float z)
{
    nlVector3 v;
    v.Set(x, y, z);
    return v;
}

void FixedUpdateTask::CallFixedUpdateTasks()
{
    mSimulationTime += g_fSimulationTick;

    bool isNetplay = NetPad_IsConnected();
    bool isClient = isNetplay && (NetPad_GetRole() == NET_ROLE_CLIENT);
    bool isHost = isNetplay && (NetPad_GetRole() == NET_ROLE_HOST);
    bool inMatch = (g_pGame != NULL && g_pBall != NULL && g_pCharacters[0] != NULL);

    if (isClient && inMatch)
    {
        NetWorldState ws;
        if (NetPad_GetLatestWorldState(&ws))
        {
            // Replicate game clock
            if (g_pGame->m_pGameClock != NULL)
            {
                g_pGame->m_pGameClock->m_fTimer = ws.game_time;
            }

            // Replicate game state
            if (ws.game_state != 255 && g_pGame->GetGameState() != (eGameState)ws.game_state)
            {
                g_pGame->ChangeGameState((eGameState)ws.game_state);
            }

            // Replicate score and HUD
            bool goalScoredOnClient = false;
            uint8_t scoringTeam = 0;
            if (g_pTeams[0] != NULL && ws.score[0] > g_pTeams[0]->m_nScore)
            {
                scoringTeam = 0;
                goalScoredOnClient = true;
            }
            else if (g_pTeams[1] != NULL && ws.score[1] > g_pTeams[1]->m_nScore)
            {
                scoringTeam = 1;
                goalScoredOnClient = true;
            }

            if (g_pTeams[0] != NULL) g_pTeams[0]->m_nScore = ws.score[0];
            if (g_pTeams[1] != NULL) g_pTeams[1]->m_nScore = ws.score[1];

            if (goalScoredOnClient)
            {
                HUDOverlay* HUD = (HUDOverlay*)nlSingleton<OverlayManager>::Instance()->GetScene(OVERLAY_HUD);
                if (HUD != NULL)
                {
                    HUD->UpdateScore();
                    HUD->DisplayNewScore();
                }

                struct GoalScoredDataExt
                {
                    GoalScoredData data;
                    int sideOfInterest;
                };

                GoalScoredDataExt* pGoalScored = new ((u8*)&g_pEventManager->CreateValidEvent(5, 0x3C)->m_data) GoalScoredDataExt();
                pGoalScored->data.uNumGoalsScored = 1;
                pGoalScored->data.uTeamIndex = scoringTeam;
                pGoalScored->data.uGoalType = g_pBall->m_uGoalType;
                pGoalScored->data.uIsHyper = g_pBall->m_unk_0xA5;
                pGoalScored->data.v3ShotPosition = g_pBall->m_v3ShotOrigin;
                pGoalScored->data.pScorer = (g_pTeams[scoringTeam] != NULL) ? g_pTeams[scoringTeam]->GetCaptain() : NULL;
                pGoalScored->data.pAssister = NULL;
                pGoalScored->sideOfInterest = -1;
                pGoalScored->data.pLastTouch[0] = (g_pTeams[0] != NULL) ? g_pTeams[0]->GetCaptain() : NULL;
                pGoalScored->data.pLastTouch[1] = (g_pTeams[1] != NULL) ? g_pTeams[1]->GetCaptain() : NULL;
            }

            // Replicate ShootToScore meter & special moves
            bool wasCaptainStsOn = g_pGame->mbCaptainShotToScoreOn;
            g_pGame->mbCaptainShotToScoreOn = (ws.captain_sts_on != 0);

            if (!wasCaptainStsOn && ws.captain_sts_on)
            {
                WorldDarkening::Instance().Fade(10.0f, 0.8f);
                if (g_pBall->GetDrawableBall() != NULL)
                {
                    g_pBall->GetDrawableBall()->m_uObjectFlags |= 0x40;
                }
            }
            else if (wasCaptainStsOn && !ws.captain_sts_on)
            {
                WorldDarkening::Instance().Fade(-10.0f, 0.0f);
                if (g_pBall->GetDrawableBall() != NULL)
                {
                    g_pBall->GetDrawableBall()->m_uObjectFlags &= ~0x40;
                }
            }

            if (!ShootToScoreMeter::instance.m_bMeterVisible && ws.sts_meter_visible)
            {
                ShootToScoreMeter::instance.TurnOnMeter((ShootToScoreMeter::STSMeterType)ws.sts_meter_type, ws.sts_yellow_width / 360.0f);
                if (ws.ball_owner >= 0 && ws.ball_owner < 10 && g_pCharacters[ws.ball_owner] != NULL)
                {
                    nlVector3 pos = g_pCharacters[ws.ball_owner]->m_v3Position;
                    pos.z += 1.8f;
                    ShootToScoreMeter::instance.m_v3MeterPosition = pos;
                    ShootToScoreMeter::instance.m_v3OriginalMeterPosition = pos;
                }
            }

            ShootToScoreMeter::instance.m_bMeterVisible = (ws.sts_meter_visible != 0);
            if (ws.sts_meter_visible)
            {
                ShootToScoreMeter::instance.m_MeterType = (ShootToScoreMeter::STSMeterType)ws.sts_meter_type;
                ShootToScoreMeter::instance.m_fGreenBarAngle = 180.0f * ws.sts_green_angle;
                ShootToScoreMeter::instance.m_fGreenRegionWidth = 180.0f * ws.sts_green_width;
                ShootToScoreMeter::instance.m_fYellowRegionWidth = 180.0f * ws.sts_yellow_width;
                ShootToScoreMeter::instance.SetWhiteBarPosition(ws.sts_white_bar_pos);
                if (ws.ball_owner >= 0 && ws.ball_owner < 10 && g_pCharacters[ws.ball_owner] != NULL)
                {
                    nlVector3 pos = g_pCharacters[ws.ball_owner]->m_v3Position;
                    pos.z += 1.8f;
                    ShootToScoreMeter::instance.m_v3MeterPosition = pos;
                    ShootToScoreMeter::instance.m_v3OriginalMeterPosition = pos;
                }
            }

            // Replicate Team Powerups (HUD)
            bool powChanged = false;
            for (int t = 0; t < 2; t++)
            {
                if (g_pTeams[t] != NULL)
                {
                    if (g_pTeams[t]->m_ePowerupList[0].eType != (ePowerUpType)ws.team_powerups[t][0] ||
                        g_pTeams[t]->m_ePowerupList[1].eType != (ePowerUpType)ws.team_powerups[t][1] ||
                        g_pTeams[t]->m_ePowerupList[0].nnumOfPowerups != ws.team_num_powerups[t][0] ||
                        g_pTeams[t]->m_ePowerupList[1].nnumOfPowerups != ws.team_num_powerups[t][1])
                    {
                        g_pTeams[t]->m_ePowerupList[0].eType = (ePowerUpType)ws.team_powerups[t][0];
                        g_pTeams[t]->m_ePowerupList[1].eType = (ePowerUpType)ws.team_powerups[t][1];
                        g_pTeams[t]->m_ePowerupList[0].nnumOfPowerups = ws.team_num_powerups[t][0];
                        g_pTeams[t]->m_ePowerupList[1].nnumOfPowerups = ws.team_num_powerups[t][1];
                        powChanged = true;
                    }
                }
            }
            if (powChanged)
            {
                HUDOverlay* HUD = (HUDOverlay*)nlSingleton<OverlayManager>::Instance()->GetScene(OVERLAY_HUD);
                if (HUD != NULL)
                {
                    HUD->DisplayPowerUps();
                }
            }

            // Ensure HUD overlay is visible on client
            nlSingleton<OverlayManager>::Instance()->SetVisible(OVERLAY_HUD, true, true);

            // Detect item explosions / impacts on client
            static struct PrevPowerupState {
                uint8_t active;
                uint8_t type;
                float posX, posY, posZ;
            } s_prevFieldPowerups[8] = { 0 };

            for (int k = 0; k < 8; k++)
            {
                bool prevActive = s_prevFieldPowerups[k].active;
                bool currActive = (k < ws.num_field_powerups) ? ws.field_powerups[k].active : 0;

                if (prevActive && !currActive)
                {
                    uint8_t type = s_prevFieldPowerups[k].type;
                    nlVector3 impactPos;
                    impactPos.x = s_prevFieldPowerups[k].posX;
                    impactPos.y = s_prevFieldPowerups[k].posY;
                    impactPos.z = s_prevFieldPowerups[k].posZ;

                    EffectsGroup* pGroup = NULL;
                    if (type == POWER_UP_BOBOMB)
                    {
                        pGroup = fxGetGroup("bobomb_explode_med");
                        Wiper::Instance().DoWipe("camera_shake");
                    }
                    else if (type == POWER_UP_GREEN_SHELL)
                    {
                        pGroup = fxGetGroup("green_shell_explode");
                    }
                    else if (type == POWER_UP_RED_SHELL)
                    {
                        pGroup = fxGetGroup("red_shell_explode");
                    }
                    else if (type == POWER_UP_SPINY_SHELL)
                    {
                        pGroup = fxGetGroup("spiny_shell_explode");
                    }
                    else if (type == POWER_UP_FREEZE_SHELL)
                    {
                        pGroup = fxGetGroup("freeze_shell_explode");
                    }
                    else if (type == POWER_UP_BANANA)
                    {
                        pGroup = fxGetGroup("banana_explode");
                    }

                    if (pGroup != NULL)
                    {
                        EmissionController* pEc = EmissionManager::Create(pGroup, 0);
                        if (pEc != NULL)
                        {
                            pEc->SetPosition(impactPos);
                        }
                    }
                }

                s_prevFieldPowerups[k].active = currActive;
                if (currActive)
                {
                    s_prevFieldPowerups[k].type = ws.field_powerups[k].type;
                    s_prevFieldPowerups[k].posX = ws.field_powerups[k].posX;
                    s_prevFieldPowerups[k].posY = ws.field_powerups[k].posY;
                    s_prevFieldPowerups[k].posZ = ws.field_powerups[k].posZ;
                }
            }

            // Detect ball impact against electric fence walls on client
            static nlVector3 s_prevBallVel = { 0.0f, 0.0f, 0.0f };
            nlVector3 currBallVel = { ws.ball.velX, ws.ball.velY, ws.ball.velZ };
            nlVector3 ballPos = { ws.ball.posX, ws.ball.posY, ws.ball.posZ };

            if (fabsf(ballPos.x) >= 26.5f || fabsf(ballPos.y) >= 16.5f)
            {
                float velDiff = nlGetLength3D(currBallVel.x - s_prevBallVel.x, currBallVel.y - s_prevBallVel.y, currBallVel.z - s_prevBallVel.z);
                if (velDiff > 8.0f)
                {
                    nlVector3 wallNormal = { 0.0f, 0.0f, 0.0f };
                    if (fabsf(ballPos.x) >= 26.5f) wallNormal.x = (ballPos.x > 0.0f) ? -1.0f : 1.0f;
                    else wallNormal.y = (ballPos.y > 0.0f) ? -1.0f : 1.0f;

                    EmitElectricFenceBallEffect(ballPos, wallNormal, 0, false);
                }
            }
            s_prevBallVel = currBallVel;

            // Replicate ball
            g_pBall->m_v3Position.Set(ws.ball.posX, ws.ball.posY, ws.ball.posZ);
            g_pBall->WarpTo(MakeVector3(ws.ball.posX, ws.ball.posY, ws.ball.posZ));
            g_pBall->SetVelocity(MakeVector3(ws.ball.velX, ws.ball.velY, ws.ball.velZ), SPINTYPE_NONE, NULL);

            if (g_pBall->GetDrawableBall() != NULL)
            {
                g_pBall->GetDrawableBall()->m_translation.Set(ws.ball.posX, ws.ball.posY, ws.ball.posZ);
                g_pBall->GetDrawableBall()->m_worldMatrixUpToDate = 0;
                g_pBall->GetDrawableBall()->m_uObjectFlags |= 1;
            }

            if (ws.ball_owner >= 0 && ws.ball_owner < 10 && g_pCharacters[ws.ball_owner] != NULL)
            {
                if (g_pBall->m_pOwner != (cPlayer*)g_pCharacters[ws.ball_owner])
                {
                    g_pBall->SetOwner((cPlayer*)g_pCharacters[ws.ball_owner]);
                }
            }
            else if (g_pBall->m_pOwner != NULL)
            {
                g_pBall->ClearOwner();
            }

            g_pBall->SetVisible(true);
            g_pBall->Update(g_fSimulationTick);

            // Replicate characters
            for (int i = 0; i < 10; i++)
            {
                cCharacter* pChar = g_pCharacters[i];
                if (pChar == NULL)
                    continue;

                g_pCurrentlyUpdatingCharacter = pChar;

                pChar->SetPosition(MakeVector3(ws.characters[i].posX, ws.characters[i].posY, ws.characters[i].posZ));
                pChar->SetVelocity(MakeVector3(ws.characters[i].velX, ws.characters[i].velY, ws.characters[i].velZ));
                pChar->SetFacingDirection(ws.characters[i].facing);

                if (ws.characters[i].animID >= 0 && pChar->m_eAnimID != ws.characters[i].animID)
                {
                    int newAnimID = ws.characters[i].animID;
                    pChar->SetAnimState(newAnimID, true, 0.0f, false, false);

                    const char* animName = (pChar->m_pAnimInventory != NULL && newAnimID < pChar->m_pAnimInventory->m_nNumProperties) ? pChar->m_pAnimInventory->m_pAnimProperties[newAnimID].enumName : NULL;
                    if (animName != NULL)
                    {
                        if (strstr(animName, "electrocution") != NULL || strstr(animName, "electric") != NULL)
                        {
                            nlVector3 effectPos = pChar->m_v3Position;
                            effectPos.z += 0.6f;
                            nlVector3 wallNormal;
                            if (fabsf(pChar->m_v3Position.x) > fabsf(pChar->m_v3Position.y))
                            {
                                wallNormal.x = (pChar->m_v3Position.x > 0.0f) ? -1.0f : 1.0f;
                                wallNormal.y = 0.0f;
                                wallNormal.z = 0.0f;
                            }
                            else
                            {
                                wallNormal.x = 0.0f;
                                wallNormal.y = (pChar->m_v3Position.y > 0.0f) ? -1.0f : 1.0f;
                                wallNormal.z = 0.0f;
                            }

                            CharacterElectrocutionEffect(pChar, effectPos, wallNormal);
                            pChar->SetElectrocutionTextureEnabled(true);
                        }
                    }
                }

                int8_t padIdx = ws.char_pad[i];
                if (padIdx >= 0 && padIdx < 4)
                {
                    if (((cPlayer*)pChar)->m_pController != &AIPadManager::mAIPads[padIdx])
                    {
                        ((cPlayer*)pChar)->SetAIPad(&AIPadManager::mAIPads[padIdx]);
                    }
                }
                else if (((cPlayer*)pChar)->m_pController != NULL)
                {
                    ((cPlayer*)pChar)->SetAIPad(NULL);
                }

                if (pChar->m_pPoseTree != NULL)
                {
                    pChar->m_pPoseTree = pChar->m_pPoseTree->Update(g_fSimulationTick);
                }

                g_pCurrentlyUpdatingCharacter = NULL;
            }
        }

        // On client: skip AIUpdateTask, NPC manager AI, and physics engine.
        // Update player poses and ball presentation tasks:
        PrePhysicsAITask(g_fSimulationTick);
        PostPhysicsAITask(g_fSimulationTick);
    }
    else
    {
        // Normal authoritative execution (Host or local match)
        AIUpdateTask(g_fSimulationTick);
        BasicStadium::GetCurrentStadium()->mpNPCManager->UpdateAINPCs(g_fSimulationTick);
        PrePhysicsAITask(g_fSimulationTick);
        PhysicsUpdate(g_PhysicsWorld, g_fSimulationTick);
        PostPhysicsAITask(g_fSimulationTick);

        if (isHost && inMatch)
        {
            static uint32_t s_netFrameIndex = 0;
            NetWorldState ws;
            memset(&ws, 0, sizeof(ws));
            ws.frame_index = ++s_netFrameIndex;
            ws.game_time = (g_pGame->m_pGameClock != NULL) ? g_pGame->m_pGameClock->m_fTimer : 0.0f;
            ws.score[0] = (g_pTeams[0] != NULL) ? (uint8_t)g_pTeams[0]->m_nScore : 0;
            ws.score[1] = (g_pTeams[1] != NULL) ? (uint8_t)g_pTeams[1]->m_nScore : 0;
            ws.game_state = (uint8_t)g_pGame->GetGameState();

            ws.sts_meter_visible = ShootToScoreMeter::instance.m_bMeterVisible ? 1 : 0;
            ws.sts_white_bar_pos = ShootToScoreMeter::instance.m_fWhiteBarAngle / 180.0f;
            ws.sts_green_angle = ShootToScoreMeter::instance.m_fGreenBarAngle / 180.0f;
            ws.sts_green_width = ShootToScoreMeter::instance.m_fGreenRegionWidth / 180.0f;
            ws.sts_yellow_width = ShootToScoreMeter::instance.m_fYellowRegionWidth / 180.0f;
            ws.sts_meter_type = (uint8_t)ShootToScoreMeter::instance.m_MeterType;
            ws.captain_sts_on = g_pGame->mbCaptainShotToScoreOn ? 1 : 0;

            for (int t = 0; t < 2; t++)
            {
                if (g_pTeams[t] != NULL)
                {
                    ws.team_powerups[t][0] = (int8_t)g_pTeams[t]->m_ePowerupList[0].eType;
                    ws.team_powerups[t][1] = (int8_t)g_pTeams[t]->m_ePowerupList[1].eType;
                    ws.team_num_powerups[t][0] = (uint8_t)g_pTeams[t]->m_ePowerupList[0].nnumOfPowerups;
                    ws.team_num_powerups[t][1] = (uint8_t)g_pTeams[t]->m_ePowerupList[1].nnumOfPowerups;
                }
            }

            uint8_t pCount = 0;
            for (int i = 0; i < 25 && pCount < 8; i++)
            {
                PowerupBase* pPow = g_pPowerups[i];
                if (pPow != NULL)
                {
                    ws.field_powerups[pCount].posX = pPow->m_v3Position.x;
                    ws.field_powerups[pCount].posY = pPow->m_v3Position.y;
                    ws.field_powerups[pCount].posZ = pPow->m_v3Position.z;
                    ws.field_powerups[pCount].orientation = (uint16_t)pPow->m_aOrientation;
                    ws.field_powerups[pCount].type = (uint8_t)pPow->m_eType;
                    ws.field_powerups[pCount].active = 1;
                    pCount++;
                }
            }
            ws.num_field_powerups = pCount;

            ws.ball_owner = (g_pBall->m_pOwner != NULL) ? (int8_t)GetCharacterIndex(g_pBall->m_pOwner) : -1;
            ws.ball.posX = g_pBall->m_v3Position.x;
            ws.ball.posY = g_pBall->m_v3Position.y;
            ws.ball.posZ = g_pBall->m_v3Position.z;
            ws.ball.velX = g_pBall->m_v3Velocity.x;
            ws.ball.velY = g_pBall->m_v3Velocity.y;
            ws.ball.velZ = g_pBall->m_v3Velocity.z;

            for (int i = 0; i < 10; i++)
            {
                cCharacter* pChar = g_pCharacters[i];
                if (pChar != NULL)
                {
                    ws.characters[i].posX = pChar->m_v3Position.x;
                    ws.characters[i].posY = pChar->m_v3Position.y;
                    ws.characters[i].posZ = pChar->m_v3Position.z;
                    ws.characters[i].velX = pChar->m_v3Velocity.x;
                    ws.characters[i].velY = pChar->m_v3Velocity.y;
                    ws.characters[i].velZ = pChar->m_v3Velocity.z;
                    ws.characters[i].facing = pChar->m_aActualFacingDirection;
                    ws.characters[i].animID = (int16_t)pChar->m_eAnimID;

                    int8_t padIdx = -1;
                    if (((cPlayer*)pChar)->m_pController != NULL)
                    {
                        for (int p = 0; p < 4; p++)
                        {
                            if (((cPlayer*)pChar)->m_pController == &AIPadManager::mAIPads[p])
                            {
                                padIdx = (int8_t)p;
                                break;
                            }
                        }
                    }
                    ws.char_pad[i] = padIdx;
                }
                else
                {
                    ws.characters[i].animID = -1;
                    ws.char_pad[i] = -1;
                }
            }

            NetPad_SendWorldState(&ws);
        }
    }

    if (NetMesh::s_bAnimatedNetMeshEnabled)
    {
        bool i = true;
        float goalieX = (float)fabs(g_pTeams[0]->GetGoalie()->m_v3Position.x);
        if (goalieX > cField::GetGoalLineX(1U))
        {
        }
        else
        {
            goalieX = (float)fabs(g_pTeams[1]->GetGoalie()->m_v3Position.x);
            if (goalieX > cField::GetGoalLineX(1U))
            {
            }
            else
            {
                i = false;
            }
        }

        cBall* pBall = g_pBall;
        PhysicsAIBall* pPhysicsBall = pBall->m_pPhysicsBall;
        NetMesh::spPositiveXNetMesh->Update(g_fSimulationTick, pBall->m_v3Position, pBall->m_v3PrevPosition, i, pPhysicsBall);
        pPhysicsBall = (pBall = g_pBall)->m_pPhysicsBall;
        NetMesh::spNegativeXNetMesh->Update(g_fSimulationTick, pBall->m_v3Position, pBall->m_v3PrevPosition, i, pPhysicsBall);
    }

    SidelineExplodableManager::Update(g_fSimulationTick);
    ReplayManager::Instance()->GrabSnapshot();
}
