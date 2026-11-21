#include <MarioKartWii/Item/ItemBehaviour.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/ItemPlayer.hpp>
#include <MarioKartWii/Item/Obj/ItemObj.hpp>
#include <MarioKartWii/Kart/KartStatus.hpp>
#include <MarioKartWii/Driver/DriverManager.hpp>
#include <MarioKartWii/3D/Model/ModelDirector.hpp>
#include <include/c_math.h>
#include <Extensions/ItemExpansion/ThrownItemUtil.hpp>
#include <Extensions/ItemExpansion/FireFlower.hpp>
#include <Sound/ExtraSound.hpp>

extern "C" Item::Behavior expandedBehaviourTable[ITEM_MAX];

namespace Pulsar {
namespace Race {

static const u16 FIRE_FLOWER_DURATION_FRAMES = 600;
static const u8 FIRE_FLOWER_SHOTS = 10;
static const u16 CPU_FIRST_SHOT_FRAMES = 45;
static const u16 CPU_SHOT_INTERVAL_FRAMES = 40;

static const u16 FIREBALL_LIFETIME_FRAMES = 204;
static const u32 MAX_FIREBALLS = 24;
static const float FIREBALL_MODEL_SCALE = 0.3f;
static const float FIREBALL_RADIUS = 40.0f * FIREBALL_MODEL_SCALE;
static const float FIREBALL_GRAVITY = 1.6f;
static const float FIREBALL_BOUNCE_SPEED = 16.0f;
static const float FIREBALL_MAX_FALL_SPEED = 60.0f;
static const float FIREBALL_SPEED = 120.0f;
static const float FIREBALL_SPAWN_DISTANCE = 130.0f;
static const float FIREBALL_SPAWN_HEIGHT = 40.0f;
static const float FIREBALL_MAX_STEP = FIREBALL_RADIUS * 1.5f;
static const u32 FIREBALL_MAX_SUBSTEPS = 16;
static const float FIREBALL_MIN_FLOOR_NORMAL_Y = 0.5f;
static const float FIREBALL_ROLL_FACTOR = 1.0f;
static const char FIREBALL_BRRES[] = "honeBall.brres";  // in CommonAssets.szs; its first model is used
static const float TWO_PI = 6.2831853f;

static const u16 ITEMPLAYER_STICK_BACK = 0x20;

static const u32 FIREBALL_KCL_MASK = ITEM_KCL_FLOOR_TYPES | ITEM_KCL_WALL_TYPES | ITEM_KCL_KILL_TYPES;

static const ItemObjId BREAKABLE_OBJS[] = { OBJ_GREEN_SHELL, OBJ_RED_SHELL, OBJ_BANANA, OBJ_BOBOMB };

struct FlowerHolder {
    u16 framesLeft;
    u16 cpuTimer;
    u8 shotsLeft;
    bool active;
};
static FlowerHolder holders[12];

struct Fireball {
    ModelDirector* model;
    Vec3 position;
    Vec3 velocity;
    float speed;
    float rollAngle;
    u16 age;
    u8 ownerId;
    bool active;
    Sound::Extra::Emitter* emitter;
    nw4r::snd::SoundHandle moveSound;
};
static Fireball fireballs[MAX_FIREBALLS];
static nw4r::g3d::ResFile fireballRes;
static bool missingModelLogged = false;

static void HideFireball(Fireball& fb) {
    fb.active = false;
    if (fb.model != nullptr) fb.model->ToggleVisible(false);
}

static void PoofFireball(Fireball& fb) {
    if (fb.active) PlayItemVanishEffect(fb.position);
    HideFireball(fb);
}

static Fireball& AllocFireball() {
    Fireball* oldest = &fireballs[0];
    for (u32 i = 0; i < MAX_FIREBALLS; ++i) {
        Fireball& fb = fireballs[i];
        if (!fb.active) return fb;
        if (fb.age > oldest->age) oldest = &fb;
    }
    PoofFireball(*oldest);
    return *oldest;
}

static void UpdateFireballModel(const Fireball& fb) {
    if (fb.model == nullptr) return;

    float axisX = 1.0f;
    float axisZ = 0.0f;
    const float horizontal = Length2D(fb.velocity.x, fb.velocity.z);
    if (horizontal > 0.001f) {
        axisX = fb.velocity.z / horizontal;
        axisZ = -fb.velocity.x / horizontal;
    }
    const float c = static_cast<float>(cos(static_cast<double>(fb.rollAngle)));
    const float s = static_cast<float>(sin(static_cast<double>(fb.rollAngle)));
    const float t = 1.0f - c;
    const float k = FIREBALL_MODEL_SCALE;

    nw4r::math::MTX34 mtx;
    mtx.n00 = (t * axisX * axisX + c) * k; mtx.n01 = -s * axisZ * k; mtx.n02 = t * axisX * axisZ * k;
    mtx.n10 = s * axisZ * k;               mtx.n11 = c * k;          mtx.n12 = -s * axisX * k;
    mtx.n20 = t * axisX * axisZ * k;       mtx.n21 = s * axisX * k;  mtx.n22 = (t * axisZ * axisZ + c) * k;
    mtx.n03 = fb.position.x;
    mtx.n13 = fb.position.y;
    mtx.n23 = fb.position.z;
    SetItemModelMtx(fb.model, mtx);
}

static void UpdateFireballSound(Fireball& fb) {
    Sound::Extra::SetPosition(fb.emitter, fb.position);
    Sound::Extra::HoldAt(fb.emitter, Sound::Extra::EXTRA_SND_SE_ITM_FIREBALL_MOVE, fb.moveSound);
}

static void Reflect(Vec3& velocity, const Vec3& normal) {
    if (normal.y <= -FIREBALL_MIN_FLOOR_NORMAL_Y) {
        if (velocity.y > 0.0f) velocity.y = -velocity.y;
        return;
    }
    float nx = normal.x;
    float nz = normal.z;
    const float len = Length2D(nx, nz);
    if (len < 0.001f) return;
    nx /= len;
    nz /= len;
    const float into = velocity.x * nx + velocity.z * nz;
    if (into >= 0.0f) return;
    velocity.x -= 2.0f * into * nx;
    velocity.z -= 2.0f * into * nz;
}

static bool CollideWithCourse(Fireball& fb, const Vec3& prevPos) {
    CollisionInfo info;
    u32 types;
    if (!CheckItemCourseCollision(fb.position, prevPos, FIREBALL_KCL_MASK, FIREBALL_RADIUS, info, types)) return true;
    if ((types & ITEM_KCL_KILL_TYPES) != 0) return false;

    fb.position.x += info.dirToClosestTri.x;
    fb.position.y += info.dirToClosestTri.y;
    fb.position.z += info.dirToClosestTri.z;

    if ((types & ITEM_KCL_FLOOR_TYPES) != 0) {
        const Vec3& floor = info.dirToClosestFloorTri;
        if (floor.y >= FIREBALL_MIN_FLOOR_NORMAL_Y) {
            if (fb.velocity.y <= 0.0f) fb.velocity.y = FIREBALL_BOUNCE_SPEED;
        }
        else Reflect(fb.velocity, floor);
    }
    if ((types & ITEM_KCL_WALL_TYPES) != 0) Reflect(fb.velocity, info.dirToClosestWallTri);
    return true;
}

static bool Advance(Fireball& fb) {
    const Vec3& v = fb.velocity;
    const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    u32 steps = 1;
    if (lenSq > FIREBALL_MAX_STEP * FIREBALL_MAX_STEP) {
        steps = static_cast<u32>(sqrt(static_cast<double>(lenSq)) / FIREBALL_MAX_STEP) + 1;
        if (steps > FIREBALL_MAX_SUBSTEPS) steps = FIREBALL_MAX_SUBSTEPS;
    }
    const float fraction = 1.0f / static_cast<float>(steps);
    for (u32 i = 0; i < steps; ++i) {
        const Vec3 prevPos = fb.position;
        fb.position.x += fb.velocity.x * fraction;
        fb.position.y += fb.velocity.y * fraction;
        fb.position.z += fb.velocity.z * fraction;
        if (!CollideWithCourse(fb, prevPos)) return false;
    }
    return true;
}

static void KeepHorizontalSpeed(Fireball& fb) {
    const float horizontal = Length2D(fb.velocity.x, fb.velocity.z);
    if (horizontal < 0.001f) return;
    const float scale = fb.speed / horizontal;
    fb.velocity.x *= scale;
    fb.velocity.z *= scale;
}

static bool HitKart(const Fireball& fb, const Vec3& prevPos) {
    const s32 victim = FindKartInPath(fb.ownerId, prevPos, fb.position, FIREBALL_RADIUS, 0);
    if (victim == NO_KART_IN_PATH) return false;
    DamageKartWithItem(static_cast<u8>(victim), fb.ownerId, SPINOUT_FIRE);
    PlayItemSoundOnKart(static_cast<u8>(victim), Sound::Extra::EXTRA_SND_SE_ITM_FIREBALL_HIT_DAMAGE);
    return true;
}

static bool HitItem(const Fireball& fb, const Vec3& prevPos) {
    Item::Obj* obj = FindItemInPath(fb.ownerId, prevPos, fb.position, FIREBALL_RADIUS, BREAKABLE_OBJS,
                                    sizeof(BREAKABLE_OBJS) / sizeof(BREAKABLE_OBJS[0]), false);
    if (obj == nullptr) return false;
    obj->ProcessOtherCollision(2, fb.position, fb.velocity);
    return true;
}

static void SpawnFireball(const Item::Player& thrower, bool backward) {
    if (thrower.id >= 12) return;
    const Mtx34& mtx = thrower.GetMtx();
    float dirX = mtx.n02;
    float dirZ = mtx.n22;
    const float dirLen = Length2D(dirX, dirZ);
    if (dirLen < 0.001f) return;
    dirX /= dirLen;
    dirZ /= dirLen;
    if (backward) {
        dirX = -dirX;
        dirZ = -dirZ;
    }

    PlayItemThrowAnimation(thrower, backward);
    PlayItemSoundOnKart(thrower.id, Sound::Extra::EXTRA_SND_SE_ITM_FIREBALL_THROW);

    Fireball& fb = AllocFireball();
    const Vec3& kartPos = thrower.GetPosition();
    fb.position.x = kartPos.x;
    fb.position.y = kartPos.y + FIREBALL_SPAWN_HEIGHT;
    fb.position.z = kartPos.z;
    fb.speed = FIREBALL_SPEED;
    fb.rollAngle = 0.0f;
    fb.age = 0;
    fb.ownerId = thrower.id;
    fb.active = true;

    fb.velocity.x = dirX * FIREBALL_SPAWN_DISTANCE;
    fb.velocity.y = 0.0f;
    fb.velocity.z = dirZ * FIREBALL_SPAWN_DISTANCE;
    if (!Advance(fb)) {
        PoofFireball(fb);
        return;
    }
    fb.velocity.y = FIREBALL_BOUNCE_SPEED * 0.5f;
    KeepHorizontalSpeed(fb);

    if (fb.model != nullptr) fb.model->ToggleVisible(true);
    UpdateFireballModel(fb);
    UpdateFireballSound(fb);
}

static void UpdateFireballs() {
    for (u32 i = 0; i < MAX_FIREBALLS; ++i) {
        Fireball& fb = fireballs[i];
        if (!fb.active) continue;
        if (++fb.age >= FIREBALL_LIFETIME_FRAMES) {
            PoofFireball(fb);
            continue;
        }

        fb.velocity.y -= FIREBALL_GRAVITY;
        if (fb.velocity.y < -FIREBALL_MAX_FALL_SPEED) fb.velocity.y = -FIREBALL_MAX_FALL_SPEED;
        const Vec3 prevPos = fb.position;
        if (!Advance(fb)) {
            PoofFireball(fb);
            continue;
        }
        KeepHorizontalSpeed(fb);
        if (HitKart(fb, prevPos) || HitItem(fb, prevPos)) {
            PoofFireball(fb);
            continue;
        }

        fb.rollAngle += fb.speed / FIREBALL_RADIUS * FIREBALL_ROLL_FACTOR;
        while (fb.rollAngle > TWO_PI) fb.rollAngle -= TWO_PI;
        UpdateFireballModel(fb);
        UpdateFireballSound(fb);
    }
}

static void ResetHolder(FlowerHolder& holder) {
    holder.framesLeft = 0;
    holder.cpuTimer = 0;
    holder.shotsLeft = 0;
    holder.active = false;
}

static void EndFireFlower(Item::Player& itemPlayer) {
    if (itemPlayer.id < 12) ResetHolder(holders[itemPlayer.id]);
    Item::PlayerInventory& inventory = itemPlayer.inventory;
    if (inventory.currentItemId == FIRE_FLOWER) inventory.RemoveItems(inventory.currentItemCount);
}

void LoseFireFlower(Item::PlayerInventory& inventory) {
    const Item::Player* itemPlayer = inventory.itemPlayer;
    if (itemPlayer != nullptr && itemPlayer->id < 12) ResetHolder(holders[itemPlayer->id]);
    inventory.ClearAll();
}

static void ThrowFireball(Item::Player& itemPlayer, bool backward) {
    const u8 playerId = itemPlayer.id;
    if (playerId >= 12 || itemPlayer.isRemote || itemPlayer.inventory.currentItemId != FIRE_FLOWER) return;

    FlowerHolder& holder = holders[playerId];
    if (!holder.active) {
        holder.active = true;
        holder.framesLeft = FIRE_FLOWER_DURATION_FRAMES;
        holder.shotsLeft = FIRE_FLOWER_SHOTS;
    }
    holder.cpuTimer = 0;

    SpawnFireball(itemPlayer, backward);
    if (DriverMgr::isOnlineRace) SendEncodedFireballEvent(playerId, backward);

    if (holder.shotsLeft > 0) --holder.shotsLeft;
    if (holder.shotsLeft == 0) EndFireFlower(itemPlayer);
}

void UseFireFlower(Item::Player& itemPlayer) {
    ThrowFireball(itemPlayer, (itemPlayer.bitfield & ITEMPLAYER_STICK_BACK) != 0);
}

void SpawnRemoteFireball(const Item::Player& thrower, bool backward) {
    SpawnFireball(thrower, backward);
}

static void UpdateFlowerHolders() {
    Item::Manager* mgr = Item::Manager::sInstance;
    if (mgr == nullptr || mgr->players == nullptr) return;

    u8 count = mgr->playerCount;
    if (count > 12) count = 12;
    for (u8 i = 0; i < count; ++i) {
        Item::Player& itemPlayer = mgr->players[i];
        FlowerHolder& holder = holders[i];
        if (itemPlayer.isRemote || itemPlayer.inventory.currentItemId != FIRE_FLOWER) {
            ResetHolder(holder);
            continue;
        }

        if (!itemPlayer.isHuman) {
            const u16 wait = holder.active ? CPU_SHOT_INTERVAL_FRAMES : CPU_FIRST_SHOT_FRAMES;
            if (holder.cpuTimer < wait) ++holder.cpuTimer;
            else if (CanCpuThrowItem(itemPlayer)) {
                ThrowFireball(itemPlayer, false);
                if (!holder.active) continue;
            }
        }

        if (holder.active && holder.framesLeft > 0 && --holder.framesLeft == 0) EndFireFlower(itemPlayer);
    }
}

static void UpdateFireFlower() {
    UpdateFlowerHolders();
    UpdateFireballs();
}
RaceFrameHook FireFlowerUpdate(UpdateFireFlower);

static void InitFireFlower() {
    for (u32 i = 0; i < 12; ++i) ResetHolder(holders[i]);
    for (u32 i = 0; i < MAX_FIREBALLS; ++i) {
        fireballs[i].active = false;
        fireballs[i].age = 0;
        if (fireballs[i].emitter == nullptr) fireballs[i].emitter = Sound::Extra::CreateEmitter();
    }

    Item::Behavior& behavior = expandedBehaviourTable[FIRE_FLOWER];
    behavior.unknkown_0x0 = 1;
    behavior.unknkown_0x1 = 0;
    behavior.objId = OBJ_FIRE_FLOWER;
    behavior.numberOfItems = 1;
    behavior.unknown_0xc = 0;
    behavior.unknown_0x10 = 0;
    behavior.useType = Item::ITEMUSE_USE;
    behavior.useFunction = UseFireFlower;

    ModelDirector* models[MAX_FIREBALLS];
    CreateItemModels(FIREBALL_BRRES, fireballRes, models, MAX_FIREBALLS, missingModelLogged);
    for (u32 i = 0; i < MAX_FIREBALLS; ++i) fireballs[i].model = models[i];
}
RaceLoadHook FireFlowerInit(InitFireFlower);

} // namespace Race
} // namespace Pulsar
