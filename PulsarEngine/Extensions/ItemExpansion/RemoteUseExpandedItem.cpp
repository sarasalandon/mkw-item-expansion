#include <MarioKartWii/Item/ItemManager.hpp>
#include <MarioKartWii/Item/Obj/ObjProperties.hpp>
#include <MarioKartWii/Item/Obj/Gesso.hpp>
#include <MarioKartWii/Item/Obj/ItemEVENT.hpp>
#include <MarioKartWii/Driver/DriverManager.hpp>
#include <MarioKartWii/Input/InputManager.hpp>
#include <MarioKartWii/CourseMgr.hpp>
#include <MarioKartWii/Item/ItemBehaviour.hpp>
#include <MarioKartWii/Item/ItemSlot.hpp>
#include <MarioKartWii/Item/Obj/ItemObjHolder.hpp>
#include <MarioKartWii/RKNet/PacketMgr.hpp>
#include <Extensions/ItemExpansion/ItemObjDrop.hpp>
#include <Extensions/ItemExpansion/Boo.hpp>
#include <Extensions/ItemExpansion/Feather.hpp>
#include <Extensions/ItemExpansion/FireFlower.hpp>
#include <Extensions/ItemExpansion/Boomerang.hpp>
#include <PulsarSystem.hpp>

namespace Pulsar {
namespace Race {

extern "C" void* Pul_SendItemUseEvent(ItemObjId objId, u8 data);

enum CustomEventKind {
    CUSTOM_EVENT_BOO_USE,
    CUSTOM_EVENT_FEATHER_USE,
    CUSTOM_EVENT_SHROOM_STAR_USE,
    CUSTOM_EVENT_BOO_DROP,
    CUSTOM_EVENT_FEATHER_DROP,
    CUSTOM_EVENT_FIREBALL_FORWARD,
    CUSTOM_EVENT_FIREBALL_BACKWARD,
    CUSTOM_EVENT_BOOMERANG_FORWARD,
    CUSTOM_EVENT_BOOMERANG_BACKWARD,
    CUSTOM_EVENT_KIND_COUNT
};
static const u8 CUSTOM_EVENT_FLAG = 0x80;
static const u8 CUSTOM_EVENT_PLAYERS = 12;
typedef char CustomEventKindsFitInDataByte[(CUSTOM_EVENT_KIND_COUNT * CUSTOM_EVENT_PLAYERS <= 0x80) ? 1 : -1];

static void SendCustomEvent(CustomEventKind kind, u8 playerId) {
    if (playerId >= CUSTOM_EVENT_PLAYERS) return;
    if (RKNet::PacketMgr::sInstance == nullptr || Item::EVENTBuffer::sInstance == nullptr) return;
    Pul_SendItemUseEvent(OBJ_MUSHROOM, static_cast<u8>(CUSTOM_EVENT_FLAG | (kind * CUSTOM_EVENT_PLAYERS + playerId)));
}

void SendEncodedCustomUseEvent(ItemObjId realObjId, u8 playerId) {
    if (realObjId == OBJ_BOO) SendCustomEvent(CUSTOM_EVENT_BOO_USE, playerId);
    else if (realObjId == OBJ_FEATHER) SendCustomEvent(CUSTOM_EVENT_FEATHER_USE, playerId);
    else if (realObjId == OBJ_SHROOM_STAR) SendCustomEvent(CUSTOM_EVENT_SHROOM_STAR_USE, playerId);
}

void SendEncodedCustomDropEvent(ItemId dropType, u8 playerId) {
    if (dropType == BOO) SendCustomEvent(CUSTOM_EVENT_BOO_DROP, playerId);
    else if (dropType == FEATHER) SendCustomEvent(CUSTOM_EVENT_FEATHER_DROP, playerId);
}

void SendEncodedFireballEvent(u8 playerId, bool backward) {
    SendCustomEvent(backward ? CUSTOM_EVENT_FIREBALL_BACKWARD : CUSTOM_EVENT_FIREBALL_FORWARD, playerId);
}

void SendEncodedBoomerangEvent(u8 playerId, bool backward, bool lastThrow) {
    const CustomEventKind kind = backward ? CUSTOM_EVENT_BOOMERANG_BACKWARD : CUSTOM_EVENT_BOOMERANG_FORWARD;
    SendCustomEvent(kind, playerId);
    if (lastThrow) SendCustomEvent(kind, playerId);
}

static bool InterceptCustomRecvEVENT(Item::ObjHolder* holder, RKNet::EVENTAction action, u16 unk2,
                                      const Item::EVENTBuffer::Entry& entry, u32 time, bool flag) {
    ItemObjId objId = entry.objId;

    if (objId == OBJ_MUSHROOM && action == RKNet::EVENTACTION_USE && (entry.data[2] & CUSTOM_EVENT_FLAG) != 0) {
        const u8 code = entry.data[2] & ~CUSTOM_EVENT_FLAG;
        const u8 kind = code / CUSTOM_EVENT_PLAYERS;
        const u8 playerId = code % CUSTOM_EVENT_PLAYERS;
        Item::Manager* mgr = Item::Manager::sInstance;
        if (mgr == nullptr || playerId >= mgr->playerCount || kind >= CUSTOM_EVENT_KIND_COUNT) return false;
        Item::Player& player = mgr->players[playerId];

        switch (kind) {
            case CUSTOM_EVENT_BOO_USE:
                ApplyBooRemoteEffect(playerId);
                break;
            case CUSTOM_EVENT_FEATHER_USE:
                ApplyFeatherRemoteEffect(player);
                break;
            case CUSTOM_EVENT_SHROOM_STAR_USE:
                SetShroomStarActive(playerId);
                break;
            case CUSTOM_EVENT_BOO_DROP:
            case CUSTOM_EVENT_FEATHER_DROP:
                if (player.isRemote) {
                    EjectDroppedItem(kind == CUSTOM_EVENT_BOO_DROP ? BOO : FEATHER, player.GetPosition(), playerId);
                }
                break;
            case CUSTOM_EVENT_FIREBALL_FORWARD:
            case CUSTOM_EVENT_FIREBALL_BACKWARD:
                if (player.isRemote) SpawnRemoteFireball(player, kind == CUSTOM_EVENT_FIREBALL_BACKWARD);
                break;
            case CUSTOM_EVENT_BOOMERANG_FORWARD:
            case CUSTOM_EVENT_BOOMERANG_BACKWARD: {
                const u16 stamp = static_cast<u16>((entry.data[0] << 8) | entry.data[1]);
                if (player.isRemote) SpawnRemoteBoomerang(player, kind == CUSTOM_EVENT_BOOMERANG_BACKWARD, stamp);
                break;
            }
        }
        return false;
    }

    if (objId > OBJ_NONE && action == RKNet::EVENTACTION_USE) {
        const Item::UseEntry& useEntry = reinterpret_cast<const Item::UseEntry&>(entry);
        Item::Manager* mgr = Item::Manager::sInstance;
        if (mgr == nullptr) return false;
        u8 playerId = Item::EVENTBuffer::GetUSEPlayerId(useEntry);
        if (playerId >= mgr->playerCount) return false;
        Item::Player& player = mgr->players[playerId];
        switch (objId) {
            case OBJ_BOO:
                ApplyBooRemoteEffect(playerId);
                break;
            case OBJ_FEATHER:
                ApplyFeatherRemoteEffect(player);
                break;
            default:
                break;
        }
        return false;
    }
    if (objId > OBJ_NONE) {
        return false;
    }
    return holder->ProcessRecvEVENT(action, unk2, entry, time, flag);
}
kmCall(0x8079bf88, InterceptCustomRecvEVENT);

}//namespace Race
}//namespace Pulsar