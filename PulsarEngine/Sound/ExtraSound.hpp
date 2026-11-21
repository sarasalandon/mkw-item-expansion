#ifndef _PUL_EXTRASOUND_
#define _PUL_EXTRASOUND_

#include <kamek.hpp>
#include <core/nw4r/snd.hpp>
#include <Sound/ExtraSoundIds.hpp>

namespace Pulsar {
namespace Sound {
namespace Extra {

bool IsReady();

bool Play(u32 soundId);
bool Play(u32 soundId, nw4r::snd::SoundHandle& handle);
bool Hold(u32 soundId, nw4r::snd::SoundHandle& handle);
void Stop(nw4r::snd::SoundHandle& handle, int fadeOutFrames = 0);

class Emitter;
static const u32 MAX_EMITTERS = 64;

Emitter* CreateEmitter();
void SetPosition(Emitter* emitter, const Vec3& position);
bool PlayAt(Emitter* emitter, u32 soundId);
bool HoldAt(Emitter* emitter, u32 soundId, nw4r::snd::SoundHandle& handle);

}  // namespace Extra
}  // namespace Sound
}  // namespace Pulsar

#endif
