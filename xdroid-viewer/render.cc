#include "render.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "audio.h"
#include "common.h"
#include "net.h"

namespace xv {
namespace {

constexpr float kTextDip = 13;

// One fullscreen triangle over the viewport; the pixel shader turns NV12
// (BT.709, limited range) into RGB.
constexpr char kShaders[] = R"(
cbuffer Constants : register(b0) {
  float2 uv_scale;      // Display part of the texture.
  float2 texture_size;  // Luma texels.
  float2 chroma_shift;  // H.264 chroma sits on the left luma column.
  float2 luma_max;      // Last luma texel inside the display part.
  float saturation;     // Chroma gain: 1 for an OLED, more for an LCD.
  int4 grids_a;         // Mid picture: 1/4 and 1/8 res texels,
  int4 grids_b;         // 32x32 regions, 8x8 blocks (MidGrids).
};
Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
SamplerState linear_clamp : register(s0);

struct Vertex {
  float4 position : SV_Position;
  float2 uv : TEXCOORD;
};

Vertex Vs(uint id : SV_VertexID) {
  float2 t = float2((id << 1) & 2, id & 2);
  Vertex v;
  v.position = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  v.uv = t * uv_scale;
  return v;
}

float3 ToRgb(float y, float2 c) {
  y = (y - 16.0 / 255) * (255.0 / 219);
  c = (c - 128.0 / 255) * (255.0 / 224 * saturation);
  return saturate(float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y,
                         y + 1.8556 * c.x));
}

float2 Chroma(float2 uv) {
  return chroma.Sample(linear_clamp, uv + chroma_shift);
}

float4 Bilinear(Vertex v) : SV_Target {
  return float4(ToRgb(luma.Sample(linear_clamp, v.uv), Chroma(v.uv)), 1);
}

// sinc(x) sinc(x / 2) for |x| < 2.
float4 LanczosWeights(float f) {
  float4 x = abs(float4(1 + f, f, 1 - f, 2 - f)) * 3.14159265 + 1e-5;
  return sin(x) * sin(x / 2) * 2 / (x * x);
}

// Lanczos-2 on luma, where the detail is; chroma stays bilinear.
float4 Lanczos(Vertex v) : SV_Target {
  float2 position = v.uv * texture_size - 0.5;
  float2 base = floor(position);
  float4 wx = LanczosWeights(position.x - base.x);
  float4 wy = LanczosWeights(position.y - base.y);
  float sum = 0;
  [unroll] for (int j = 0; j < 4; ++j) {
    float row = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
      int2 texel = int2(clamp(base + float2(i - 1, j - 1), 0, luma_max));
      row += wx[i] * luma.Load(int3(texel, 0));
    }
    sum += wy[j] * row;
  }
  sum /= dot(wx, 1) * dot(wy, 1);
  return float4(ToRgb(sum, Chroma(v.uv)), 1);
}

// Mid picture of A and B, two refreshes apart. A pyramid search gives each
// 8x8 block its motion: 1/8 res over 32x32 regions, then 1/4 res, then full
// res, each level a texel around the one above, so no level has to match
// across more than half a texel. The last step is symmetric, A at p - d and
// B at p + d with d half the motion, so each texel of the mid picture has a
// vector and none is a hole.
#ifdef CS
Texture2D<float> luma_a : register(t2);
Texture2D<float> luma_b : register(t3);
Texture2D<float2> chroma_a : register(t4);
Texture2D<float2> chroma_b : register(t5);
RWStructuredBuffer<float2> quarter : register(u0);  // A, B at 1/4 res.
RWStructuredBuffer<float2> eighth : register(u1);   // A, B at 1/8 res.
RWStructuredBuffer<uint> region : register(u2);     // Best key (Coarse).
RWStructuredBuffer<int2> motion : register(u3);     // d of each block.
RWTexture2D<float> mid_luma : register(u4);
RWTexture2D<float2> mid_chroma : register(u5);

int2 Quarters() { return grids_a.xy; }
int2 Eighths() { return grids_a.zw; }
int2 Regions() { return grids_b.xy; }
int2 Blocks() { return grids_b.zw; }
float A(int2 p) { return luma_a.Load(int3(clamp(p, 0, int2(luma_max)), 0)); }
float B(int2 p) { return luma_b.Load(int3(clamp(p, 0, int2(luma_max)), 0)); }
float2 Quarter(int2 q) {
  q = clamp(q, 0, Quarters() - 1);
  return quarter[q.y * Quarters().x + q.x];
}
float2 Eighth(int2 e) {
  e = clamp(e, 0, Eighths() - 1);
  return eighth[e.y * Eighths().x + e.x];
}

// Four bilinear taps, each the mean of 2x2 texels.
float2 Taps(int2 q) {
  float2 sum = 0;
  [unroll] for (int i = 0; i < 4; ++i) {
    float2 uv = (q * 4 + 1 + 2 * int2(i & 1, i >> 1)) / texture_size;
    sum += float2(luma_a.SampleLevel(linear_clamp, uv, 0),
                  luma_b.SampleLevel(linear_clamp, uv, 0));
  }
  return sum / 4;
}

[numthreads(8, 8, 1)]
void Shrink(uint3 id : SV_DispatchThreadID) {
  int2 q = id.xy;
  if (any(q >= Quarters())) {
    return;
  }
  quarter[q.y * Quarters().x + q.x] = Taps(q);
  if (all((q & 1) == 0)) {
    eighth[q.y / 2 * Eighths().x + q.x / 2] =
        (Taps(q) + Taps(q + int2(1, 0)) + Taps(q + int2(0, 1)) + Taps(q + 1)) /
        4;
  }
}

// Every motion up to 8 texels (64 at full res) on the 6x6 texels around the
// region; a thread per region and motion row (id.z), the least cost wins
// through one atomic. A small cost per texel of motion keeps flat sky at
// rest.
[numthreads(8, 8, 1)]
void Coarse(uint3 id : SV_DispatchThreadID) {
  int2 r = id.xy;
  if (any(r >= Regions())) {
    return;
  }
  int vy = int(id.z) - 8;
  uint best = ~0u;
  for (int vx = -8; vx <= 8; ++vx) {
    float cost = 0.001 * (abs(vx) + abs(vy));
    for (int j = -1; j < 5; ++j) {
      [unroll] for (int i = -1; i < 5; ++i) {
        int2 e = r * 4 + int2(i, j);
        cost += abs(Eighth(e).x - Eighth(e + int2(vx, vy)).y);
      }
    }
    best = min(best, min(uint(cost * 1024), 0x7fffffu) << 9 |
                         uint(id.z * 17 + vx + 8));
  }
  InterlockedMin(region[r.y * Regions().x + r.x], best);
}

// The motion Coarse picked, in 1/4 texels.
int2 Region(int2 r) {
  uint index = region[r.y * Regions().x + r.x] & 511;
  return (int2(index % 17, index / 17) - 8) * 2;
}

// Rest and the motion of the block's region and its two nearest neighbors,
// each a texel around at 1/4 res on the 4x4 texels around the block; then
// a texel around at full res on the block's 64.
[numthreads(8, 8, 1)]
void Search(uint3 id : SV_DispatchThreadID) {
  int2 b = id.xy;
  if (any(b >= Blocks())) {
    return;
  }
  int2 n = Regions();
  int2 r = min(b / 4, n - 1);
  int2 side = clamp(r + int2((b & 3) >= 2) * 2 - 1, 0, n - 1);
  int2 candidates[4] = {int2(0, 0), Region(r), Region(int2(side.x, r.y)),
                        Region(int2(r.x, side.y))};
  int2 best = 0;
  float least = 1e9;
  for (int k = 0; k < 4; ++k) {
    for (int t = 0; t < 9; ++t) {
      int2 v = candidates[k] + int2(t % 3, t / 3) - 1;
      float cost = 0.001 * (abs(v.x) + abs(v.y));
      [unroll] for (int j = -1; j < 3; ++j) {
        [unroll] for (int i = -1; i < 3; ++i) {
          int2 q = b * 2 + int2(i, j);
          cost += abs(Quarter(q).x - Quarter(q + v).y);
        }
      }
      if (cost < least) {
        least = cost;
        best = v;
      }
    }
  }
  int2 coarse = best * 2;  // 1/4 texels of motion: 2 texels of d.
  least = 1e9;
  for (int t = 0; t < 9; ++t) {
    int2 d = coarse + int2(t % 3, t / 3) - 1;
    float cost = 0.001 * (abs(d.x) + abs(d.y));
    for (int j = 0; j < 8; ++j) {
      [unroll] for (int i = 0; i < 8; ++i) {
        int2 p = b * 8 + int2(i, j);
        cost += abs(A(p - d) - B(p + d));
      }
    }
    if (cost < least) {
      least = cost;
      best = d;
    }
  }
  motion[b.y * Blocks().x + b.x] = best;
}

float Mismatch(int2 p0, int2 d) {
  float cost = 0.001 * (abs(d.x) + abs(d.y));
  [unroll] for (int j = -1; j < 3; ++j) {
    [unroll] for (int i = -1; i < 3; ++i) {
      int2 p = p0 + int2(i, j);
      cost += abs(A(p - d) - B(p + d));
    }
  }
  return cost;
}

// Per texel the best of rest, its block's d and the nearest neighbors': the
// quad's 4x4 texels around it and, weighted 8, the texel's own match. Block
// edges follow the picture; a still HUD keeps its edge texels. Chroma takes
// the quad's best.
[numthreads(8, 8, 1)]
void Warp(uint3 id : SV_DispatchThreadID) {
  int2 q = id.xy;  // Chroma texel, 2x2 luma quad.
  int2 p0 = q * 2;
  if (any(p0 >= int2(texture_size))) {
    return;
  }
  int2 n = Blocks();
  int2 b = min(p0 / 8, n - 1);
  int2 side = clamp(b + int2((p0 & 7) >= 4) * 2 - 1, 0, n - 1);
  int2 candidates[4] = {int2(0, 0), motion[b.y * n.x + b.x],
                        motion[b.y * n.x + side.x],
                        motion[side.y * n.x + b.x]};
  float window[4];
  float least = 1e9;
  int2 d = 0;
  [unroll] for (int k = 0; k < 4; ++k) {
    window[k] = Mismatch(p0, candidates[k]);
    if (window[k] < least) {
      least = window[k];
      d = candidates[k];
    }
  }
  [unroll] for (int t = 0; t < 4; ++t) {
    int2 p = p0 + int2(t & 1, t >> 1);
    float low = 1e9, value = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
      float a = A(p - candidates[k]), b = B(p + candidates[k]);
      float cost = window[k] + 8 * abs(a - b);
      if (cost < low) {
        low = cost;
        value = (a + b) / 2;
      }
    }
    mid_luma[p] = value;
  }
  float2 uv = (q + 0.5) / (texture_size / 2);
  float2 shift = d / texture_size;
  mid_chroma[q] = (chroma_a.SampleLevel(linear_clamp, uv - shift, 0) +
                   chroma_b.SampleLevel(linear_clamp, uv + shift, 0)) / 2;
}
#endif
)";

struct Constants {
  float uv_scale[2];
  float texture_size[2];
  float chroma_shift[2];
  float luma_max[2];
  float saturation;
  float pad[3];  // Constant buffers come in 16-byte steps.
  int32_t grids[8];
};

// F5 cycles these. The phone's OLED shows the game more saturated than a
// typical sRGB LCD; a chroma gain (luma kept) brings the LCD close to it.
constexpr float kSaturation[] = {1.0f, 1.25f, 1.5f};
constexpr const char* kDisplayName[] = {"oled", "lcd", "lcd+"};
constexpr const char* kScaleName[] = {"fit", "fill", "integer"};
constexpr wchar_t kSettingsKey[] = L"Software\\XDroid360\\Viewer";

// The mid picture's grids: 1/4 and 1/8 res texels, 32x32 regions, 8x8
// blocks.
struct MidGrids {
  UINT size[4][2];
  explicit MidGrids(const Picture& p) {
    UINT x = (p.width + 3) / 4, y = (p.height + 3) / 4;
    UINT ex = (x + 1) / 2, ey = (y + 1) / 2;
    size[0][0] = x, size[0][1] = y;
    size[1][0] = ex, size[1][1] = ey;
    size[2][0] = (ex + 3) / 4, size[2][1] = (ey + 3) / 4;
    size[3][0] = (p.width + 7) / 8, size[3][1] = (p.height + 7) / 8;
  }
  UINT count(int i) const { return size[i][0] * size[i][1]; }
};

Constants MakeConstants(const Picture& picture, float saturation) {
  float tw = float(picture.texture_width);
  float th = float(picture.texture_height);
  Constants constants{{picture.width / tw, picture.height / th},
                      {tw, th},
                      {0.5f / tw, 0},
                      {picture.width - 1.0f, picture.height - 1.0f},
                      saturation,
                      {},
                      {}};
  std::memcpy(constants.grids, MidGrids(picture).size, sizeof constants.grids);
  return constants;
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  // CS: the compute passes, whose groupshared memory no other stage has.
  D3D_SHADER_MACRO cs[] = {{"CS", "1"}, {nullptr, nullptr}};
  if (FAILED(D3DCompile(kShaders, sizeof kShaders - 1, "viewer",
                        target[0] == 'c' ? cs : nullptr, nullptr, entry,
                        target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                        0, &code, &errors))) {
    std::fprintf(stderr, "xv: shader %s: %s\n", entry,
                 errors ? static_cast<const char*>(errors->GetBufferPointer())
                        : "D3DCompile failed");
    return nullptr;
  }
  return code;
}

}  // namespace

Renderer::~Renderer() {
  Stop();
  for (HANDLE handle : {wake_, timer_}) {
    if (handle) {
      CloseHandle(handle);
    }
  }
}

bool Renderer::Init(ID3D11Device* device, HWND window) {
  device_ = device;
  window_ = window;
  device->GetImmediateContext(&context_);
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  ComPtr<IDXGIFactory5> factory5;
  if (FAILED(device_.As(&lock_)) || FAILED(device_.As(&dxgi_device)) ||
      FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  BOOL tearing = FALSE;
  tearing_supported_ =
      SUCCEEDED(factory.As(&factory5)) &&
      SUCCEEDED(factory5->CheckFeatureSupport(
          DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)) &&
      tearing;
  flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
           (tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_NONE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Flags = flags_;
  ComPtr<IDXGISwapChain1> swapchain;
  if (FAILED(factory->CreateSwapChainForHwnd(device, window, &desc, nullptr,
                                             nullptr, &swapchain)) ||
      FAILED(swapchain.As(&swapchain_))) {
    std::fprintf(stderr, "xv: no flip-model swapchain\n");
    return false;
  }
  // The window does its own borderless fullscreen.
  factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  // One queued frame at most: a picture never waits behind an older one.
  swapchain_->SetMaximumFrameLatency(1);
  waitable_ = swapchain_->GetFrameLatencyWaitableObject();

  ComPtr<ID3DBlob> vs = Compile("Vs", "vs_5_0");
  ComPtr<ID3DBlob> bilinear = Compile("Bilinear", "ps_5_0");
  ComPtr<ID3DBlob> lanczos = Compile("Lanczos", "ps_5_0");
  if (!vs || !bilinear || !lanczos) {
    return false;
  }
  device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                             nullptr, &vertex_shader_);
  device->CreatePixelShader(bilinear->GetBufferPointer(),
                            bilinear->GetBufferSize(), nullptr,
                            &pixel_shaders_[0]);
  device->CreatePixelShader(lanczos->GetBufferPointer(),
                            lanczos->GetBufferSize(), nullptr,
                            &pixel_shaders_[1]);
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  device->CreateSamplerState(&sampler, &sampler_);
  D3D11_BUFFER_DESC constants{};
  constants.ByteWidth = sizeof(Constants);
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  device->CreateBuffer(&constants, nullptr, &constants_);
  const char* passes[] = {"Shrink", "Coarse", "Search", "Warp"};
  for (int i = 0; i < 4; ++i) {
    ComPtr<ID3DBlob> cs = Compile(passes[i], "cs_5_0");
    if (!cs || FAILED(device->CreateComputeShader(cs->GetBufferPointer(),
                                                  cs->GetBufferSize(), nullptr,
                                                  &mid_shaders_[i]))) {
      interp_ = false;  // The viewer still runs, at the stream's own rate.
    }
  }

  ComPtr<ID2D1Device> d2d_device;
  if (FAILED(D2D1CreateFactory(
          D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
          reinterpret_cast<void**>(d2d_factory_.GetAddressOf()))) ||
      FAILED(d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device)) ||
      FAILED(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                             &d2d_)) ||
      FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
    return false;
  }
  d2d_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &text_brush_);
  d2d_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &panel_brush_);
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
  return timer_ && CreateTarget();
}

void Renderer::Start(Decoder* decoder, Server* server, const AudioOut* audio,
                     bool paced, bool tune, bool interp) {
  decoder_ = decoder;
  server_ = server;
  audio_ = audio;
  stop_ = false;
  paced_ = paced;
  tune_ = tune;
  interp_ = interp_ && interp;
  // Kept across runs: only what a key changes.
  DWORD display = 0;
  DWORD size = sizeof display;
  if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"display",
                   RRF_RT_REG_DWORD, nullptr, &display,
                   &size) == ERROR_SUCCESS &&
      display < std::size(kSaturation)) {
    display_ = int(display);
  }
  if (tune_) {
    server_->SetEncoderRate(tuner_.arm());  // Sent again at each connection.
  }
  MONITORINFOEXW monitor{};
  monitor.cbSize = sizeof monitor;
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                  &monitor);
  EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode);
  if (mode.dmDisplayFrequency > 1) {
    pacer_.SetPeriod(1000000 / mode.dmDisplayFrequency);  // DXGI refines it.
  }
  // One CSV per run for the smoothness hunt: a line per new picture shown.
  if (const wchar_t* base = _wgetenv(L"LOCALAPPDATA")) {
    std::wstring dir = std::wstring(base) + L"\\XDroid360";
    CreateDirectoryW(dir.c_str(), nullptr);
    frame_log_ = _wfopen((dir + L"\\viewer-frames.csv").c_str(), L"w");
  }
  if (frame_log_) {
    // pts/aux: phone clock (viewer = phone - offset); the rest NowUs. sync:
    // the last vblank DXGI saw, with its present and refresh counts.
    std::fprintf(frame_log_,
                 "# refresh %lu Hz\n"
                 "pts_us,aux_us,receipt_us,decoded_us,present_us,offset_us,"
                 "tear,drops,present_count,sync_present,sync_refresh,"
                 "sync_us,paced,guard_us,audio_buffer_us,audio_target_us,"
                 "audio_underruns,audio_overflows,audio_low_latency,"
                 "encoder_rate,mid\n",
                 mode.dmDisplayFrequency);
  }
  thread_ = std::thread(&Renderer::Run, this);
}

void Renderer::Stop() {
  if (!thread_.joinable()) {
    return;
  }
  stop_ = true;
  SetEvent(wake_);
  thread_.join();
  if (frame_log_) {
    std::fclose(frame_log_);
    frame_log_ = nullptr;
  }
}

void Renderer::Resize() {
  resize_ = true;
  SetEvent(wake_);
}

void Renderer::Redraw() { SetEvent(wake_); }

void Renderer::SetDpi(UINT dpi) {
  dpi_ = dpi;
  SetEvent(wake_);
}

bool Renderer::OnKey(UINT key) {
  switch (key) {
    case VK_F1:
      overlay_ = !overlay_;
      break;
    case VK_F2:
      scale_ = (scale_ + 1) % 3;
      break;
    case VK_F3:
      filter_ = filter_ ^ 1;
      break;
    case VK_F5: {
      DWORD display = DWORD(display_ = (display_ + 1) % 3);
      RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"display", REG_DWORD,
                      &display, sizeof display);
      break;
    }
    default:
      return false;
  }
  if (key != VK_F1) {
    toast_key_ = key;
    toast_until_us_ = NowUs() + 2000000;
  }
  SetEvent(wake_);
  return true;
}

// Back buffer 0 is always the one to draw in a flip-model swapchain, so the
// views survive every Present and only a resize remakes them.
bool Renderer::CreateTarget() {
  ComPtr<ID3D11Texture2D> buffer;
  ComPtr<IDXGISurface> surface;
  DXGI_SWAP_CHAIN_DESC1 desc;
  if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
      FAILED(
          device_->CreateRenderTargetView(buffer.Get(), nullptr, &target_)) ||
      FAILED(buffer.As(&surface)) ||
      FAILED(d2d_->CreateBitmapFromDxgiSurface(
          surface.Get(),
          D2D1::BitmapProperties1(
              D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                D2D1_ALPHA_MODE_IGNORE)),
          &d2d_target_))) {
    return false;
  }
  d2d_->SetTarget(d2d_target_.Get());
  swapchain_->GetDesc1(&desc);
  target_width_ = desc.Width;
  target_height_ = desc.Height;
  return true;
}

void Renderer::Run() {
  HANDLE events[] = {decoder_->frame_event(), wake_};
  const Picture* picture = nullptr;
  bool held = false;  // The last present was a mid picture: `picture` next.
  HRESULT result = S_OK;
  while (!stop_ && SUCCEEDED(result)) {
    bool tear = tear_;
    // Paced while pictures flow: a present every refresh, decided late.
    bool paced = paced_ && !tear && picture &&
                 (decoder_->Queued() ||
                  NowUs() - picture->times.decoded_us < 1000000);
    // Else, without a picture or with the overlay or a toast on, redraw 4
    // times a second so the text stays current; else once a second, so a
    // device lost while no frame comes still shows up in Present.
    if (!paced && !held) {
      bool text = overlay_ || !picture || NowUs() < toast_until_us_;
      WaitForMultipleObjects(2, events, FALSE, text ? 250 : 1000);
    }
    if (stop_) {
      break;
    }
    if (resize_.exchange(false)) {
      RECT client;
      GetClientRect(window_, &client);
      UINT width = client.right - client.left;
      UINT height = client.bottom - client.top;
      if (width && height &&
          (width != target_width_ || height != target_height_)) {
        lock_->Enter();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        target_.Reset();
        d2d_->SetTarget(nullptr);
        d2d_target_.Reset();
        result = swapchain_->ResizeBuffers(0, width, height,
                                           DXGI_FORMAT_UNKNOWN, flags_);
        if (SUCCEEDED(result) && !CreateTarget()) {
          result = E_FAIL;
        }
        lock_->Leave();
        if (FAILED(result)) {
          break;
        }
      }
    }
    // Ready for a new frame: Present will not block. Unpaced, the picture
    // taken next is the newest one.
    WaitForSingleObject(waitable_, 100);
    // After a mid picture its real one goes out at once, a refresh later.
    uint32_t step = held ? 0 : paced ? Decide(picture) : UINT32_MAX;
    picture = decoder_->Acquire(step);
    lock_->Enter();
    const Picture* mid =
        paced && step == 1 ? Mid(decoder_->Previous(), *picture) : nullptr;
    Draw(mid ? mid : picture);
    // Present flushes the immediate context the decoder shares.
    result = swapchain_->Present(tear ? 0 : 1,
                                 tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
    lock_->Leave();
    if (picture && !mid && SUCCEEDED(result)) {
      Count(*picture, NowUs(), tear, paced, held);
    }
    held = mid != nullptr;
  }
  if (FAILED(result)) {
    // A lost device (driver update, TDR, GPU unplugged) is not rebuilt: the
    // viewer closes, so its exit still switches the phone back off.
    std::fprintf(stderr, "xv: swapchain failed (%08lx, device %08lx)\n", result,
                 device_->GetDeviceRemovedReason());
    PostMessageW(window_, WM_CLOSE, 0, 0);
  }
}

uint32_t Renderer::Decide(const Picture* shown) {
  DXGI_FRAME_STATISTICS sync{};  // Zero until DXGI has seen a vblank.
  swapchain_->GetFrameStatistics(&sync);
  int64_t deadline = pacer_.Deadline(sync, NowUs());
  LARGE_INTEGER due{};
  due.QuadPart = -std::max<int64_t>(deadline - NowUs(), 0) * 10;
  SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
  HANDLE events[] = {decoder_->frame_event(), wake_, timer_};
  for (;;) {
    bool final = NowUs() >= deadline;
    const Picture* next = decoder_->Peek();
    uint32_t step = pacer_.Step(&shown->times, next ? &next->times : nullptr,
                                decoder_->Queued());
    if (step || final || stop_) {
      return step;
    }
    WaitForMultipleObjects(3, events, FALSE, 100);
  }
}

void Renderer::Draw(const Picture* picture) {
  const float black[4] = {0, 0, 0, 1};
  context_->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
  context_->ClearRenderTargetView(target_.Get(), black);
  if (picture) {
    float sx = float(target_width_) / picture->width;
    float sy = float(target_height_) / picture->height;
    float scale = scale_ == 1 ? std::max(sx, sy) : std::min(sx, sy);
    if (scale_ == 2 && scale >= 1) {
      scale = std::floor(scale);
    }
    D3D11_VIEWPORT viewport{};
    viewport.Width = std::round(picture->width * scale);
    viewport.Height = std::round(picture->height * scale);
    viewport.TopLeftX = std::round((target_width_ - viewport.Width) / 2);
    viewport.TopLeftY = std::round((target_height_ - viewport.Height) / 2);
    viewport.MaxDepth = 1;
    Constants constants = MakeConstants(*picture, kSaturation[display_]);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11ShaderResourceView* views[] = {picture->luma.Get(),
                                         picture->chroma.Get()};
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    // Both stages read the constants: an unbound buffer reads as zeros, and
    // uv_scale 0 paints every pixel with texel (0, 0).
    context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->PSSetShader(pixel_shaders_[filter_].Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 2, views);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->RSSetViewports(1, &viewport);
    context_->Draw(3, 0);
  }
  // The last switch for its 2 s, else the tuner's progress.
  char toast[48] = "";
  if (UINT key = toast_key_; NowUs() < toast_until_us_) {
    std::snprintf(toast, sizeof toast, "%s%s",
                  key == VK_F2   ? "scale: "
                  : key == VK_F3 ? "filter: "
                  : key == VK_F5 ? "display: "
                                 : "auto-tune done",
                  key == VK_F2   ? kScaleName[scale_]
                  : key == VK_F3 ? filter_ ? "lanczos2" : "bilinear"
                  : key == VK_F5 ? kDisplayName[display_]
                                 : "");
  } else if (tune_ && picture && !tuner_.done()) {
    std::snprintf(toast, sizeof toast, "auto-tune %d%%", tuner_.percent());
  }
  if (*toast) {
    DrawText(toast, true);
  }
  // A status (no token from the app) stays over the last picture.
  const char* status = server_ ? server_->status() : nullptr;
  if (status || !picture) {
    DrawText(status ? status : "XDroid360 mirror: waiting for the app");
    return;
  }
  if (!overlay_) {
    return;
  }
  Server::Clock clock = server_->clock();
  AudioOut::Stats audio = audio_->GetStats();
  std::string tune = !tune_          ? "off"
                     : tuner_.done() ? tuner_.Report()
                                     : std::to_string(tuner_.percent()) + "%";
  char text[640];
  std::snprintf(text, sizeof text,
                "%.1f fps  %s  %s%s  drops %u\n"
                "encode %.1f  transfer %.1f  decode %.1f  present %.1f ms\n"
                "phone to present ~%.1f ms  rtt %.1f ms%s\n"
                "video %.0f kbps\n"
                "audio %.1f / %.1f ms  underruns %u  %s\n"
                "%s  %s  %s\n"
                "encoder rate %d  auto-tune %s",
                shown_.fps, decoder_->name(),
                tear_    ? "tear"
                : paced_ ? "vsync paced"
                         : "vsync",
                interp_ && paced_ && !tear_ ? " + mid" : "",
                decoder_->drops(), shown_.encode_ms, shown_.transfer_ms,
                shown_.decode_ms, shown_.present_ms, shown_.total_ms,
                clock.rtt_us / 1000.0, clock.valid ? "" : " (no clock yet)",
                shown_.kbps, audio.buffered_us / 1000.0,
                audio.target_us / 1000.0, audio.underruns,
                AudioLowLatency() ? "low latency" : "safe", kScaleName[scale_],
                filter_ ? "lanczos2" : "bilinear", kDisplayName[display_],
                server_->encoder_rate(), tune.c_str());
  DrawText(text);
}

const Picture* Renderer::Mid(const Picture* a, const Picture& b) {
  // Only a 30 fps step on 60 Hz: any other gap has no single mid moment.
  double period = pacer_.period_us();
  if (!interp_ || !a ||
      std::abs(double(b.times.pts_us - a->times.pts_us) - 2 * period) >
          period / 2 ||
      a->texture_width != b.texture_width ||
      a->texture_height != b.texture_height || a->width != b.width ||
      a->height != b.height) {
    return nullptr;
  }
  if ((mid_.texture_width != b.texture_width ||
       mid_.texture_height != b.texture_height || mid_.width != b.width ||
       mid_.height != b.height) &&
      !CreateMid(b)) {
    std::fprintf(stderr, "xv: no mid picture resources\n");
    interp_ = false;
    return nullptr;
  }
  Constants constants = MakeConstants(b, kSaturation[display_]);
  context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
  ID3D11ShaderResourceView* views[] = {a->luma.Get(), b.luma.Get(),
                                       a->chroma.Get(), b.chroma.Get()};
  ID3D11UnorderedAccessView* outputs[6];
  for (int i = 0; i < 6; ++i) {
    outputs[i] = mid_views_[i].Get();
  }
  context_->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
  context_->CSSetShaderResources(2, 4, views);
  context_->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  context_->CSSetUnorderedAccessViews(0, 6, outputs, nullptr);
  // Threads: 1/4 res texels, regions, blocks, chroma texels; 8x8 a group.
  MidGrids grids(b);
  UINT threads[4][2] = {{grids.size[0][0], grids.size[0][1]},
                        {grids.size[2][0], grids.size[2][1]},
                        {grids.size[3][0], grids.size[3][1]},
                        {b.texture_width / 2, b.texture_height / 2}};
  UINT none[4] = {~0u, ~0u, ~0u, ~0u};
  context_->ClearUnorderedAccessViewUint(mid_views_[2].Get(), none);
  for (int i = 0; i < 4; ++i) {
    context_->CSSetShader(mid_shaders_[i].Get(), nullptr, 0);
    context_->Dispatch((threads[i][0] + 7) / 8, (threads[i][1] + 7) / 8,
                       i == 1 ? 17 : 1);  // Coarse: a motion row each.
  }
  // Unbound, else the draw could not read the mid picture.
  ID3D11ShaderResourceView* no_views[4] = {};
  ID3D11UnorderedAccessView* no_outputs[6] = {};
  context_->CSSetShaderResources(2, 4, no_views);
  context_->CSSetUnorderedAccessViews(0, 6, no_outputs, nullptr);
  mid_.times = b.times;
  return &mid_;
}

bool Renderer::CreateMid(const Picture& like) {
  mid_ = Picture{};
  MidGrids grids(like);
  for (int i = 0; i < 4; ++i) {  // float2 texels, uint keys, int2 vectors.
    UINT stride = i == 2 ? 4 : 8;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = grids.count(i) * stride;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = grids.count(i);
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &buffer)) ||
        FAILED(device_->CreateUnorderedAccessView(buffer.Get(), &view,
                                                  &mid_views_[i]))) {
      return false;
    }
  }
  // NV12's two planes as their own textures, so Draw reads them unchanged.
  DXGI_FORMAT formats[] = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM};
  ComPtr<ID3D11ShaderResourceView>* planes[] = {&mid_.luma, &mid_.chroma};
  for (int i = 0; i < 2; ++i) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = like.texture_width >> i;
    desc.Height = like.texture_height >> i;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = formats[i];
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture)) ||
        FAILED(device_->CreateUnorderedAccessView(texture.Get(), nullptr,
                                                  &mid_views_[4 + i])) ||
        FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr,
                                                 planes[i]->GetAddressOf()))) {
      return false;
    }
  }
  mid_.texture_width = like.texture_width;
  mid_.texture_height = like.texture_height;
  mid_.width = like.width;
  mid_.height = like.height;
  return true;
}

ComPtr<ID3D11Texture2D> Renderer::DrawOnly(const Picture* picture, int filter) {
  filter_ = filter;
  lock_->Enter();
  Draw(picture);
  lock_->Leave();
  ComPtr<ID3D11Resource> resource;
  ComPtr<ID3D11Texture2D> texture;
  target_->GetResource(&resource);
  resource.As(&texture);
  return texture;
}

std::vector<uint8_t> ReadBack(ID3D11Device* device, ID3D11Texture2D* texture,
                              UINT* pitch) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
    return {};
  }
  context->CopyResource(staging.Get(), texture);
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return {};
  }
  auto bytes = static_cast<const uint8_t*>(mapped.pData);
  std::vector<uint8_t> rows(bytes,
                            bytes + size_t(mapped.RowPitch) * desc.Height);
  context->Unmap(staging.Get(), 0);
  *pitch = mapped.RowPitch;
  return rows;
}

void Renderer::DrawText(const char* ascii, bool toast) {
  std::wstring text(ascii, ascii + std::strlen(ascii));
  UINT dpi = dpi_;
  // The Direct2D target stays at 96 DPI (one DIP per pixel), so the font
  // follows the monitor scale instead.
  if (dpi != text_dpi_) {
    text_format_.Reset();
    dwrite_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, kTextDip * dpi / 96,
                              L"en-us", &text_format_);
    text_dpi_ = dpi;
  }
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS metrics;
  if (!text_format_ ||
      FAILED(dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()),
                                       text_format_.Get(), target_width_,
                                       target_height_, &layout)) ||
      (toast && FAILED(layout->SetFontSize(1.5f * kTextDip * dpi / 96,
                                           {0, UINT32(text.size())}))) ||
      FAILED(layout->GetMetrics(&metrics))) {
    return;
  }
  float margin = 6.0f * dpi / 96;
  float width = metrics.width + 2 * margin;
  float height = metrics.height + 2 * margin;
  float x = toast ? std::round((target_width_ - width) / 2) : 0;
  float y = toast ? std::round(target_height_ * 0.9f - height) : 0;
  d2d_->BeginDraw();
  d2d_->FillRectangle(D2D1::RectF(x, y, x + width, y + height),
                      panel_brush_.Get());
  d2d_->DrawTextLayout(D2D1::Point2F(x + margin, y + margin), layout.Get(),
                       text_brush_.Get());
  d2d_->EndDraw();
}

void Renderer::Count(const Picture& picture, int64_t present_us, bool tear,
                     bool paced, bool mid) {
  if (picture.times.decoded_us == last_decoded_us_) {
    return;  // Shown before: a redraw, not a new picture.
  }
  last_decoded_us_ = picture.times.decoded_us;
  const FrameTimes& t = picture.times;
  Server::Clock clock = server_->clock();
  int64_t offset = clock.offset_us;
  if (frame_log_) {
    UINT presents = 0;
    DXGI_FRAME_STATISTICS sync{};  // Zero until DXGI has seen a vblank.
    swapchain_->GetLastPresentCount(&presents);
    swapchain_->GetFrameStatistics(&sync);
    AudioOut::Stats audio = audio_->GetStats();
    std::fprintf(frame_log_,
                 "%lld,%lld,%lld,%lld,%lld,%lld,%d,%u,%u,%u,%u,%lld,%d,%.0f,"
                 "%u,%u,%u,%u,%d,%d,%d\n",
                 t.pts_us, t.aux_us, t.receipt_us, t.decoded_us, present_us,
                 offset, tear, decoder_->drops(), presents, sync.PresentCount,
                 sync.SyncRefreshCount, QpcToUs(sync.SyncQPCTime.QuadPart),
                 paced, pacer_.guard_us(), audio.buffered_us, audio.target_us,
                 audio.underruns, audio.overflows, AudioLowLatency(),
                 server_->encoder_rate(), mid);
  }
  if (tune_ && !tuner_.done() && clock.valid) {
    // Arms compare within one present mode: a switch starts over.
    bool restart = tear != tune_tear_;
    if (restart) {
      tune_tear_ = tear;
      tuner_ = {};
    }
    if (tuner_.Add(t.pts_us - offset, present_us) || restart) {
      server_->SetEncoderRate(tuner_.arm());
    }
    if (tuner_.done()) {
      std::string report = tuner_.Report();
      std::fprintf(stderr, "xv: auto-tune %s\n", report.c_str());
      if (frame_log_) {
        std::fprintf(frame_log_, "# tune %s\n", report.c_str());
      }
      toast_key_ = 0;
      toast_until_us_ = present_us + 3000000;
    }
  }
  if (!totals_.start_us) {
    totals_ = {present_us, server_->video_bytes()};
    return;
  }
  ++totals_.frames;
  totals_.encode_us += t.aux_us - t.pts_us;
  totals_.transfer_us += t.receipt_us - (t.aux_us - offset);
  totals_.decode_us += t.decoded_us - t.receipt_us;
  totals_.present_us += present_us - t.decoded_us;
  totals_.total_us += present_us - (t.pts_us - offset);
  int64_t elapsed = present_us - totals_.start_us;
  if (elapsed < 1000000) {
    return;
  }
  uint64_t bytes = server_->video_bytes();
  double n = totals_.frames * 1000.0;
  shown_ = {totals_.frames * 1e6 / elapsed,
            totals_.encode_us / n,
            totals_.transfer_us / n,
            totals_.decode_us / n,
            totals_.present_us / n,
            totals_.total_us / n,
            (bytes - totals_.start_bytes) * 8000.0 / elapsed};
  totals_ = {present_us, bytes};
}

}  // namespace xv
