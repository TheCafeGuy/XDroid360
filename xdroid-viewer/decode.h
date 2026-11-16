// MIRROR-1 PC viewer: H.264 decode with the Media Foundation MFT (DXVA when
// the GPU takes it) into a queue of NV12 textures.
#pragma once

#include <d3d11_4.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <vector>

#include "xdroid_mirror_proto.h"

namespace xv {

using Microsoft::WRL::ComPtr;

// The device decoder and renderer share: BGRA for Direct2D, video for DXVA,
// multithread protected because two threads use its immediate context.
ComPtr<ID3D11Device> CreateDevice();

// The NAL units (start code included) of each access unit of an Annex B
// stream, split at access unit delimiters.
std::vector<std::vector<std::span<const uint8_t>>> SplitAccessUnits(
    std::span<const uint8_t> stream);
// H.264 type of a NAL unit that starts with its start code.
inline uint8_t NalType(std::span<const uint8_t> nal) {
  return nal[nal[2] == 1 ? 3 : 4] & 0x1f;
}

// --probe and --selftest: <exe dir>/testdata/<name>, empty when missing.
std::vector<uint8_t> ReadTestStream(const wchar_t* name);
// p50, p95 and max of decode times, in ms.
std::array<double, 3> LatencyMs(std::vector<int64_t> times_us);

struct FrameTimes {
  int64_t pts_us;      // Phone: the frame entered the encoder.
  int64_t aux_us;      // Phone: the encoder output it.
  int64_t receipt_us;  // Viewer: the access unit arrived.
  int64_t decoded_us;  // Viewer: the GPU finished it.
};

struct Picture {
  ComPtr<ID3D11Texture2D> texture;  // NV12 at the decoder's surface size.
  ComPtr<ID3D11ShaderResourceView> luma;
  ComPtr<ID3D11ShaderResourceView> chroma;
  uint32_t texture_width = 0;
  uint32_t texture_height = 0;
  uint32_t width = 0;  // Display size inside the texture.
  uint32_t height = 0;
  FrameTimes times{};
};

class Decoder {
 public:
  Decoder();
  ~Decoder();

  // Restarts for a new stream; false when no decoder takes the codec.
  bool Start(ID3D11Device* device, xdroid::mirror::Codec codec, uint32_t width,
             uint32_t height);
  // Codec config: goes in front of the next access unit.
  void SetConfig(std::span<const uint8_t> config);
  // Decodes one access unit and publishes every picture it yields; never
  // waits for the renderer.
  void Decode(std::span<const uint8_t> access_unit, bool key,
              const FrameTimes& times);

  // Renderer side. Queued(): published pictures not shown yet; Peek(): the
  // oldest of them, nullptr when none. Acquire() steps at most `step` of them
  // (all but the last count as drops) and returns the one to show, nullptr
  // before the first; it stays valid until the next Acquire().
  uint32_t Queued() const {
    return uint32_t(published_.load(std::memory_order_acquire) - shown_);
  }
  const Picture* Peek() const {
    return Queued() ? &pictures_[shown_ % kSlots] : nullptr;
  }
  const Picture* Acquire(uint32_t step = UINT32_MAX);
  // The picture before the one on screen, kept for the renderer's mid
  // picture; nullptr before the second.
  const Picture* Previous() const {
    return shown_ >= 2 ? &pictures_[(shown_ - 2) % kSlots] : nullptr;
  }
  HANDLE frame_event() const { return frame_event_; }
  const char* name() const { return dxva_ ? "DXVA" : "software"; }
  // Pictures never shown: stepped over, or the queue was full.
  uint32_t drops() const { return drops_; }

  // Runs on the decoding thread for every picture, before it is published.
  std::function<void(const FrameTimes&)> on_frame;

 private:
  bool Open(bool dxva, uint32_t width, uint32_t height);
  bool SetOutputType();
  void Drain();
  void Publish(IMFSample* sample);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext4> context_;
  ComPtr<ID3D11Multithread> lock_;
  ComPtr<ID3D11Fence> fence_;
  uint64_t fence_value_ = 0;
  HANDLE fence_event_;
  ComPtr<IMFDXGIDeviceManager> manager_;
  UINT manager_token_ = 0;
  ComPtr<IMFTransform> mft_;
  bool provides_samples_ = false;
  ComPtr<IMFSample> output_;  // Software mode: the MFT wants our sample.
  uint32_t coded_width_ = 0;
  uint32_t coded_height_ = 0;
  uint32_t stride_ = 0;  // Software output: bytes per row.
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::atomic<bool> dxva_{false};
  std::vector<uint8_t> config_;
  std::deque<FrameTimes> pending_;

  // Ring: picture n lives in slot n % kSlots. The decoder writes slot
  // published_ while it is neither queued, the one on screen (shown_ - 1)
  // nor Previous().
  static constexpr uint64_t kSlots = 8;
  std::array<Picture, kSlots> pictures_;
  std::atomic<uint64_t> published_{0};
  std::atomic<uint64_t> shown_{0};  // Pictures the renderer took.
  HANDLE frame_event_;
  std::atomic<uint32_t> drops_{0};
};

}  // namespace xv
