#ifndef _PUL_RACE_THROWN_ITEM_UTIL_HPP_
#define _PUL_RACE_THROWN_ITEM_UTIL_HPP_

#include <kamek.hpp>
#include <MarioKartWii/Item/ItemPlayer.hpp>
#include <MarioKartWii/Item/Obj/ItemObj.hpp>
#include <MarioKartWii/Kart/KartStatus.hpp>
#include <MarioKartWii/KCL/Collision.hpp>
#include <MarioKartWii/KCL/KCLController.hpp>
#include <MarioKartWii/3D/Model/ModelDirector.hpp>

namespace Pulsar {
namespace Race {

static const u32 ITEM_KCL_FLOOR_TYPES = 0x20E80FFF;
static const u32 ITEM_KCL_WALL_TYPES = KCL_BITFIELD_WALL | KCL_BITFIELD_ITEM_WALL | KCL_BITFIELD_WALL_3
                                     | KCL_BITFIELD_SPECIAL_WALL;
static const u32 ITEM_KCL_KILL_TYPES = KCL_BITFIELD_FALL_BOUNDARY | KCL_BITFIELD_SOLID_FALL;

static const s32 NO_KART_IN_PATH = -1;

float Length2D(float x, float z);
float DistanceSqToSegment(const Vec3& point, const Vec3& from, const Vec3& to);

void PlayItemThrowAnimation(const Item::Player& thrower, bool backward);
void PlayItemVanishEffect(const Vec3& position);
void PlayItemSoundOnKart(u8 playerId, u32 soundId);

bool CheckItemCourseCollision(const Vec3& pos, const Vec3& prevPos, u32 kclMask, float radius, CollisionInfo& info,
                              u32& types);

bool CanKartBeHitByItem(const Kart::Status* status);
bool CanCpuThrowItem(const Item::Player& itemPlayer);
s32 FindKartInPath(u8 ownerId, const Vec3& from, const Vec3& to, float radius, u32 ignoredMask);
bool DamageKartWithItem(u8 victimId, u8 ownerId, DamageType damage);
Item::Obj* FindItemInPath(u8 ownerId, const Vec3& from, const Vec3& to, float radius, const ItemObjId* objIds,
                          u32 objIdCount, bool ignoreAllOfOwner);
u32 CreateItemModels(const char* brres, nw4r::g3d::ResFile& res, ModelDirector** models, u32 count,
                     bool& missingLogged);
void SetItemModelMtx(ModelDirector* model, const nw4r::math::MTX34& mtx);

} // namespace Race
} // namespace Pulsar

#endif
