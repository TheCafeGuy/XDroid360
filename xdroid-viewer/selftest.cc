// MIRROR-1 PC viewer, --selftest: the viewer core (listener, decoder, audio
// buffer, renderer) against a fake app on loopback. Shows no window, plays
// nothing.
#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

#include "audio.h"
#include "common.h"
#include "decode.h"
#include "net.h"
#include "render.h"

namespace xv {
namespace {

// The fake phone clock runs this far ahead, so the clock sync has work.
constexpr int64_t kPhoneOffsetUs = 987654321;
constexpr int64_t kFrameUs = 33333;
constexpr int64_t kEncodeUs = 4000;
constexpr uint32_t kRate = 48000;
// The phone's audio pump: a burst of 256-frame guest frames every 40 ms, some
// late (mirror1.log: gaps up to 75 ms in play, 154 ms once at boot). SendAudio
// polls every 4 ms and batches up to 16 guest frames a packet.
constexpr uint32_t kSlotFrames = 256;
constexpr int64_t kBurstUs = 40000;
constexpr int64_t kLateUs[] = {0, 0, 0,     24000, 0, 0, 47000, 0,
                               0, 0, 33000, 0,     0, 0, 10000, 0};
// One burst this late once the buffer settled: the only chop allowed.
constexpr int kStallBurst = 50;
constexpr int64_t kStallUs = 110000;
constexpr int64_t kAudioUs = 6000000;

int64_t PhoneUs() { return NowUs() + kPhoneOffsetUs; }

struct Timer {
  HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  ~Timer() { CloseHandle(handle); }
};

// Sleep() would round the 5.33 ms audio cadence up to whole milliseconds.
void SleepUntil(int64_t when_us) {
  thread_local Timer timer;
  int64_t left = when_us - NowUs();
  if (left > 0) {
    LARGE_INTEGER due;
    due.QuadPart = -left * 10;
    SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer.handle, INFINITE);
  }
}

SOCKET Connect(uint16_t port) {
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  BOOL no_delay = TRUE;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&no_delay), sizeof no_delay);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof address);
  return socket;
}

// The app's hello for the viewer's |server| hello, proving |client_key|.
mp::ClientHello MakeHello(const mp::ServerHello& server, mp::Stream stream,
                          const uint8_t* client_key) {
  mp::ClientHello hello{};
  std::memcpy(hello.magic, mp::kMagic, sizeof hello.magic);
  hello.version = mp::kVersion;
  hello.stream = uint8_t(stream);
  bool video = stream == mp::Stream::kVideo;
  hello.codec = uint8_t(video ? mp::Codec::kAvc : mp::Codec::kNone);
  hello.width = video ? 1280 : 0;
  hello.height = video ? 720 : 0;
  hello.audio_rate = video ? 0 : kRate;
  hello.audio_channels = video ? 0 : 2;
  std::memset(hello.nonce, 0x40 + int(stream), mp::kNonceBytes);
  mp::HelloMac(client_key, 'C', server, hello, hello.mac);
  return hello;
}

// The app's half of the handshake: true once the viewer proved its key.
bool Hello(SOCKET socket, const Tokens& tokens, mp::Stream stream,
           const uint8_t* client_key) {
  mp::ServerHello server;
  if (!RecvExact(socket, &server, sizeof server, NowUs() + 1000000)) {
    return false;
  }
  mp::ClientHello hello = MakeHello(server, stream, client_key);
  mp::ServerProof proof, want;
  mp::HelloMac(tokens.server.data(), 'S', server, hello, want.mac);
  return SendAll(socket, &hello, sizeof hello) &&
         RecvExact(socket, &proof, sizeof proof, NowUs() + 1000000) &&
         !std::memcmp(proof.mac, want.mac, mp::kMacBytes);
}

// True when the viewer closed the connection before the deadline.
bool Closed(SOCKET socket, int64_t deadline_us) {
  char byte;
  return !RecvExact(socket, &byte, 1, deadline_us) && NowUs() < deadline_us;
}

bool SendPacket(SOCKET socket, mp::Packet type, uint8_t flags,
                std::span<const uint8_t> payload, int64_t pts_us,
                int64_t aux_us) {
  mp::PacketHeader header{uint8_t(type), flags, 0, uint32_t(payload.size()),
                          pts_us,        aux_us};
  return SendAll(socket, &header, sizeof header) &&
         (payload.empty() ||
          SendAll(socket, payload.data(), int(payload.size())));
}

struct Sample {
  int64_t t_us;  // Since the first audio packet was due.
  AudioOut::Stats stats;
};

bool Row(const char* check, const char* result, bool ok) {
  std::printf("  %-16s %-46s %s\n", check, result, ok ? "ok" : "FAIL");
  return ok;
}

// What the window shows. Drawn 1:1 into a hidden window, each pixel is one
// texel, so its BT.709 luma must follow the decoded one; a flat, flipped or
// misplaced picture fails. Correlation, not equality: the test stream's
// saturated colors clip in RGB.
bool CheckRender(ID3D11Device* device, const Picture* picture) {
  HWND window = picture ? CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0,
                                          picture->width, picture->height,
                                          nullptr, nullptr, nullptr, nullptr)
                        : nullptr;
  bool pass = true;
  {
    Renderer renderer;
    UINT luma_pitch = 0;
    std::vector<uint8_t> luma;
    if (window && renderer.Init(device, window)) {
      luma = ReadBack(device, picture->texture.Get(), &luma_pitch);
    }
    for (int filter : {0, 1}) {
      UINT pitch = 0;
      std::vector<uint8_t> shown;
      if (!luma.empty()) {
        shown =
            ReadBack(device, renderer.DrawOnly(picture, filter).Get(), &pitch);
      }
      // Of gray, want, gray^2, want^2, gray * want.
      double sums[5] = {};
      int count = 0;
      for (uint32_t y = 4; !shown.empty() && y < picture->height; y += 8) {
        for (uint32_t x = 4; x < picture->width; x += 8) {
          const uint8_t* bgra = &shown[y * pitch + 4 * x];
          double gray = 0.0722 * bgra[0] + 0.7152 * bgra[1] + 0.2126 * bgra[2];
          double want = std::clamp(
              (luma[y * luma_pitch + x] - 16) * 255.0 / 219, 0.0, 255.0);
          double terms[5] = {gray, want, gray * gray, want * want, gray * want};
          for (int i = 0; i < 5; ++i) {
            sums[i] += terms[i];
          }
          ++count;
        }
      }
      for (double& sum : sums) {
        sum /= std::max(count, 1);  // Means.
      }
      double gray_var = sums[2] - sums[0] * sums[0];
      double want_var = sums[3] - sums[1] * sums[1];
      double r =
          gray_var > 0 && want_var > 0
              ? (sums[4] - sums[0] * sums[1]) / std::sqrt(gray_var * want_var)
              : 0;
      double spread = std::sqrt(std::max(0.0, want_var));
      char text[96];
      std::snprintf(text, sizeof text, "luma correlation %.4f, spread %.0f", r,
                    spread);
      pass &= Row(filter ? "render lanczos2" : "render bilinear", text,
                  count && r >= 0.98 && spread >= 8);
    }
  }
  if (window) {
    DestroyWindow(window);
  }
  return pass;
}


// The mid picture of a textured plane moved by (12, 6) texels under a still
// HUD box (random 4x4 cells) must be the plane moved by (6, 3) and the box
// unchanged; a plain blend of the two pictures misses by far more.
bool CheckMid(ID3D11Device* device) {
  constexpr int kW = 1280, kH = 720;
  // Value noise on an 8-texel grid: smooth, and no two places alike.
  auto at = [](int i, int j) {
    uint32_t h = (uint32_t(i) * 73856093u) ^ (uint32_t(j) * 19349663u);
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return double((h ^ (h >> 15)) & 255);
  };
  auto noise = [&](double x, double y) {
    double fx = x / 8, fy = y / 8;
    int i = int(std::floor(fx)), j = int(std::floor(fy));
    double u = fx - i, v = fy - j;
    return (at(i, j) * (1 - u) + at(i + 1, j) * u) * (1 - v) +
           (at(i, j + 1) * (1 - u) + at(i + 1, j + 1) * u) * v;
  };
  auto hud = [](int x, int y) {
    return x >= 96 && x < 352 && y >= 48 && y < 128;
  };
  auto scene = [&](int x, int y, double t) {  // t: 0 A, 0.5 mid, 1 B.
    return hud(x, y) ? (at(x / 4, y / 4 + 1000) >= 128 ? 220.0 : 40.0)
                     : 16 + noise(x - 12 * t, y - 6 * t) * 219 / 255;
  };
  Picture pictures[2];
  for (int k = 0; k < 2; ++k) {
    std::vector<uint8_t> nv12(kW * kH * 3 / 2, 128);  // Gray chroma.
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        nv12[y * kW + x] = uint8_t(std::lround(scene(x, y, k)));
      }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{nv12.data(), kW, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    Picture& p = pictures[k];
    device->CreateTexture2D(&desc, &data, &p.texture);
    view.Format = DXGI_FORMAT_R8_UNORM;
    device->CreateShaderResourceView(p.texture.Get(), &view, &p.luma);
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    device->CreateShaderResourceView(p.texture.Get(), &view, &p.chroma);
    p.texture_width = p.width = kW;
    p.texture_height = p.height = kH;
    p.times.pts_us = k * 33333;
  }
  HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64,
                                nullptr, nullptr, nullptr, nullptr);
  double error = 0, blend = 0, still = 0, ms = 0;
  int moving = 0, box = 0;
  {
    Renderer renderer;
    const Picture* mid = pictures[1].luma && window &&
                                 renderer.Init(device, window)
                             ? renderer.Mid(&pictures[0], pictures[1])
                             : nullptr;
    std::vector<uint8_t> rows;
    UINT pitch = 0;
    if (mid) {
      // 50 more, then the read-back waits for the GPU: an upper bound.
      int64_t start = NowUs();
      for (int i = 0; i < 50; ++i) {
        renderer.Mid(&pictures[0], pictures[1]);
      }
      ComPtr<ID3D11Resource> resource;
      ComPtr<ID3D11Texture2D> texture;
      mid->luma->GetResource(&resource);
      resource.As(&texture);
      rows = ReadBack(device, texture.Get(), &pitch);
      ms = (NowUs() - start) / 50000.0;
    }
    for (int y = 16; !rows.empty() && y < kH - 16; y += 2) {
      for (int x = 16; x < kW - 16; x += 2) {
        double want = scene(x, y, 0.5);
        double got = rows[y * pitch + x];
        if (hud(x, y)) {
          still += std::abs(got - want);
          ++box;
        } else {
          error += std::abs(got - want);
          blend += std::abs((scene(x, y, 0) + scene(x, y, 1)) / 2 - want);
          ++moving;
        }
      }
    }
  }
  if (window) {
    DestroyWindow(window);
  }
  error /= std::max(moving, 1);
  blend /= std::max(moving, 1);
  still /= std::max(box, 1);
  char text[96];
  std::snprintf(text, sizeof text, "error %.1f (blend %.1f) hud %.1f, %.2f ms",
                error, blend, still, ms);
  return Row("mid picture", text,
             moving && box && error <= 2 && still <= 2 && blend >= 3 * error);
}

}  // namespace

int RunSelftest() {
  std::vector<uint8_t> stream = ReadTestStream(L"test720.h264");
  ComPtr<ID3D11Device> device = CreateDevice();
  if (stream.empty() || !device) {
    std::printf("selftest: needs testdata/test720.h264 and a D3D11 GPU\n");
    return 1;
  }
  auto units = SplitAccessUnits(stream);
  Decoder decoder;
  AudioOut audio;
  Server server;
  std::vector<int64_t> decode_us;
  decoder.on_frame = [&](const FrameTimes& t) {
    decode_us.push_back(t.decoded_us - t.receipt_us);
  };
  Server::Callbacks callbacks = Route(device.Get(), &decoder, &audio, true);
  std::atomic<int> hellos{0};
  callbacks.on_hello =
      [&, route = callbacks.on_hello](const mp::ClientHello& hello) {
        ++hellos;
        route(hello);
      };
  std::mutex tokens_mutex;
  Tokens tokens{};
  std::atomic<int> publishes{0};
  callbacks.on_tokens = [&](Tokens& t) {
    std::lock_guard lock(tokens_mutex);
    tokens = t;
    ++publishes;
    return false;
  };
  auto current = [&] {
    std::lock_guard lock(tokens_mutex);
    return tokens;
  };
  // Failed handshake `n` happened and the keys survived it.
  auto kept = [&](uint32_t n, const Tokens& before) {
    Sleep(50);
    return server.rejections() == n && current().Hex() == before.Hex();
  };
  if (!server.Start(0, callbacks)) {
    return 1;
  }
  bool pass = true;
  char text[96];
  std::printf("selftest:\n");

  // Rejection 1: a phone app that reached the port but has no client key.
  const Tokens before = current();
  auto wrong = before.client;
  wrong[0] ^= 1;
  SOCKET socket = Connect(server.port());
  int64_t sent = NowUs();
  bool rejected = !Hello(socket, before, mp::Stream::kVideo, wrong.data());
  int64_t rejected_us = NowUs() - sent;
  closesocket(socket);
  rejected &= rejected_us < 500000 && kept(1, before);
  std::snprintf(text, sizeof text, "closed after %.1f ms, keys kept",
                rejected_us / 1e3);
  pass &= Row("wrong key", text, rejected);

  // Rejection 2: a client that never sends its hello. It keeps a valid one
  // for this connection's nonce, to replay below.
  socket = Connect(server.port());
  mp::ServerHello server_hello;
  bool silent =
      RecvExact(socket, &server_hello, sizeof server_hello, NowUs() + 1000000);
  const mp::ClientHello old_hello =
      MakeHello(server_hello, mp::Stream::kVideo, before.client.data());
  sent = NowUs();
  silent &= Closed(socket, sent + 3000000);
  double silent_s = (NowUs() - sent) / 1e6;
  closesocket(socket);
  silent &= silent_s > 0.4 && silent_s < 0.9 && kept(2, before);
  std::snprintf(text, sizeof text, "dropped after %.2f s, keys kept", silent_s);
  pass &= Row("no ClientHello", text, silent);

  // Rejection 3: that hello, valid for the old nonce, replayed.
  socket = Connect(server.port());
  bool replay = RecvExact(socket, &server_hello, sizeof server_hello,
                          NowUs() + 1000000) &&
                SendAll(socket, &old_hello, sizeof old_hello);
  sent = NowUs();
  replay &= Closed(socket, sent + 1000000);
  int64_t replay_us = NowUs() - sent;
  closesocket(socket);
  replay &= replay_us < 500000 && kept(3, before);
  std::snprintf(text, sizeof text, "closed after %.1f ms, keys kept",
                replay_us / 1e3);
  pass &= Row("replayed hello", text, replay);

  // A second viewer on a taken port: it must not publish its tokens.
  Server second;
  int second_publishes = 0;
  bool busy = !second.Start(
      server.port(),
      {[&](Tokens&) { return ++second_publishes, false; }, {}, {}});
  busy &= second_publishes == 0;
  pass &=
      Row("busy port", busy ? "refused, nothing published" : "published", busy);

  // The public APK's mirror_token file: Hex() form with its newline.
  Tokens parsed{};
  bool hex =
      parsed.FromHex(" " + before.Hex() + "\r\n") &&
      parsed.Hex() == before.Hex() && !parsed.FromHex(before.Hex().substr(1)) &&
      !parsed.FromHex(std::string(64, 'A')) && parsed.Hex() == before.Hex();
  pass &= Row("token file",
              hex ? "64 hex digits read, others refused" : "wrong", hex);

  // The app: audio first, so every ping goes to the audio connection.
  SOCKET audio_socket = Connect(server.port());
  SOCKET video_socket = Connect(server.port());
  bool connected =
      Hello(audio_socket, before, mp::Stream::kAudio, before.client.data()) &&
      Hello(video_socket, before, mp::Stream::kVideo, before.client.data());
  Sleep(100);
  connected &= hellos == 2 && server.rejections() == 3;
  pass &= Row("handshakes", connected ? "audio + video" : "refused", connected);
  int streaming_publishes = publishes;

  std::mutex audio_mutex;  // Audio packets and pongs share the socket.
  std::atomic<int> pings{0};
  std::atomic<int> key_requests{0};
  std::thread control([&] {
    mp::ControlMessage message;
    while (RecvExact(audio_socket, &message, sizeof message, 0)) {
      if (message.type == uint8_t(mp::Control::kPing)) {
        std::lock_guard lock(audio_mutex);
        SendPacket(audio_socket, mp::Packet::kPong, 0, {}, PhoneUs(),
                   message.value);
        ++pings;
      } else if (message.type == uint8_t(mp::Control::kRequestKey)) {
        ++key_requests;
      }
    }
  });
  int64_t start = NowUs() + 20000;
  std::thread video([&] {
    std::vector<uint8_t> config;
    for (std::span<const uint8_t> nal : units[0]) {
      if (NalType(nal) == 7 || NalType(nal) == 8) {
        config.insert(config.end(), nal.begin(), nal.end());
      }
    }
    SendPacket(video_socket, mp::Packet::kConfig, 0, config, PhoneUs(), 0);
    for (size_t i = 0; i < units.size(); ++i) {
      std::vector<uint8_t> unit;
      bool key = false;
      for (std::span<const uint8_t> nal : units[i]) {
        uint8_t type = NalType(nal);
        if (type != 7 && type != 8) {
          unit.insert(unit.end(), nal.begin(), nal.end());
        }
        key |= type == 5;
      }
      SleepUntil(start + int64_t(i) * kFrameUs);
      int64_t aux = PhoneUs();
      SendPacket(video_socket, mp::Packet::kVideo, key ? mp::kFlagKey : 0, unit,
                 aux - kEncodeUs, aux);
    }
  });
  std::thread sound([&] {
    // When each guest frame leaves the pump: late, never reordered.
    std::vector<int64_t> ready;
    for (int k = 0; k * kBurstUs < kAudioUs; ++k) {
      const int64_t late =
          k == kStallBurst ? kStallUs : kLateUs[k % std::size(kLateUs)];
      const int64_t burst_us =
          std::max(ready.empty() ? 0 : ready.back(), k * kBurstUs + late);
      for (int j = 0; int64_t(ready.size()) * kSlotFrames * 1000000 / kRate <
                      (k + 1) * kBurstUs;
           ++j) {
        ready.push_back(burst_us + j * 500);
      }
    }
    std::vector<int16_t> samples;
    size_t sent = 0;
    for (int64_t poll = 0; sent < ready.size(); poll += 4000) {
      SleepUntil(start + poll);
      const int64_t now = NowUs() - start;
      while (sent < ready.size() && ready[sent] <= now) {
        size_t n = 1;
        while (n < 16 && sent + n < ready.size() && ready[sent + n] <= now) {
          ++n;
        }
        samples.resize(n * kSlotFrames * 2);
        for (size_t f = 0; f < n * kSlotFrames; ++f) {
          double phase = 2 * std::numbers::pi * 440 *
                         double(sent * kSlotFrames + f) / kRate;
          samples[2 * f] = samples[2 * f + 1] = int16_t(8000 * std::sin(phase));
        }
        std::lock_guard lock(audio_mutex);
        SendPacket(audio_socket, mp::Packet::kAudio, 0,
                   {reinterpret_cast<const uint8_t*>(samples.data()),
                    samples.size() * sizeof(int16_t)},
                   start + ready[sent] + kPhoneOffsetUs, PhoneUs());
        sent += n;
      }
    }
  });
  // Stands in for the renderer, which keeps the decoder's queue short.
  std::atomic<bool> draining{true};
  std::thread drain([&] {
    while (draining) {
      WaitForSingleObject(decoder.frame_event(), 20);
      decoder.Acquire();
    }
  });
  std::vector<Sample> stats;
  for (int64_t t = 0; t < kAudioUs; t += 10000) {
    SleepUntil(start + t);
    stats.push_back({t, audio.GetStats()});
    if (t == 1000000) {
      server.SendControl(mp::Control::kRequestKey, 0);
    }
  }
  sound.join();
  video.join();
  Sleep(300);  // The last access units decode.
  draining = false;
  drain.join();
  pass &= CheckRender(device.Get(), decoder.Acquire());
  pass &= CheckMid(device.Get());
  Server::Clock clock = server.clock();
  bool quiet = publishes == streaming_publishes;
  {
    std::lock_guard lock(audio_mutex);
    closesocket(audio_socket);
  }
  control.join();
  closesocket(video_socket);

  // Both streams gone, as when the app restarts: the same keys go out again.
  int64_t closed = NowUs();
  while (quiet && publishes == streaming_publishes &&
         NowUs() < closed + 4500000) {
    Sleep(10);
  }
  double idle_s = (NowUs() - closed) / 1e6;
  bool republished = quiet && publishes == streaming_publishes + 1 &&
                     current().Hex() == before.Hex() &&
                     server.rejections() == 3 && idle_s > 2 && idle_s < 4;
  std::snprintf(text, sizeof text, "same keys %.2f s after the streams%s",
                idle_s, quiet ? "" : ", also while streaming");
  pass &= Row("idle republish", text, republished);
  server.Stop();
  audio.Stop();

  bool frames_ok = decode_us.size() + 5 >= units.size();
  std::snprintf(text, sizeof text, "%zu/%zu (%s)", decode_us.size(),
                units.size(), decoder.name());
  pass &= Row("frames decoded", text, frames_ok);
  auto [p50, p95, max] = LatencyMs(decode_us);
  std::snprintf(text, sizeof text, "p50 %.2f p95 %.2f max %.2f ms", p50, p95,
                max);
  pass &= Row("decode", text, !decode_us.empty());
  int64_t clock_error = clock.offset_us - kPhoneOffsetUs;
  std::snprintf(text, sizeof text, "offset error %lld us, rtt %lld us",
                clock_error, clock.rtt_us);
  pass &= Row("clock", text, clock.valid && std::abs(clock_error) <= 1000);
  std::snprintf(text, sizeof text, "%d pings answered, %d key request",
                pings.load(), key_requests.load());
  pass &= Row("control", text, pings >= 5 && key_requests == 1);

  // The first second fills the buffer and meets the late bursts; judge the
  // steady state after it. The stall is later than all of them: one chop.
  const Sample* settled =
      &*std::find_if(stats.begin(), stats.end(),
                     [](const Sample& s) { return s.t_us >= 1000000; });
  uint32_t underruns = stats.back().stats.underruns - settled->stats.underruns;
  std::snprintf(text, sizeof text, "%u after 1 s (%u total)", underruns,
                stats.back().stats.underruns);
  pass &= Row("underruns", text, underruns <= 1);
  double sum = 0;
  double worst = 0;
  int count = 0;
  double max_ratio = 0;
  for (const Sample& s : stats) {
    if (s.t_us >= kAudioUs - 1000000) {
      double deviation =
          (double(s.stats.buffered_us) - s.stats.target_us) / 1e3;
      sum += deviation;
      worst = std::max(worst, std::abs(deviation));
      ++count;
    }
    if (s.t_us >= 1000000) {
      max_ratio = std::max(max_ratio, std::abs(s.stats.ratio - 1));
    }
  }
  double mean = sum / count;
  std::snprintf(text, sizeof text,
                "mean %+.2f ms, max |dev| %.2f, target %.1f ms", mean, worst,
                stats.back().stats.target_us / 1e3);
  // Target 0: the output never left its startup mute.
  pass &= Row("buffer - target", text,
              worst <= 3 && stats.back().stats.target_us > 0);
  std::snprintf(text, sizeof text, "max |ratio - 1| %.5f", max_ratio);
  pass &= Row("resampler", text, max_ratio <= 0.005);
  // Rate 30 has the least delay but shows every other picture; 240 is 1 ms
  // ahead of 120, inside the margin: the cheaper 120 wins.
  Tuner tuner;
  for (int64_t i = 0, pts_us = 10000000; !tuner.done(); ++i, pts_us += 16667) {
    int rate = tuner.arm();
    if (rate != 30 || i % 2) {
      tuner.Add(pts_us, pts_us + (rate == 30    ? 30000
                                  : rate == 60  ? 45000
                                  : rate == 120 ? 41000
                                                : 40000));
    }
  }
  pass &= Row("auto-tune", tuner.Report().c_str(), tuner.arm() == 120);
  std::printf("selftest: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

}  // namespace xv
