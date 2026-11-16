// XDroid360 MIRROR-1: streams the guest output (H.264 or HEVC) and the game
// audio (s16 stereo, 48 kHz) to the PC viewer (xdroid-viewer) over
// `adb reverse`. Design: docs/design/mirror.md. Off unless the viewer writes
// xdroid_mirror = true and a token into live.toml.
#ifndef XDROID_MIRROR_H_
#define XDROID_MIRROR_H_

#include <atomic>
#include <string_view>

namespace xdroid {

// Starts the mirror threads (once). They idle until xdroid_mirror is true.
void InstallMirror();

// live.toml key xdroid_mirror_token: 64 hex digits, the server token then the
// client token. Anything else clears it. Never logged.
void MirrorSetToken(std::string_view hex);
// live.toml string cvars: xdroid_mirror_cast (TV-2 receiver ip:port; empty:
// PC viewer) and xdroid_mirror_keys. False: |name| is neither.
bool MirrorSetString(std::string_view name, std::string_view value);

// UI thread, emulator teardown, after SetLiveGraphicsSystem(nullptr) and
// before the graphics system goes away: returns once the capture thread
// holds nothing of the old device. Takes no lock.
void MirrorDetachGraphics();

// Paint thread, after a successful present.
void MirrorOnPresent();

// True while a viewer gets the audio stream.
extern std::atomic<bool> g_mirror_audio_armed;
inline bool MirrorAudioArmed() {
  return g_mirror_audio_armed.load(std::memory_order_relaxed);
}
// Guest audio thread: one guest frame (6 channels x 256 samples, channel
// after channel, big-endian floats), scaled by |gain|. One |source| (audio
// client) is streamed: the first after arming, or a new one once it was
// silent for 100 ms. No lock, no allocation, no syscall.
void MirrorOnAudioFrame(const void* source, const float* frame, float gain);
// Audio callback, after the level forensics: |pairs| stereo floats of phone
// output, silenced while xdroid_mirror_mute_local and a viewer gets audio.
void MirrorMuteLocal(float* out, int32_t pairs);

}  // namespace xdroid

#endif  // XDROID_MIRROR_H_
