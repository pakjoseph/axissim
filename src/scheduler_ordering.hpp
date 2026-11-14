#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace axissim::detail {

struct SchedulerEventKey {
  double time_us = 0.0;
  std::uint64_t sequence = 0;
};

[[nodiscard]] inline std::uint64_t take_scheduler_sequence(std::uint64_t& next_sequence) {
  if (next_sequence == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("scheduler sequence identifier exhausted");
  }
  return next_sequence++;
}

// priority_queue uses this as a "later than" comparator so its top is the
// earliest event. NaNs are ordered after finite values and treated as one
// timestamp class; valid simulation inputs should not generate them, but the
// comparator remains a strict weak ordering even for malformed event keys.
[[nodiscard]] inline bool scheduler_event_later(const SchedulerEventKey& left,
                                                const SchedulerEventKey& right,
                                                bool deterministic_v1) noexcept {
  const bool left_nan = std::isnan(left.time_us);
  const bool right_nan = std::isnan(right.time_us);
  if (left_nan != right_nan) return left_nan;
  if (!left_nan) {
    if (left.time_us > right.time_us) return true;
    if (left.time_us < right.time_us) return false;
  }
  return deterministic_v1 && left.sequence > right.sequence;
}

[[nodiscard]] inline bool scheduler_time_precedes(double left, double right) noexcept {
  return scheduler_event_later({right, 0}, {left, 0}, false);
}

}  // namespace axissim::detail
