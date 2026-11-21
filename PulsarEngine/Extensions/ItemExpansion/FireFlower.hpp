#ifndef _PUL_RACE_FIRE_FLOWER_HPP_
#define _PUL_RACE_FIRE_FLOWER_HPP_

#include <kamek.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/ItemPlayer.hpp>

namespace Pulsar {
namespace Race {

// How many players can hold a Fire Flower at once (ExpandedCapacityCheck in ItemSlotExpansion.cpp)
static const u32 FIRE_FLOWER_MAX_HELD = 4;

void UseFireFlower(Item::Player& itemPlayer);
void SpawnRemoteFireball(const Item::Player& thrower, bool backward);
void LoseFireFlower(Item::PlayerInventory& inventory);
void SendEncodedFireballEvent(u8 playerId, bool backward);

} // namespace Race
} // namespace Pulsar

#endif
