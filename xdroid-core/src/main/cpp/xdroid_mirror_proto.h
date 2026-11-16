// MIRROR-1 wire protocol, shared by the app (xdroid-core) and the PC viewer
// (xdroid-viewer). Plain C++17 with no platform header: the structs go on the
// wire as they are, so every field is little-endian (both ends are) and every
// struct has a fixed size without padding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xdroid::mirror {

constexpr uint8_t kMagic[4] = {'X', 'D', 'M', '1'};
constexpr uint16_t kVersion = 2;
constexpr uint16_t kDefaultPort = 27360;
constexpr uint32_t kTokenBytes = 16;  // One key: server or client.
constexpr uint32_t kNonceBytes = 16;
constexpr uint32_t kMacBytes = 16;

// Connection setup. The viewer listens on 127.0.0.1:port on the PC, maps the
// port with `adb reverse tcp:port tcp:port` and writes the port and two keys
// (server, client) into the app's live.toml (only the app can read its
// files). No key byte goes on the wire: each side proves its key with a MAC
// over both fresh nonces, so another app on the phone that connects to the
// port or squats it learns nothing it can replay. One connection per stream:
//   1. viewer -> app: ServerHello, the viewer's nonce.
//   2. app -> viewer: ClientHello, the app's nonce and HelloMac(client key,
//      'C'). The viewer closes on a wrong MAC.
//   3. viewer -> app: ServerProof, HelloMac(server key, 'S'). The app closes
//      on a wrong MAC before it sends any frame.
struct ServerHello {
  uint8_t magic[4];
  uint16_t version;
  uint16_t reserved;
  uint8_t nonce[kNonceBytes];
};

enum class Stream : uint8_t { kVideo = 1, kAudio = 2 };
enum class Codec : uint8_t { kNone = 0, kAvc = 1, kHevc = 2 };

struct ClientHello {
  uint8_t magic[4];
  uint16_t version;
  uint8_t stream;  // Stream.
  uint8_t codec;   // Codec of the video stream, kNone for audio.
  uint8_t nonce[kNonceBytes];
  uint16_t width;  // Encoded video size.
  uint16_t height;
  uint32_t audio_rate;     // Hz of the audio stream.
  uint8_t audio_channels;  // Interleaved s16 channels of the audio stream.
  uint8_t reserved[3];
  uint8_t mac[kMacBytes];  // HelloMac(client key, 'C').
};

struct ServerProof {
  uint8_t mac[kMacBytes];  // HelloMac(server key, 'S').
};

// SipHash-2-4 with a 128-bit tag (Aumasson and Bernstein, public domain
// algorithm), keyed by a 16-byte token: the handshake MAC.
inline uint64_t SipRotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
inline uint64_t SipLoad(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    v |= uint64_t(p[i]) << (8 * i);
  }
  return v;
}
inline void SipHash128(const uint8_t key[16], const uint8_t* data, size_t size,
                       uint8_t out[16]) {
  const uint64_t k0 = SipLoad(key, 8), k1 = SipLoad(key + 8, 8);
  uint64_t v[4] = {k0 ^ 0x736f6d6570736575, k1 ^ 0x646f72616e646f6d ^ 0xee,
                   k0 ^ 0x6c7967656e657261, k1 ^ 0x7465646279746573};
  auto rounds = [&v](int n) {
    while (n--) {
      v[0] += v[1], v[1] = SipRotl(v[1], 13) ^ v[0], v[0] = SipRotl(v[0], 32);
      v[2] += v[3], v[3] = SipRotl(v[3], 16) ^ v[2];
      v[0] += v[3], v[3] = SipRotl(v[3], 21) ^ v[0];
      v[2] += v[1], v[1] = SipRotl(v[1], 17) ^ v[2], v[2] = SipRotl(v[2], 32);
    }
  };
  for (size_t i = 0;; i += 8) {
    const bool last = size - i < 8;
    const uint64_t m = last ? SipLoad(data + i, size - i) | uint64_t(size) << 56
                            : SipLoad(data + i, 8);
    v[3] ^= m;
    rounds(2);
    v[0] ^= m;
    if (last) {
      break;
    }
  }
  for (uint8_t half = 0xee, *o = out; o < out + 16; half = 0xdd, o += 8) {
    v[half == 0xee ? 2 : 1] ^= half;
    rounds(4);
    const uint64_t h = v[0] ^ v[1] ^ v[2] ^ v[3];
    for (int i = 0; i < 8; ++i) {
      o[i] = uint8_t(h >> (8 * i));
    }
  }
}

// MAC of |label|, the viewer's nonce and the ClientHello up to its mac.
inline void HelloMac(const uint8_t key[kTokenBytes], uint8_t label,
                     const ServerHello& server, const ClientHello& client,
                     uint8_t mac[kMacBytes]) {
  uint8_t message[1 + kNonceBytes + offsetof(ClientHello, mac)];
  message[0] = label;
  std::memcpy(message + 1, server.nonce, kNonceBytes);
  std::memcpy(message + 1 + kNonceBytes, &client, offsetof(ClientHello, mac));
  SipHash128(key, message, sizeof(message), mac);
}

// app -> viewer: a header, then `size` payload bytes.
enum class Packet : uint8_t {
  kConfig = 1,  // Codec config (SPS/PPS, plus VPS for HEVC), Annex B.
  kVideo = 2,   // One access unit, Annex B.
  kAudio = 3,   // Interleaved s16 PCM.
  kPong = 4,    // Answer to Control::kPing, no payload.
};
enum : uint8_t {
  kFlagKey = 1,   // kVideo: decodable without earlier frames.
  kFlagLoss = 2,  // kAudio: samples were lost before this packet.
};
// Times are the phone's CLOCK_MONOTONIC in microseconds; kPing/kPong map
// them onto the viewer's clock.
struct PacketHeader {
  uint8_t type;  // Packet.
  uint8_t flags;
  uint16_t reserved;
  uint32_t size;
  // kConfig, kVideo: the frame entered the encoder. kAudio: the guest
  // submitted the first sample. kPong: the app read the ping.
  int64_t pts_us;
  // kVideo: the encoder output the frame (aux - pts = encode time). kAudio:
  // the app sent the packet. kPong: the ping's value, echoed.
  int64_t aux_us;
};

// viewer -> app, on either connection.
enum class Control : uint8_t {
  kPing = 1,        // value: viewer clock in us, echoed in kPong.
  kRequestKey = 2,  // The app asks the encoder for a sync frame.
  kSetKbps = 3,     // value: video bitrate in kbit/s.
  kSetRate = 4,     // value: encoder operating rate in frames/s.
};
struct ControlMessage {
  uint8_t type;  // Control.
  uint8_t reserved[7];
  int64_t value;
};

static_assert(sizeof(ServerHello) == 24);
static_assert(sizeof(ClientHello) == 52);
static_assert(sizeof(ServerProof) == kMacBytes);
static_assert(sizeof(PacketHeader) == 24);
static_assert(sizeof(ControlMessage) == 16);

}  // namespace xdroid::mirror
