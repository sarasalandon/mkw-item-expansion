#include <MarioKartWii/Item/ItemBehaviour.hpp>
#include <MarioKartWii/Driver/DriverManager.hpp>
#include <MarioKartWii/Audio/AudioManager.hpp>
#include <core/nw4r/snd/SoundStartable.hpp>
#include <core/nw4r/snd/SoundArchivePlayer.hpp>
#include <PulsarSystem.hpp>
#include <Gamemodes/KO/KOMgr.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>
#include <include/c_math.h>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Kart/KartCollision.hpp>
#include <MarioKartWii/Item/Obj/KouraRed.hpp>
#include <core/egg/3D/Scn.hpp>
#include <Network/PacketExpansion.hpp>
#include <MarioKartWii/3D/Camera/CameraMgr.hpp>
#include <MarioKartWii/3D/Model/ModelDirector.hpp>
#include <Extensions/ItemExpansion/Boo.hpp>
#include <Extensions/ItemExpansion/ItemObjDrop.hpp>
#include <MarioKartWii/Race/Racedata.hpp>
#include <MarioKartWii/RKNet/ITEM.hpp>
#include <MarioKartWii/UI/Ctrl/CtrlRace/CtrlRace2DMap.hpp>
#include <MarioKartWii/UI/Ctrl/CtrlRace/CtrlRaceBalloon.hpp>
#include <MarioKartWii/Effect/EffectMgr.hpp>
#include <core/rvl/OS/OS.hpp>
#include <Sound/ExtraSound.hpp>

// Please make sure to credit sarasalandon (Lanny on Tockdom) if you decide to use or modify this code in your own project!

// Expanded behaviourTable in mod BSS (from ItemSlotExpansion.cpp)
extern "C" Item::Behavior expandedBehaviourTable[ITEM_MAX];

namespace Pulsar {
namespace Race {

static const u32 BOO_DURATION_FRAMES = 450;      // 7.5 seconds at 60fps
static const u32 BOO_STEAL_DELAY_FRAMES = 120;   // 2 seconds delay before item given
static const u32 DRAW_OPTIONS_NORMAL = 0xA;
static const u32 DRAW_OPTIONS_TRANSPARENT = 1;
static const u8 NO_PLAYER = 0xFF;

enum BooVisual {
    BOO_VISUAL_NORMAL,
    BOO_VISUAL_TRANSPARENT,
    BOO_VISUAL_INVISIBLE
};

struct BooPlayer {
    ItemId stolenItem;  // what the steal hands over (a Mushroom when nobody ahead had anything)
    u32 stolenCount;
    u16 booFrames;      // Boo frames left
    u16 stealFrames;    // frames until stolenItem arrives, 0 = no steal pending
    u8 visual;          // BooVisual the kart's models have
};
static BooPlayer booPlayers[12];

// Bit n is set from player n's Boo use until the frame their models are back to normal. A pending steal or a Boo look
// only exists while it is set, so with no Boo out every hook below, and UpdateBooStates, costs a single load.
static u16 booMask = 0;
static u32 booFramesSinceLoad = 0;

static inline bool IsInBoo(u32 playerId) {
    return playerId < 12 && ((booMask >> playerId) & 1) != 0;
}

static inline bool IsKartInBoo(const Kart::Link& kart) {
    return booMask != 0 && IsInBoo(kart.GetPlayerIdx());
}

bool IsPlayerInBooState(u8 playerId) {
    return IsInBoo(playerId);
}

static inline bool HasValidVtable(const void* ptr) {
    if (ptr == nullptr) return false;
    const u32 vtable = *static_cast<const u32*>(ptr);
    return (vtable >= 0x80000000 && vtable < 0x81800000) ||
           (vtable >= 0x90000000 && vtable < 0x94000000);
}

// Boo laugh: SE_BOO in /sound/revo_extra.brsar (Sound/ExtraSound, built with Tools/RsarEditor).
static nw4r::snd::SoundHandle booSoundHandle;
static bool booSoundFailLogged = false;
static void PlayBooLaugh() {
    if (booSoundHandle.basicSound != nullptr) return; // previous laugh still playing
    if (Sound::Extra::Play(Sound::Extra::EXTRA_SND_SE_BOO, booSoundHandle)) return;
    if (!booSoundFailLogged) {
        OS::Report("[Boo] EXTRA_SND_SE_BOO did not play (extra sound archive ready: %d)\n", Sound::Extra::IsReady());
        booSoundFailLogged = true;
    }
}

static bool IsFullBooInvisibilityEnabled() {
    return System::sInstance != nullptr && System::sInstance->IsContext(PULSAR_BOO_FULL_INVIS);
}

static bool IsLocalRacePlayer(u8 playerId) {
    if (playerId >= 12 || Racedata::sInstance == nullptr) return false;
    return Racedata::sInstance->racesScenario.players[playerId].playerType == PLAYER_REAL_LOCAL;
}

static bool IsLocalViewerSpectating() {
    const System* system = System::sInstance;
    if (system != nullptr) {
        if (system->IsContext(PULSAR_MODE_KO) && system->koMgr != nullptr && system->koMgr->isSpectating) return true;
    }
    const Racedata* racedata = Racedata::sInstance;
    return racedata != nullptr && racedata->racesScenario.localPlayerCount == 0;
}

static bool CanHideBooPlayerCompletely(u8 playerId) {
    return IsFullBooInvisibilityEnabled() && !IsLocalRacePlayer(playerId) && !IsLocalViewerSpectating();
}

// Only ever Toggle(true) here: the game recomputes culling every frame, and a Toggle(false) from a RaceFrameHook
// un-culls far karts right before hits create their effects (late/repeated spin stars)
static void HideBooEffects(u8 playerId) {
    Effects::Mgr* effectsMgr = Effects::Mgr::sInstance;
    if (effectsMgr == nullptr || effectsMgr->players == nullptr) return;
    if (playerId >= effectsMgr->playerCount) return;

    Effects::Player* playerEffects = effectsMgr->players[playerId];
    if (playerEffects == nullptr) return;

    playerEffects->KillEffects(true);
    playerEffects->Toggle(true);
}

static void BooUpdateGasSmokeEffects(Effects::Player* playerEffects) {
    if (playerEffects == nullptr) return;

    if (booMask != 0 && playerEffects->kartPlayer != nullptr) {
        const u8 playerId = playerEffects->kartPlayer->GetPlayerIdx();
        if (IsInBoo(playerId) && CanHideBooPlayerCompletely(playerId)) {
            playerEffects->KillEffects(true);
            return;
        }
    }

    playerEffects->UpdateGasSmokeEffects();
}
kmCall(0x80695c18, BooUpdateGasSmokeEffects);

static const u32 BOO_VICTIM_ATTACK_FRAMES = 162;
static const float BOO_MODEL_SCALE = 1.0f;

// The Boo that flies in front of a local human's camera when they rob or get robbed; one model per screen
struct BooVictimScreen {
    ModelDirector* model;
    u8 playerId;  // whose camera it follows, NO_PLAYER while idle
    u16 timer;
};

static BooVictimScreen booVictimScreens[4];
static u32 booVictimScreenCount = 0;
static bool booVictimSystemReady = false;

static void UpdateBooVictimPosition(BooVictimScreen& screen) {
    if (screen.playerId >= 12 || screen.model == nullptr) return;

    Kart::Manager* kartMgr = Kart::Manager::sInstance;
    if (kartMgr == nullptr) return;

    Kart::Player* kp = kartMgr->GetKartPlayer(screen.playerId);
    if (kp == nullptr) return;

    const Vec3& kartPos = kp->GetPosition();

    float fwdX, fwdZ;
    bool gotCamera = false;

    if (kp->HasCamera()) {
        RaceCameraMgr* camMgr = RaceCameraMgr::sInstance;
        if (camMgr != nullptr && camMgr->sortedCameras != nullptr) {
            u32 screenIdx = kp->GetScreenIdx();
            if (screenIdx < camMgr->cameraCount) {
                RaceCamera* cam = camMgr->sortedCameras[screenIdx];
                if (cam != nullptr) {
                    EGG::Matrix34f& viewMtx = cam->GetViewMatrix();
                    fwdX = viewMtx.n20;
                    fwdZ = viewMtx.n22;
                    gotCamera = true;
                }
            }
        }
    }
    if (!gotCamera) {
        const Mtx34& kartMtx = kp->GetMtx();
        fwdX = -kartMtx.n02;
        fwdZ = -kartMtx.n22;
    }
    float lenSq = fwdX * fwdX + fwdZ * fwdZ;
    if (lenSq < 0.0001f) return;
    float invLen = 1.0f / static_cast<float>(sqrt(static_cast<double>(lenSq)));
    fwdX *= invLen;
    fwdZ *= invLen;

    float rightX = fwdZ;
    float rightZ = -fwdX;

    nw4r::math::MTX34 booMtx;
    booMtx.n00 = rightX * BOO_MODEL_SCALE; booMtx.n01 = 0.0f;                   booMtx.n02 = fwdX * BOO_MODEL_SCALE;
    booMtx.n10 = 0.0f;                     booMtx.n11 = 1.0f * BOO_MODEL_SCALE;  booMtx.n12 = 0.0f;
    booMtx.n20 = rightZ * BOO_MODEL_SCALE; booMtx.n21 = 0.0f;                   booMtx.n22 = fwdZ * BOO_MODEL_SCALE;
    booMtx.n03 = kartPos.x + (-100.0f) * fwdX;
    booMtx.n13 = kartPos.y + 150.0f;
    booMtx.n23 = kartPos.z + (-100.0f) * fwdZ;

    for (int j = 0; j < 2; ++j) {
        EGG::ScnMdlEx* ex = screen.model->scnMdlEx[j];
        if (ex != nullptr && ex->scnObj != nullptr) {
            ex->scnObj->SetMtx(nw4r::g3d::ScnObj::MTX_LOCAL, booMtx);
        }
    }
}

extern "C" void GessoManager_createInstance();
static void InitBooVictimSystem() {
    GessoManager_createInstance();
    booVictimSystemReady = false;
    booVictimScreenCount = 0;
    for (u32 i = 0; i < 4; ++i) {
        booVictimScreens[i].model = nullptr;
        booVictimScreens[i].playerId = NO_PLAYER;
        booVictimScreens[i].timer = 0;
    }

    if (!ModelDirector::BRRESExists(ARCHIVE_HOLDER_COMMON, "boo.brres")) return;

    nw4r::g3d::ResFile booRes;
    ModelDirector::BindBRRES(booRes, ARCHIVE_HOLDER_COMMON, "boo.brres");
    if (booRes.data == nullptr) return;

    u8 screenCount = Racedata::sInstance->racesScenario.screenCount;
    if (screenCount > 4) screenCount = 4;
    booVictimScreenCount = screenCount;

    for (u32 i = 0; i < booVictimScreenCount; ++i) {
        ModelDirector* model = new ModelDirector(0xb, 8);
        model->LoadWithAnm("boo", booRes, nullptr);

        for (u32 j = 0; j < 4; ++j) {
            if (j == i) model->EnableScreen(j);
            else model->DisableScreen(j);
        }

        model->LinkAnimation(0, booRes, "attack", ANMTYPE_CHR, true,
            nullptr, ARCHIVE_HOLDER_COMMON, 0);

        model->DisableScreen(i);
        booVictimScreens[i].model = model;
    }

    booVictimSystemReady = true;
}
kmCall(0x807994a4, InitBooVictimSystem);

static void ResetBooVictimScreens() {
    for (u32 i = 0; i < 4; ++i) {
        BooVictimScreen& screen = booVictimScreens[i];
        screen.playerId = NO_PLAYER;
        screen.timer = 0;
        if (screen.model != nullptr) screen.model->DisableScreen(i);
    }
}

static void ActivateBooVictimScreen(u8 playerId) {
    if (!booVictimSystemReady || playerId >= 12) return;

    const RacedataPlayer& rp = Racedata::sInstance->racesScenario.players[playerId];
    if (rp.playerType != PLAYER_REAL_LOCAL) return;

    const s8 hudSlot = rp.hudSlotId;
    if (hudSlot < 0 || static_cast<u32>(hudSlot) >= booVictimScreenCount) return;

    BooVictimScreen& screen = booVictimScreens[hudSlot];
    if (screen.playerId != NO_PLAYER) return; // already playing

    screen.playerId = playerId;
    screen.timer = 0;

    screen.model->EnableScreen(hudSlot);
    screen.model->modelTransformator->PlayAnmNoBlend(0, 0.0f, 1.0f);
    UpdateBooVictimPosition(screen);
}

static void UpdateBooVictimScreens() {
    if (!booVictimSystemReady) return;
    for (u32 i = 0; i < booVictimScreenCount; ++i) {
        BooVictimScreen& screen = booVictimScreens[i];
        if (screen.playerId == NO_PLAYER) continue;
        UpdateBooVictimPosition(screen);
        if (++screen.timer >= BOO_VICTIM_ATTACK_FRAMES) {
            screen.playerId = NO_PLAYER;
            screen.timer = 0;
            screen.model->DisableScreen(i);
        }
    }
}
RaceFrameHook BooVictimScreenUpdate(UpdateBooVictimScreens);

static u8 GetPlayerCount() {
    Kart::Manager* mgr = Kart::Manager::sInstance;
    return (mgr == nullptr) ? 0 : mgr->playerCount;
}

static u8 GetPlayerPosition(u8 playerId) {
    Raceinfo* info = Raceinfo::sInstance;
    if (info == nullptr || info->players == nullptr || playerId >= 12) return 12;
    RaceinfoPlayer* player = info->players[playerId];
    return (player == nullptr) ? 12 : player->position;
}

// With teams on, a Boo never steals from a teammate
static bool IsOnOtherTeam(u8 myPlayerId, u8 otherPlayerId) {
    const RacedataScenario& scenario = Racedata::sInstance->racesScenario;
    if ((scenario.settings.modeFlags & 0x2) == 0) return true;
    const Team mine = scenario.players[myPlayerId].team;
    const Team theirs = scenario.players[otherPlayerId].team;
    return mine == TEAM_NONE || theirs == TEAM_NONE || mine != theirs;
}

static ItemId GetPlayerItem(u8 playerId) {
    Item::Manager* mgr = Item::Manager::sInstance;
    if (mgr == nullptr || mgr->players == nullptr || playerId >= 12 || playerId >= mgr->playerCount) {
        return ITEM_NONE;
    }

    Item::Player& player = mgr->players[playerId];

    if (!player.isRemote) {
        return player.inventory.currentItemId;
    }
    return Network::GetNetworkPlayerItem(playerId);
}

static int GetPlayerItemCount(u8 playerId) {
    Item::Manager* mgr = Item::Manager::sInstance;
    if (mgr == nullptr || mgr->players == nullptr || playerId >= 12 || playerId >= mgr->playerCount) {
        return 0;
    }

    Item::Player& player = mgr->players[playerId];

    if (!player.isRemote) {
        return player.inventory.currentItemCount;
    }
    return Network::GetNetworkPlayerItemCount(playerId);
}

static bool IsStealable(ItemId item) {
    return item != ITEM_NONE && item != POW_BLOCK && item != BOO;
}

static u8 FindBooVictim(u8 thiefId) {
    const u8 thiefPosition = GetPlayerPosition(thiefId);
    u8 playerCount = GetPlayerCount();
    if (playerCount > 12) playerCount = 12;

    u8 victimId = NO_PLAYER;
    u8 victimPosition = 0;
    for (u8 i = 0; i < playerCount; ++i) {
        if (i == thiefId) continue;
        if ((!IsOnOtherTeam(thiefId, i) || GetPlayerPosition(i) >= thiefPosition)) continue;
        if (!IsStealable(GetPlayerItem(i))) continue;
        const u8 position = GetPlayerPosition(i);
        if (victimId == NO_PLAYER || position > victimPosition) {
            victimId = i;
            victimPosition = position;
        }
    }
    return victimId;
}

static void CompleteItemSteal(u8 thiefId) {
    Item::Manager* mgr = Item::Manager::sInstance;
    if (mgr == nullptr || mgr->players == nullptr || thiefId >= mgr->playerCount) return;

    const BooPlayer& boo = booPlayers[thiefId];
    Item::Player& player = mgr->players[thiefId];
    player.inventory.SetItemWithCount(boo.stolenItem, boo.stolenCount == 0 ? 1 : boo.stolenCount, false);

    if (DriverMgr::isOnlineRace && !player.isRemote) {
        RKNet::ITEMHandler* itemHandler = RKNet::ITEMHandler::sInstance;
        if (itemHandler != nullptr) {
            itemHandler->SetItem(thiefId, boo.stolenItem);
        }
    }

    if (IsLocalRacePlayer(thiefId)) {
        ActivateBooVictimScreen(thiefId);
        PlayBooLaugh();
    }
}

static bool CanStartBoo(u8 playerId) {
    return playerId < 12 && !IsInBoo(playerId) && booPlayers[playerId].stealFrames == 0;
}

// Runs on every console: the user's through UseBoo, the others through the Boo EVENT. Each console only empties its own
// players' inventories, so a remote victim loses the item when their console runs this.
static void StartBoo(u8 playerId) {
    BooPlayer& boo = booPlayers[playerId];
    boo.stolenItem = MUSHROOM;
    boo.stolenCount = 1;

    const u8 victimId = FindBooVictim(playerId);
    if (victimId != NO_PLAYER) {
        boo.stolenItem = GetPlayerItem(victimId);
        boo.stolenCount = GetPlayerItemCount(victimId);

        Item::Manager* itemMgr = Item::Manager::sInstance;
        if (itemMgr != nullptr && itemMgr->players != nullptr &&
            victimId < itemMgr->playerCount && !itemMgr->players[victimId].isRemote) {
            itemMgr->players[victimId].inventory.ClearAllAndDestroyDropItems();
            if (IsLocalRacePlayer(victimId)) {
                PlayBooLaugh();
                ActivateBooVictimScreen(victimId);
            }
        }
    }

    boo.stealFrames = BOO_STEAL_DELAY_FRAMES;
    boo.booFrames = BOO_DURATION_FRAMES;
    booMask |= static_cast<u16>(1 << playerId);
    ResetBooSpawnTimer();
}

void UseBoo(Item::Player& itemPlayer) {
    const u8 playerId = itemPlayer.id;
    if (!CanStartBoo(playerId)) return;
    StartBoo(playerId);
    if (DriverMgr::isOnlineRace && !itemPlayer.isRemote) {
        SendEncodedCustomUseEvent(OBJ_BOO, playerId);
    }
}

void ApplyBooRemoteEffect(u8 playerId) {
    if (CanStartBoo(playerId)) StartBoo(playerId);
}

// What ApplyToKartModels does to each model: set its draw priority (DRAW_OPTIONS_TRANSPARENT = see-through)
// and/or insert it into or remove it from the scene
struct KartModelOp {
    bool setPriority;
    bool setVisible;
    bool visible;
    u32 priority;
};
static const KartModelOp HIDE_KART_MODELS = { false, true, false, 0 };

static void ApplyToPart(Kart::Part* part, const KartModelOp& op) {
    if (part == nullptr || !part->isModelLoaded || !HasValidVtable(part)) return;
    if (op.setPriority) part->UpdateModelDrawPriority(op.priority);
    if (op.setVisible) part->ToggleModelVisible(op.visible);
}

static void ApplyToModel(ModelDirector* model, const KartModelOp& op) {
    if (model == nullptr || !HasValidVtable(model)) return;
    if (op.setPriority) model->UpdateDrawPriority(op.priority);
    if (op.setVisible) model->ToggleVisible(op.visible);
}

// Every model of a kart and its driver. The bodies override UpdateModelDrawPriority but not ToggleModelVisible, so their
// sub-parts (kart arms, bike handle and swing arm) are visited here too.
static void ApplyToKartModels(Kart::Pointers& pointers, const KartModelOp& op) {
    Kart::Body* body = pointers.kartBody;
    if (body != nullptr && body->isModelLoaded && HasValidVtable(body)) {
        ApplyToPart(body, op);
        u8* const bodyBytes = reinterpret_cast<u8*>(body);
        if (pointers.values != nullptr && pointers.values->isBike != 0) {
            ApplyToPart(reinterpret_cast<Kart::Part*>(bodyBytes + 0x238), op); // handle
            ApplyToPart(reinterpret_cast<Kart::Part*>(bodyBytes + 0x2CC), op); // swing arm
        }
        else if (bodyBytes[0x228] == 0) {
            Kart::Part** arms = *reinterpret_cast<Kart::Part***>(bodyBytes + 0x238);
            u16 armCount = *reinterpret_cast<u16*>(bodyBytes + 0x23C);
            if (armCount > 4) armCount = 4;
            for (u16 j = 0; arms != nullptr && j < armCount; ++j) ApplyToPart(arms[j], op);
        }
    }

    if (pointers.values != nullptr && pointers.wheels != nullptr) {
        u16 wheelCount = pointers.values->wheelCount0;
        if (wheelCount > 4) wheelCount = 4;
        for (u16 j = 0; j < wheelCount; ++j) ApplyToPart(pointers.wheels[j], op);
    }

    if (pointers.values != nullptr && pointers.suspensions != nullptr) {
        u16 suspCount = pointers.values->wheelCount1;
        if (suspCount > 4) suspCount = 4;
        for (u16 j = 0; j < suspCount; ++j) ApplyToPart(pointers.suspensions[j], op);
    }

    DriverController* driver = pointers.driverController;
    if (driver == nullptr || !HasValidVtable(driver)) return;
    ApplyToModel(driver->driverModel, op);
    ApplyToModel(driver->driverModel_lod, op);
    ApplyToModel(driver->miiHeads, op);
    if (driver->ticoModel != nullptr && HasValidVtable(driver->ticoModel)) ApplyToModel(driver->ticoModel->tico, op);
    if (driver->toadetteHair != nullptr && HasValidVtable(driver->toadetteHair)) {
        ApplyToModel(driver->toadetteHair, op);
        if (driver->toadetteHair->cb != nullptr) ApplyToModel(driver->toadetteHair->cb->hair, op);
    }
    u32 mdlCount = driver->mdlDirectorsCount;
    if (mdlCount > 6) mdlCount = 6;
    for (u32 modelIndex = 0; modelIndex < mdlCount; ++modelIndex) ApplyToModel(driver->mdlDirectors[modelIndex], op);
}

static void ApplyBooVisual(Kart::Pointers& pointers, u8 from, u8 to) {
    KartModelOp op;
    op.setPriority = true;
    op.priority = (to == BOO_VISUAL_NORMAL) ? DRAW_OPTIONS_NORMAL : DRAW_OPTIONS_TRANSPARENT;
    op.setVisible = to == BOO_VISUAL_INVISIBLE || from == BOO_VISUAL_INVISIBLE;
    op.visible = to != BOO_VISUAL_INVISIBLE;
    ApplyToKartModels(pointers, op);

    if (pointers.kartShadow != nullptr && HasValidVtable(pointers.kartShadow)) {
        pointers.kartShadow->EnableDraw(to == BOO_VISUAL_NORMAL);
    }
}

extern "C" void CtrlRace2DMapCharacter_calcSelf(CtrlRace2DMapCharacter* mapCharacter);
extern "C" void CtrlRaceNameBalloon_BaseDraw(CtrlRaceNameBalloon* nameBalloon, u32 curZIdx);

static void BooCtrlRace2DMapCharacterOnUpdate(CtrlRace2DMapCharacter* mapCharacter) {
    CtrlRace2DMapCharacter_calcSelf(mapCharacter);
    if (mapCharacter == nullptr) return;
    const u8 playerId = mapCharacter->playerId;

    if (IsInBoo(playerId) && CanHideBooPlayerCompletely(playerId)) {
        mapCharacter->isHidden = true;
    }
}
kmWritePointer(0x808d38b8, BooCtrlRace2DMapCharacterOnUpdate); // CtrlRace2DMapCharacter vtable, OnUpdate

static void BooCtrlRaceNameBalloonDraw(CtrlRaceNameBalloon* nameBalloon, u32 curZIdx) {
    if (nameBalloon == nullptr) return;

    const u32 balloonPlayerId = static_cast<u32>(nameBalloon->playerId); // -1 (no player) fails IsInBoo
    if (IsInBoo(balloonPlayerId) && CanHideBooPlayerCompletely(static_cast<u8>(balloonPlayerId))) {
        nameBalloon->isHidden = true;
        return;
    }

    CtrlRaceNameBalloon_BaseDraw(nameBalloon, curZIdx);
}
kmWritePointer(0x808d3e6c, BooCtrlRaceNameBalloonDraw); // CtrlRaceNameBalloon vtable, Draw

extern "C" void RedShellResetTargeting(Item::ObjKouraRed*);

// A red shell that locked on before its target went Boo drops the lock (BooRedShellEvalTarget stops it picking a Boo again)
static void DropRedShellLocks(Item::Manager& itemMgr, u16 playerMask) {
    Item::ObjHolder& redShellHolder = itemMgr.itemObjHolders[OBJ_RED_SHELL];
    for (u32 j = 0; j < redShellHolder.capacity; ++j) {
        Item::Obj* obj = redShellHolder.itemObj[j];
        if (obj == nullptr || (obj->bitfield74 & 0x1)) continue;
        Item::ObjKouraRed* redShell = static_cast<Item::ObjKouraRed*>(obj);
        const u8 target = redShell->GetTargetedPlayerId(); // 12 = none
        if (target >= 12 || ((playerMask >> target) & 1) == 0) continue;
        RedShellResetTargeting(redShell);
        *(u32*)((u32)redShell + 0x2b8) = 6;
        *(u8*)((u32)redShell + 0x268) = 0xFF;
        *(u8*)((u32)redShell + 0x269) = 1;
    }
}

static void UpdateBooStates() {
    Kart::Manager* kartMgr = Kart::Manager::sInstance;
    Item::Manager* itemMgr = Item::Manager::sInstance;
    if (kartMgr == nullptr || itemMgr == nullptr) return;

    if (++booFramesSinceLoad < 60 || booMask == 0) return;

    u8 playerCount = kartMgr->playerCount;
    if (playerCount > 12) playerCount = 12;
    const bool viewerCanHide = IsFullBooInvisibilityEnabled() && !IsLocalViewerSpectating();
    u16 inBooMask = 0;

    for (u8 i = 0; i < playerCount; ++i) {
        const u16 bit = static_cast<u16>(1 << i);
        if ((booMask & bit) == 0) continue;
        Kart::Player* kartPlayer = kartMgr->GetKartPlayer(i);
        if (kartPlayer == nullptr) continue;
        BooPlayer& boo = booPlayers[i];

        if (boo.stealFrames > 0 && --boo.stealFrames == 0) CompleteItemSteal(i);

        // Respawning ends the Boo, and an item still on its way is lost
        const Kart::Status* status = kartPlayer->pointers.kartStatus;
        if (status != nullptr && ((status->bitfield1 & 0x2) || (status->bitfield2 & 0x6000))) {
            booMask &= ~bit;
            boo.booFrames = 0;
            boo.stealFrames = 0;
        }

        const bool hasBoo = (booMask & bit) != 0 && boo.booFrames > 0;
        if (hasBoo) {
            --boo.booFrames;
            inBooMask |= bit;
            if (kartPlayer->pointers.kartMovement != nullptr) {
                kartPlayer->pointers.kartMovement->offroadInvincibilityFrames = 60;
                kartPlayer->pointers.kartMovement->kclSpeedFactor = 1.0f;
            }
        }
        else booMask &= ~bit; // ran out last frame

        const bool hideCompletely = viewerCanHide && !IsLocalRacePlayer(i);
        if (hasBoo && hideCompletely) HideBooEffects(i);

        const u8 visual = !hasBoo ? BOO_VISUAL_NORMAL : (hideCompletely ? BOO_VISUAL_INVISIBLE : BOO_VISUAL_TRANSPARENT);
        if (visual != boo.visual) {
            ApplyBooVisual(kartPlayer->pointers, boo.visual, visual);
            boo.visual = visual;
        }
        else if (visual == BOO_VISUAL_INVISIBLE) {
            ApplyToKartModels(kartPlayer->pointers, HIDE_KART_MODELS); // the game re-shows some of them every frame
        }
    }

    if (inBooMask != 0) DropRedShellLocks(*itemMgr, inBooMask);
}
RaceFrameHook BooUpdate(UpdateBooStates);

static void ConditionalUpdateOffroad(Kart::Movement& movement) {
    if (IsKartInBoo(movement)) {
        movement.kclSpeedFactor = 1.0f;
        movement.kclRotFactor = movement.GetStats().handlingFactors[0];
        return;
    }
    movement.UpdateOffroad();
}
kmCall(0x80578dc0, ConditionalUpdateOffroad);
kmCall(0x805792ec, ConditionalUpdateOffroad);
kmCall(0x8057a13c, ConditionalUpdateOffroad);

// The four collision hooks below run from KartSub::calcPass1, once per kart per frame (the kart one per nearby kart)

static void BooCheckPlayerCollision(Kart::Collision& collision, const Kart::Player& other) {
    if (booMask != 0 && (IsInBoo(other.GetPlayerIdx()) || IsInBoo(collision.GetPlayerIdx()))) return;
    collision.CheckKartCollision(other);
}
kmCall(0x80596dc8, BooCheckPlayerCollision);

static void BooCheckObjectCollision(Kart::Collision& collision) {
    Kart::Manager* mgr = Kart::Manager::sInstance;
    Kart::Player* kp = nullptr;
    if (mgr != nullptr && IsKartInBoo(collision)) kp = mgr->GetKartPlayer(collision.GetPlayerIdx());
    if (kp == nullptr) {
        collision.CheckObjectCollision();
        return;
    }
    // With bitfield2 0x02000000 set, CheckObjectCollision returns right after its per-frame bookkeeping. Never leave it
    // set: it also drops all track collision. The object-collision hitbox still has to follow the kart.
    u32& bf2 = kp->pointers.kartStatus->bitfield2;
    const u32 savedBf2 = bf2;
    bf2 |= 0x02000000;
    collision.CheckObjectCollision();
    bf2 = savedBf2;
    kp->pointers.kartObject->Update(kp->GetMtx(), kp->GetSpeed());
}
kmCall(0x80596dfc, BooCheckObjectCollision);

static void BooCheckItemCollision(Kart::Collision& collision) {
    if (IsKartInBoo(collision)) return;
    collision.CheckItemCollision();
}
kmCall(0x80596e08, BooCheckItemCollision);

static bool BooCheckKartCollision(Item::Obj& obj, Kart::Player* kartPlayer, u32 r5) {
    if (kartPlayer != nullptr && IsKartInBoo(*kartPlayer)) return false;
    return obj.CheckKartCollision(kartPlayer, r5);
}
kmCall(0x80799cf8, BooCheckKartCollision);
kmCall(0x80795464, BooCheckKartCollision);

extern "C" double RedShellEvalTarget(Item::ObjKouraRed*, u32, float*, int);
static double BooRedShellEvalTarget(Item::ObjKouraRed* shell, u32 targetPlayerId, float* param3, int param4) {
    if (IsInBoo(targetPlayerId)) return 0.0;
    return RedShellEvalTarget(shell, targetPlayerId, param3, param4);
}
kmCall(0x807b3c48, BooRedShellEvalTarget);

static void ApplyLightningEffect(Kart::Movement& movement) {
    if (IsKartInBoo(movement)) return; // Boo immunity
    movement.ApplyLightning();
}
kmCall(0x80798790, ApplyLightningEffect);

static void ApplyPowEffect(Kart::Movement& movement) {
    if (IsKartInBoo(movement)) return;
    movement.calcPow();
}
kmCall(0x805795d8, ApplyPowEffect);

void ResetBooStates() {
    booFramesSinceLoad = 0;
    if (booSoundHandle.basicSound != nullptr) {
        booSoundHandle.basicSound->Stop(0);
        booSoundHandle.basicSound = nullptr;
    }
    booMask = 0;
    for (int i = 0; i < 12; i++) {
        BooPlayer& boo = booPlayers[i];
        boo.stolenItem = ITEM_NONE;
        boo.stolenCount = 0;
        boo.booFrames = 0;
        boo.stealFrames = 0;
        boo.visual = BOO_VISUAL_NORMAL;
    }
    Item::Behavior& booBehavior = expandedBehaviourTable[BOO];
    booBehavior.unknkown_0x0 = 1;
    booBehavior.unknkown_0x1 = 0;
    booBehavior.objId = OBJ_BOO;
    booBehavior.numberOfItems = 1;
    booBehavior.unknown_0xc = 0;
    booBehavior.unknown_0x10 = 0;
    booBehavior.useType = Item::ITEMUSE_USE;
    booBehavior.useFunction = UseBoo;
    ResetBooVictimScreens();
}
RaceLoadHook BooReset(ResetBooStates);

asmFunc EnableBooOpacity() {
    ASM(
    nofralloc;
    lwz r3, 0x30 (r3);
    lwz r0, 0xB70 (r5);  // racesScenario.settings.gamemode
    cmpwi r0, 0xB;  // MODE_AWARD
    beqlr;
    cmpwi r0, 0xC;  // MODE_CREDITS
    beqlr;
    ori r4, r4, 0x1;
    blr;
    )
}
kmCall(0x805b15d8, EnableBooOpacity);

} // namespace Race
} // namespace Pulsar
