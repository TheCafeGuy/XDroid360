#include "net.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio.h"
#include "common.h"
#include "decode.h"

namespace xv {
namespace {

// The accept thread is serial: a silent peer holds it this long at most.
constexpr int64_t kHandshakeUs = 500000;
constexpr int64_t kPingUs = 500000;
constexpr int64_t kRepublishUs = 3000000;
constexpr int64_t kRereadUs = 1000000;
// Far above any access unit, so a corrupt size fails fast.
constexpr uint32_t kMaxPayload = 16 << 20;

}  // namespace

bool SendAll(SOCKET socket, const void* data, int size) {
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    int sent = send(socket, p, size, 0);
    if (sent <= 0) {
      return false;
    }
    p += sent;
    size -= sent;
  }
  return true;
}

bool RecvExact(SOCKET socket, void* data, int size, int64_t deadline_us) {
  char* p = static_cast<char*>(data);
  while (size > 0) {
    if (deadline_us) {
      int64_t left = deadline_us - NowUs();
      if (left <= 0) {
        return false;
      }
      fd_set set;
      FD_ZERO(&set);
      FD_SET(socket, &set);
      timeval timeout{long(left / 1000000), long(left % 1000000)};
      if (select(0, &set, nullptr, nullptr, &timeout) != 1) {
        return false;
      }
    }
    int got = recv(socket, p, size, 0);
    if (got <= 0) {
      return false;
    }
    p += got;
    size -= got;
  }
  return true;
}

namespace {

// Constant time, so a wrong MAC leaks nothing through timing.
bool SameMac(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (uint32_t i = 0; i < mp::kMacBytes; ++i) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

void Random(uint8_t* data, ULONG size) {
  if (BCryptGenRandom(nullptr, data, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
    // No randomness means no secret: never fall back to a guessable one.
    std::fprintf(stderr, "xv: BCryptGenRandom failed\n");
    std::abort();
  }
}

Tokens NewTokens() {
  Tokens tokens;
  Random(tokens.server.data(), mp::kTokenBytes);
  Random(tokens.client.data(), mp::kTokenBytes);
  return tokens;
}

// Sizes the decoder and the audio player can take; anything else is refused
// like a bad MAC.
bool Sane(const mp::ClientHello& hello) {
  if (hello.stream == uint8_t(mp::Stream::kVideo)) {
    return (hello.codec == uint8_t(mp::Codec::kAvc) ||
            hello.codec == uint8_t(mp::Codec::kHevc)) &&
           hello.width >= 16 && hello.width <= 4096 && hello.height >= 16 &&
           hello.height <= 4096;
  }
  return hello.stream == uint8_t(mp::Stream::kAudio) &&
         hello.audio_rate >= 8000 && hello.audio_rate <= 192000 &&
         hello.audio_channels >= 1 && hello.audio_channels <= 8;
}

}  // namespace

std::string Tokens::Hex() const {
  std::string hex;
  for (const auto* token : {&server, &client}) {
    for (uint8_t byte : *token) {
      hex += "0123456789abcdef"[byte >> 4];
      hex += "0123456789abcdef"[byte & 15];
    }
  }
  return hex;
}

bool Tokens::FromHex(std::string_view hex) {
  constexpr std::string_view kSpace = " \t\r\n";
  hex.remove_prefix(std::min(hex.find_first_not_of(kSpace), hex.size()));
  hex = hex.substr(0, hex.find_last_not_of(kSpace) + 1);
  if (hex.size() != 4 * mp::kTokenBytes) {
    return false;
  }
  Tokens parsed{};
  for (size_t i = 0; i < hex.size(); ++i) {
    char c = hex[i];
    int nibble = c >= '0' && c <= '9'   ? c - '0'
                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                        : -1;
    if (nibble < 0) {
      return false;
    }
    uint8_t& byte =
        (i < 2 * mp::kTokenBytes ? parsed.server
                                 : parsed.client)[i / 2 % mp::kTokenBytes];
    byte = uint8_t(byte << 4 | nibble);
  }
  *this = parsed;
  return true;
}

Server::~Server() { Stop(); }

bool Server::Start(uint16_t port, Callbacks callbacks) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return false;
  }
  listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  // Exclusive: no other local process can bind the port over the viewer.
  BOOL exclusive = TRUE;
  setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int length = sizeof address;
  if (bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof address) ||
      listen(listen_, 4) ||
      getsockname(listen_, reinterpret_cast<sockaddr*>(&address), &length)) {
    std::fprintf(stderr, "xv: cannot listen on 127.0.0.1:%u (%d)\n", port,
                 WSAGetLastError());
    closesocket(listen_);
    listen_ = INVALID_SOCKET;
    WSACleanup();
    return false;
  }
  port_ = ntohs(address.sin_port);
  std::fprintf(stderr, "xv: listening on 127.0.0.1:%u\n", port_);
  // Only now: an app that reads the tokens finds the port open, and a viewer
  // that cannot listen never touches the phone.
  callbacks_ = std::move(callbacks);
  tokens_ = NewTokens();
  soon_ = callbacks_.on_tokens(tokens_);
  stop_ = false;
  accept_ = std::thread(&Server::Accept, this);
  return true;
}

void Server::Stop() {
  if (listen_ == INVALID_SOCKET) {
    return;
  }
  stop_ = true;
  // Wakes the select() of the accept thread.
  closesocket(listen_);
  accept_.join();
  listen_ = INVALID_SOCKET;
  for (Connection& connection : connections_) {
    Close(&connection);
  }
  WSACleanup();
}

void Server::SendControl(mp::Control type, int64_t value) {
  mp::ControlMessage message{};
  message.type = uint8_t(type);
  message.value = value;
  for (int index : {1, 0}) {
    Connection& connection = connections_[index];
    std::lock_guard lock(connection.send_mutex);
    if (connection.alive) {
      SendAll(connection.socket, &message, sizeof message);
      return;
    }
  }
}

void Server::SetEncoderRate(int rate) {
  encoder_rate_ = rate;
  SendControl(mp::Control::kSetRate, rate);
}

Server::Clock Server::clock() const {
  std::lock_guard lock(clock_mutex_);
  Clock clock;
  if (!sample_count_) {
    return clock;
  }
  uint32_t count = std::min<uint32_t>(sample_count_, samples_.size());
  // The lowest round trip had the least queueing, so its midpoint guess is
  // the tightest.
  const Sample* best = &samples_[0];
  for (uint32_t i = 1; i < count; ++i) {
    if (samples_[i].rtt_us < best->rtt_us) {
      best = &samples_[i];
    }
  }
  clock.rtt_us = samples_[(sample_count_ - 1) % samples_.size()].rtt_us;
  clock.offset_us = best->offset_us;
  clock.valid = true;
  return clock;
}

void Server::Accept() {
  int64_t next_ping = NowUs() + kPingUs;
  int64_t quiet_since = NowUs();  // Last stream seen, or last re-publish.
  while (!stop_) {
    int64_t wait = std::max<int64_t>(next_ping - NowUs(), 0);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listen_, &set);
    timeval timeout{long(wait / 1000000), long(wait % 1000000)};
    int ready = select(0, &set, nullptr, nullptr, &timeout);
    if (stop_ || ready < 0) {
      break;
    }
    if (NowUs() >= next_ping) {
      SendControl(mp::Control::kPing, NowUs());
      next_ping = NowUs() + kPingUs;
      // No stream for a while: the app may have restarted (it deletes
      // live.toml) or adb may have lost the tunnel, so publish again.
      if (connections_[0].alive || connections_[1].alive) {
        quiet_since = NowUs();
      } else if (NowUs() - quiet_since >= (soon_ ? kRereadUs : kRepublishUs)) {
        soon_ = callbacks_.on_tokens(tokens_);
        quiet_since = NowUs();
      }
    }
    if (ready == 1) {
      SOCKET socket = accept(listen_, nullptr, nullptr);
      if (socket != INVALID_SOCKET) {
        Handshake(socket);
      }
    }
  }
}

void Server::Handshake(SOCKET socket) {
  BOOL no_delay = TRUE;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&no_delay), sizeof no_delay);
  mp::ServerHello hello{};
  std::memcpy(hello.magic, mp::kMagic, sizeof hello.magic);
  hello.version = mp::kVersion;
  Random(hello.nonce, mp::kNonceBytes);
  mp::ClientHello client{};
  mp::ServerProof proof;
  uint8_t want[mp::kMacBytes];
  bool ok = SendAll(socket, &hello, sizeof hello) &&
            RecvExact(socket, &client, sizeof client, NowUs() + kHandshakeUs) &&
            !std::memcmp(client.magic, mp::kMagic, sizeof client.magic) &&
            client.version == mp::kVersion && Sane(client);
  if (ok) {
    mp::HelloMac(tokens_.client.data(), 'C', hello, client, want);
    mp::HelloMac(tokens_.server.data(), 'S', hello, client, proof.mac);
    ok = SameMac(client.mac, want) && SendAll(socket, &proof, sizeof proof);
  }
  if (!ok) {
    // The peer saw only MACs bound to fresh nonces: the keys stay valid.
    std::fprintf(stderr, "xv: handshake failed\n");
    closesocket(socket);
    ++rejections_;
    return;
  }
  // A new connection of a stream replaces the old one (the app restarted).
  Connection& connection = connections_[client.stream - 1];
  Close(&connection);
  std::fprintf(
      stderr, "xv: %s stream connected\n",
      client.stream == uint8_t(mp::Stream::kVideo) ? "video" : "audio");
  callbacks_.on_hello(client);
  std::lock_guard lock(connection.send_mutex);
  connection.socket = socket;
  connection.alive = true;
  if (int rate = encoder_rate_) {
    mp::ControlMessage message{};
    message.type = uint8_t(mp::Control::kSetRate);
    message.value = rate;
    SendAll(socket, &message, sizeof message);
  }
  connection.reader = std::thread(&Server::Read, this, &connection, socket);
}

void Server::Read(Connection* connection, SOCKET socket) {
  std::vector<uint8_t> payload;
  mp::PacketHeader header;
  while (RecvExact(socket, &header, sizeof header, 0) &&
         header.size <= kMaxPayload) {
    payload.resize(header.size);
    if (header.size && !RecvExact(socket, payload.data(), header.size, 0)) {
      break;
    }
    int64_t now = NowUs();
    switch (mp::Packet(header.type)) {
      case mp::Packet::kPong: {
        // aux = viewer clock at the ping, pts = phone clock when the app
        // read it: assume the read happened halfway through the round trip.
        int64_t rtt = now - header.aux_us;
        std::lock_guard lock(clock_mutex_);
        samples_[sample_count_++ % samples_.size()] = {
            rtt, header.pts_us - (header.aux_us + rtt / 2)};
        continue;
      }
      case mp::Packet::kConfig:
      case mp::Packet::kVideo:
        video_bytes_ += header.size;
        break;
      default:
        break;
    }
    callbacks_.on_packet(header, payload, now);
  }
  // Free the socket now, not at the next connect of this stream.
  std::lock_guard lock(connection->send_mutex);
  if (connection->socket == socket) {
    closesocket(socket);
    connection->socket = INVALID_SOCKET;
  }
  connection->alive = false;
}

void Server::Close(Connection* connection) {
  {
    std::lock_guard lock(connection->send_mutex);
    if (connection->socket != INVALID_SOCKET) {
      // closesocket() fails the reader's blocking recv() at once, whatever
      // the peer does; the reader holds its own copy of the handle.
      shutdown(connection->socket, SD_BOTH);
      closesocket(connection->socket);
      connection->socket = INVALID_SOCKET;
    }
    connection->alive = false;
  }
  // The reader may have closed its own socket and exited already.
  if (connection->reader.joinable()) {
    connection->reader.join();
  }
}

Server::Callbacks Route(ID3D11Device* device, Decoder* decoder, AudioOut* audio,
                        bool null_sink) {
  // Written on the accept thread before the audio reader starts.
  auto channels = std::make_shared<uint32_t>(0);
  // LOCALAPPDATA/XDroid360/viewer-audio.csv: a row per audio packet of the
  // last stream, the arrival pattern the jitter buffer has to ride out.
  auto trace = std::make_shared<std::unique_ptr<FILE, int (*)(FILE*)>>(
      nullptr, std::fclose);
  Server::Callbacks callbacks;
  callbacks.on_hello = [=](const mp::ClientHello& hello) {
    if (hello.stream == uint8_t(mp::Stream::kVideo)) {
      if (!decoder->Start(device, mp::Codec(hello.codec), hello.width,
                          hello.height)) {
        std::fprintf(stderr, "xv: no decoder for codec %u\n", hello.codec);
      }
      return;
    }
    audio->Stop();
    *channels = audio->Start(hello.audio_rate, hello.audio_channels, null_sink)
                    ? hello.audio_channels
                    : 0;
    if (const wchar_t* base = null_sink ? nullptr : _wgetenv(L"LOCALAPPDATA")) {
      std::wstring dir = std::wstring(base) + L"\\XDroid360";
      CreateDirectoryW(dir.c_str(), nullptr);
      trace->reset(_wfopen((dir + L"\\viewer-audio.csv").c_str(), L"w"));
      if (*trace) {
        std::fputs("pts_us,aux_us,receipt_us,frames,flags\n", trace->get());
      }
    }
  };
  callbacks.on_packet = [=](const mp::PacketHeader& header,
                            std::span<const uint8_t> payload,
                            int64_t receipt_us) {
    switch (mp::Packet(header.type)) {
      case mp::Packet::kConfig:
        decoder->SetConfig(payload);
        break;
      case mp::Packet::kVideo:
        decoder->Decode(payload, header.flags & mp::kFlagKey,
                        {header.pts_us, header.aux_us, receipt_us, 0});
        break;
      case mp::Packet::kAudio:
        if (*channels) {
          const uint32_t frames = uint32_t(payload.size() / (2 * *channels));
          audio->Push(reinterpret_cast<const int16_t*>(payload.data()), frames,
                      receipt_us, header.flags & mp::kFlagLoss);
          if (*trace) {
            std::fprintf(trace->get(), "%lld,%lld,%lld,%u,%u\n",
                         (long long)header.pts_us, (long long)header.aux_us,
                         (long long)receipt_us, frames, header.flags);
          }
        }
        break;
      default:
        break;
    }
  };
  return callbacks;
}

}  // namespace xv
