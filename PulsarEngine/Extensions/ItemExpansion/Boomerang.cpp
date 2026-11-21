#include <MarioKartWii/Item/ItemBehaviour.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/ItemPlayer.hpp>
#include <MarioKartWii/Item/Obj/ItemObj.hpp>
#include <MarioKartWii/Driver/DriverManager.hpp>
#include <MarioKartWii/RKNet/ITEM.hpp>
#include <MarioKartWii/3D/Model/ModelDirector.hpp>
#include <include/c_math.h>
#include <Extensions/ItemExpansion/ThrownItemUtil.hpp>
#include <Extensions/ItemExpansion/Boomerang.hpp>
#include <Sound/ExtraSound.hpp>

// Expanded behaviourTable in mod BSS (from ItemSlotExpansion.cpp)
extern "C" Item::Behavior expandedBehaviourTable[ITEM_MAX];

namespace Pulsar {
namespace Race {

static const u8 BOOMERANG_THROWS = 3;
static const u16 BOOMERANG_OUT_FRAMES = 48;
static const u16 BOOMERANG_HOVER_FRAMES = 48;
static const u16 BOOMERANG_RETURN_FRAMES = 24;
static const float BOOMERANG_DISTANCE = 6000.0f;
static const u16 BOOMERANG_LAST_THROW_FRAMES = 90;
static const float BOOMERANG_LAST_THROW_SPEED = 2.0f * BOOMERANG_DISTANCE / BOOMERANG_OUT_FRAMES;
static const float BOOMERANG_HEIGHT = 80.0f;
static const float BOOMERANG_SPIN_SPEED = 0.6f;
static const float BOOMERANG_MODEL_SCALE = 1.0f;
static const float BOOMERANG_RADIUS = 55.0f * BOOMERANG_MODEL_SCALE;
static const float BOOMERANG_MAX_STEP = BOOMERANG_RADIUS * 1.5f;
static const u32 BOOMERANG_MAX_SUBSTEPS = 16;
static const u32 MAX_BOOMERANGS = 16;
static const char BOOMERANG_BRRES[] = "boomerang.brres";
static const float TWO_PI = 6.2831853f;
static const u16 CPU_FIRST_THROW_FRAMES = 45;
static const u16 CPU_THROW_INTERVAL_FRAMES = 40;

static const u16 ITEMPLAYER_STICK_BACK = 0x20;

static const u32 BOOMERANG_KCL_MASK = ITEM_KCL_FLOOR_TYPES | ITEM_KCL_WALL_TYPES;
static const u32 LAST_THROW_KCL_MASK = BOOMERANG_KCL_MASK | ITEM_KCL_KILL_TYPES;

static const ItemObjId BREAKABLE_OBJS[] = { OBJ_GREEN_SHELL, OBJ_RED_SHELL, OBJ_BANANA, OBJ_BOBOMB };
static const u32 MAX_ITEM_BREAKS_A_FRAME = 4;

static const s8 NO_BOOMERANG = -1;

enum BoomerangPhase {
    PHASE_OUT,
    PHASE_HOVER,
    PHASE_RETURN,
    PHASE_LAST_THROW
};

struct Boomerang {
    ModelDirector* model;
    Vec3 position;
    float dirX;
    float dirZ;
    float spin;
    u32 hitMask;
    u16 frame;
    u16 age;
    u8 phase;
    u8 ownerId;
    u8 wallBounces;
    bool stoppedAtWall;
    bool returnsToInventory;
    bool active;
    Sound::Extra::Emitter* emitter;
    nw4r::snd::SoundHandle flightSound;
};
static Boomerang boomerangs[MAX_BOOMERANGS];
static nw4r::g3d::ResFile boomerangRes;
static bool missingModelLogged = false;

struct BoomerangHolder {
    s8 outIdx;
    u8 throwsDone;
    u16 cpuTimer;
};
static BoomerangHolder holders[12];

struct RemoteThrow {
    u16 stamp;
    s8 idx;
};
static RemoteThrow remoteThrows[12];


static void ResetHolder(BoomerangHolder& holder) {
    holder.outIdx = NO_BOOMERANG;
    holder.throwsDone = 0;
    holder.cpuTimer = 0;
}

static void HideBoomerang(Boomerang& b) {
    b.active = false;
    b.returnsToInventory = false;
    if (b.model != nullptr) b.model->ToggleVisible(false);
}

static void ForgetThrow(u32 idx) {
    const Boomerang& b = boomerangs[idx];
    if (b.ownerId >= 12) return;
    BoomerangHolder& holder = holders[b.ownerId];
    if (holder.outIdx == static_cast<s8>(idx)) {
        holder.outIdx = NO_BOOMERANG;
        holder.throwsDone = 0;
    }
}

static void PoofBoomerang(u32 idx) {
    Boomerang& b = boomerangs[idx];
    if (b.active) {
        PlayItemVanishEffect(b.position);
        Sound::Extra::SetPosition(b.emitter, b.position);
        Sound::Extra::PlayAt(b.emitter, Sound::Extra::EXTRA_SND_SE_ITM_BOOMERANG_BROKEN);
    }
    ForgetThrow(idx);
    HideBoomerang(b);
}

static u32 AllocBoomerang() {
    u32 oldest = 0;
    for (u32 i = 0; i < MAX_BOOMERANGS; ++i) {
        if (!boomerangs[i].active) return i;
        if (boomerangs[i].age > boomerangs[oldest].age) oldest = i;
    }
    PoofBoomerang(oldest);
    return oldest;
}

static void UpdateBoomerangSound(Boomerang& b) {
    Sound::Extra::SetPosition(b.emitter, b.position);
    Sound::Extra::HoldAt(b.emitter, Sound::Extra::EXTRA_SND_SE_ITM_BOOMERANG, b.flightSound);
}

static void UpdateBoomerangModel(const Boomerang& b) {
    if (b.model == nullptr) return;
    const float c = static_cast<float>(cos(static_cast<double>(b.spin)));
    const float s = static_cast<float>(sin(static_cast<double>(b.spin)));
    const float k = BOOMERANG_MODEL_SCALE;

    nw4r::math::MTX34 mtx;
    mtx.n00 = c * k;  mtx.n01 = 0.0f; mtx.n02 = s * k;
    mtx.n10 = 0.0f;   mtx.n11 = k;    mtx.n12 = 0.0f;
    mtx.n20 = -s * k; mtx.n21 = 0.0f; mtx.n22 = c * k;
    mtx.n03 = b.position.x;
    mtx.n13 = b.position.y;
    mtx.n23 = b.position.z;
    SetItemModelMtx(b.model, mtx);
}

static bool HitWall(Boomerang& b, const Vec3& normal) {
    float nx = normal.x;
    float nz = normal.z;
    const float len = Length2D(nx, nz);
    if (len < 0.001f) return true;
    nx /= len;
    nz /= len;
    const float into = b.dirX * nx + b.dirZ * nz;
    if (into >= 0.0f) return true;

    if (b.wallBounces == 0) {
        b.dirX -= 2.0f * into * nx;
        b.dirZ -= 2.0f * into * nz;
        b.wallBounces = 1;
        return true;
    }
    if (b.phase == PHASE_LAST_THROW) return false;
    b.stoppedAtWall = true;
    return true;
}

static bool Fly(Boomerang& b, float distance) {
    if (distance <= 0.0f || b.stoppedAtWall) return true;
    u32 steps = static_cast<u32>(distance / BOOMERANG_MAX_STEP) + 1;
    if (steps > BOOMERANG_MAX_SUBSTEPS) steps = BOOMERANG_MAX_SUBSTEPS;
    const float stepLength = distance / static_cast<float>(steps);
    const u32 mask = b.phase == PHASE_LAST_THROW ? LAST_THROW_KCL_MASK : BOOMERANG_KCL_MASK;

    for (u32 i = 0; i < steps; ++i) {
        const Vec3 prevPos = b.position;
        b.position.x += b.dirX * stepLength;
        b.position.z += b.dirZ * stepLength;

        CollisionInfo info;
        u32 types;
        if (!CheckItemCourseCollision(b.position, prevPos, mask, BOOMERANG_RADIUS, info, types)) continue;
        if ((types & ITEM_KCL_KILL_TYPES) != 0) return false;
        b.position.x += info.dirToClosestTri.x;
        b.position.y += info.dirToClosestTri.y;
        b.position.z += info.dirToClosestTri.z;
        if ((types & ITEM_KCL_WALL_TYPES) != 0) {
            if (!HitWall(b, info.dirToClosestWallTri)) return false;
            if (b.stoppedAtWall) return true;
        }
    }
    return true;
}

static float OutStep(u16 frame) {
    const float before = 1.0f - static_cast<float>(frame) / BOOMERANG_OUT_FRAMES;
    const float after = 1.0f - static_cast<float>(frame + 1) / BOOMERANG_OUT_FRAMES;
    return BOOMERANG_DISTANCE * (before * before - after * after);
}

static Vec3 GetCatchPoint(u8 playerId) {
    Vec3 point;
    point.x = 0.0f;
    point.y = 0.0f;
    point.z = 0.0f;
    Item::Manager* mgr = Item::Manager::sInstance;
    if (mgr == nullptr || mgr->players == nullptr || playerId >= mgr->playerCount) return point;
    const Vec3& kartPos = mgr->players[playerId].GetPosition();
    point.x = kartPos.x;
    point.y = kartPos.y + BOOMERANG_HEIGHT;
    point.z = kartPos.z;
    return point;
}

static void HitThings(Boomerang& b, const Vec3& prevPos) {
    for (;;) {
        const s32 victim = FindKartInPath(b.ownerId, prevPos, b.position, BOOMERANG_RADIUS, b.hitMask);
        if (victim == NO_KART_IN_PATH) break;
        b.hitMask |= 1 << victim;
        DamageKartWithItem(static_cast<u8>(victim), b.ownerId, SPINOUT_BANANA);
        PlayItemSoundOnKart(static_cast<u8>(victim), Sound::Extra::EXTRA_SND_SE_ITM_BOOMERANG_HITENEMY);
    }

    Vec3 velocity;
    velocity.x = b.position.x - prevPos.x;
    velocity.y = b.position.y - prevPos.y;
    velocity.z = b.position.z - prevPos.z;
    for (u32 n = 0; n < MAX_ITEM_BREAKS_A_FRAME; ++n) {
        Item::Obj* obj = FindItemInPath(b.ownerId, prevPos, b.position, BOOMERANG_RADIUS, BREAKABLE_OBJS,
                                        sizeof(BREAKABLE_OBJS) / sizeof(BREAKABLE_OBJS[0]), true);
        if (obj == nullptr) break;
        obj->ProcessOtherCollision(2, b.position, velocity);
    }
}

static void CatchBoomerang(u32 idx) {
    Boomerang& b = boomerangs[idx];
    Item::Manager* mgr = Item::Manager::sInstance;
    if (b.returnsToInventory && b.ownerId < 12 && mgr != nullptr && mgr->players != nullptr
        && b.ownerId < mgr->playerCount) {
        Item::Player& thrower = mgr->players[b.ownerId];
        BoomerangHolder& holder = holders[b.ownerId];
        if (holder.outIdx == static_cast<s8>(idx)) {
            holder.outIdx = NO_BOOMERANG;
            holder.cpuTimer = 0;
            if (thrower.inventory.currentItemId == ITEM_NONE && !thrower.roulette.isTheRouletteSpinning) {
                thrower.inventory.SetItemWithCount(BOOMERANG, 1, false);
                RKNet::ITEMHandler* itemHandler = RKNet::ITEMHandler::sInstance;
                if (DriverMgr::isOnlineRace && itemHandler != nullptr) itemHandler->SetItem(b.ownerId, BOOMERANG);
            }
            else holder.throwsDone = 0;
        }
    }
    HideBoomerang(b);
}

static void UpdateBoomerangs() {
    for (u32 i = 0; i < MAX_BOOMERANGS; ++i) {
        Boomerang& b = boomerangs[i];
        if (!b.active) continue;
        if (b.age < 0xFFFF) ++b.age;
        const Vec3 prevPos = b.position;
        bool caught = false;

        switch (b.phase) {
            case PHASE_OUT:
                if (!Fly(b, OutStep(b.frame))) {
                    PoofBoomerang(i);
                    continue;
                }
                if (++b.frame >= BOOMERANG_OUT_FRAMES) {
                    b.phase = PHASE_HOVER;
                    b.frame = 0;
                }
                break;
            case PHASE_HOVER:
                if (++b.frame >= BOOMERANG_HOVER_FRAMES) {
                    b.phase = PHASE_RETURN;
                    b.frame = 0;
                    b.hitMask = 0;
                }
                break;
            case PHASE_RETURN: {
                const Vec3 target = GetCatchPoint(b.ownerId);
                const float share = 1.0f / static_cast<float>(BOOMERANG_RETURN_FRAMES - b.frame);
                b.position.x += (target.x - b.position.x) * share;
                b.position.y += (target.y - b.position.y) * share;
                b.position.z += (target.z - b.position.z) * share;
                caught = ++b.frame >= BOOMERANG_RETURN_FRAMES;
                break;
            }
            case PHASE_LAST_THROW:
                if (!Fly(b, BOOMERANG_LAST_THROW_SPEED) || ++b.frame >= BOOMERANG_LAST_THROW_FRAMES) {
                    PoofBoomerang(i);
                    continue;
                }
                break;
        }

        HitThings(b, prevPos);
        if (caught) {
            CatchBoomerang(i);
            continue;
        }
        b.spin += BOOMERANG_SPIN_SPEED;
        while (b.spin > TWO_PI) b.spin -= TWO_PI;
        UpdateBoomerangModel(b);
        UpdateBoomerangSound(b);
    }
}
static s8 SpawnBoomerang(const Item::Player& thrower, bool backward, bool lastThrow) {
    if (thrower.id >= 12) return NO_BOOMERANG;
    const Mtx34& mtx = thrower.GetMtx();
    float dirX = mtx.n02;
    float dirZ = mtx.n22;
    const float dirLen = Length2D(dirX, dirZ);
    if (dirLen < 0.001f) return NO_BOOMERANG;
    dirX /= dirLen;
    dirZ /= dirLen;
    if (backward) {
        dirX = -dirX;
        dirZ = -dirZ;
    }
    PlayItemThrowAnimation(thrower, backward);

    const u32 idx = AllocBoomerang();
    Boomerang& b = boomerangs[idx];
    b.position = GetCatchPoint(thrower.id);
    b.dirX = dirX;
    b.dirZ = dirZ;
    b.spin = 0.0f;
    b.hitMask = 0;
    b.frame = 0;
    b.age = 0;
    b.phase = lastThrow ? PHASE_LAST_THROW : PHASE_OUT;
    b.ownerId = thrower.id;
    b.wallBounces = 0;
    b.stoppedAtWall = false;
    b.returnsToInventory = false;
    b.active = true;

    if (b.model != nullptr) b.model->ToggleVisible(true);
    UpdateBoomerangModel(b);
    UpdateBoomerangSound(b);
    return static_cast<s8>(idx);
}

static void ThrowBoomerang(Item::Player& itemPlayer, bool backward) {
    const u8 playerId = itemPlayer.id;
    if (playerId >= 12 || itemPlayer.isRemote || itemPlayer.inventory.currentItemId != BOOMERANG) return;

    BoomerangHolder& holder = holders[playerId];
    const bool lastThrow = holder.throwsDone + 1 >= BOOMERANG_THROWS;
    const s8 idx = SpawnBoomerang(itemPlayer, backward, lastThrow);
    if (idx == NO_BOOMERANG) return;

    Item::PlayerInventory& inventory = itemPlayer.inventory;
    inventory.RemoveItems(inventory.currentItemCount);
    RKNet::ITEMHandler* itemHandler = RKNet::ITEMHandler::sInstance;
    if (DriverMgr::isOnlineRace) {
        if (itemHandler != nullptr) itemHandler->RemoveItem(playerId);
        SendEncodedBoomerangEvent(playerId, backward, lastThrow);
    }

    holder.cpuTimer = 0;
    if (lastThrow) {
        holder.outIdx = NO_BOOMERANG;
        holder.throwsDone = 0;
    }
    else {
        ++holder.throwsDone;
        holder.outIdx = idx;
        boomerangs[idx].returnsToInventory = true;
    }
}

void UseBoomerang(Item::Player& itemPlayer) {
    ThrowBoomerang(itemPlayer, (itemPlayer.bitfield & ITEMPLAYER_STICK_BACK) != 0);
}

void SpawnRemoteBoomerang(const Item::Player& thrower, bool backward, u16 throwStamp) {
    const u8 playerId = thrower.id;
    if (playerId >= 12) return;
    RemoteThrow& last = remoteThrows[playerId];

    if (last.idx != NO_BOOMERANG && last.stamp == throwStamp) {
        Boomerang& b = boomerangs[last.idx];
        if (b.active && b.ownerId == playerId && b.phase == PHASE_OUT) b.phase = PHASE_LAST_THROW;
        last.idx = NO_BOOMERANG;
        return;
    }
    last.stamp = throwStamp;
    last.idx = SpawnBoomerang(thrower, backward, false);
}

void LoseBoomerang(Item::PlayerInventory& inventory) {
    const Item::Player* itemPlayer = inventory.itemPlayer;
    if (itemPlayer != nullptr && itemPlayer->id < 12 && holders[itemPlayer->id].outIdx == NO_BOOMERANG) {
        holders[itemPlayer->id].throwsDone = 0;
    }
    inventory.ClearAll();
}

u32 CountBoomerangsComingBack() {
    u32 count = 0;
    for (u32 i = 0; i < MAX_BOOMERANGS; ++i) {
        if (boomerangs[i].active && boomerangs[i].phase != PHASE_LAST_THROW) ++count;
    }
    return count;
}

static void UpdateBoomerangHolders() {
    Item::Manager* mgr = Item::Manager::sInstance;
    if (mgr == nullptr || mgr->players == nullptr) return;

    u8 count = mgr->playerCount;
    if (count > 12) count = 12;
    for (u8 i = 0; i < count; ++i) {
        Item::Player& itemPlayer = mgr->players[i];
        BoomerangHolder& holder = holders[i];
        if (itemPlayer.isRemote) {
            ResetHolder(holder);
            continue;
        }
        const ItemId held = itemPlayer.inventory.currentItemId;
        const bool rolling = itemPlayer.roulette.isTheRouletteSpinning != 0;

        if (holder.outIdx != NO_BOOMERANG) {
            if (held != ITEM_NONE || rolling) {
                boomerangs[holder.outIdx].returnsToInventory = false;
                holder.outIdx = NO_BOOMERANG;
                holder.throwsDone = 0;
            }
        }
        else if (held != BOOMERANG) holder.throwsDone = 0;

        if (itemPlayer.isHuman || held != BOOMERANG || holder.outIdx != NO_BOOMERANG) {
            holder.cpuTimer = 0;
            continue;
        }
        const u16 wait = holder.throwsDone == 0 ? CPU_FIRST_THROW_FRAMES : CPU_THROW_INTERVAL_FRAMES;
        if (holder.cpuTimer < wait) ++holder.cpuTimer;
        else if (CanCpuThrowItem(itemPlayer)) ThrowBoomerang(itemPlayer, false);
    }
}

static void UpdateBoomerangItem() {
    UpdateBoomerangHolders();
    UpdateBoomerangs();
}
RaceFrameHook BoomerangUpdate(UpdateBoomerangItem);

static void InitBoomerang() {
    for (u32 i = 0; i < 12; ++i) {
        ResetHolder(holders[i]);
        remoteThrows[i].stamp = 0;
        remoteThrows[i].idx = NO_BOOMERANG;
    }
    for (u32 i = 0; i < MAX_BOOMERANGS; ++i) {
        boomerangs[i].active = false;
        boomerangs[i].returnsToInventory = false;
        boomerangs[i].age = 0;
        boomerangs[i].ownerId = 0xFF;
        if (boomerangs[i].emitter == nullptr) boomerangs[i].emitter = Sound::Extra::CreateEmitter();
    }

    Item::Behavior& behavior = expandedBehaviourTable[BOOMERANG];
    behavior.unknkown_0x0 = 1;
    behavior.unknkown_0x1 = 0;
    behavior.objId = OBJ_BOOMERANG;
    behavior.numberOfItems = 1;
    behavior.unknown_0xc = 0;
    behavior.unknown_0x10 = 0;
    behavior.useType = Item::ITEMUSE_USE;
    behavior.useFunction = UseBoomerang;

    ModelDirector* models[MAX_BOOMERANGS];
    CreateItemModels(BOOMERANG_BRRES, boomerangRes, models, MAX_BOOMERANGS, missingModelLogged);
    for (u32 i = 0; i < MAX_BOOMERANGS; ++i) boomerangs[i].model = models[i];
}
RaceLoadHook BoomerangInit(InitBoomerang);

} // namespace Race
} // namespace Pulsar
