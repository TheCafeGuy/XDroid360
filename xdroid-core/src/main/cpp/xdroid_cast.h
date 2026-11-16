// TV-2: Cast Streaming sender for the TV (Cast built-in mirroring receiver
// 0F5096E8): the mirror encoder's H.264 frames and the game audio as Opus,
// Cast RTP over one UDP socket, AES-128-CTR per frame, RTCP sender reports,
// NACK resends and PLI key requests. The session itself (CastV2 LAUNCH,
// OFFER/ANSWER) is set up outside; this side only needs the receiver's
// ip:port and the AES key and IV mask of the OFFER (audio: the same key, the
// IV mask with byte 0 flipped by 0x80, so the two streams never share a
// keystream). Wire format ported from openscreen cast/streaming (BSD):
// rtp_packetizer.cc, frame_crypto.cc, compound_rtcp_parser.cc.
#pragma once

#include <array>
#include <cstddef>
#include <climits>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

struct AMediaCodec;

namespace xdroid {

class CastSender {
 public:
  enum StreamId { kVideo, kAudio };
  ~CastSender();
  // |key_iv|: AES key then IV mask. False if |host_port| is not ip:port.
  bool Open(std::string_view host_port, const uint8_t* key_iv);
  // One encoded frame (video: Annex B; codec config is kept and sent ahead
  // of every key frame). Video: Mirror Out thread; audio: Mirror Net.
  bool Send(StreamId stream, const uint8_t* data, size_t size, bool key,
            bool config, int64_t pts_us);
  // Reads RTCP (ACK, NACK, PLI), resends lost packets and sends sender
  // reports every 100 ms. Sets |want_key| on a PLI. Mirror Net thread only.
  bool Poll(bool& want_key);
  int fd() const { return fd_; }

 private:
  struct Frame {
    uint32_t id = UINT32_MAX;
    int64_t sent_us = 0;
    std::vector<uint8_t> bytes;     // Packets back to back.
    std::vector<uint16_t> lengths;  // One per packet.
  };
  struct Stream {
    uint32_t ssrc;
    uint8_t payload_type;
    uint32_t rtp_hz;
    std::array<uint8_t, 16> iv_mask{};
    std::vector<uint8_t> config, cipher;
    std::array<Frame, 64> frames;
    uint32_t next_id = 0, acked = UINT32_MAX;
    uint16_t sequence = 0;
    uint32_t packets = 0, octets = 0;
    uint64_t ack_us_sum = 0, acks = 0, ack_us_max = 0;
    // Send time minus capture time, the largest of late (decays 0.1 ms a
    // frame): added to the RTP time so the TV's playout delay only has to
    // cover the network, not the encoder (see Send).
    int64_t lag_us = INT64_MIN;
  };
  void Resend(Stream& s, uint32_t frame_id, uint16_t packet_id, uint8_t mask);

  int fd_ = -1;
  std::array<uint8_t, 176> round_keys_{};
  std::mutex mutex_;  // streams_ and the counters below.
  Stream streams_[2] = {{11111, 96, 90000}, {22222, 127, 48000}};
  int64_t report_us_ = 0, log_us_ = 0;
  uint64_t resent_ = 0, plis_ = 0;
};

// Game audio (16-bit stereo, 48 kHz) to Opus frames for CastSender, with the
// system encoder (c2.android.opus.encoder, 20 ms frames). Mirror Net only.
class CastOpus {
 public:
  ~CastOpus();
  bool Open(int32_t kbps);
  // Queues whole 20 ms frames (one per input buffer; 5.3 ms slots filled
  // the few buffers and dropped 14%). Keeps up to 100 ms while the encoder
  // has no free buffer, then drops the oldest.
  void Push(const int16_t* pcm, size_t pairs, int64_t stamp_us);
  void Drain(CastSender& sender);

 private:
  AMediaCodec* codec_ = nullptr;
  std::vector<int16_t> pending_;  // Interleaved stereo.
  int64_t pending_us_ = 0;        // Sample clock at pending_[0].
  uint64_t slots_ = 0, full_ = 0, frames_ = 0;  // Logged every 10 s.
  int64_t log_us_ = 0;
};

}  // namespace xdroid
