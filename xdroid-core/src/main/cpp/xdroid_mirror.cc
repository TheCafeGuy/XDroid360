// XDroid360 MIRROR-1: the guest output and the game audio to the PC viewer.
// See xdroid_mirror.h and docs/design/mirror.md.
//
// Threads:
// - "Mirror Net" connects to the viewer (127.0.0.1, mapped by `adb reverse`),
//   runs the handshake, reads control messages, answers pings and sends the
//   audio. It owns the sockets and the session number.
// - "Mirror Cap" wakes on every present while a session runs and copies the
//   newest guest output into the encoder's input surface with one blit on
//   the graphics queue (xdroid_mirror_input 1: blit, read back, convert to
//   NV12 on the CPU and post it as a gralloc buffer to the same surface).
// - "Mirror Out" (one per encoder) sends the encoded frames.
// Game threads only touch atomics: MirrorOnPresent bumps a futex word and
// MirrorOnAudioFrame fills one ring slot.
#include "xdroid_mirror.h"

#include <android/data_space.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <arm_neon.h>
#include <arpa/inet.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xdroid_cast.h"
#include "xdroid_live.h"
#include "xdroid_mirror_proto.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_util.h"
#include "xenia/ui/xdroid_ui_hooks.h"

DEFINE_bool(xdroid_mirror, false,
            "MIRROR-1: stream the game to xdroid-viewer on the PC over adb "
            "reverse. The viewer sets it, with xdroid_mirror_token.",
            "XDroid360");
DEFINE_string(xdroid_mirror_cast, "",
              "TV-2: Cast receiver ip:port (UDP port of the ANSWER) to stream "
              "to instead of the PC viewer; xdroid_mirror_token holds the "
              "OFFER's AES key then IV mask. Empty: PC viewer.",
              "XDroid360");
DEFINE_int32(xdroid_mirror_port, 27360,
             "MIRROR-1: viewer port on 127.0.0.1 (adb reverse).", "XDroid360");
DEFINE_int32(xdroid_mirror_height, 720,
             "MIRROR-1: encoded height (144 to 1440); the width is 16:9.",
             "XDroid360");
DEFINE_int32(xdroid_mirror_kbps, 16000,
             "MIRROR-1: video bitrate in kbit/s.", "XDroid360");
DEFINE_int32(xdroid_mirror_operating_rate, 120,
             "MIRROR-1: encoder operating rate in frames/s, 30 to 240 "
             "(live). 30 = the first builds: about 16 ms a frame.",
             "XDroid360");
DEFINE_int32(xdroid_mirror_codec, 1, "MIRROR-1: 1 = H.264, 2 = HEVC.",
             "XDroid360");
DEFINE_int32(xdroid_mirror_bitrate_mode, 2,
             "MIRROR-1: encoder bitrate mode: 1 = VBR, 2 = CBR, 3 = CBR with "
             "frame drops.",
             "XDroid360");
DEFINE_int32(xdroid_mirror_intra_refresh, 30,
             "MIRROR-1: intra refresh period in frames, so no frame is a big "
             "key frame; 0 = key frames only.",
             "XDroid360");
DEFINE_int32(xdroid_mirror_key_interval, 10,
             "MIRROR-1: seconds between encoder key frames (1 to 60).",
             "XDroid360");
DEFINE_int32(xdroid_mirror_input, 1,
             "MIRROR-1 R2: 1 = own BT.709 NV12 from the CPU into the encoder "
             "input surface (default; falls back to 0 when the surface layout "
             "probe or a lock fails), 0 = RGBA swapchain on that surface (the "
             "vendor converts it on the GPU, queued behind the game). OnePlus "
             "c2.qti.avc 720p with the low-latency key: present-to-output "
             "28.7-30.1 -> 9.5-11.9 ms, TV p50/p95 36-41/63-66 -> 23-25/30-34 "
             "ms, game 30 FPS both. Live: the encoder restarts.",
             "XDroid360");
DEFINE_string(xdroid_mirror_keys, "vendor.qti-ext-enc-low-latency.enable=1",
              "MIRROR-1: extra encoder format keys, \"name=value,...\": an "
              "integer is set as int32, a number with '.' as float, else a "
              "string. Default: a Qualcomm vendor key (other encoders ignore "
              "it); \"\" turns it off. Live: the encoder restarts.",
              "XDroid360");
DEFINE_bool(xdroid_mirror_audio, true,
            "MIRROR-1: also stream the game audio.", "XDroid360");
DEFINE_bool(xdroid_mirror_mute_local, true,
            "MIRROR-1: silence the phone speaker while the viewer gets the "
            "game audio.",
            "XDroid360");
DEFINE_bool(xdroid_mirror_blank_local, true,
            "MIRROR-1: black phone screen while a viewer is connected (no "
            "guest paint, dark OLED).",
            "XDroid360");

namespace xdroid {

std::atomic<bool> g_mirror_audio_armed{false};

namespace {

namespace mp = mirror;
using xe::ui::vulkan::VulkanDevice;
using xe::ui::vulkan::VulkanInstance;
using xe::ui::vulkan::VulkanPresenter;

// R2 failed once: RGBA swapchain for the rest of the process.
std::atomic<bool> g_yuv_failed{false};

int64_t NowUs() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return int64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int32_t ClampRate(int64_t rate) {
  return int32_t(std::clamp<int64_t>(rate, 30, 240));
}

int32_t ClampKbps(int64_t kbps) {
  return int32_t(std::clamp<int64_t>(kbps, 500, 200000));
}

// Token: the server key (the viewer proves it) then the client key (the
// app proves it). Only the app and the viewer know it.
using Token = std::array<uint8_t, 2 * mp::kTokenBytes>;
std::mutex g_token_mutex;
Token g_token{};
bool g_token_valid = false;

// Live string cvars (TV-2 receiver, extra encoder keys): a std::string cvar
// is no word-sized store, so the live loader hands them over here instead of
// writing the cvar under the reading threads. Unset: the startup value.
struct LiveString {
  std::string value;
  bool set = false;
};
LiveString g_cast_live, g_keys_live;
// Bumped when the keys change: the capture thread compares it on every wake
// without the mutex or a string copy.
std::atomic<uint32_t> g_keys_gen{0};

bool GetToken(Token& token) {
  std::lock_guard<std::mutex> lock(g_token_mutex);
  token = g_token;
  return g_token_valid;
}

std::string GetLive(const LiveString& live, const std::string& startup) {
  std::lock_guard<std::mutex> lock(g_token_mutex);
  return live.set ? live.value : startup;
}

std::string GetCast() {
  return GetLive(g_cast_live, cvars::xdroid_mirror_cast);
}

// Constant time, so the time of a wrong guess tells nothing.
bool SameBytes(const uint8_t* a, const uint8_t* b, size_t size) {
  uint8_t diff = 0;
  for (size_t i = 0; i < size; ++i) {
    diff |= a[i] ^ b[i];
  }
  return !diff;
}

// Session state. The net thread writes it; the capture thread follows
// g_session and stores the session it serves in g_session_ack.
std::atomic<uint32_t> g_session{0};
std::atomic<uint32_t> g_session_ack{0};
std::atomic<uint32_t> g_wake{0};  // Futex word of the capture thread.
std::atomic<int> g_video_fd{-1};
std::atomic<uint32_t> g_width{0};
std::atomic<uint32_t> g_height{0};
std::atomic<uint8_t> g_codec{uint8_t(mp::Codec::kAvc)};
std::atomic<bool> g_want_key{false};
std::atomic<int32_t> g_kbps{0};  // Session bitrate: the cvar, then the viewer.
std::atomic<int32_t> g_rate{0};  // Session encoder operating rate, the same.
std::atomic<bool> g_net_error{false};
std::atomic<CastSender*> g_cast{nullptr};  // TV session, else the viewer.

// Emulator teardown: the capture thread frees its Capture, so no Vulkan
// object of the old device survives, and acks the generation.
std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_gfx_gen{0};
std::atomic<uint32_t> g_gfx_ack{0};

void Wake() {
  g_wake.fetch_add(1, std::memory_order_release);
  g_wake.notify_one();
}

// Sends every byte; SO_SNDTIMEO bounds each call.
bool SendAll(int fd, iovec* iov, int count) {
  while (count) {
    msghdr message{};
    message.msg_iov = iov;
    message.msg_iovlen = size_t(count);
    ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent <= 0) {
      return false;
    }
    while (count && size_t(sent) >= iov->iov_len) {
      sent -= ssize_t(iov->iov_len);
      ++iov;
      --count;
    }
    if (count) {
      iov->iov_base = static_cast<uint8_t*>(iov->iov_base) + sent;
      iov->iov_len -= size_t(sent);
    }
  }
  return true;
}

mp::PacketHeader Header(mp::Packet type, uint8_t flags, size_t size,
                        int64_t pts_us, int64_t aux_us) {
  mp::PacketHeader header{};
  header.type = uint8_t(type);
  header.flags = flags;
  header.size = uint32_t(size);
  header.pts_us = pts_us;
  header.aux_us = aux_us;
  return header;
}

// Audio ring: one slot per guest frame (256 stereo samples, 5.3 ms). One
// writer (see MirrorOnAudioFrame), one reader (net thread). The
// reader checks g_audio_claim after its copy and drops slots the writer
// started to overwrite meanwhile.
constexpr uint32_t kGuestSamples = 256;
constexpr uint32_t kAudioSlots = 64;   // 341 ms.
constexpr uint32_t kAudioBatch = 16;   // Slots per packet at most.
constexpr uint32_t kAudioMaxLag = 32;  // An older backlog is dropped.
struct AudioSlot {
  int64_t stamp_us;
  int16_t pcm[kGuestSamples * 2];
};
AudioSlot g_audio_ring[kAudioSlots];
std::atomic<uint64_t> g_audio_claim{0};  // Slots the writer started.
std::atomic<uint64_t> g_audio_seq{0};    // Slots the writer finished.
std::atomic<const void*> g_audio_source{nullptr};
std::atomic<int64_t> g_audio_source_us{0};  // Last frame of g_audio_source.

// Sends the new slots; false if the socket failed.
bool SendAudio(int fd, uint64_t& read, bool& loss) {
  const uint64_t written = g_audio_seq.load(std::memory_order_acquire);
  if (written - read > kAudioMaxLag) {
    read = written - 2;
    loss = true;
  }
  while (read < written) {
    const uint32_t count =
        uint32_t(std::min<uint64_t>(written - read, kAudioBatch));
    AudioSlot slots[kAudioBatch];
    for (uint32_t i = 0; i < count; ++i) {
      slots[i] = g_audio_ring[(read + i) % kAudioSlots];
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    // Slot `read + i` is whole while the writer has not started slot
    // `read + i + kAudioSlots`, which reuses it.
    const uint64_t claimed = g_audio_claim.load(std::memory_order_relaxed);
    uint32_t first = 0;
    if (claimed > read + kAudioSlots) {
      first = uint32_t(std::min<uint64_t>(claimed - read - kAudioSlots, count));
      loss = true;
    }
    if (first < count) {
      mp::PacketHeader header = Header(
          mp::Packet::kAudio, loss ? mp::kFlagLoss : 0,
          (count - first) * sizeof(slots[0].pcm), slots[first].stamp_us,
          NowUs());
      iovec iov[kAudioBatch + 1];
      iov[0] = {&header, sizeof(header)};
      for (uint32_t i = first; i < count; ++i) {
        iov[1 + i - first] = {slots[i].pcm, sizeof(slots[i].pcm)};
      }
      if (!SendAll(fd, iov, int(1 + count - first))) {
        return false;
      }
      loss = false;
    }
    read += count;
  }
  return true;
}

// TV-2: the new slots into the Opus encoder, its frames to the TV.
void SendCastAudio(CastSender& cast, CastOpus& opus, uint64_t& read) {
  const uint64_t written = g_audio_seq.load(std::memory_order_acquire);
  if (written - read > kAudioMaxLag) {
    read = written - 2;
  }
  for (; read < written; ++read) {
    const AudioSlot slot = g_audio_ring[read % kAudioSlots];
    std::atomic_thread_fence(std::memory_order_acquire);
    // Whole while the writer has not started the slot that reuses it.
    if (g_audio_claim.load(std::memory_order_relaxed) <= read + kAudioSlots) {
      opus.Push(slot.pcm, kGuestSamples, slot.stamp_us);
    }
  }
  opus.Drain(cast);
}

bool SendPong(int fd, int64_t value) {
  mp::PacketHeader header = Header(mp::Packet::kPong, 0, 0, NowUs(), value);
  iovec iov = {&header, sizeof(header)};
  return SendAll(fd, &iov, 1);
}

class Capture {
 public:
  // Encoder, input surface and swapchain for the current session; null on
  // failure (logged when |log|).
  static std::unique_ptr<Capture> Create(bool log);
  ~Capture();
  // Copies the newest guest output if it changed. False: recreate.
  bool Frame();
  // xdroid_mirror_input or xdroid_mirror_keys changed since the creation.
  bool Stale() const {
    return requested_ != cvars::xdroid_mirror_input ||
           keys_gen_ != g_keys_gen.load(std::memory_order_relaxed);
  }

 private:
  Capture() = default;
  bool Init();
  bool CreateEncoder();
  bool CreateSwapchain();
  bool CreateReadback();
  bool CreateCommands();
  bool PostYuv();
  void OutLoop();
  void SetParameter(const char* key, int32_t value, bool real = false);
  bool Fail(const char* what, int64_t code) {
    if (log_) {
      XELOGW("XDROID-MIRROR: {} failed ({})", what, code);
    }
    return false;
  }

  bool log_ = true;
  VulkanPresenter* presenter_ = nullptr;
  VulkanDevice* device_ = nullptr;
  int fd_ = -1;
  mp::Codec codec_ = mp::Codec::kAvc;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  int32_t kbps_ = 0;
  int32_t rate_ = 0;
  int tier_ = 0;  // Encoder keys accepted: 2 all, 1 no profile, 0 minimal.
  // R2: NV12 we convert from a read back blit goes to the input surface.
  bool yuv_ = false;
  int requested_ = 0;      // xdroid_mirror_input at creation.
  uint32_t keys_gen_ = 0;  // g_keys_gen at creation.
  // R2 gralloc layout from the probe: bytes from the Y pointer.
  size_t y_stride_ = 0;
  size_t c_stride_ = 0;
  ptrdiff_t cb_ = 0;
  ptrdiff_t cr_ = 0;
  bool cached_ = false;  // Read back memory is HOST_CACHED.
  VkImage rgba_ = VK_NULL_HANDLE;
  VkDeviceMemory rgba_memory_ = VK_NULL_HANDLE;
  VkBuffer readback_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
  const uint8_t* mapped_ = nullptr;

  AMediaCodec* encoder_ = nullptr;
  bool encoder_started_ = false;
  ANativeWindow* window_ = nullptr;

  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_ = {};
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkFilter filter_ = VK_FILTER_NEAREST;
  std::vector<VkImage> images_;
  std::vector<VkSemaphore> present_semaphores_;
  VkSemaphore acquire_semaphore_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  // A blit was submitted and its fence not yet seen: the destructor waits.
  bool submitted_ = false;
  uint64_t serial_ = UINT64_MAX;

  std::thread out_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> encoder_failed_{false};
  std::atomic<uint64_t> copies_{0};
  std::atomic<uint64_t> drops_{0};
  uint64_t frames_ = 0;  // Out thread only.
  uint64_t bytes_ = 0;
  int64_t encode_us_ = 0;
  // Capture thread: acquire to present, and the longest present (it holds
  // the queue lock the GPU thread needs). The Out thread logs both.
  std::atomic<int64_t> capture_us_{0};
  std::atomic<int64_t> present_max_us_{0};
  bool acquire_logged_ = false;
};

std::unique_ptr<Capture> Capture::Create(bool log) {
  std::unique_ptr<Capture> capture(new Capture());
  capture->log_ = log;
  capture->requested_ = cvars::xdroid_mirror_input;
  capture->keys_gen_ = g_keys_gen.load(std::memory_order_relaxed);
  capture->yuv_ = capture->requested_ == 1 &&
                  !g_yuv_failed.load(std::memory_order_relaxed);
  if (!capture->Init()) {
    if (!capture->yuv_) {
      return nullptr;
    }
    g_yuv_failed.store(true, std::memory_order_relaxed);
    return Create(log);
  }
  return capture;
}

bool Capture::Init() {
  xe::gpu::GraphicsSystem* graphics_system = GetLiveGraphicsSystem();
  presenter_ = graphics_system ? dynamic_cast<VulkanPresenter*>(
                                     graphics_system->presenter())
                               : nullptr;
  if (!presenter_) {
    return Fail("Vulkan presenter lookup", 0);
  }
  device_ = presenter_->vulkan_device();
  if (!device_->vulkan_instance()->extensions().ext_KHR_android_surface) {
    return Fail("VK_KHR_android_surface lookup", 0);
  }
  fd_ = g_video_fd.load(std::memory_order_relaxed);
  width_ = g_width.load(std::memory_order_relaxed);
  height_ = g_height.load(std::memory_order_relaxed);
  codec_ = mp::Codec(g_codec.load(std::memory_order_relaxed));
  kbps_ = g_kbps.load(std::memory_order_relaxed);
  rate_ = g_rate.load(std::memory_order_relaxed);
  yuv_ = yuv_ && !(width_ & 1) && !(height_ & 1);
  if (!CreateEncoder() ||
      !(yuv_ ? CreateReadback() : CreateSwapchain()) || !CreateCommands()) {
    return false;
  }
  out_ = std::thread([this] { OutLoop(); });
  XELOGI(
      "XDROID-MIRROR start: {} {}x{}, {} kbps, bitrate mode {}, intra "
      "refresh {}, key every {} s, encoder keys {}, swapchain {}x{} format {} "
      "filter {}, {} images, input {} (y stride {}, chroma stride {}, cb {}, "
      "cr {}, read back cached {})",
      codec_ == mp::Codec::kHevc ? "HEVC" : "H.264", width_, height_, kbps_,
      cvars::xdroid_mirror_bitrate_mode, cvars::xdroid_mirror_intra_refresh,
      std::clamp(cvars::xdroid_mirror_key_interval, 1, 60), tier_,
      extent_.width, extent_.height, int(format_),
      filter_ == VK_FILTER_LINEAR ? "linear" : "nearest", images_.size(),
      yuv_ ? "own NV12 surface" : "RGBA swapchain", y_stride_, c_stride_, cb_,
      cr_, cached_);
  return true;
}

// Logs and frees a format the codec reports: accepted and vendor keys.
void LogFormat(const char* what, AMediaFormat* format) {
  if (format) {
    const char* text = AMediaFormat_toString(format);
    XELOGI("XDROID-MIRROR encoder {}: {}", what, text ? text : "?");
    AMediaFormat_delete(format);
  }
}

// xdroid_mirror_keys: "name=value,..." extra keys for vendor experiments; an
// integer goes in as int32, a number with '.' as float, else a string.
// A name may hold blanks ("vendor.imgtxr filter.forcelinear.value"): only the
// ones around ',' and '=' go. Returns what it set, for the log.
std::string SetExtraKeys(AMediaFormat* format, const std::string& keys) {
  const auto trim = [](const std::string& text) {
    const size_t first = text.find_first_not_of(' ');
    return first == std::string::npos
               ? std::string()
               : text.substr(first, text.find_last_not_of(' ') - first + 1);
  };
  std::string applied;
  for (size_t start = 0, end; start < keys.size(); start = end + 1) {
    end = std::min(keys.find(',', start), keys.size());
    const std::string item = keys.substr(start, end - start);
    const size_t equals = item.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string name = trim(item.substr(0, equals));
    const std::string text = trim(item.substr(equals + 1));
    if (name.empty()) {
      continue;
    }
    const char* value = text.c_str();
    char* int_end = nullptr;
    char* float_end = nullptr;
    errno = 0;
    const long number = std::strtol(value, &int_end, 10);
    const bool integer = !errno && number == int32_t(number);
    const float real = std::strtof(value, &float_end);
    const char* type = "string";
    if (*value && !*int_end && integer) {
      AMediaFormat_setInt32(format, name.c_str(), int32_t(number));
      type = "int32";
    } else if (*value && !*float_end && std::strchr(value, '.')) {
      AMediaFormat_setFloat(format, name.c_str(), real);
      type = "float";
    } else {
      AMediaFormat_setString(format, name.c_str(), value);
    }
    applied += (applied.empty() ? "" : ", ") + name + "=" + text + " (" + type +
               ")";
  }
  return applied;
}

bool Capture::CreateEncoder() {
  const bool hevc = codec_ == mp::Codec::kHevc;
  const std::string keys = GetLive(g_keys_live, cvars::xdroid_mirror_keys);
  std::string applied;
  const char* mime = hevc ? "video/hevc" : "video/avc";
  // Tier 2 sets every low-latency key, tier 1 drops profile and level, tier 0
  // keeps only what the encoder needs; each failed configure gets a fresh
  // codec.
  media_status_t status = AMEDIA_ERROR_UNKNOWN;
  for (tier_ = 2; tier_ >= 0; --tier_) {
    encoder_ = AMediaCodec_createEncoderByType(mime);
    if (!encoder_) {
      return Fail(mime, 0);
    }
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, "mime", mime);
    AMediaFormat_setInt32(format, "width", int32_t(width_));
    AMediaFormat_setInt32(format, "height", int32_t(height_));
    // COLOR_FormatSurface in both modes: R2 posts NV12 to the surface.
    AMediaFormat_setInt32(format, "color-format", 0x7F000789);
    AMediaFormat_setInt32(format, "bitrate", kbps_ * 1000);
    AMediaFormat_setInt32(format, "bitrate-mode",
                          std::clamp(cvars::xdroid_mirror_bitrate_mode, 1, 3));
    AMediaFormat_setInt32(format, "frame-rate", 30);
    AMediaFormat_setInt32(format, "i-frame-interval",
                          std::clamp(cvars::xdroid_mirror_key_interval, 1, 60));
    if (tier_ >= 1) {
      // The encoder takes about half of 1 / rate for a frame: 16 ms at 30.
      AMediaFormat_setFloat(format, "operating-rate", float(rate_));
      AMediaFormat_setInt32(format, "priority", 0);  // Realtime.
      AMediaFormat_setInt32(format, "latency", 1);
      AMediaFormat_setInt32(format, "max-bframes", 0);
      if (cvars::xdroid_mirror_intra_refresh > 0) {
        AMediaFormat_setInt32(format, "intra-refresh-period",
                              cvars::xdroid_mirror_intra_refresh);
      }
      AMediaFormat_setInt32(format, "prepend-sps-pps-to-idr-frames", 1);
      // Static screens still send a frame every 100 ms.
      AMediaFormat_setInt64(format, "repeat-previous-frame-after", 100000);
      AMediaFormat_setInt32(format, "color-standard", 1);  // BT.709.
      AMediaFormat_setInt32(format, "color-range", 2);     // Limited.
      AMediaFormat_setInt32(format, "color-transfer", 3);  // SDR.
    }
    if (tier_ >= 2) {
      // AVC High / HEVC Main; level 4.1 up to 1080p30, else 5.
      AMediaFormat_setInt32(format, "profile", hevc ? 1 : 8);
      AMediaFormat_setInt32(format, "level", height_ > 1080 ? 0x4000 : 0x1000);
    }
    applied = SetExtraKeys(format, keys);
    status = AMediaCodec_configure(encoder_, format, nullptr, nullptr,
                                   AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (status == AMEDIA_OK) {
      break;
    }
    AMediaCodec_delete(encoder_);
    encoder_ = nullptr;
  }
  if (!encoder_) {
    return Fail("encoder configure", status);
  }
  if ((status = AMediaCodec_createInputSurface(encoder_, &window_)) !=
          AMEDIA_OK ||
      !window_) {
    return Fail("encoder input surface", status);
  }
  if ((status = AMediaCodec_start(encoder_)) != AMEDIA_OK) {
    return Fail("encoder start", status);
  }
  encoder_started_ = true;
  if (log_) {
    if (!applied.empty()) {
      XELOGI("XDROID-MIRROR encoder extra keys: {}", applied);
    }
    LogFormat("input", AMediaCodec_getInputFormat(encoder_));
  }
  return true;
}

bool Capture::CreateSwapchain() {
  const VulkanInstance* instance = device_->vulkan_instance();
  const VulkanInstance::Functions& ifn = instance->functions();
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  const VkPhysicalDevice physical_device = device_->physical_device();
  const uint32_t queue_family = device_->queue_family_graphics_compute();

  VkAndroidSurfaceCreateInfoKHR surface_info = {
      VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  surface_info.window = window_;
  VkResult result = ifn.vkCreateAndroidSurfaceKHR(
      instance->instance(), &surface_info, nullptr, &surface_);
  if (result != VK_SUCCESS) {
    return Fail("vkCreateAndroidSurfaceKHR", result);
  }
  VkBool32 supported = VK_FALSE;
  ifn.vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family,
                                           surface_, &supported);
  if (!supported) {
    return Fail("present support", 0);
  }
  VkSurfaceCapabilitiesKHR caps;
  result = ifn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device,
                                                          surface_, &caps);
  if (result != VK_SUCCESS) {
    return Fail("surface capabilities", result);
  }
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    return Fail("swapchain transfer usage", caps.supportedUsageFlags);
  }
  uint32_t format_count = 0;
  ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_,
                                           &format_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  ifn.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_,
                                           &format_count, formats.data());
  VkSurfaceFormatKHR surface_format = {VK_FORMAT_UNDEFINED,
                                       VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  for (VkFormat wanted :
       {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
    for (const VkSurfaceFormatKHR& candidate : formats) {
      if (surface_format.format == VK_FORMAT_UNDEFINED &&
          candidate.format == wanted) {
        surface_format = candidate;
      }
    }
  }
  if (surface_format.format == VK_FORMAT_UNDEFINED) {
    return Fail("surface format", format_count);
  }
  format_ = surface_format.format;
  VkFormatProperties source_properties, target_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device,
                                          VulkanPresenter::kGuestOutputFormat,
                                          &source_properties);
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, format_,
                                          &target_properties);
  if (!(source_properties.optimalTilingFeatures &
        VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
      !(target_properties.optimalTilingFeatures &
        VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
    return Fail("blit support", 0);
  }
  filter_ = (source_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                ? VK_FILTER_LINEAR
                : VK_FILTER_NEAREST;

  VkSwapchainCreateInfoKHR swapchain_info = {
      VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swapchain_info.surface = surface_;
  swapchain_info.minImageCount = caps.minImageCount;
  swapchain_info.imageFormat = surface_format.format;
  swapchain_info.imageColorSpace = surface_format.colorSpace;
  extent_ = caps.currentExtent.width == UINT32_MAX
                ? VkExtent2D{width_, height_}
                : caps.currentExtent;
  swapchain_info.imageExtent = extent_;
  swapchain_info.imageArrayLayers = 1;
  swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swapchain_info.preTransform =
      (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
          : caps.currentTransform;
  const VkCompositeAlphaFlagsKHR alpha = caps.supportedCompositeAlpha;
  swapchain_info.compositeAlpha =
      (alpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
          ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
      : (alpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
          ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
          : VkCompositeAlphaFlagBitsKHR(alpha & (~alpha + 1));
  swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swapchain_info.clipped = VK_TRUE;
  result =
      dfn.vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain_);
  if (result != VK_SUCCESS) {
    return Fail("vkCreateSwapchainKHR", result);
  }
  uint32_t image_count = 0;
  dfn.vkGetSwapchainImagesKHR(device, swapchain_, &image_count, nullptr);
  images_.resize(image_count);
  dfn.vkGetSwapchainImagesKHR(device, swapchain_, &image_count,
                              images_.data());

  VkSemaphoreCreateInfo semaphore_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  present_semaphores_.resize(image_count, VK_NULL_HANDLE);
  for (VkSemaphore& semaphore : present_semaphores_) {
    if (dfn.vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) !=
        VK_SUCCESS) {
      return Fail("vkCreateSemaphore", 0);
    }
  }
  if (dfn.vkCreateSemaphore(device, &semaphore_info, nullptr,
                            &acquire_semaphore_) != VK_SUCCESS) {
    return Fail("vkCreateSemaphore", 0);
  }
  return true;
}

// R2: an RGBA image the blit fits the guest output into, a host buffer it is
// copied to for the NV12 conversion, and the input surface as a CPU producer
// of NV12, so the vendor has no RGB to convert on the GPU.
bool Capture::CreateReadback() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  format_ = VK_FORMAT_R8G8B8A8_UNORM;
  extent_ = {width_, height_};
  VkFormatProperties source_properties;
  device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
      device_->physical_device(), VulkanPresenter::kGuestOutputFormat,
      &source_properties);
  filter_ = (source_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                ? VK_FILTER_LINEAR
                : VK_FILTER_NEAREST;
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format_;
  image_info.extent = {width_, height_, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!xe::ui::vulkan::util::CreateDedicatedAllocationImage(
          device_, image_info, xe::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          rgba_, rgba_memory_)) {
    return Fail("readback image", 0);
  }
  const VkDeviceSize size = VkDeviceSize(width_) * height_ * 4;
  void* mapped = nullptr;
  // kReadback prefers HOST_CACHED: the conversion reads every byte, slow from
  // write-combined memory.
  uint32_t memory_type = 0;
  if (!xe::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device_, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          xe::ui::vulkan::util::MemoryPurpose::kReadback, readback_,
          readback_memory_, &memory_type) ||
      dfn.vkMapMemory(device, readback_memory_, 0, VK_WHOLE_SIZE, 0,
                      &mapped) != VK_SUCCESS) {
    return Fail("readback buffer", 0);
  }
  mapped_ = static_cast<const uint8_t*>(mapped);
  cached_ = device_->memory_types().host_cached & (1u << memory_type);
  // ANativeWindow_lock reports only the luma stride: the chroma layout comes
  // from a probe buffer of the same format and usage, once.
  const AHardwareBuffer_Desc desc = {width_, height_, 1,
                                     AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,
                                     AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                         AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                                         AHARDWAREBUFFER_USAGE_VIDEO_ENCODE};
  AHardwareBuffer* probe = nullptr;
  AHardwareBuffer_Planes planes = {};
  if (AHardwareBuffer_allocate(&desc, &probe) ||
      AHardwareBuffer_lockPlanes(probe, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                 -1, nullptr, &planes)) {
    planes.planeCount = 0;
  } else {
    const auto* y = static_cast<const uint8_t*>(planes.planes[0].data);
    y_stride_ = planes.planes[0].rowStride;
    c_stride_ = planes.planes[1].rowStride;
    cb_ = static_cast<const uint8_t*>(planes.planes[1].data) - y;
    cr_ = static_cast<const uint8_t*>(planes.planes[2].data) - y;
    AHardwareBuffer_unlock(probe, nullptr);
  }
  if (probe) {
    AHardwareBuffer_release(probe);
  }
  // The NEON loop writes interleaved chroma (NV12 or NV21) only.
  if (planes.planeCount != 3 || planes.planes[0].pixelStride != 1 ||
      planes.planes[1].pixelStride != 2 || planes.planes[2].pixelStride != 2 ||
      planes.planes[2].rowStride != c_stride_ || std::abs(cb_ - cr_) != 1) {
    XELOGW(
        "XDROID-MIRROR: own NV12 surface off: probe {} planes, y stride {} "
        "pixel {}, chroma stride {} {} pixel {} {}, cb {} cr {}",
        planes.planeCount, y_stride_, planes.planes[0].pixelStride, c_stride_,
        planes.planes[2].rowStride, planes.planes[1].pixelStride,
        planes.planes[2].pixelStride, cb_, cr_);
    return false;
  }
  // ADATASPACE_BT709 = BT.709 standard, SMPTE 170M transfer, limited range:
  // the color-standard 1, color-transfer 3, color-range 2 the format declares.
  if (ANativeWindow_setBuffersGeometry(window_, int32_t(width_),
                                       int32_t(height_),
                                       AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420) ||
      ANativeWindow_setBuffersDataSpace(window_, ADATASPACE_BT709)) {
    return Fail("NV12 surface geometry", 0);
  }
  return true;
}

bool Capture::CreateCommands() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  VkCommandPoolCreateInfo pool_info = {
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &pool_) !=
      VK_SUCCESS) {
    return Fail("vkCreateCommandPool", 0);
  }
  VkCommandBufferAllocateInfo allocate_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = pool_;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &allocate_info, &command_buffer_) !=
      VK_SUCCESS) {
    return Fail("vkAllocateCommandBuffers", 0);
  }
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
    return Fail("vkCreateFence", 0);
  }
  return true;
}

Capture::~Capture() {
  stop_.store(true, std::memory_order_relaxed);
  if (out_.joinable()) {
    out_.join();
  }
  if (device_) {
    const VulkanDevice::Functions& dfn = device_->functions();
    const VkDevice device = device_->device();
    if (copies_.load(std::memory_order_relaxed) || submitted_) {
      // Presents may still wait on present_semaphores_: drain the queue.
      // The capture thread holds no other lock here.
      const VulkanDevice::Queue::Acquisition queue = device_->AcquireQueue(
          device_->queue_family_graphics_compute(), 0);
      dfn.vkQueueWaitIdle(queue.queue());
    }
    // A blit the GPU never finished keeps its fence, pool and semaphores:
    // leaking them beats freeing objects in use.
    if (!submitted_ || dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE,
                                           1000000000ull) == VK_SUCCESS) {
      for (VkSemaphore semaphore : present_semaphores_) {
        dfn.vkDestroySemaphore(device, semaphore, nullptr);
      }
      dfn.vkDestroySemaphore(device, acquire_semaphore_, nullptr);
      dfn.vkDestroyFence(device, fence_, nullptr);
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      dfn.vkDestroyImage(device, rgba_, nullptr);
      dfn.vkFreeMemory(device, rgba_memory_, nullptr);
      dfn.vkDestroyBuffer(device, readback_, nullptr);
      dfn.vkFreeMemory(device, readback_memory_, nullptr);
    }
    dfn.vkDestroySwapchainKHR(device, swapchain_, nullptr);
    const VulkanInstance* instance = device_->vulkan_instance();
    instance->functions().vkDestroySurfaceKHR(instance->instance(), surface_,
                                              nullptr);
  }
  if (encoder_) {
    if (encoder_started_) {
      AMediaCodec_stop(encoder_);
    }
    AMediaCodec_delete(encoder_);
  }
  if (window_) {
    ANativeWindow_release(window_);
  }
  if (encoder_started_) {
    XELOGI(
        "XDROID-MIRROR stop: {} frames, {} copies, {} drops, {:.1f} MB, "
        "present-to-output {:.2f} ms avg",
        frames_, copies_.load(), drops_.load(), double(bytes_) / 1e6,
        frames_ ? double(encode_us_) / 1000.0 / double(frames_) : 0.0);
  }
}

bool Capture::Frame() {
  if (encoder_failed_.load(std::memory_order_relaxed)) {
    return Fail("encoder output", 0);
  }
  VkImage source;
  VkExtent2D source_extent;
  uint64_t serial;
  std::shared_ptr<void> keep;
  if (!presenter_->AcquireGuestOutputForCopy(source, source_extent, serial,
                                             keep) ||
      serial == serial_ || !source_extent.width || !source_extent.height) {
    return true;
  }
  const int64_t start_us = NowUs();
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  uint32_t index = 0;
  VkResult result =
      yuv_ ? VK_SUCCESS
           : dfn.vkAcquireNextImageKHR(device, swapchain_, 0,
                                       acquire_semaphore_, VK_NULL_HANDLE,
                                       &index);
  if (log_ && !acquire_logged_) {
    acquire_logged_ = true;
    XELOGI("XDROID-MIRROR first swapchain acquire: {}", int(result));
  }
  if (result == VK_NOT_READY || result == VK_TIMEOUT) {
    // The encoder still holds every buffer: retry on the next present.
    drops_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    return Fail("vkAcquireNextImageKHR", result);
  }
  // The acquire may have waited: copy the newest guest output, not the one
  // the wake saw, so a refreshed slot is not copied twice. On false the
  // outputs stay as they were; serials only grow.
  presenter_->AcquireGuestOutputForCopy(source, source_extent, serial, keep);
  serial_ = serial;
  const VkImage target = yuv_ ? rgba_ : images_[index];

  dfn.vkResetCommandPool(device, pool_, 0);
  VkCommandBufferBeginInfo begin_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(command_buffer_, &begin_info);
  // Same source transitions as VulkanPresenter::CaptureGuestOutput.
  VkImageMemoryBarrier barriers[2] = {};
  for (VkImageMemoryBarrier& barrier : barriers) {
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = xe::ui::vulkan::util::InitializeSubresourceRange();
  }
  barriers[0].srcAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
  barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barriers[0].oldLayout = VulkanPresenter::kGuestOutputInternalLayout;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].image = source;
  barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].image = target;
  dfn.vkCmdPipelineBarrier(command_buffer_,
                           VulkanPresenter::kGuestOutputInternalStageMask |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 2, barriers);
  // Fit the guest output into the frame with its aspect; black bars around.
  uint32_t fit_width = extent_.width, fit_height = extent_.height;
  if (uint64_t(source_extent.width) * extent_.height >
      uint64_t(source_extent.height) * extent_.width) {
    fit_height = uint32_t(uint64_t(extent_.width) * source_extent.height /
                          source_extent.width);
  } else {
    fit_width = uint32_t(uint64_t(extent_.height) * source_extent.width /
                         source_extent.height);
  }
  const int32_t x = int32_t(extent_.width - fit_width) / 2;
  const int32_t y = int32_t(extent_.height - fit_height) / 2;
  if (fit_width != extent_.width || fit_height != extent_.height) {
    VkClearColorValue black = {};
    black.float32[3] = 1.0f;
    const VkImageSubresourceRange range =
        xe::ui::vulkan::util::InitializeSubresourceRange();
    dfn.vkCmdClearColorImage(command_buffer_, target,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
    VkMemoryBarrier clear_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    clear_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clear_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &clear_barrier, 0, nullptr, 0, nullptr);
  }
  VkImageBlit blit = {};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(source_extent.width),
                        int32_t(source_extent.height), 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.dstOffsets[0] = {x, y, 0};
  blit.dstOffsets[1] = {x + int32_t(fit_width), y + int32_t(fit_height), 1};
  dfn.vkCmdBlitImage(command_buffer_, source,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter_);
  barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barriers[0].dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].newLayout = VulkanPresenter::kGuestOutputInternalLayout;
  barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barriers[1].dstAccessMask = yuv_ ? VK_ACCESS_TRANSFER_READ_BIT : 0;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].newLayout = yuv_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                               : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VulkanPresenter::kGuestOutputInternalStageMask |
                               VK_PIPELINE_STAGE_TRANSFER_BIT |
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, nullptr, 0, nullptr, 2, barriers);
  if (yuv_) {
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent_.width, extent_.height, 1};
    dfn.vkCmdCopyImageToBuffer(command_buffer_, rgba_,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_,
                               1, &region);
    VkBufferMemoryBarrier host_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.buffer = readback_;
    host_barrier.size = VK_WHOLE_SIZE;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &host_barrier, 0, nullptr);
  }
  dfn.vkEndCommandBuffer(command_buffer_);

  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.waitSemaphoreCount = yuv_ ? 0 : 1;
  submit_info.pWaitSemaphores = &acquire_semaphore_;
  submit_info.pWaitDstStageMask = &wait_stage;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer_;
  submit_info.signalSemaphoreCount = yuv_ ? 0 : 1;
  submit_info.pSignalSemaphores =
      present_semaphores_.empty() ? nullptr : &present_semaphores_[index];
  VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = submit_info.pSignalSemaphores;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &swapchain_;
  present_info.pImageIndices = &index;
  {
    const VulkanDevice::Queue::Acquisition queue = device_->AcquireQueue(
        device_->queue_family_graphics_compute(), 0);
    result = device_->SubmitAndUpdateLost(queue.queue(), 1, &submit_info,
                                          fence_);
  }
  if (result != VK_SUCCESS) {
    return Fail("vkQueueSubmit", result);
  }
  submitted_ = true;
  // The blit takes well under a millisecond. Waiting for it here, outside the
  // queue lock, keeps the source image alive (keep) only as long as needed and
  // lets the present below never wait for GPU work while it holds the lock
  // the GPU thread needs.
  result = dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, 1000000000ull);
  if (result != VK_SUCCESS) {
    return Fail("vkWaitForFences", result);
  }
  dfn.vkResetFences(device, 1, &fence_);
  submitted_ = false;
  keep.reset();
  // R2: present max is lock, convert and post; copy avg also covers them.
  int64_t present_us = NowUs();
  if (yuv_ && !PostYuv()) {
    return false;
  }
  int64_t posted_us = NowUs();
  present_us = posted_us - present_us;
  if (!yuv_) {
    const VulkanDevice::Queue::Acquisition queue = device_->AcquireQueue(
        device_->queue_family_graphics_compute(), 0);
    present_us = NowUs();
    result = dfn.vkQueuePresentKHR(queue.queue(), &present_info);
    posted_us = NowUs();
    present_us = posted_us - present_us;
  }
  copies_.fetch_add(1, std::memory_order_relaxed);
  capture_us_.fetch_add(posted_us - start_us, std::memory_order_relaxed);
  if (present_us > present_max_us_.load(std::memory_order_relaxed)) {
    present_max_us_.store(present_us, std::memory_order_relaxed);
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    return Fail("vkQueuePresentKHR", result);
  }
  return true;
}

// R2: BT.709 limited range, chroma averaged over 2x2, the same math as the
// scalar loop this replaced, 16 pixels of two rows a step. |width| is even
// and >= 16: the last step overlaps the one before instead of a scalar tail.
void ToYuv(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* luma,
           size_t luma_stride, uint8_t* chroma, size_t chroma_stride,
           bool cb_first) {
  const auto y = [](const uint8x16x4_t& p) {
    const uint8x16_t r = vdupq_n_u8(47), g = vdupq_n_u8(157),
                     b = vdupq_n_u8(16);
    uint16x8_t lo = vmull_u8(vget_low_u8(p.val[0]), vget_low_u8(r));
    lo = vmlal_u8(lo, vget_low_u8(p.val[1]), vget_low_u8(g));
    lo = vmlal_u8(lo, vget_low_u8(p.val[2]), vget_low_u8(b));
    uint16x8_t hi = vmull_high_u8(p.val[0], r);
    hi = vmlal_high_u8(hi, p.val[1], g);
    hi = vmlal_high_u8(hi, p.val[2], b);
    return vaddq_u8(vrshrn_high_n_u16(vrshrn_n_u16(lo, 8), hi, 8),
                    vdupq_n_u8(16));
  };
  // ((kr * r + kg * g + kb * b + 512) >> 10) + 128 over the 2x2 sums.
  const auto c = [](int16x8_t r, int16x8_t g, int16x8_t b, int16_t kr,
                    int16_t kg, int16_t kb) {
    int32x4_t lo = vmull_n_s16(vget_low_s16(r), kr);
    lo = vmlal_n_s16(lo, vget_low_s16(g), kg);
    lo = vmlal_n_s16(lo, vget_low_s16(b), kb);
    int32x4_t hi = vmull_high_n_s16(r, kr);
    hi = vmlal_high_n_s16(hi, g, kg);
    hi = vmlal_high_n_s16(hi, b, kb);
    return vqmovun_s16(vaddq_s16(
        vrshrn_high_n_s32(vrshrn_n_s32(lo, 10), hi, 10), vdupq_n_s16(128)));
  };
  for (uint32_t row = 0; row < height; row += 2) {
    const uint8_t* in = rgba + size_t(row) * width * 4;
    uint8_t* y0 = luma + size_t(row) * luma_stride;
    uint8_t* out = chroma + size_t(row / 2) * chroma_stride;
    for (uint32_t x = 0; x < width; x += 16) {
      x = std::min(x, width - 16);
      const uint8x16x4_t p0 = vld4q_u8(in + size_t(x) * 4);
      const uint8x16x4_t p1 = vld4q_u8(in + (size_t(width) + x) * 4);
      vst1q_u8(y0 + x, y(p0));
      vst1q_u8(y0 + luma_stride + x, y(p1));
      int16x8_t sum[3];
      for (int i = 0; i < 3; ++i) {
        sum[i] =
            vreinterpretq_s16_u16(vpadalq_u8(vpaddlq_u8(p0.val[i]), p1.val[i]));
      }
      const uint8x8_t cb = c(sum[0], sum[1], sum[2], -26, -87, 113);
      const uint8x8_t cr = c(sum[0], sum[1], sum[2], 113, -103, -10);
      vst2_u8(out + x, (uint8x8x2_t{{cb_first ? cb : cr, cb_first ? cr : cb}}));
    }
  }
}

// R2: the read back frame into a gralloc buffer of the input surface. The
// BufferQueue stamps it at the post, as at the Vulkan present of mode 0, and
// the encoder echoes the stamp as PTS: present-to-output keeps its meaning.
bool Capture::PostYuv() {
  ANativeWindow_Buffer buffer;
  if (const int32_t error = ANativeWindow_lock(window_, &buffer, nullptr)) {
    // The gralloc refuses a CPU lock of this format: mode 0 from here on.
    g_yuv_failed.store(true, std::memory_order_relaxed);
    return Fail("ANativeWindow_lock", error);
  }
  // Only the luma stride can be checked against the probe here.
  const bool match = buffer.format == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 &&
                     uint32_t(buffer.width) == width_ &&
                     uint32_t(buffer.height) == height_ &&
                     size_t(buffer.stride) == y_stride_;
  if (match) {
    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = readback_memory_;
    range.size = VK_WHOLE_SIZE;
    device_->functions().vkInvalidateMappedMemoryRanges(device_->device(), 1,
                                                        &range);
    uint8_t* bits = static_cast<uint8_t*>(buffer.bits);
    ToYuv(mapped_, width_, height_, bits, y_stride_, bits + std::min(cb_, cr_),
          c_stride_, cb_ < cr_);
  } else {
    // The buffer posted below is garbage, but the encoder is torn down next.
    g_yuv_failed.store(true, std::memory_order_relaxed);
    XELOGW(
        "XDROID-MIRROR: own NV12 surface off: locked buffer format {:#x} {}x{} "
        "stride {}, probe {}x{} stride {}",
        buffer.format, buffer.width, buffer.height, buffer.stride, width_,
        height_, y_stride_);
  }
  if (const int32_t error = ANativeWindow_unlockAndPost(window_)) {
    return Fail("ANativeWindow_unlockAndPost", error);
  }
  return match;
}

void Capture::SetParameter(const char* key, int32_t value, bool real) {
  AMediaFormat* parameters = AMediaFormat_new();
  if (real) {
    AMediaFormat_setFloat(parameters, key, float(value));
  } else {
    AMediaFormat_setInt32(parameters, key, value);
  }
  AMediaCodec_setParameters(encoder_, parameters);
  AMediaFormat_delete(parameters);
}

void Capture::OutLoop() {
  pthread_setname_np(pthread_self(), "Mirror Out");
  bool sending = true;
  int32_t kbps = kbps_;
  int32_t rate = rate_;
  uint64_t window_frames = 0, window_bytes = 0, window_keys = 0;
  uint64_t window_copies = 0;  // copies_ at the window start.
  int64_t window_encode_us = 0, window_capture_us = 0;
  int64_t window_start_us = NowUs();
  size_t window_max_bytes = 0;
  while (!stop_.load(std::memory_order_relaxed)) {
    if (g_want_key.exchange(false, std::memory_order_relaxed)) {
      SetParameter("request-sync", 0);
    }
    if (const int32_t want = g_kbps.load(std::memory_order_relaxed);
        want != kbps) {
      kbps = want;
      SetParameter("video-bitrate", kbps * 1000);
    }
    if (const int32_t want = g_rate.load(std::memory_order_relaxed);
        want != rate) {
      rate = want;
      SetParameter("operating-rate", rate, true);
      XELOGI("XDROID-MIRROR: encoder operating rate {}", rate);
    }
    AMediaCodecBufferInfo info;
    const ssize_t index =
        AMediaCodec_dequeueOutputBuffer(encoder_, &info, 100000);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED && log_) {
      LogFormat("output", AMediaCodec_getOutputFormat(encoder_));
    }
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER ||
        index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
        index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    // 1100 and 1101 are positive: the codec was reclaimed, not a buffer.
    if (index < 0 || index == AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE ||
        index == AMEDIACODEC_ERROR_RECLAIMED) {
      if (log_) {
        XELOGW("XDROID-MIRROR: encoder output failed ({})", int64_t(index));
      }
      encoder_failed_.store(true, std::memory_order_relaxed);
      return;
    }
    size_t capacity = 0;
    uint8_t* data =
        AMediaCodec_getOutputBuffer(encoder_, size_t(index), &capacity);
    if (sending && data && info.size > 0) {
      const bool config = info.flags & 2;  // BUFFER_FLAG_CODEC_CONFIG.
      const bool key = info.flags & 1;     // BUFFER_FLAG_KEY_FRAME.
      const int64_t now_us = NowUs();
      mp::PacketHeader header =
          Header(config ? mp::Packet::kConfig : mp::Packet::kVideo,
                 key ? mp::kFlagKey : 0, size_t(info.size),
                 info.presentationTimeUs, now_us);
      iovec iov[2] = {{&header, sizeof(header)},
                      {data + info.offset, size_t(info.size)}};
      CastSender* cast = g_cast.load(std::memory_order_acquire);
      if (cast ? !cast->Send(CastSender::kVideo, data + info.offset,
                             size_t(info.size), key, config,
                             info.presentationTimeUs)
               : !SendAll(fd_, iov, 2)) {
        sending = false;
        g_net_error.store(true, std::memory_order_relaxed);
      }
      if (!config) {
        const int64_t encode_us =
            std::clamp<int64_t>(now_us - info.presentationTimeUs, 0, 1000000);
        ++frames_;
        bytes_ += uint64_t(info.size);
        encode_us_ += encode_us;
        ++window_frames;
        window_bytes += uint64_t(info.size);
        window_encode_us += encode_us;
        window_keys += key;
        window_max_bytes = std::max(window_max_bytes, size_t(info.size));
        if (window_frames == 1800) {
          const double seconds = double(now_us - window_start_us) / 1e6;
          const uint64_t copies = copies_.load(std::memory_order_relaxed);
          const int64_t capture_us =
              capture_us_.load(std::memory_order_relaxed);
          XELOGI(
              "XDROID-MIRROR: {:.1f} fps, {:.2f} Mbit/s, present-to-output "
              "{:.2f} ms avg, largest frame {} KB, {} keys, copy {:.2f} ms "
              "avg, present max {:.2f} ms, {} copies, {} drops",
              double(window_frames) / seconds,
              double(window_bytes) * 8.0 / 1e6 / seconds,
              double(window_encode_us) / 1000.0 / double(window_frames),
              window_max_bytes / 1024, window_keys,
              double(capture_us - window_capture_us) / 1000.0 /
                  double(std::max<uint64_t>(copies - window_copies, 1)),
              double(present_max_us_.exchange(0, std::memory_order_relaxed)) /
                  1000.0,
              copies, drops_.load(std::memory_order_relaxed));
          window_frames = window_bytes = window_keys = 0;
          window_encode_us = 0;
          window_max_bytes = 0;
          window_copies = copies;
          window_capture_us = capture_us;
          window_start_us = now_us;
        }
      }
    }
    AMediaCodec_releaseOutputBuffer(encoder_, size_t(index), false);
  }
}

void CaptureLoop() {
  pthread_setname_np(pthread_self(), "Mirror Cap");
  std::unique_ptr<Capture> capture;
  uint32_t active = 0;
  uint32_t gfx_seen = 0;
  int64_t retry_us = 0;
  bool logged = false;
  for (uint32_t seen = 0;; g_wake.wait(seen, std::memory_order_acquire)) {
    seen = g_wake.load(std::memory_order_acquire);
    if (const uint32_t gen = g_gfx_gen.load(std::memory_order_acquire);
        gen != gfx_seen) {
      capture.reset();
      gfx_seen = gen;
      g_gfx_ack.store(gen, std::memory_order_release);
    }
    const uint32_t session = g_session.load(std::memory_order_acquire);
    if (session != active) {
      capture.reset();
      active = session;
      retry_us = 0;
      logged = false;
      g_session_ack.store(active, std::memory_order_release);
    }
    if (!active) {
      continue;
    }
    const int64_t now_us = NowUs();
    if (!capture && now_us >= retry_us) {
      // Only the first failure of a session is logged.
      capture = Capture::Create(!logged);
      logged = true;
      if (!capture) {
        retry_us = now_us + 1000000;
      }
    }
    if (capture && capture->Stale()) {
      capture.reset();
      continue;
    }
    if (capture && !capture->Frame()) {
      capture.reset();
      retry_us = now_us + 1000000;
    }
  }
}

int Connect(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  const timeval timeout = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(uint16_t(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address))) {
    close(fd);
    return -1;
  }
  return fd;
}

// The viewer speaks first. Each side proves its key with a MAC over both
// nonces; the app sends nothing more until the viewer proved its key.
bool Handshake(int fd, const Token& token, mp::Stream stream, mp::Codec codec,
               uint32_t width, uint32_t height) {
  mp::ServerHello server;
  if (recv(fd, &server, sizeof(server), MSG_WAITALL) !=
          ssize_t(sizeof(server)) ||
      std::memcmp(server.magic, mp::kMagic, sizeof(mp::kMagic)) ||
      server.version != mp::kVersion) {
    return false;
  }
  mp::ClientHello client = {};
  std::memcpy(client.magic, mp::kMagic, sizeof(mp::kMagic));
  client.version = mp::kVersion;
  client.stream = uint8_t(stream);
  client.codec = uint8_t(codec);
  client.width = uint16_t(width);
  client.height = uint16_t(height);
  if (stream == mp::Stream::kAudio) {
    client.audio_rate = 48000;
    client.audio_channels = 2;
  }
  arc4random_buf(client.nonce, sizeof(client.nonce));
  mp::HelloMac(token.data() + mp::kTokenBytes, 'C', server, client,
               client.mac);
  mp::ServerProof proof, want;
  mp::HelloMac(token.data(), 'S', server, client, want.mac);
  iovec iov = {&client, sizeof(client)};
  return SendAll(fd, &iov, 1) &&
         recv(fd, &proof, sizeof(proof), MSG_WAITALL) ==
             ssize_t(sizeof(proof)) &&
         SameBytes(proof.mac, want.mac, mp::kMacBytes);
}

struct ControlReader {
  uint8_t bytes[sizeof(mp::ControlMessage)];
  size_t used = 0;
};

// Reads the pending control messages; false when the viewer is gone.
bool ReadControl(int fd, ControlReader& reader, int pong_fd) {
  for (;;) {
    const ssize_t got = recv(fd, reader.bytes + reader.used,
                             sizeof(reader.bytes) - reader.used, MSG_DONTWAIT);
    if (got == 0) {
      return false;
    }
    if (got < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    reader.used += size_t(got);
    if (reader.used < sizeof(reader.bytes)) {
      continue;
    }
    reader.used = 0;
    mp::ControlMessage message;
    std::memcpy(&message, reader.bytes, sizeof(message));
    switch (mp::Control(message.type)) {
      case mp::Control::kPing:
        if (!SendPong(pong_fd, message.value)) {
          return false;
        }
        break;
      case mp::Control::kRequestKey:
        g_want_key.store(true, std::memory_order_relaxed);
        break;
      case mp::Control::kSetKbps:
        g_kbps.store(ClampKbps(message.value), std::memory_order_relaxed);
        break;
      case mp::Control::kSetRate:
        g_rate.store(ClampRate(message.value), std::memory_order_relaxed);
        break;
    }
  }
}

// Waits until the capture thread stores |value| in |ack|: the session it
// serves (0: has stopped) or the graphics generation it let go of.
void AwaitCapture(const std::atomic<uint32_t>& ack, uint32_t value) {
  const int64_t start_us = NowUs();
  bool warned = false;
  while (ack.load(std::memory_order_acquire) != value) {
    if (!warned && NowUs() - start_us > 2000000) {
      XELOGW("XDROID-MIRROR: capture thread slow to follow {}", value);
      warned = true;
    }
    SleepMs(2);
  }
}

// One connected session: audio, pings and control until the viewer leaves,
// a socket fails or xdroid_mirror turns off.
void Serve(int video_fd, int audio_fd, uint32_t session,
           CastSender* cast = nullptr) {
  g_net_error.store(false, std::memory_order_relaxed);
  g_want_key.store(false, std::memory_order_relaxed);
  g_kbps.store(ClampKbps(cvars::xdroid_mirror_kbps), std::memory_order_relaxed);
  g_rate.store(ClampRate(cvars::xdroid_mirror_operating_rate),
               std::memory_order_relaxed);
  g_video_fd.store(video_fd, std::memory_order_relaxed);
  g_cast.store(cast, std::memory_order_release);
  g_session.store(session, std::memory_order_release);
  Wake();
  AwaitCapture(g_session_ack, session);
  bool audio_on = false;
  uint64_t audio_read = 0;
  std::unique_ptr<CastOpus> opus;  // TV-2 sound.
  if (cast) {
    opus = std::make_unique<CastOpus>();
    if (!opus->Open(128)) {
      XELOGW("XDROID-CAST: no Opus encoder, the sound stays on the phone");
      opus.reset();
    }
  }
  bool audio_loss = false;
  int32_t kbps = cvars::xdroid_mirror_kbps;
  int32_t rate = cvars::xdroid_mirror_operating_rate;
  ControlReader video_reader, audio_reader;
  while (cvars::xdroid_mirror &&
         !g_net_error.load(std::memory_order_relaxed)) {
    if (const bool want_audio =
            cvars::xdroid_mirror_audio && (!cast || opus);
        want_audio != audio_on) {
      audio_on = want_audio;
      if (audio_on) {
        g_audio_source.store(nullptr, std::memory_order_relaxed);
        audio_read = g_audio_seq.load(std::memory_order_acquire);
        audio_loss = false;
      }
      g_mirror_audio_armed.store(audio_on, std::memory_order_relaxed);
    }
    xe::ui::g_xdroid_blank_local.store(cvars::xdroid_mirror_blank_local,
                                       std::memory_order_relaxed);
    // 4 ms keeps the audio under one guest frame (5.3 ms) behind.
    pollfd fds[2] = {{video_fd, POLLIN, 0}, {audio_fd, POLLIN, 0}};
    if (poll(fds, cast ? 1 : 2, audio_on ? 4 : cast ? 20 : 50) < 0 &&
        errno != EINTR) {
      break;
    }
    if (cast) {
      bool want_key = false;
      if (!cast->Poll(want_key) || GetCast().empty()) {
        break;
      }
      if (want_key) {
        g_want_key.store(true, std::memory_order_relaxed);
      }
    } else if (((fds[0].revents & POLLIN) &&
         !ReadControl(video_fd, video_reader, audio_fd)) ||
        ((fds[1].revents & POLLIN) &&
         !ReadControl(audio_fd, audio_reader, audio_fd)) ||
        ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL))) {
      break;
    }
    if (cvars::xdroid_mirror_kbps != kbps) {
      kbps = cvars::xdroid_mirror_kbps;
      g_kbps.store(ClampKbps(kbps), std::memory_order_relaxed);
    }
    if (cvars::xdroid_mirror_operating_rate != rate) {
      rate = cvars::xdroid_mirror_operating_rate;
      g_rate.store(ClampRate(rate), std::memory_order_relaxed);
    }
    if (audio_on && cast) {
      SendCastAudio(*cast, *opus, audio_read);
    } else if (audio_on && !SendAudio(audio_fd, audio_read, audio_loss)) {
      break;
    }
  }
  g_mirror_audio_armed.store(false, std::memory_order_relaxed);
  xe::ui::g_xdroid_blank_local.store(false, std::memory_order_relaxed);
  g_session.store(0, std::memory_order_release);
  Wake();
  // Unblocks a send of the Out thread; the fds stay open until the capture
  // thread let go of them, so their numbers can't be reused under it.
  if (!cast) {
    shutdown(video_fd, SHUT_RDWR);
    shutdown(audio_fd, SHUT_RDWR);
  }
  AwaitCapture(g_session_ack, 0);
  g_cast.store(nullptr, std::memory_order_release);
  g_video_fd.store(-1, std::memory_order_relaxed);
}

void NetLoop() {
  pthread_setname_np(pthread_self(), "Mirror Net");
  uint32_t session = 0;
  bool failing = false;
  for (;;) {
    Token token;
    if (!cvars::xdroid_mirror || !GetToken(token)) {
      failing = false;
      SleepMs(250);
      continue;
    }
    const uint32_t height =
        uint32_t(std::clamp(cvars::xdroid_mirror_height, 144, 1440)) & ~1u;
    const uint32_t width = (height * 16 / 9 + 1) & ~1u;
    const mp::Codec codec =
        cvars::xdroid_mirror_codec == 2 ? mp::Codec::kHevc : mp::Codec::kAvc;
    const std::string cast_addr = GetCast();
    if (!cast_addr.empty()) {
      CastSender cast;
      if (!cast.Open(cast_addr, token.data())) {
        XELOGW("XDROID-CAST: bad receiver address {}", cast_addr);
        SleepMs(1000);
        continue;
      }
      g_width.store(1280, std::memory_order_relaxed);  // The OFFER's size.
      g_height.store(720, std::memory_order_relaxed);
      g_codec.store(uint8_t(mp::Codec::kAvc), std::memory_order_relaxed);
      if (!++session) {
        ++session;
      }
      XELOGI("XDROID-CAST connected: session {}, receiver {}", session,
             cast_addr);
      Serve(cast.fd(), -1, session, &cast);
      XELOGI("XDROID-CAST disconnected: session {}", session);
      SleepMs(500);
      continue;
    }
    const int port = cvars::xdroid_mirror_port;
    const int video_fd = Connect(port);
    bool ok = video_fd >= 0 && Handshake(video_fd, token, mp::Stream::kVideo,
                                         codec, width, height);
    const int audio_fd = ok ? Connect(port) : -1;
    ok = ok && audio_fd >= 0 &&
         Handshake(audio_fd, token, mp::Stream::kAudio, mp::Codec::kNone, 0,
                   0);
    if (ok) {
      failing = false;
      g_width.store(width, std::memory_order_relaxed);
      g_height.store(height, std::memory_order_relaxed);
      g_codec.store(uint8_t(codec), std::memory_order_relaxed);
      if (!++session) {
        ++session;
      }
      XELOGI("XDROID-MIRROR connected: session {}, port {}", session, port);
      Serve(video_fd, audio_fd, session);
      XELOGI("XDROID-MIRROR disconnected: session {}", session);
    } else if (!failing) {
      failing = true;
      XELOGW(
          "XDROID-MIRROR: no viewer on 127.0.0.1:{} (adb reverse, token); "
          "retrying every second",
          port);
    }
    if (video_fd >= 0) {
      close(video_fd);
    }
    if (audio_fd >= 0) {
      close(audio_fd);
    }
    if (!ok) {
      SleepMs(1000);
    }
  }
}

}  // namespace

void InstallMirror() {
  static std::once_flag installed;
  std::call_once(installed, [] {
    // Detached: they idle on a futex and a 250 ms sleep, and live as long as
    // the process.
    std::thread(CaptureLoop).detach();
    std::thread(NetLoop).detach();
    g_installed.store(true, std::memory_order_release);
  });
}

void MirrorDetachGraphics() {
  if (!g_installed.load(std::memory_order_acquire)) {
    return;
  }
  const uint32_t gen = g_gfx_gen.fetch_add(1, std::memory_order_release) + 1;
  Wake();
  // Unbounded: returning early would let the capture thread use a freed
  // device. The capture thread holds no lock of the teardown path.
  AwaitCapture(g_gfx_ack, gen);
}

void MirrorSetToken(std::string_view hex) {
  Token token{};
  bool valid = hex.size() == 2 * token.size();
  for (size_t i = 0; valid && i < hex.size(); ++i) {
    const char c = hex[i];
    const int digit = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
    valid = digit >= 0;
    token[i / 2] = uint8_t(token[i / 2] << 4 | (digit & 0xF));
  }
  {
    std::lock_guard<std::mutex> lock(g_token_mutex);
    if (valid == g_token_valid && (!valid || token == g_token)) {
      return;  // live.toml changed elsewhere.
    }
    g_token = valid ? token : Token{};
    g_token_valid = valid;
  }
  XELOGI("XDROID-MIRROR token {}",
         valid ? "set" : (hex.empty() ? "cleared" : "INVALID"));
}

bool MirrorSetString(std::string_view name, std::string_view value) {
  const bool cast = name == "xdroid_mirror_cast";
  if (!cast && name != "xdroid_mirror_keys") {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_token_mutex);
  LiveString& live = cast ? g_cast_live : g_keys_live;
  if (!cast && value != (live.set ? live.value : cvars::xdroid_mirror_keys)) {
    g_keys_gen.fetch_add(1, std::memory_order_relaxed);
  }
  live.value.assign(value);
  live.set = true;
  return true;
}

void MirrorOnPresent() {
  if (g_session.load(std::memory_order_relaxed)) {
    Wake();
  }
}

void MirrorOnAudioFrame(const void* source, const float* frame, float gain) {
  // One audio client feeds the stream: the first one after arming, or a
  // new one once the owner sent nothing for 100 ms (its driver is gone).
  const int64_t now_us = NowUs();
  const void* owner = g_audio_source.load(std::memory_order_relaxed);
  if (owner != source &&
      ((owner && now_us - g_audio_source_us.load(std::memory_order_relaxed) <
                     100000) ||
       !g_audio_source.compare_exchange_strong(owner, source,
                                               std::memory_order_relaxed))) {
    return;
  }
  g_audio_source_us.store(now_us, std::memory_order_relaxed);
  float stereo[kGuestSamples * 2];
  xe::apu::conversion::sequential_6_BE_to_interleaved_2_LE(stereo, frame,
                                                           kGuestSamples);
  const uint64_t seq = g_audio_seq.load(std::memory_order_relaxed);
  g_audio_claim.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  AudioSlot& slot = g_audio_ring[seq % kAudioSlots];
  for (uint32_t i = 0; i < kGuestSamples * 2; ++i) {
    // fmax/fmin also turn NaN into -1.
    const float sample = std::fmin(std::fmax(stereo[i] * gain, -1.0f), 1.0f);
    slot.pcm[i] = int16_t(std::lrint(sample * 32767.0f));
  }
  slot.stamp_us = now_us;
  g_audio_seq.store(seq + 1, std::memory_order_release);
}

void MirrorMuteLocal(float* out, int32_t pairs) {
  static float gain = 1.0f;  // Audio callback thread only.
  const float target =
      MirrorAudioArmed() && cvars::xdroid_mirror_mute_local ? 0.0f : 1.0f;
  if (gain == 1.0f && target == 1.0f) {
    return;
  }
  // One callback ramps a change, so the switch does not click.
  const float step = (target - gain) / float(std::max(pairs, 1));
  for (int32_t i = 0; i < pairs * 2; i += 2) {
    gain += step;
    out[i] *= gain;
    out[i + 1] *= gain;
  }
  gain = target;
}

}  // namespace xdroid
