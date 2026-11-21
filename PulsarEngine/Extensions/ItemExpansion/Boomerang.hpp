#ifndef _PUL_RACE_BOOMERANG_HPP_
#define _PUL_RACE_BOOMERANG_HPP_

#include <kamek.hpp>
#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/ItemPlayer.hpp>

namespace Pulsar {
namespace Race {

static const u32 BOOMERANG_MAX_HELD = 4;

void UseBoomerang(Item::Player& itemPlayer);
void SpawnRemoteBoomerang(const Item::Player& thrower, bool backward, u16 throwStamp);
void LoseBoomerang(Item::PlayerInventory& inventory);
u32 CountBoomerangsComingBack();
void SendEncodedBoomerangEvent(u8 playerId, bool backward, bool lastThrow);  // RemoteUseExpandedItem.cpp

} // namespace Race
} // namespace Pulsar

#endif
