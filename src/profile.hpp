#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace axissim {

struct ProfileOperator {
  std::string id;
  std::string mapping_class;
  std::vector<std::string> dependencies;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::uint64_t macs = 0;
  std::uint64_t vector_elements = 0;
  int ops_per_element = 0;
  std::uint64_t activation_read_bytes = 0;
  std::uint64_t activation_write_bytes = 0;
  std::uint64_t weight_bytes = 0;
  std::uint64_t transfer_bytes = 0;
  bool persistent_weights = false;
  double npu_arithmetic_energy_scale = 1.0;
};

struct ProfileTransferLedgerEntry {
  std::string operator_id;
  std::string category;
  std::uint64_t bytes = 0;
};

struct WorkloadProfile {
  std::string source_url;
  std::string license;
  std::string sha256;
  std::string declared_storage_precision;
  std::string mapping_plan_id = "implicit-profile-mapping";
  std::string mapping_rule_version = "implicit";
  std::uint64_t fused_intermediate_bytes_avoided = 0;
  std::uint64_t persistent_weight_bytes_avoided_per_steady_frame = 0;
  std::uint64_t precision_traffic_bytes_avoided_per_frame = 0;
  int fusion_count = 0;
  int persistent_weight_operator_count = 0;
  std::string source_storage_precision = "profile-native";
  std::string target_storage_precision = "profile-native";
  double mapped_npu_arithmetic_energy_scale = 1.0;
  std::string accuracy_evidence_status = "not_supplied";
  std::string accuracy_metric;
  std::string accuracy_dataset;
  std::string accuracy_evaluator;
  std::string accuracy_source_url;
  double accuracy_value = 0.0;
  std::vector<ProfileOperator> operators;
};

struct MappingPlanEntry {
  std::string operator_id;
  std::string mapping_class;
  std::string weight_residency = "streamed";
};

struct MappingFusion {
  std::string producer_id;
  std::string consumer_id;
  std::string tensor;
};

struct MappingPlan {
  std::string id;
  std::string source_model_sha256;
  std::string mapping_rule_version;
  std::vector<MappingPlanEntry> entries;
  std::vector<MappingFusion> fusions;
  std::string source_storage_precision;
  std::string target_storage_precision;
  double npu_arithmetic_energy_scale = 1.0;
  std::string accuracy_evidence_path;
};

WorkloadProfile load_workload_profile(const std::string& path);
MappingPlan load_mapping_plan(const std::string& path, const WorkloadProfile& profile);
void apply_mapping_plan(WorkloadProfile& profile, const MappingPlan& plan, const Config& config);

}  // namespace axissim
