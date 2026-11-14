#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.hpp"
#include "scheduler_ordering.hpp"
#include "simulation.hpp"

namespace {

void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct ProbeEvent {
  axissim::detail::SchedulerEventKey key;
  int payload = 0;
};

struct ProbeOrder {
  bool deterministic_v1 = true;
  bool operator()(const ProbeEvent& left, const ProbeEvent& right) const {
    return axissim::detail::scheduler_event_later(left.key, right.key, deterministic_v1);
  }
};

std::vector<int> drain(std::vector<ProbeEvent> insertion_order) {
  std::priority_queue<ProbeEvent, std::vector<ProbeEvent>, ProbeOrder> queue(ProbeOrder{true});
  for (auto& event : insertion_order) queue.push(std::move(event));
  std::vector<int> popped;
  while (!queue.empty()) {
    popped.push_back(queue.top().payload);
    queue.pop();
  }
  return popped;
}

void test_queue_key_order_and_container_independence() {
  const std::vector<ProbeEvent> events = {
      {{2.0, 4}, 4}, {{1.0, 8}, 8}, {{1.0, 2}, 2}, {{1.0, 5}, 5}, {{3.0, 1}, 1}};
  std::vector<ProbeEvent> reverse = events;
  std::reverse(reverse.begin(), reverse.end());
  const std::vector<int> expected = {2, 5, 8, 4, 1};
  check(drain(events) == expected, "equal-time events must follow their preassigned stable sequence");
  check(drain(reverse) == expected, "queue insertion order must not change deterministic event order");

  std::priority_queue<ProbeEvent, std::vector<ProbeEvent>, ProbeOrder> empty(ProbeOrder{true});
  check(empty.empty(), "an empty event queue must remain empty");
  check(axissim::detail::scheduler_event_later({1.0, 0}, {1.0, 1}, false) == false
        && axissim::detail::scheduler_event_later({1.0, 1}, {1.0, 0}, false) == false,
        "legacy ordering must leave finite equal-time keys equivalent");
  std::uint64_t next_sequence = 0;
  check(axissim::detail::take_scheduler_sequence(next_sequence) == 0
        && axissim::detail::take_scheduler_sequence(next_sequence) == 1
        && next_sequence == 2, "scheduler sequence IDs must be monotonic and zero-based");
  next_sequence = std::numeric_limits<std::uint64_t>::max() - 1;
  check(axissim::detail::take_scheduler_sequence(next_sequence) == std::numeric_limits<std::uint64_t>::max() - 1,
        "last representable sequence value before overflow must be usable");
  bool exhausted = false;
  try { (void)axissim::detail::take_scheduler_sequence(next_sequence); }
  catch (const std::overflow_error&) { exhausted = true; }
  check(exhausted, "sequence exhaustion must fail explicitly rather than wrap");

  const std::vector<axissim::detail::SchedulerEventKey> keys = {
      {-1.0, 2}, {-0.0, 1}, {0.0, 3}, {1.0, 4},
      {std::numeric_limits<double>::infinity(), 5},
      {std::numeric_limits<double>::quiet_NaN(), 6},
      {std::numeric_limits<double>::quiet_NaN(), 7}};
  for (const bool stable : {false, true}) {
    const auto later = [stable](const auto& a, const auto& b) {
      return axissim::detail::scheduler_event_later(a, b, stable);
    };
    for (const auto& a : keys) {
      check(!later(a, a), "event comparator must be irreflexive");
      for (const auto& b : keys) {
        check(!(later(a, b) && later(b, a)), "event comparator must be asymmetric");
        for (const auto& c : keys) {
          if (later(a, b) && later(b, c)) check(later(a, c), "event comparator must be transitive");
          const bool ab_equivalent = !later(a, b) && !later(b, a);
          const bool bc_equivalent = !later(b, c) && !later(c, b);
          if (ab_equivalent && bc_equivalent) {
            check(!later(a, c) && !later(c, a), "event comparator equivalence must be transitive");
          }
        }
      }
    }
  }
}

bool same_trace(const std::vector<axissim::TraceEvent>& left, const std::vector<axissim::TraceEvent>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].frame != right[index].frame || left[index].task != right[index].task
        || left[index].resource != right[index].resource || left[index].start_us != right[index].start_us
        || left[index].end_us != right[index].end_us) return false;
  }
  return true;
}

void test_profile_scheduler_ties_dependencies_and_repeatability() {
  const std::string config_path = std::string(AXISSIM_SOURCE_DIR) + "/tests/scheduler_tie_profile.conf";
  auto config = axissim::load_config(config_path, { {"scheduler_ordering_policy", "deterministic_v1"} });
  axissim::validate(config);
  axissim::Simulator first(config);
  axissim::Simulator second(config);
  first.run();
  second.run();

  const std::vector<std::string> expected = {
      "profile_rvv:root_rvv", "profile_npu:root_npu",
      "profile_rvv:root_rvv", "profile_npu:root_npu",
      "profile_dma:child_rvv", "profile_dma:child_npu", "profile_npu:join", "profile_rvv:child_rvv",
      "profile_dma:child_rvv", "profile_npu:join", "profile_dma:child_npu", "profile_npu:child_npu",
      "profile_rvv:child_rvv", "profile_npu:child_npu"};
  check(first.trace().size() == expected.size(), "two-frame tie fixture must execute every declared task");
  check(same_trace(first.trace(), second.trace()), "repeated deterministic runs must produce identical traces");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    check(first.trace()[index].task == expected[index], "deterministic task dispatch order changed at trace index " + std::to_string(index));
  }
  check(first.latencies_us() == second.latencies_us() && first.makespan_us() == second.makespan_us(),
        "repeated deterministic runs must preserve timing results");
  check(first.trace()[0].start_us == 0.0 && first.trace()[2].start_us == config.frame_period_us,
        "frame release timestamps must stay ordered across equal-time completion boundaries");

  std::map<std::pair<int, std::string>, double> start;
  std::map<std::pair<int, std::string>, double> completion;
  for (const auto& event : first.trace()) {
    start[{event.frame, event.task}] = event.start_us;
    completion[{event.frame, event.task}] = event.end_us;
  }
  for (const auto frame : {0, 1}) {
    check(start[{frame, "profile_npu:join"}] >= completion[{frame, "profile_npu:root_npu"}],
          "join task must not start before its NPU dependency completes");
    check(start[{frame, "profile_npu:join"}] >= completion[{frame, "profile_rvv:root_rvv"}],
          "join task must not start before its RVV dependency completes");
  }
  check(first.trace()[4].resource == "DMA+DRAM" && first.trace()[5].resource == "DMA+DRAM",
        "same-priority equal-ready transfer requests must share the modeled DMA resource");
}

void test_runtime_scheduler_deterministic_repeated_runs() {
  const std::string config_path = std::string(AXISSIM_SOURCE_DIR) + "/tests/tiny_pipeline.conf";
  auto config = axissim::load_config(config_path, {
      {"scheduler_ordering_policy", "deterministic_v1"}, {"frames", "4"},
      {"frame_period_us", "0.1"}, {"control_period_us", "0.1"},
      {"control_deadline_us", "0.1"}, {"control_ops", "1"}});
  axissim::validate(config);
  axissim::Simulator first(config);
  axissim::Simulator second(config);
  first.run();
  second.run();
  check(same_trace(first.trace(), second.trace()), "runtime scheduler sequence IDs must restart safely per simulation");
  check(first.latencies_us() == second.latencies_us() && first.makespan_us() == second.makespan_us(),
        "runtime scheduler results must be repeatable under simultaneous frame/control releases");
}

void test_existing_resource_arbitration_precedes_stable_ties() {
  const std::string config_path = std::string(AXISSIM_SOURCE_DIR) + "/tests/tiny_pipeline.conf";
  const auto run_policy = [&config_path](const std::string& scheduler_policy, const std::string& frame_deadline) {
    auto config = axissim::load_config(config_path, {
        {"workload_mode", "rvv_stream"}, {"scheduler_ordering_policy", "deterministic_v1"},
        {"scheduler_policy", scheduler_policy}, {"frames", "1"}, {"frame_period_us", "0.1"},
        {"preprocess_elements", "1"}, {"control_period_us", "0.1"}, {"control_deadline_us", "1"},
        {"deadline_us", frame_deadline}, {"control_ops", "1"}});
    axissim::validate(config);
    axissim::Simulator simulator(config);
    simulator.run();
    check(simulator.trace().size() == 2, "the two simultaneously ready RVV and control tasks must execute once each");
    check(simulator.trace()[0].start_us == 0.0,
          "the first resource arbitration decision must occur at the shared ready timestamp");
    return simulator.trace()[0].task;
  };

  check(run_policy("fixed_priority", "10") == "safety_control",
        "fixed-priority resource arbitration must precede enqueue-sequence tie-breaking");
  check(run_policy("fcfs", "10") == "rvv_stream_kernel",
        "FCFS equal-ready arbitration must follow the earlier stable enqueue sequence");
  check(run_policy("edf", "0.1") == "rvv_stream_kernel",
        "EDF deadline order must precede enqueue-sequence tie-breaking");
}

}  // namespace

int main() {
  try {
    test_queue_key_order_and_container_independence();
    test_profile_scheduler_ties_dependencies_and_repeatability();
    test_runtime_scheduler_deterministic_repeated_runs();
    test_existing_resource_arbitration_precedes_stable_ties();
    std::cout << "PASS deterministic scheduler ordering contracts\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL deterministic scheduler ordering contracts: " << error.what() << '\n';
    return 1;
  }
}
