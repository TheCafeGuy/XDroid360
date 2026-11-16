// MIRROR-1 viewer: picks the phone encoder rate by live trial, so no key has
// to compare modes by hand. Each arm runs one window per round. A window
// scores the mean delay of its pictures plus kWeight times (their mean cadence
// error + its mean picture interval above the lowest of the round), in ms: 1%
// of pictures off by one 60 Hz refresh, or 1% missing at 60 fps, costs 1 ms.
// An arm scores the median of its rounds; the cheapest arm for the phone
// within kMarginMs of the best one wins.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

namespace xv {

class Tuner {
 public:
  // Phone encoder operating rates, in rising phone cost.
  static constexpr int kArms[] = {30, 60, 120, 240};

  bool done() const { return window_ == kWindows; }
  int arm() const { return kArms[done() ? winner_ : window_ % kArmCount]; }
  int percent() const { return window_ * 100 / kWindows; }

  // Once per new picture on the screen, both times on the viewer clock. True
  // when arm() changed: the caller sends it to the phone.
  bool Add(int64_t pts_us, int64_t screen_us) {
    if (done()) {
      return false;
    }
    int64_t gap_us = pts_us - last_pts_us_;
    int64_t screen_gap_us = screen_us - last_screen_us_;
    last_pts_us_ = pts_us;
    last_screen_us_ = screen_us;
    // A stream start, a pause or a clock jump is no data: the window starts
    // again. Shorter stalls stay in, as the penalty they are.
    if (!start_us_ || gap_us <= 0 || gap_us > 5000000) {
      start_us_ = screen_us;
      sum_ = {};
      count_ = 0;
      return false;
    }
    int64_t elapsed_us = screen_us - start_us_;
    if (elapsed_us < kSettleUs) {
      return false;  // The encoder still changes over.
    }
    sum_.delay += std::min((screen_us - pts_us) / 1000.0, 150.0);
    sum_.error +=
        std::min(std::abs(double(screen_gap_us - gap_us)) / 1000, 50.0);
    sum_.interval += gap_us / 1000.0;
    ++count_;
    if (elapsed_us < kWindowUs) {
      return false;
    }
    windows_[window_++] = {sum_.delay / count_, sum_.error / count_,
                           sum_.interval / count_};
    start_us_ = 0;
    if (done()) {
      Pick();
    }
    return true;
  }

  // "30:61.2 60:55.0 120:48.1 240:48.0 winner 120": ms, the lowest is best.
  std::string Report() const {
    std::string text;
    char item[32];
    for (int i = 0; i < kArmCount; ++i) {
      std::snprintf(item, sizeof item, "%d:%.1f ", kArms[i], scores_[i]);
      text += item;
    }
    return text + "winner " + std::to_string(arm());
  }

 private:
  static constexpr int kArmCount = int(std::size(kArms));
  static constexpr int kRounds = 3;
  static constexpr int kWindows = kArmCount * kRounds;
  static constexpr int64_t kSettleUs = 2000000;
  static constexpr int64_t kWindowUs = 6000000;
  static constexpr double kWeight = 6;
  static constexpr double kMarginMs = 2;

  struct Window {
    double delay = 0;
    double error = 0;
    double interval = 0;
  };

  void Pick() {
    double rounds[kArmCount][kRounds];
    for (int round = 0; round < kRounds; ++round) {
      const Window* w = &windows_[round * kArmCount];
      double fastest = w[0].interval;
      for (int i = 1; i < kArmCount; ++i) {
        fastest = std::min(fastest, w[i].interval);
      }
      for (int i = 0; i < kArmCount; ++i) {
        rounds[i][round] =
            w[i].delay + kWeight * (w[i].error + w[i].interval - fastest);
      }
    }
    for (int i = 0; i < kArmCount; ++i) {
      std::sort(rounds[i], rounds[i] + kRounds);
      scores_[i] = rounds[i][kRounds / 2];
    }
    double best = *std::min_element(scores_, scores_ + kArmCount);
    while (scores_[winner_] > best + kMarginMs) {
      ++winner_;
    }
  }

  Window windows_[kWindows];
  double scores_[kArmCount] = {};
  Window sum_;  // Of the window that runs.
  int count_ = 0;
  int window_ = 0;
  int winner_ = 0;
  int64_t start_us_ = 0;
  int64_t last_pts_us_ = 0;
  int64_t last_screen_us_ = 0;
};

}  // namespace xv
