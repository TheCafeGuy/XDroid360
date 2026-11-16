// MIRROR-1 PC viewer: the listener the app reaches through `adb reverse`.
#pragma once

#include <winsock2.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "xdroid_mirror_proto.h"

struct ID3D11Device;

namespace xv {

namespace mp = xdroid::mirror;

class AudioOut;
class Decoder;

// Blocking socket I/O, shared with the --selftest fake app. RecvExact reads
// exactly `size` bytes; with a deadline (viewer clock, 0 = none) it fails
// once the deadline passes, however slowly the bytes trickle in.
bool SendAll(SOCKET socket, const void* data, int size);
bool RecvExact(SOCKET socket, void* data, int size, int64_t deadline_us);

struct Tokens {
  std::array<uint8_t, mp::kTokenBytes> server;
  std::array<uint8_t, mp::kTokenBytes> client;

  // live.toml form: 64 hex digits, server token first.
  std::string Hex() const;
  // Hex() form, surrounding whitespace allowed; false (and no change)
  // unless exactly 64 lowercase hex digits.
  bool FromHex(std::string_view hex);
};

class Server {
 public:
  struct Callbacks {
    // Keys to publish: new ones at start, the same ones again after 3 s
    // with no stream (the app deletes live.toml when it starts). Runs on
    // the accept thread; returns once the app can read them. It may replace
    // the keys the handshake checks; true: call again after 1 s, not 3 s.
    std::function<bool(Tokens&)> on_tokens;
    // A stream passed the handshake; runs before its first packet.
    std::function<void(const mp::ClientHello&)> on_hello;
    // One packet of either stream, on that stream's thread. receipt_us is
    // the viewer clock once the whole payload arrived.
    std::function<void(const mp::PacketHeader&, std::span<const uint8_t>,
                       int64_t receipt_us)>
        on_packet;
  };
  // Phone clock = viewer clock + offset_us.
  struct Clock {
    int64_t rtt_us = 0;  // Latest ping.
    int64_t offset_us = 0;
    bool valid = false;
  };

  ~Server();
  // Listens on 127.0.0.1:port (0: any port), then publishes fresh tokens.
  bool Start(uint16_t port, Callbacks callbacks);
  void Stop();
  // Goes on the audio connection, else on the video one.
  void SendControl(mp::Control type, int64_t value);
  // The phone encoder's operating rate in frames/s, sent now and at the
  // start of every later session. 0: never set, the app's own (120).
  void SetEncoderRate(int rate);
  int encoder_rate() const { return encoder_rate_; }

  uint16_t port() const { return port_; }
  Clock clock() const;
  uint64_t video_bytes() const { return video_bytes_; }
  uint32_t rejections() const { return rejections_; }  // Failed handshakes.
  // A line the window shows instead of the picture; null for none.
  const char* status() const { return status_; }
  void SetStatus(const char* status) { status_ = status; }

 private:
  struct Connection {
    SOCKET socket = INVALID_SOCKET;
    std::mutex send_mutex;
    std::thread reader;
    std::atomic<bool> alive{false};
  };
  struct Sample {
    int64_t rtt_us;
    int64_t offset_us;
  };

  void Accept();
  void Handshake(SOCKET socket);
  void Read(Connection* connection, SOCKET socket);
  void Close(Connection* connection);

  Callbacks callbacks_;
  SOCKET listen_ = INVALID_SOCKET;
  uint16_t port_ = 0;
  std::thread accept_;
  std::atomic<bool> stop_{false};
  Tokens tokens_{};    // Only the accept thread touches them after Start.
  bool soon_ = false;  // on_tokens asked for the next call after 1 s.
  std::atomic<const char*> status_{nullptr};
  std::atomic<uint32_t> rejections_{0};
  std::array<Connection, 2> connections_;  // Index: Stream - 1.
  std::atomic<uint64_t> video_bytes_{0};
  std::atomic<int> encoder_rate_{0};
  mutable std::mutex clock_mutex_;
  std::array<Sample, 16> samples_{};
  uint32_t sample_count_ = 0;
};

// Wires the streams of `server` to the decoder and the audio player.
Server::Callbacks Route(ID3D11Device* device, Decoder* decoder, AudioOut* audio,
                        bool null_sink);

}  // namespace xv
