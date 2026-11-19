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

struct ExpectedDispatch {
  std::string work_unit_id;
  int frame;
  std::string task_name;
  double release_us;
  double start_us;
  double end_us;
};

axissim::StudyMeasurementRequest finite_scheduler_request() {
  axissim::StudyMeasurementRequest request;
  request.mode = axissim::StudyMeasurementMode::Finite;
  request.limits.max_task_events = 10'000;
  request.limits.max_simulated_time_us = 10'000.0;
  request.limits.max_drain_simulated_us = 10'000.0;
  request.limits.max_backlog_work_units = 1'000;
  return request;
}

void assert_timeline(const axissim::Simulator& simulator,
                     const axissim::StudyMeasurementResult& result,
                     const std::vector<ExpectedDispatch>& expected,
                     const std::string& label) {
  check(simulator.trace().size() == expected.size(), label + " trace contains every expected dispatch");
  check(result.task_intervals.size() == expected.size(), label + " observer records every expected dispatch");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& want = expected[index];
    const auto& trace = simulator.trace()[index];
    const auto& task = result.task_intervals[index];
    check(trace.frame == want.frame && trace.task == want.task_name && trace.resource == "RVV"
              && trace.start_us == want.start_us && trace.end_us == want.end_us,
          label + " scheduler trace differs at dispatch " + std::to_string(index)
              + " (actual=" + trace.task + ", frame=" + std::to_string(trace.frame)
              + ", start=" + std::to_string(trace.start_us) + ", end=" + std::to_string(trace.end_us)
              + "; expected=" + want.task_name + ", frame=" + std::to_string(want.frame)
              + ", start=" + std::to_string(want.start_us) + ", end=" + std::to_string(want.end_us) + ")");
    check(task.work_unit_id == want.work_unit_id && task.task_name == want.task_name
              && task.resource == "RVV" && task.release_us == want.release_us
              && task.ready_us == want.release_us && task.start_us == want.start_us
              && task.end_us == want.end_us && task.completion_observed
              && task.completion_status == "completed",
          label + " observer identity or interval differs at dispatch " + std::to_string(index));
  }
}

ExpectedDispatch frame_dispatch(std::uint64_t id, double release_us, double start_us, double end_us) {
  return {"frame:" + std::to_string(id), static_cast<int>(id), "rvv_stream_kernel",
          release_us, start_us, end_us};
}

ExpectedDispatch control_dispatch(std::uint64_t id, double release_us, double start_us, double end_us) {
  return {"control:" + std::to_string(id), -1, "safety_control", release_us, start_us, end_us};
}

void test_control_arrives_during_nonpreemptive_rvv_job() {
  // Synthetic fixture, not a characterized control workload. With two lanes,
  // VLMAX=2, zero strip-mine overhead, and a 1 MHz modeled RVV clock:
  //   frame: ceil(8 / 2) = 4 cycles = 4 us
  //   control: ceil(2 / 2) = 1 cycle = 1 us
  // Releases are frame:0 at 0 us and controls at 0 and 2 us (the control
  // schedule is half-open at frame_period_us=4). FCFS starts frame:0 first
  // because it was enqueued before control:0. control:1 arrives at 2 us while
  // frame:0 remains active, then waits behind the same non-preemptive job.
  auto config = axissim::load_config(std::string(AXISSIM_SOURCE_DIR) + "/tests/tiny_pipeline.conf", {
      {"workload_mode", "rvv_stream"}, {"scheduler_ordering_policy", "deterministic_v1"},
      {"scheduler_policy", "fcfs"}, {"frames", "1"}, {"frame_period_us", "4"},
      {"deadline_us", "4"}, {"control_period_us", "2"}, {"control_deadline_us", "2.5"},
      {"control_ops", "2"}, {"preprocess_elements", "8"}, {"rvv_vlen_bits", "64"},
      {"rvv_sew_bits", "32"}, {"rvv_lanes", "2"}, {"rvv_ops_per_lane_cycle", "1"},
      {"rvv_frequency_ghz", "0.001"}, {"rvv_stripmine_overhead_cycles", "0"},
      {"rvv_chunk_elements", "0"}, {"dma_double_buffering", "1"}});
  axissim::validate(config);

  axissim::Simulator simulator(config);
  const auto result = simulator.run_study(finite_scheduler_request());
  check(result.execution_status == axissim::StudyExecutionStatus::Complete,
        "non-preemptive contention fixture completes");
  const std::vector<ExpectedDispatch> expected = {
      frame_dispatch(0, 0.0, 0.0, 4.0),
      control_dispatch(0, 0.0, 4.0, 5.0),
      control_dispatch(1, 2.0, 5.0, 6.0)};
  assert_timeline(simulator, result, expected, "non-preemptive contention");

  check(result.frame_cohort.mean_latency_us.value && *result.frame_cohort.mean_latency_us.value == 4.0,
        "frame latency is completion minus frame release");
  check(result.frame_cohort.deadline_misses.value && *result.frame_cohort.deadline_misses.value == 0,
        "frame completing exactly at its deadline is on time");
  check(result.control_cohort.deadline_misses.value && *result.control_cohort.deadline_misses.value == 2,
        "both control responses miss the 2.5 us deadline under non-preemptive blocking");
  check(simulator.control_response_times_us() == std::vector<double>({5.0, 4.0}),
        "control response times are 5 us and 4 us from the hand-calculated schedule");
  check(simulator.control_tasks() == 2 && simulator.control_deadline_misses() == 2
            && simulator.deadline_misses() == 0,
        "legacy scheduler counters agree with the independently derived frame and control outcomes");
}

std::vector<ExpectedDispatch> queued_policy_oracle(const std::vector<std::string>& order) {
  // Every RVV job below is 100 elements / 2 lanes = 50 cycles. At 1 MHz,
  // that is exactly 50 us. This order was derived from release/deadline rules,
  // not from a simulator trace; dispatch i therefore occupies [50*i,50*(i+1)].
  std::vector<ExpectedDispatch> expected;
  expected.reserve(order.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    const double start = static_cast<double>(index) * 50.0;
    const double end = start + 50.0;
    const auto& id = order[index];
    if (id.rfind("frame:", 0) == 0) {
      const auto frame = static_cast<std::uint64_t>(std::stoull(id.substr(6)));
      expected.push_back(frame_dispatch(frame, frame == 0 ? 0.0 : 10.0, start, end));
    } else {
      const auto control = static_cast<std::uint64_t>(std::stoull(id.substr(8)));
      expected.push_back(control_dispatch(control, static_cast<double>(control) * 2.0, start, end));
    }
  }
  return expected;
}

void test_queued_policy_arbitration_behind_running_rvv_job() {
  // Identical synthetic jobs are used for all policies. Frame releases are
  // 0 and 10 us, control releases are 0,2,...,18 us. At 50 us there are at
  // least eleven waiting RVV jobs. Deadlines are D_frame=5 us and
  // D_control=15 us. The first three choices at the 100 us boundary differ:
  // FCFS selects control:1 (earliest ready among its queue), fixed priority
  // selects control:2 (priority 0 ahead of frames), and EDF selects frame:1
  // (absolute deadline 15 us, before control:2's 19 us).
  auto config_for = [](const std::string& policy) {
    auto config = axissim::load_config(std::string(AXISSIM_SOURCE_DIR) + "/tests/tiny_pipeline.conf", {
        {"workload_mode", "rvv_stream"}, {"scheduler_ordering_policy", "deterministic_v1"},
        {"scheduler_policy", policy}, {"frames", "2"}, {"frame_period_us", "10"},
        {"deadline_us", "5"}, {"control_period_us", "2"}, {"control_deadline_us", "15"},
        {"control_ops", "100"}, {"preprocess_elements", "100"}, {"rvv_vlen_bits", "64"},
        {"rvv_sew_bits", "32"}, {"rvv_lanes", "2"}, {"rvv_ops_per_lane_cycle", "1"},
        {"rvv_frequency_ghz", "0.001"}, {"rvv_stripmine_overhead_cycles", "0"},
        {"rvv_chunk_elements", "0"}, {"dma_double_buffering", "1"}});
    axissim::validate(config);
    return config;
  };

  const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
      // At 300 us, frame:1 and control:5 are both ready from 10 us; frame:1
      // was enqueued with the frame releases before periodic control jobs.
      {"fcfs", {"frame:0", "control:0", "control:1", "control:2", "control:3", "control:4",
                "frame:1", "control:5", "control:6", "control:7", "control:8", "control:9"}},
      {"fixed_priority", {"control:0", "control:1", "control:2", "control:3", "control:4", "control:5",
                           "control:6", "control:7", "control:8", "control:9", "frame:0", "frame:1"}},
      {"edf", {"frame:0", "control:0", "frame:1", "control:1", "control:2", "control:3",
                "control:4", "control:5", "control:6", "control:7", "control:8", "control:9"}}};

  for (const auto& [policy, order] : cases) {
    const auto config = config_for(policy);
    axissim::Simulator simulator(config);
    const auto result = simulator.run_study(finite_scheduler_request());
    check(result.execution_status == axissim::StudyExecutionStatus::Complete,
          policy + " queued contention fixture completes");
    assert_timeline(simulator, result, queued_policy_oracle(order), policy + " queued contention");
    check(simulator.max_pending_tasks() >= 3,
          policy + " fixture has at least three RVV jobs waiting behind a running task");
    check(result.frame_cohort.scheduled == 2 && result.frame_cohort.completed_by_drain == 2
              && result.frame_cohort.deadline_misses.value && *result.frame_cohort.deadline_misses.value == 2,
          policy + " records two independently predicted frame deadline misses");
    check(result.control_cohort.scheduled == 10 && result.control_cohort.completed_by_drain == 10
              && result.control_cohort.deadline_misses.value && *result.control_cohort.deadline_misses.value == 10,
          policy + " records ten independently predicted control deadline misses");
    check(result.final_unfinished_work_units == 0 && result.target_cohort_complete,
          policy + " drains all finite synthetic work units");
  }
}

void test_edf_equal_absolute_deadline_ties() {
  auto config = [](const std::string& frame_deadline, const std::string& control_deadline,
                   const std::string& frame_period, const std::string& control_period,
                   const std::string& frames, const std::string& elements,
                   const std::string& control_ops) {
    auto result = axissim::load_config(std::string(AXISSIM_SOURCE_DIR) + "/tests/tiny_pipeline.conf", {
        {"workload_mode", "rvv_stream"}, {"scheduler_ordering_policy", "deterministic_v1"},
        {"scheduler_policy", "edf"}, {"frames", frames}, {"frame_period_us", frame_period},
        {"deadline_us", frame_deadline}, {"control_period_us", control_period},
        {"control_deadline_us", control_deadline}, {"control_ops", control_ops},
        {"preprocess_elements", elements}, {"rvv_vlen_bits", "64"}, {"rvv_sew_bits", "32"},
        {"rvv_lanes", "2"}, {"rvv_ops_per_lane_cycle", "1"}, {"rvv_frequency_ghz", "0.001"},
        {"rvv_stripmine_overhead_cycles", "0"}, {"rvv_chunk_elements", "0"},
        {"dma_double_buffering", "1"}});
    axissim::validate(result);
    return result;
  };

  // Equal absolute deadline and equal readiness: frame:0 and control:0 both
  // release at 0 us with relative deadline 5 us. Frames are enqueued first,
  // so deterministic_v1 resolves this exact tie to frame:0.
  const auto sequence_config = config("5", "5", "20", "20", "1", "8", "2");
  axissim::Simulator sequence_first(sequence_config);
  const auto sequence_result = sequence_first.run_study(finite_scheduler_request());
  axissim::Simulator sequence_second(sequence_config);
  const auto sequence_repeat = sequence_second.run_study(finite_scheduler_request());
  const std::vector<ExpectedDispatch> sequence_expected = {
      frame_dispatch(0, 0.0, 0.0, 4.0), control_dispatch(0, 0.0, 4.0, 5.0)};
  assert_timeline(sequence_first, sequence_result, sequence_expected, "EDF enqueue-sequence tie");
  check(same_trace(sequence_first.trace(), sequence_second.trace()),
        "equal-deadline enqueue tie has exact repeated-run trace equality");

  // Equal absolute deadlines do not require equal relative deadlines. At the
  // 100 us arbitration, control:1 has release=3 and D=9 (absolute deadline 12),
  // while frame:1 has release=4 and D=8 (absolute deadline 12). Control:1 is
  // ready earlier but frame:1 has the earlier enqueue sequence because frame
  // jobs are enqueued before control jobs. EDF must choose control:1 by ready
  // time before using sequence as the final tie-break. The initial frame:0 runs
  // 0-50 us (deadline 8 precedes control:0's deadline 9), then control:0 runs
  // 50-100 us.
  const auto readiness_config = config("8", "9", "4", "3", "2", "100", "100");
  axissim::Simulator readiness_first(readiness_config);
  const auto readiness_result = readiness_first.run_study(finite_scheduler_request());
  axissim::Simulator readiness_second(readiness_config);
  (void)readiness_second.run_study(finite_scheduler_request());
  const std::vector<ExpectedDispatch> readiness_expected = {
      frame_dispatch(0, 0.0, 0.0, 50.0), control_dispatch(0, 0.0, 50.0, 100.0),
      control_dispatch(1, 3.0, 100.0, 150.0), frame_dispatch(1, 4.0, 150.0, 200.0),
      control_dispatch(2, 6.0, 200.0, 250.0)};
  assert_timeline(readiness_first, readiness_result, readiness_expected, "EDF absolute-deadline readiness tie");
  check(same_trace(readiness_first.trace(), readiness_second.trace()),
        "equal-absolute-deadline readiness case has exact repeated-run trace equality");
}

}  // namespace

int main() {
  try {
    test_queue_key_order_and_container_independence();
    test_profile_scheduler_ties_dependencies_and_repeatability();
    test_runtime_scheduler_deterministic_repeated_runs();
    test_existing_resource_arbitration_precedes_stable_ties();
    test_control_arrives_during_nonpreemptive_rvv_job();
    test_queued_policy_arbitration_behind_running_rvv_job();
    test_edf_equal_absolute_deadline_ties();
    std::cout << "PASS deterministic scheduler ordering contracts\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL deterministic scheduler ordering contracts: " << error.what() << '\n';
    return 1;
  }
}
