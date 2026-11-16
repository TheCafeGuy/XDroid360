// MIRROR-1 PC viewer: shows the decoded pictures, paced once per refresh in
// vsync (pace.h), else each the moment it lands; plus the F1 overlay. Paced,
// a 30 fps stream on 60 Hz gets a mid picture in the refresh that would
// repeat the older one, and each real picture comes one refresh later.
#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_3.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "decode.h"
#include "pace.h"
#include "tune.h"

namespace xv {

class AudioOut;
class Server;

// Copies `texture` to the CPU: desc.Height rows of *pitch bytes (for NV12,
// the luma plane).
std::vector<uint8_t> ReadBack(ID3D11Device* device, ID3D11Texture2D* texture,
                              UINT* pitch);

class Renderer {
 public:
  ~Renderer();
  bool Init(ID3D11Device* device, HWND window);
  // Starts the render thread; every pointer outlives Stop().
  // `paced`, `tune` and `interp` are the launch options --unpaced,
  // --no-autotune and --no-interp.
  void Start(Decoder* decoder, Server* server, const AudioOut* audio,
             bool paced, bool tune, bool interp);
  void Stop();

  // Window thread calls: the render thread does the work.
  void Resize();
  void Redraw();
  void SetDpi(UINT dpi);
  // The mirror.md present mode default: paced, so mid pictures can come;
  // with --no-interp tear in fullscreen, vsync in a window.
  void SetFullscreen(bool fullscreen) {
    tear_ = fullscreen && tearing_supported_ && !interp_;
  }
  // F1 overlay, F2 scale, F3 filter, F5 display (OLED, LCD, LCD+; kept
  // across runs); false for other keys. F2 to F5 also show a toast for 2 s
  // with the mode now active. Nothing else has a key: the tuner (tune.h)
  // picks the phone encoder rate, and the other switches are launch options
  // (main.cc), so that no run inherits a mode it has no key to leave.
  bool OnKey(UINT key);
  // --selftest, without Start: draws `picture` with `filter` (0 bilinear,
  // 1 Lanczos-2) into the back buffer, no Present, and returns that buffer.
  ComPtr<ID3D11Texture2D> DrawOnly(const Picture* picture, int filter);
  // The picture halfway between `a` and `b` (pts 2 refreshes apart, same
  // size), nullptr when they don't qualify. Owns the device lock or runs
  // before Start (--selftest); valid until the next call.
  const Picture* Mid(const Picture* a, const Picture& b);

 private:
  // Averages over the last second of new pictures, for the overlay.
  struct Totals {
    int64_t start_us = 0;
    uint64_t start_bytes = 0;
    uint32_t frames = 0;
    int64_t encode_us = 0;
    int64_t transfer_us = 0;
    int64_t decode_us = 0;
    int64_t present_us = 0;
    int64_t total_us = 0;
  };
  struct Shown {
    double fps = 0;
    double encode_ms = 0;
    double transfer_ms = 0;
    double decode_ms = 0;
    double present_ms = 0;
    double total_ms = 0;
    double kbps = 0;
  };

  void Run();
  bool CreateTarget();
  // Waits for the paced decision of the next vblank: pictures to step.
  uint32_t Decide(const Picture* shown);
  void Draw(const Picture* picture);
  // Top left panel; `toast`: larger, centered near the bottom.
  void DrawText(const char* ascii, bool toast = false);
  // `mid`: the picture came a refresh after its mid picture.
  void Count(const Picture& picture, int64_t present_us, bool tear, bool paced,
             bool mid);
  bool CreateMid(const Picture& like);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Multithread> lock_;
  ComPtr<IDXGISwapChain2> swapchain_;
  UINT flags_ = 0;
  bool tearing_supported_ = false;
  HANDLE waitable_ = nullptr;
  ComPtr<ID3D11RenderTargetView> target_;
  UINT target_width_ = 0;
  UINT target_height_ = 0;
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shaders_[2];  // Bilinear, Lanczos-2.
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11Buffer> constants_;
  // Mid picture: Shrink, Coarse, Search, Warp; views u0 to u5 of them.
  ComPtr<ID3D11ComputeShader> mid_shaders_[4];
  ComPtr<ID3D11UnorderedAccessView> mid_views_[6];
  Picture mid_;
  ComPtr<ID2D1Factory1> d2d_factory_;
  ComPtr<ID2D1DeviceContext> d2d_;
  ComPtr<ID2D1Bitmap1> d2d_target_;
  ComPtr<ID2D1SolidColorBrush> text_brush_;
  ComPtr<ID2D1SolidColorBrush> panel_brush_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IDWriteTextFormat> text_format_;
  UINT text_dpi_ = 0;

  HWND window_ = nullptr;
  HANDLE wake_ = nullptr;
  HANDLE timer_ = nullptr;  // High resolution: the paced decision.
  Pacer pacer_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> resize_{false};
  std::atomic<UINT> dpi_{96};
  std::atomic<bool> overlay_{false};
  std::atomic<int> scale_{0};   // Fit, fill, integer.
  std::atomic<int> filter_{1};  // Bilinear, Lanczos-2.
  std::atomic<bool> tear_{false};
  std::atomic<bool> paced_{true};
  std::atomic<bool> interp_{true};
  std::atomic<int> display_{0};     // kSaturation index; --selftest stays OLED.
  std::atomic<UINT> toast_key_{0};  // Last switch key; 0: the tuner ended.
  std::atomic<int64_t> toast_until_us_{0};

  Decoder* decoder_ = nullptr;
  Server* server_ = nullptr;
  const AudioOut* audio_ = nullptr;
  int64_t last_decoded_us_ = 0;
  Tuner tuner_;
  bool tune_ = false;
  bool tune_tear_ = false;
  FILE* frame_log_ = nullptr;  // LOCALAPPDATA/XDroid360/viewer-frames.csv
  Totals totals_;
  Shown shown_;
};

}  // namespace xv
