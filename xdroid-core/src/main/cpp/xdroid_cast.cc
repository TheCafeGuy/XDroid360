#include "xdroid_cast.h"

#include <arm_neon.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include "xenia/base/logging.h"

namespace xdroid {
namespace {

constexpr size_t kMaxPayload = 1200;
constexpr size_t kHeader = 12 + 7;  // RTP, then Cast with a reference id.

int64_t NowUs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

void Put16(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void Put32(uint8_t* p, uint32_t v) {
  Put16(p, v >> 16);
  Put16(p + 2, v);
}
uint32_t Get16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t Get32(const uint8_t* p) { return Get16(p) << 16 | Get16(p + 2); }

// AES-128 with the ARMv8 crypto instructions (every Snapdragon since the
// 810). The key schedule's SubWord uses AESE on a word copied to all four
// columns, where ShiftRows changes nothing.
__attribute__((target("aes"))) void ExpandKey(const uint8_t* key,
                                              uint8_t* round_keys) {
  static constexpr uint8_t kRcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 27, 54};
  uint32_t w[44];
  std::memcpy(w, key, 16);
  for (int i = 4; i < 44; ++i) {
    uint32_t t = w[i - 1];
    if (i % 4 == 0) {
      t = vgetq_lane_u32(vreinterpretq_u32_u8(vaeseq_u8(
                             vreinterpretq_u8_u32(vdupq_n_u32(t)),
                             vdupq_n_u8(0))),
                         0);
      t = (t >> 8 | t << 24) ^ kRcon[i / 4 - 1];
    }
    w[i] = w[i - 4] ^ t;
  }
  std::memcpy(round_keys, w, sizeof(w));
}

// CTR over |size| bytes: nonce = IV mask with the frame id XORed into bytes
// 8..11 (big-endian), counter = nonce + block index (128-bit big-endian).
__attribute__((target("aes"))) void Ctr(const uint8_t* round_keys,
                                        const uint8_t* iv_mask,
                                        uint32_t frame_id, const uint8_t* in,
                                        uint8_t* out, size_t size) {
  uint8x16_t rk[11];
  for (int r = 0; r < 11; ++r) {
    rk[r] = vld1q_u8(round_keys + 16 * r);
  }
  uint8_t nonce[16];
  std::memcpy(nonce, iv_mask, 16);
  for (int i = 0; i < 4; ++i) {
    nonce[8 + i] ^= uint8_t(frame_id >> (24 - 8 * i));
  }
  uint64_t hi = 0, lo = 0;
  for (int i = 0; i < 8; ++i) {
    hi = hi << 8 | nonce[i];
    lo = lo << 8 | nonce[8 + i];
  }
  for (size_t at = 0; at < size; at += 16) {
    uint8_t block[16];
    for (int i = 0; i < 8; ++i) {
      block[i] = uint8_t(hi >> (56 - 8 * i));
      block[8 + i] = uint8_t(lo >> (56 - 8 * i));
    }
    uint8x16_t s = vld1q_u8(block);
    for (int r = 0; r < 9; ++r) {
      s = vaesmcq_u8(vaeseq_u8(s, rk[r]));
    }
    s = veorq_u8(vaeseq_u8(s, rk[9]), rk[10]);
    vst1q_u8(block, s);
    const size_t n = std::min<size_t>(16, size - at);
    for (size_t i = 0; i < n; ++i) {
      out[at + i] = in[at + i] ^ block[i];
    }
    if (!++lo) {
      ++hi;
    }
  }
}

}  // namespace

CastSender::~CastSender() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool CastSender::Open(std::string_view host_port, const uint8_t* key_iv) {
  const size_t colon = host_port.rfind(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  const std::string host(host_port.substr(0, colon));
  const int port = std::atoi(std::string(host_port.substr(colon + 1)).c_str());
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(uint16_t(port));
  if (port <= 0 || port > 65535 ||
      inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    return false;
  }
  fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd_ < 0) {
    return false;
  }
  const int buffer = 1 << 20;
  setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
  const int tos = 0xB8;  // DSCP EF: Wi-Fi WMM voice/video queue.
  setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  if (connect(fd_, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address))) {
    return false;
  }
  ExpandKey(key_iv, round_keys_.data());
  for (Stream& s : streams_) {
    std::memcpy(s.iv_mask.data(), key_iv + 16, 16);
  }
  streams_[kAudio].iv_mask[0] ^= 0x80;
  report_us_ = log_us_ = NowUs();
  return true;
}

bool CastSender::Send(StreamId stream, const uint8_t* data, size_t size,
                      bool key, bool config, int64_t pts_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  Stream& s = streams_[stream];
  if (config) {
    s.config.assign(data, data + size);
    return true;
  }
  const size_t extra = key ? s.config.size() : 0;
  const size_t total = extra + size;
  const uint32_t id = s.next_id++;
  // Plain frame into cipher, then encrypted in place.
  s.cipher.resize(total);
  std::memcpy(s.cipher.data(), s.config.data(), extra);
  std::memcpy(s.cipher.data() + extra, data, size);
  Ctr(round_keys_.data(), s.iv_mask.data(), id, s.cipher.data(),
      s.cipher.data(), total);
  Frame& frame = s.frames[id % s.frames.size()];
  frame.id = id;
  frame.sent_us = NowUs();
  const size_t count =
      std::max<size_t>(1, (total + kMaxPayload - 1) / kMaxPayload);
  frame.bytes.resize(count * kHeader + total);
  frame.lengths.resize(count);
  // The TV plays a frame at its RTP time + targetDelay. Stamped with the
  // capture time, encoder time (video ~14 ms, audio ~30 ms with the 20 ms
  // Opus frame) ate the 20 ms delay: frames came late, the TV dropped them
  // (no sound, stutter). Shifted by the recent send lag they count from
  // the send; the shift moves slowly, so each stream's clock stays smooth.
  const int64_t lag = frame.sent_us - pts_us;
  s.lag_us = s.lag_us == INT64_MIN || lag > s.lag_us
                 ? lag
                 : s.lag_us - std::min<int64_t>(s.lag_us - lag, 100);
  const uint32_t rtp =
      uint32_t(uint64_t(pts_us + s.lag_us) * s.rtp_hz / 1000000);
  uint8_t* p = frame.bytes.data();
  bool ok = true;
  for (size_t i = 0; i < count; ++i) {
    const size_t at = i * kMaxPayload;
    const size_t n = std::min(kMaxPayload, total - std::min(total, at));
    p[0] = 0x80;
    p[1] = uint8_t((i + 1 == count ? 0x80 : 0) | s.payload_type);
    Put16(p + 2, s.sequence++);
    Put32(p + 4, rtp);
    Put32(p + 8, s.ssrc);
    p[12] = uint8_t((key ? 0x80 : 0) | 0x40);  // Key, has a reference id.
    p[13] = uint8_t(id);
    Put16(p + 14, uint32_t(i));
    Put16(p + 16, uint32_t(count - 1));
    p[18] = uint8_t(key ? id : id - 1);
    std::memcpy(p + kHeader, s.cipher.data() + at, n);
    frame.lengths[i] = uint16_t(kHeader + n);
    // A full socket buffer drops the packet; the receiver NACKs it.
    ok &= send(fd_, p, kHeader + n, 0) >= 0 || errno == EAGAIN ||
          errno == ENOBUFS;
    p += kHeader + n;
    ++s.packets;
    s.octets += uint32_t(n);
  }
  return ok;
}

void CastSender::Resend(Stream& s, uint32_t frame_id, uint16_t packet_id,
                        uint8_t mask) {
  Frame& frame = s.frames[frame_id % s.frames.size()];
  if (frame.id != frame_id) {
    return;
  }
  const uint8_t* p = frame.bytes.data();
  for (size_t i = 0; i < frame.lengths.size(); ++i) {
    const bool want = packet_id == 0xFFFF || i == packet_id ||
                      (i > packet_id && i <= packet_id + 8u &&
                       (mask >> (i - packet_id - 1) & 1));
    if (want) {
      send(fd_, p, frame.lengths[i], 0);
      ++resent_;
    }
    p += frame.lengths[i];
  }
}

bool CastSender::Poll(bool& want_key) {
  uint8_t packet[1500];
  for (;;) {
    const ssize_t got = recv(fd_, packet, sizeof(packet), 0);
    if (got < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        break;
      }
      return errno == ECONNREFUSED ? false : true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i + 4 <= size_t(got);) {
      const uint8_t* body = packet + i;
      const size_t length = (Get16(body + 2) + 1) * 4;
      if (i + length > size_t(got)) {
        break;
      }
      const uint8_t type = body[1], format = body[0] & 0x1F;
      // Feedback names the media ssrc it is about.
      Stream* s = nullptr;
      for (Stream& c : streams_) {
        if (length >= 12 && Get32(body + 8) == c.ssrc) {
          s = &c;
        }
      }
      if (s == &streams_[kVideo] && type == 206 && format == 1) {
        want_key = true;
        ++plis_;
      } else if (s && type == 206 && format == 15 && length >= 20 &&
                 !std::memcmp(body + 12, "CAST", 4) && s->next_id) {
        // 8-bit frame ids to the newest sent id with that low byte.
        const auto expand = [s](uint8_t low) {
          return s->next_id - 1 - uint8_t(uint8_t(s->next_id - 1) - low);
        };
        const uint32_t ack = expand(body[16]);
        const int64_t now_us = NowUs();
        for (uint32_t id = s->acked + 1;
             s->acked != ack && int32_t(ack - id) >= 0; ++id) {
          const Frame& frame = s->frames[id % s->frames.size()];
          if (frame.id == id) {
            const uint64_t us = uint64_t(now_us - frame.sent_us);
            s->ack_us_sum += us;
            s->ack_us_max = std::max(s->ack_us_max, us);
            ++s->acks;
          }
        }
        if (s->acked == UINT32_MAX || int32_t(ack - s->acked) > 0) {
          s->acked = ack;
        }
        const size_t losses = std::min<size_t>(body[17], (length - 20) / 4);
        for (size_t l = 0; l < losses; ++l) {
          const uint8_t* loss = body + 20 + 4 * l;
          Resend(*s, expand(loss[0]), uint16_t(Get16(loss + 1)), loss[3]);
        }
      }
      i += length;
    }
  }
  const int64_t now_us = NowUs();
  if (now_us - report_us_ < 100000) {
    return true;
  }
  // Sender reports: map RTP time (the stream's clock over the monotonic
  // clock) to NTP time of the same clock; the receiver measures its offset.
  report_us_ = now_us;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Stream& s : streams_) {
    if (!s.next_id) {
      continue;
    }
    uint8_t sr[28];
    sr[0] = 0x80;
    sr[1] = 200;
    Put16(sr + 2, 6);
    Put32(sr + 4, s.ssrc);
    Put32(sr + 8, uint32_t(now_us / 1000000 + 2208988800u));
    Put32(sr + 12, uint32_t((uint64_t(now_us % 1000000) << 32) / 1000000));
    Put32(sr + 16, uint32_t(uint64_t(now_us) * s.rtp_hz / 1000000));
    Put32(sr + 20, s.packets);
    Put32(sr + 24, s.octets);
    send(fd_, sr, sizeof(sr), 0);
  }
  if (now_us - log_us_ >= 10000000) {
    const auto avg_ms = [](const Stream& s) {
      return s.acks ? double(s.ack_us_sum) / 1000.0 / double(s.acks) : 0.0;
    };
    const Stream &v = streams_[kVideo], &a = streams_[kAudio];
    XELOGI(
        "XDROID-CAST: 10 s: video {} sent, {} acked, ack {:.1f} ms avg "
        "{:.1f} max, capture-to-send {:.1f} ms; audio {} sent, {} acked, ack "
        "{:.1f} ms avg, capture-to-send {:.1f} ms; {} packets resent, {} key "
        "requests",
        v.next_id, v.acks, avg_ms(v), double(v.ack_us_max) / 1000.0,
        double(v.lag_us) / 1000.0, a.next_id, a.acks, avg_ms(a),
        double(a.lag_us) / 1000.0, resent_, plis_);
    log_us_ = now_us;
    for (Stream& s : streams_) {
      s.ack_us_sum = s.acks = s.ack_us_max = 0;
    }
  }
  return true;
}

CastOpus::~CastOpus() {
  if (codec_) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
  }
}

bool CastOpus::Open(int32_t kbps) {
  codec_ = AMediaCodec_createEncoderByType("audio/opus");
  if (!codec_) {
    return false;
  }
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/opus");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, 48000);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, kbps * 1000);
  AMediaFormat_setInt32(format, "pcm-encoding", 2);  // 16-bit.
  const bool ok =
      AMediaCodec_configure(codec_, format, nullptr, nullptr,
                            AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
      AMediaCodec_start(codec_) == AMEDIA_OK;
  AMediaFormat_delete(format);
  if (!ok) {
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }
  return ok;
}

void CastOpus::Push(const int16_t* pcm, size_t pairs, int64_t stamp_us) {
  constexpr size_t kFrame = 960 * 2, kKeep = 5 * kFrame;  // 20 ms, 100 ms.
  ++slots_;
  // A sample clock, not the slots' arrival stamps: the guest hands slots
  // over in bursts, and re-stamping every 80 ms made the RTP times jump,
  // which the TV played as choppy sound. Re-anchored only after a real gap.
  if (pending_.empty() && std::abs(stamp_us - pending_us_) > 100000) {
    pending_us_ = stamp_us;
  }
  pending_.insert(pending_.end(), pcm, pcm + pairs * 2);
  size_t at = 0;
  while (pending_.size() - at >= kFrame) {
    const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (index < 0) {
      break;
    }
    size_t capacity = 0;
    uint8_t* buffer =
        AMediaCodec_getInputBuffer(codec_, size_t(index), &capacity);
    const size_t bytes = std::min(capacity, kFrame * 2);
    std::memcpy(buffer, pending_.data() + at, bytes);
    AMediaCodec_queueInputBuffer(codec_, size_t(index), 0, bytes,
                                 uint64_t(pending_us_), 0);
    at += kFrame;
    pending_us_ += 20000;
  }
  if (pending_.size() - at > kKeep) {
    const size_t drop = (pending_.size() - at - kKeep + 1) / 2 * 2;
    full_ += drop / 2;
    at += drop;
    pending_us_ += int64_t(drop / 2) * 1000000 / 48000;
  }
  pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(at));
}

void CastOpus::Drain(CastSender& sender) {
  AMediaCodecBufferInfo info;
  for (;;) {
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
        index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    if (index < 0) {
      break;
    }
    size_t capacity = 0;
    const uint8_t* data =
        AMediaCodec_getOutputBuffer(codec_, size_t(index), &capacity);
    // Config buffers (OpusHead, delays) are for containers, not the wire.
    if (data && info.size > 0 && !(info.flags & 2)) {
      sender.Send(CastSender::kAudio, data + info.offset, size_t(info.size),
                  true, false, info.presentationTimeUs);
      ++frames_;
    }
    AMediaCodec_releaseOutputBuffer(codec_, size_t(index), false);
  }
  const int64_t now_us = NowUs();
  if (!log_us_) {
    log_us_ = now_us;
  } else if (now_us - log_us_ >= 10000000) {
    // 1875 slots (256 samples) and 500 frames (20 ms) per 10 s at 48 kHz.
    XELOGI("XDROID-CAST audio: 10 s: {} slots in, {} samples dropped "
           "(encoder full), {} Opus frames",
           slots_, full_, frames_);
    slots_ = full_ = frames_ = 0;
    log_us_ = now_us;
  }
}

}  // namespace xdroid
