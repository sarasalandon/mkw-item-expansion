#include <kamek.hpp>
#include <core/RK/RKSystem.hpp>
#include <core/rvl/dvd/dvd.hpp>
#include <core/rvl/OS/OS.hpp>
#include <core/egg/mem/Heap.hpp>
#include <core/egg/Audio/ArcPlayer.hpp>
#include <core/egg/Audio/AudioUtility.hpp>
#include <MarioKartWii/Audio/AudioManager.hpp>
#include <core/nw4r/snd/Sound3DManager.hpp>
#include <core/nw4r/snd/Sound3DActor.hpp>
#include <core/nw4r/snd/Sound3DListener.hpp>
#include <MarioKartWii/Audio/RSARPlayer.hpp>
#include <MarioKartWii/Scene/RootScene.hpp>
#include <Sound/ExtraSound.hpp>

namespace Pulsar {
namespace Sound {
namespace Extra {
using namespace nw4r;

static const char archivePath[] = "/sound/revo_extra.brsar";

static const u32 playerWorkSize = 0x40000;
static const u32 playerWorkSizeSmall = 0x20000;

static EGG::ArcPlayer* arcPlayer = nullptr;
static snd::SoundArchivePlayer* archivePlayer = nullptr;
static snd::SoundHeap* soundHeap = nullptr;
static bool initDone = false;
static bool isReady = false;
static snd::SoundHandle oneShotHandle;

static const u32 heapCount = 5;
static const char* const heapNames[heapCount] = { "RootScene MEM2", "RootScene MEM1", "EGGSystem", "EGGRootMEM2", "EGGRootMEM1" };

static EGG::ExpHeap* HeapAt(u32 i) {
    RootScene* root = RootScene::sInstance;
    switch(i) {
        case 0:
            if(root == nullptr) return nullptr;
            return root->expHeapGroup.heaps[1];
        case 1:
            if(root == nullptr) return nullptr;
            return root->expHeapGroup.heaps[0];
        case 2: return RKSystem::mInstance.EGGSystem;
        case 3: return RKSystem::mInstance.EGGRootMEM2;
        default: return RKSystem::mInstance.EGGRootMEM1;
    }
}

static void* AllocFromPersistentHeap(u32 size) {
    for(u32 i = 0; i < heapCount; ++i) {
        EGG::ExpHeap* heap = HeapAt(i);
        if(heap == nullptr) continue;
        void* ret = EGG::Heap::alloc(size, 0x20, heap);
        if(ret != nullptr) {
            OS::Report("[ExtraSound] 0x%x bytes from %s\n", size, heapNames[i]);
            return ret;
        }
    }
    return nullptr;
}

static void ReportHeaps() {
    for(u32 i = 0; i < heapCount; ++i) {
        EGG::ExpHeap* heap = HeapAt(i);
        if(heap == nullptr) continue;
        OS::Report("[ExtraSound]   %s: 0x%x free, largest block 0x%x\n", heapNames[i], heap->getTotalFreeSize(), heap->getAllocatableSize(0x20));
    }
}

static u32 RoundUp32(u32 size) { return ((size + 0x1F) & ~0x1F) + 0x40; }

static const u32 maxListeners = 4;
static const u32 listenerCopySize = 0x4C;
static const u32 objSlack = 0x10;
static u32 managerMem[(sizeof(snd::Sound3DManager) + objSlack) / 4];
static u32 listenerMem[maxListeners][(sizeof(snd::Sound3DListener) + objSlack) / 4];
static u32 emitterMem[MAX_EMITTERS][(sizeof(snd::Sound3DActor) + objSlack) / 4];
static snd::Sound3DManager* manager3D = nullptr;
static snd::Sound3DListener* listeners[maxListeners];
static u32 emitterCount = 0;
static snd::SoundHandle emitterOneShotHandle;

static ut::detail::LinkListImpl& ListenerListOf(snd::Sound3DManager& manager) {
    return reinterpret_cast<ut::detail::LinkListImpl&>(manager.listenerList);
}

static void Init3D() {
    const snd::SoundArchive& archive = archivePlayer->GetSoundArchive();
    snd::Sound3DManager* manager = new(managerMem) snd::Sound3DManager;
    const u32 paramSize = manager->GetRequiredMemSize(&archive);
    void* paramMem = AllocFromPersistentHeap(paramSize);
    if(paramMem == nullptr || !manager->Setup(&archive, paramMem, paramSize)) {
        OS::Report("[ExtraSound] no memory for the 3D sound params (0x%x bytes): positional sounds are off\n", paramSize);
        return;
    }
    for(u32 i = 0; i < maxListeners; ++i) listeners[i] = new(listenerMem[i]) snd::Sound3DListener;
    manager3D = manager;
}

static void SyncListeners() {
    const snd::Sound3DListener* source[maxListeners];
    u32 count = 0;
    Audio::Manager* game = Audio::Manager::sInstance;
    if(game != nullptr) {
        ut::detail::LinkListImpl& gameList = ListenerListOf(game->sound3DManager);
        for(ut::LinkListNode* node = gameList.node.next; node != &gameList.node && count < maxListeners; node = node->next) {
            source[count++] = reinterpret_cast<const snd::Sound3DListener*>(
                reinterpret_cast<const u8*>(node) - offsetof(snd::Sound3DListener, link));
        }
    }

    ut::detail::LinkListImpl& ours = ListenerListOf(*manager3D);
    while(static_cast<u32>(ours.count) < count) ours.Insert(ours.GetEndIter(), &listeners[ours.count]->link);
    while(static_cast<u32>(ours.count) > count) ours.Erase(&listeners[ours.count - 1]->link);
    for(u32 i = 0; i < count; ++i) memcpy(listeners[i], source[i], listenerCopySize);
}

static void Init() {
    initDone = true;
    const s32 entry = DVD::ConvertPathToEntryNum(archivePath);
    OS::Report("[ExtraSound] init: %s -> disc entry %d\n", archivePath, entry);
    if(entry < 0) {
        OS::Report("[ExtraSound] %s is not on the disc; check the file name and the Riivolution folder\n", archivePath);
        return;
    }

    DVD::FileInfo info;
    if(!DVD::Open(archivePath, &info)) {
        OS::Report("[ExtraSound] could not open %s\n", archivePath);
        return;
    }
    const u32 fileSize = info.length;
    DVD::Close(&info);
    u32 soundHeapSize = ((fileSize + 0x1F) & ~0x1F) + playerWorkSize;

    const u32 objSize = RoundUp32(sizeof(snd::SoundHeap)) + RoundUp32(sizeof(snd::SoundArchivePlayer)) + RoundUp32(sizeof(EGG::ArcPlayer));
    u8* objMem = static_cast<u8*>(AllocFromPersistentHeap(objSize));
    void* heapMem = AllocFromPersistentHeap(soundHeapSize);
    if(heapMem == nullptr) {
        soundHeapSize = ((fileSize + 0x1F) & ~0x1F) + playerWorkSizeSmall;
        heapMem = AllocFromPersistentHeap(soundHeapSize);
    }
    if(objMem == nullptr || heapMem == nullptr) {
        OS::Report("[ExtraSound] could not allocate 0x%x bytes (archive is 0x%x); free space per heap:\n", soundHeapSize + objSize, fileSize);
        ReportHeaps();
        return;
    }

    soundHeap = new(objMem) snd::SoundHeap;
    objMem += RoundUp32(sizeof(snd::SoundHeap));
    archivePlayer = new(objMem) snd::SoundArchivePlayer;
    objMem += RoundUp32(sizeof(snd::SoundArchivePlayer));
    arcPlayer = new(objMem) EGG::ArcPlayer(archivePlayer, soundHeap);

    if(!soundHeap->Create(heapMem, soundHeapSize)) {
        OS::Report("[ExtraSound] SoundHeap::Create failed\n");
        return;
    }
    if(arcPlayer->OpenDVDArchive(archivePath, soundHeap) == nullptr || !archivePlayer->IsAvailable()) {
        OS::Report("[ExtraSound] could not open %s (bad file or heap too small)\n", archivePath);
        return;
    }

    const u32 groupCount = archivePlayer->GetSoundArchive().GetGroupCount();
    if(groupCount == 0) {
        OS::Report("[ExtraSound] %s has no loadable groups: its last group is reserved. Open and save it with RsarEditor\n", archivePath);
    }
    u32 loaded = 0;
    for(u32 group = 0; group < groupCount; ++group) {
        if(archivePlayer->LoadGroup(group, soundHeap, 0)) ++loaded;
        else OS::Report("[ExtraSound] LoadGroup %d failed (heap too small?)\n", group);
    }
    isReady = true;
    OS::Report("[ExtraSound] ready: %d of %d groups loaded\n", loaded, groupCount);
    Init3D();
}

static bool hbmActive = false;
static bool pauseApplied = false;

static void PauseAllExtra(bool pause) {
    for(u32 i = 0; i < archivePlayer->soundPlayerCount; ++i) archivePlayer->GetSoundPlayer(i).PauseAllSound(pause, 0);
}

static void FollowGamePause() {
    const Audio::RSARPlayer* game = Audio::RSARPlayer::sInstance;
    const bool pause = hbmActive || (game != nullptr && game->isPaused);
    if(pause) PauseAllExtra(true);
    else if(pauseApplied) PauseAllExtra(false);
    pauseApplied = pause;
}

static void HBMEnterWithExtra() {
    EGG::AudioUtility::HBM::enter();
    hbmActive = true;
}
kmCall(0x807177e0, HBMEnterWithExtra);

static void HBMExitWithExtra(bool r3) {
    EGG::AudioUtility::HBM::exit(r3);
    hbmActive = false;
}
kmCall(0x80717870, HBMExitWithExtra);
kmCall(0x807178bc, HBMExitWithExtra);

static void ManagerCalcWithExtra(EGG::SimpleAudioMgr* mgr) {
    if(!initDone) Init();
    if(isReady) {
        FollowGamePause();
        if(manager3D != nullptr) SyncListeners();
        archivePlayer->Update();
    }
    mgr->EGG::SimpleAudioMgr::Calc();
}
kmCall(0x8071765c, ManagerCalcWithExtra);

bool IsReady() {
    if(!initDone) {
        static bool warned = false;
        if(!warned) OS::Report("[ExtraSound] not started: the audio hook at 0x8071765c has not run yet\n");
        warned = true;
    }
    return isReady;
}

bool Play(u32 soundId, snd::SoundHandle& handle) {
    if(!isReady) return false;
    const snd::SoundStartable::StartResult r = archivePlayer->detail_StartSound(&handle, soundId, nullptr);
    if(r != snd::SoundStartable::START_SUCCESS) OS::Report("[ExtraSound] sound %d failed to start (result %d)\n", soundId, r);
    return r == snd::SoundStartable::START_SUCCESS;
}

bool Play(u32 soundId) {
    return Play(soundId, oneShotHandle);
}

bool Hold(u32 soundId, snd::SoundHandle& handle) {
    if(!isReady) return false;
    return archivePlayer->detail_HoldSound(&handle, soundId, nullptr) == snd::SoundStartable::START_SUCCESS;
}

void Stop(snd::SoundHandle& handle, int fadeOutFrames) {
    if(handle.basicSound != nullptr) handle.basicSound->Stop(fadeOutFrames);
}

static snd::Sound3DActor* ActorOf(Emitter* emitter) {
    return reinterpret_cast<snd::Sound3DActor*>(emitter);
}

Emitter* CreateEmitter() {
    if(!isReady || manager3D == nullptr || emitterCount >= MAX_EMITTERS) return nullptr;
    snd::Sound3DActor* actor = new(emitterMem[emitterCount++]) snd::Sound3DActor(*archivePlayer, *manager3D);
    return reinterpret_cast<Emitter*>(actor);
}

void SetPosition(Emitter* emitter, const Vec3& position) {
    if(emitter == nullptr) return;
    math::VEC3 pos;
    pos.x = position.x;
    pos.y = position.y;
    pos.z = position.z;
    ActorOf(emitter)->SetPosition(pos);
}

bool PlayAt(Emitter* emitter, u32 soundId) {
    if(!isReady || emitter == nullptr) return false;
    snd::SoundStartable* startable = ActorOf(emitter);
    const snd::SoundStartable::StartResult r = startable->detail_StartSound(&emitterOneShotHandle, soundId, nullptr);
    if(r != snd::SoundStartable::START_SUCCESS && r != snd::SoundStartable::START_ERR_LOW_PRIORITY) {
        OS::Report("[ExtraSound] sound %d failed to start on an emitter (result %d)\n", soundId, r);
    }
    return r == snd::SoundStartable::START_SUCCESS;
}

bool HoldAt(Emitter* emitter, u32 soundId, snd::SoundHandle& handle) {
    if(!isReady || emitter == nullptr) return false;
    snd::SoundStartable* startable = ActorOf(emitter);
    return startable->detail_HoldSound(&handle, soundId, nullptr) == snd::SoundStartable::START_SUCCESS;
}

}  // namespace Extra
}  // namespace Sound
}  // namespace Pulsar
