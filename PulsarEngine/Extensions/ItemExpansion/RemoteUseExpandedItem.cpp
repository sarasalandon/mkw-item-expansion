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
#include <PulsarSystem.hpp>

namespace Pulsar {
namespace Race {

// Custom item uses (Boo, Feather, Shroom-Star) and drops are sent as a vanilla Mushroom USE EVENT, because the EVENT
// type field is 5 bits and 0x10 (OBJ_NONE) already means "empty slot", so OBJ_BOO and later can't go on the wire.
//
// A USE entry is always 3 bytes (EVENTHandler::ImportReceivedEntries sizes entries by type, 0x8079d76c):
// [sender's frameOfInput u16][1 data byte]. Vanilla's data byte is the playerId (0-11), so the top bit marks ours:
//     0x80 | (kind << 4) | playerId
//
// The first two bytes MUST be the real frame. The old encoding put 0xFF, objId there, i.e. a "timestamp" of 0xFFxx.
// The received-entry dedup in ItemRain.cpp (0x8065BDF8, CTGP's C265BDF8, installed in rain races) never overwrites a slot whose stored
// timestamp is newer, so once a custom event had used one of a player's 24 send slots, every later EVENT they sent in
// that slot (a Mushroom, a red shell break, a shot...) was processed with the stale custom payload. That made a
// Mushroom replay the last Boo/Feather, left red shells unbroken on other screens until they timed out, and fed
// garbage player ids to the shell/cloud handlers. vanilla sendItemUseEvent writes the frame itself.
extern "C" void* Pul_SendItemUseEvent(ItemObjId objId, u8 data);

enum CustomEventKind {
    CUSTOM_EVENT_BOO_USE,
    CUSTOM_EVENT_FEATHER_USE,
    CUSTOM_EVENT_SHROOM_STAR_USE,
    CUSTOM_EVENT_BOO_DROP,
    CUSTOM_EVENT_FEATHER_DROP,
    CUSTOM_EVENT_KIND_COUNT
};
static const u8 CUSTOM_EVENT_FLAG = 0x80;

static void SendCustomEvent(CustomEventKind kind, u8 playerId) {
    if (playerId >= 12) return;
    if (RKNet::PacketMgr::sInstance == nullptr || Item::EVENTBuffer::sInstance == nullptr) return; // not online
    Pul_SendItemUseEvent(OBJ_MUSHROOM, static_cast<u8>(CUSTOM_EVENT_FLAG | (kind << 4) | playerId));
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

static bool InterceptCustomRecvEVENT(Item::ObjHolder* holder, RKNet::EVENTAction action, u16 unk2,
                                      const Item::EVENTBuffer::Entry& entry, u32 time, bool flag) {
    ItemObjId objId = entry.objId;

    // entry.data is the raw payload: [frame u16][data byte]
    if (objId == OBJ_MUSHROOM && action == RKNet::EVENTACTION_USE && (entry.data[2] & CUSTOM_EVENT_FLAG) != 0) {
        const u8 kind = (entry.data[2] >> 4) & 0x7;
        const u8 playerId = entry.data[2] & 0xF;
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
                // Locally-authored drops are already spawned on this console.
                if (player.isRemote) {
                    EjectDroppedItem(kind == CUSTOM_EVENT_BOO_DROP ? BOO : FEATHER, player.GetPosition(), playerId);
                }
                break;
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