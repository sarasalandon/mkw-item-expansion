#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/Obj/ItemObjHolder.hpp>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Kart/KartPlayer.hpp>
#include <MarioKartWii/Kart/KartDamage.hpp>
#include <MarioKartWii/Driver/DriverController.hpp>
#include <MarioKartWii/CourseMgr.hpp>
#include <MarioKartWii/Effect/EffectMgr.hpp>
#include <MarioKartWii/Race/Racedata.hpp>
#include <MarioKartWii/Archive/ArchiveMgr.hpp>
#include <core/egg/3D/Scn.hpp>
#include <core/rvl/OS/OS.hpp>
#include <include/c_math.h>
#include <Extensions/ItemExpansion/Boo.hpp>
#include <Extensions/ItemExpansion/ThrownItemUtil.hpp>
#include <Sound/ExtraSound.hpp>

namespace Pulsar {
namespace Race {

float Length2D(float x, float z) {
    return static_cast<float>(sqrt(static_cast<double>(x * x + z * z)));
}

float DistanceSqToSegment(const Vec3& point, const Vec3& from, const Vec3& to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    const float lenSq = dx * dx + dy * dy + dz * dz;
    float t = 0.0f;
    if (lenSq > 0.0001f) {
        t = ((point.x - from.x) * dx + (point.y - from.y) * dy + (point.z - from.z) * dz) / lenSq;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
    }
    const float cx = from.x + dx * t - point.x;
    const float cy = from.y + dy * t - point.y;
    const float cz = from.z + dz * t - point.z;
    return cx * cx + cy * cy + cz * cz;
}

extern "C" void DriverController_PlayThrowAnm(DriverController* driver, const u32* thrownObj);
static const u32 OBJ_FORWARD_THROW_BIT = 0x02000000;
static u32 forwardThrowObj[0x80 / 4];
static u32 backwardThrowObj[0x80 / 4];
static const u32 DRIVER_HELD_THROW_FRAMES = 0x6DC;
static const u32 DRIVER_HELD_THROW_OBJ = 0x6E0;

void PlayItemThrowAnimation(const Item::Player& thrower, bool backward) {
    DriverController* driver = thrower.model2;
    if (driver == nullptr) return;
    forwardThrowObj[0x78 / 4] = OBJ_FORWARD_THROW_BIT;
    backwardThrowObj[0x78 / 4] = 0;
    DriverController_PlayThrowAnm(driver, backward ? backwardThrowObj : forwardThrowObj);

    u8* const driverBytes = reinterpret_cast<u8*>(driver);
    *reinterpret_cast<s32*>(driverBytes + DRIVER_HELD_THROW_FRAMES) = 0;
    *reinterpret_cast<const u32**>(driverBytes + DRIVER_HELD_THROW_OBJ) = nullptr;
}

extern "C" void EffectsItems_CreateItemVanEffect(Effects::Items* items, const Vec3& position);

void PlayItemVanishEffect(const Vec3& position) {
    Effects::Mgr* effectsMgr = Effects::Mgr::sInstance;
    if (effectsMgr != nullptr && effectsMgr->items != nullptr) EffectsItems_CreateItemVanEffect(effectsMgr->items, position);
}


static Sound::Extra::Emitter* kartEmitters[12];

static bool GetKartPosition(u8 playerId, Vec3& position) {
    Kart::Manager* kartMgr = Kart::Manager::sInstance;
    if (kartMgr == nullptr || playerId >= kartMgr->playerCount || playerId >= 12) return false;
    Kart::Player* kart = kartMgr->GetKartPlayer(playerId);
    if (kart == nullptr) return false;
    position = kart->GetPosition();
    return true;
}

static void CreateKartEmitters() {
    for (u32 i = 0; i < 12; ++i) {
        if (kartEmitters[i] == nullptr) kartEmitters[i] = Sound::Extra::CreateEmitter();
    }
}
RaceLoadHook KartSoundEmitters(CreateKartEmitters);

static void MoveKartEmitters() {
    for (u8 i = 0; i < 12; ++i) {
        Vec3 position;
        if (kartEmitters[i] != nullptr && GetKartPosition(i, position)) Sound::Extra::SetPosition(kartEmitters[i], position);
    }
}
RaceFrameHook KartSoundEmittersUpdate(MoveKartEmitters);

void PlayItemSoundOnKart(u8 playerId, u32 soundId) {
    if (playerId >= 12) return;
    Sound::Extra::Emitter* emitter = kartEmitters[playerId];
    Vec3 position;
    if (emitter == nullptr || !GetKartPosition(playerId, position)) return;
    Sound::Extra::SetPosition(emitter, position);
    Sound::Extra::PlayAt(emitter, soundId);
}


bool CheckItemCourseCollision(const Vec3& pos, const Vec3& prevPos, u32 kclMask, float radius, CollisionInfo& info,
                              u32& types) {
    types = 0;
    CourseMgr* course = CourseMgr::sInstance;
    if (course == nullptr) return false;

    memset(&info, 0, sizeof(info));
    KCLTypeHolder kcl;
    kcl.bitfield = static_cast<KCLBitfield>(0);
    if (!course->IsCollidingNoTriangleCheck(pos, prevPos, static_cast<KCLBitfield>(kclMask), &info, &kcl, 0, radius)) {
        return false;
    }
    types = kcl.bitfield;
    return true;
}


static const u32 UNHITTABLE_BITFIELD0 = 0x10;
static const u32 UNHITTABLE_BITFIELD1 = 0x1A;
static const u32 UNHITTABLE_BITFIELD2 = 0xC6000;

bool CanKartBeHitByItem(const Kart::Status* status) {
    return status != nullptr && (status->bitfield0 & UNHITTABLE_BITFIELD0) == 0
        && (status->bitfield1 & UNHITTABLE_BITFIELD1) == 0 && (status->bitfield2 & UNHITTABLE_BITFIELD2) == 0;
}

static const u32 IN_DAMAGE_BITFIELD1 = 0x1;

bool CanCpuThrowItem(const Item::Player& itemPlayer) {
    const Kart::Status* status = itemPlayer.pointers->kartStatus;
    return CanKartBeHitByItem(status) && (status->bitfield1 & IN_DAMAGE_BITFIELD1) == 0
        && itemPlayer.inventory.loseDelayDueToDmg == 0;
}

static bool AreTeammates(u8 a, u8 b) {
    if (Racedata::sInstance == nullptr) return false;
    const RacedataScenario& scenario = Racedata::sInstance->racesScenario;
    if ((scenario.settings.modeFlags & 0x2) == 0) return false;
    const Team team = scenario.players[a].team;
    return team != TEAM_NONE && team == scenario.players[b].team;
}

s32 FindKartInPath(u8 ownerId, const Vec3& from, const Vec3& to, float radius, u32 ignoredMask) {
    Kart::Manager* kartMgr = Kart::Manager::sInstance;
    Item::Manager* itemMgr = Item::Manager::sInstance;
    if (kartMgr == nullptr || itemMgr == nullptr || itemMgr->players == nullptr) return NO_KART_IN_PATH;

    u8 count = kartMgr->playerCount;
    if (count > itemMgr->playerCount) count = itemMgr->playerCount;
    if (count > 12) count = 12;
    for (u8 i = 0; i < count; ++i) {
        if (i == ownerId || (ignoredMask & (1 << i)) != 0 || AreTeammates(ownerId, i) || IsPlayerInBooState(i)) continue;
        Kart::Player* kart = kartMgr->GetKartPlayer(i);
        if (kart == nullptr || !CanKartBeHitByItem(kart->pointers.kartStatus)) continue;
        const float reach = kart->GetTriggersCollisionRadius() + radius;
        if (DistanceSqToSegment(kart->GetPosition(), from, to) < reach * reach) return i;
    }
    return NO_KART_IN_PATH;
}

extern "C" void PlayerModel_OnHitOther(DriverController* driver);
extern "C" void DriverSnd_PlayHitOtherVoice(void* driverSound, u32 hitPlayerId);
extern "C" void KartLink_ToggleHitBy(const Kart::Link* hitKart, u32 userPlayerId, u32 kind);
static const u32 DRIVER_SOUND = 0xC;

static void CreditHit(u8 throwerId, const Kart::Player& hitKart) {
    Item::Manager* itemMgr = Item::Manager::sInstance;
    if (itemMgr == nullptr || itemMgr->players == nullptr || throwerId >= itemMgr->playerCount) return;
    DriverController* throwerDriver = itemMgr->players[throwerId].model2;
    if (throwerDriver != nullptr) {
        PlayerModel_OnHitOther(throwerDriver);
        void* throwerSound = *reinterpret_cast<void**>(reinterpret_cast<u8*>(throwerDriver) + DRIVER_SOUND);
        if (throwerSound != nullptr) DriverSnd_PlayHitOtherVoice(throwerSound, hitKart.GetPlayerIdx());
    }
    KartLink_ToggleHitBy(&hitKart, throwerId, 0);
}

bool DamageKartWithItem(u8 victimId, u8 ownerId, DamageType damage) {
    Kart::Manager* kartMgr = Kart::Manager::sInstance;
    Item::Manager* itemMgr = Item::Manager::sInstance;
    if (kartMgr == nullptr || itemMgr == nullptr || itemMgr->players == nullptr) return false;
    if (victimId >= itemMgr->playerCount || victimId >= kartMgr->playerCount || victimId == ownerId) return false;
    if (itemMgr->players[victimId].isRemote) return false;

    Kart::Player* kart = kartMgr->GetKartPlayer(victimId);
    if (kart == nullptr) return false;
    Kart::Damage* kartDamage = kart->pointers.kartDamage;
    if (kartDamage == nullptr || !kartDamage->SetDamage(damage, 0, false, nullptr, 0xC, 1)) return false;
    CreditHit(ownerId, *kart);
    return true;
}

static const u32 OBJ_HELD_BITS = 0x20001;
static const u32 OBJ_DYING_BITS = 0xE000C0;
static const u32 OBJ_RADIUS = 0xB8;

Item::Obj* FindItemInPath(u8 ownerId, const Vec3& from, const Vec3& to, float radius, const ItemObjId* objIds,
                          u32 objIdCount, bool ignoreAllOfOwner) {
    Item::Manager* mgr = Item::Manager::sInstance;
    if (mgr == nullptr) return nullptr;

    for (u32 k = 0; k < objIdCount; ++k) {
        Item::ObjHolder& holder = mgr->itemObjHolders[objIds[k]];
        if (holder.itemObj == nullptr) continue;
        u32 count = holder.spawnedCount;
        if (count > holder.capacity) count = holder.capacity;
        for (u32 j = 0; j < count; ++j) {
            Item::Obj* obj = holder.itemObj[j];
            if (obj == nullptr || obj->entity == nullptr) continue;
            if ((obj->bitfield74 & 0x1) != 0 || (obj->bitfield7c & 0x4) != 0) continue;
            if ((obj->bitfield78 & OBJ_DYING_BITS) != 0) continue;
            if (obj->playerUsedItemId == ownerId && (ignoreAllOfOwner || (obj->bitfield78 & OBJ_HELD_BITS) != 0)) continue;
            const float objRadius = *reinterpret_cast<const float*>(reinterpret_cast<const u8*>(obj) + OBJ_RADIUS);
            const float reach = objRadius + radius;
            if (DistanceSqToSegment(obj->position, from, to) < reach * reach) return obj;
        }
    }
    return nullptr;
}


u32 CreateItemModels(const char* brres, nw4r::g3d::ResFile& res, ModelDirector** models, u32 count,
                     bool& missingLogged) {
    for (u32 i = 0; i < count; ++i) models[i] = nullptr;
    if (!ModelDirector::BRRESExists(ARCHIVE_HOLDER_COMMON, brres)) {
        if (!missingLogged) {
            OS::Report("[Pulsar] %s is missing from the common archive, that item will be invisible\n", brres);
            missingLogged = true;
        }
        return 0;
    }
    nw4r::g3d::ResFile check;
    check.data = ArchiveMgr::sInstance != nullptr
        ? static_cast<nw4r::g3d::ResFileData*>(ArchiveMgr::sInstance->GetFile(ARCHIVE_HOLDER_COMMON, brres))
        : static_cast<nw4r::g3d::ResFileData*>(nullptr);
    if (check.data == nullptr || !check.CheckRevision()) {
        if (!missingLogged) {
            OS::Report("[Pulsar] %s isn't in Mario Kart Wii's model format (MDL0 version 11), that item will be invisible\n",
                       brres);
            missingLogged = true;
        }
        return 0;
    }

    ModelDirector::BindBRRES(res, ARCHIVE_HOLDER_COMMON, brres);
    if (res.data == nullptr || res.GetResMdlNumEntries() == 0) return 0;

    int modelIdx = 0;
    u32 made = 0;
    for (; made < count; ++made) {
        ModelDirector* model = new ModelDirector(0xB, 0);
        if (model == nullptr) break;
        model->LoadWithAnm(modelIdx, res, nullptr);
        model->ToggleVisible(false);
        models[made] = model;
    }
    return made;
}

void SetItemModelMtx(ModelDirector* model, const nw4r::math::MTX34& mtx) {
    if (model == nullptr) return;
    for (int j = 0; j < 2; ++j) {
        EGG::ScnMdlEx* ex = model->scnMdlEx[j];
        if (ex != nullptr && ex->scnObj != nullptr) ex->scnObj->SetMtx(nw4r::g3d::ScnObj::MTX_LOCAL, mtx);
    }
}

} // namespace Race
} // namespace Pulsar
