/******************************************************************************
This source code is licensed under the MIT license found in the LICENSE file in
the root directory of this source tree.
 *******************************************************************************/

#include "extern/memory_backend/analytical/AnalyticalMemory.hh"
#include <json/json.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "astra-sim/system/Common.hh"
#include "astra-sim/system/WorkloadLayerHandlerData.hh"
#include "astra-sim/system/AstraMemoryAPI.hh"
using AstraSim::MemoryLocationType;

using namespace std;
using namespace AstraSim;
using namespace Analytical;
using json = nlohmann::json;

namespace {

Callable* completion_target(WorkloadLayerHandlerData* wlhd) {
  if (wlhd->completion_target != nullptr) {
    return wlhd->completion_target;
  }
  if (wlhd->workload != nullptr) {
    return wlhd->workload;
  }
  throw invalid_argument("memory request has no completion target");
}

}  // namespace

AnalyticalMemory::AnalyticalMemory(
    string memory_configuration) {
  ifstream conf_file;

  conf_file.open(memory_configuration);
  if (!conf_file) {
    cerr << "Unable to open file: " << memory_configuration << endl;
    exit(1);
  }

  json j;
  conf_file >> j;

  if (j.contains("memory-type")) {
    string mem_type_str = j["memory-type"];
    if (mem_type_str.compare("NO_MEMORY_EXPANSION") == 0) {
      mem_type = NO_MEMORY_EXPANSION;
    } else if (mem_type_str.compare("PER_NODE_MEMORY_EXPANSION") == 0) {
      mem_type = PER_NODE_MEMORY_EXPANSION;
    } else if (mem_type_str.compare("PER_NPU_MEMORY_EXPANSION") == 0) {
      mem_type = PER_NPU_MEMORY_EXPANSION;
    } else if (mem_type_str.compare("MEMORY_POOL") == 0) {
      mem_type = MEMORY_POOL;
    } else {
      cerr << "Unsupported memory type: " << mem_type_str << endl;
      exit(1);
    }
  }

  if (j.contains("memory-location")) {
    string mem_loc_type_str = j["memory-location"];
    if (mem_loc_type_str.compare("INVALID_MEMORY") == 0) {
      std::cout << "Detected INVALID_MEMORY" << std::endl;
      mem_loc_type = MemoryLocationType::INVALID_MEMORY;
    } else if (mem_loc_type_str.compare("LOCAL_MEMORY") == 0) {
      mem_loc_type = MemoryLocationType::LOCAL_MEMORY;
    } else if (mem_loc_type_str.compare("REMOTE_MEMORY") == 0) {
      mem_loc_type = MemoryLocationType::REMOTE_MEMORY;
    } else if (mem_loc_type_str.compare("CXL_MEMORY") == 0) {
      mem_loc_type = MemoryLocationType::CXL_MEMORY;
    } else if (mem_loc_type_str.compare("STORAGE_MEMORY") == 0) {
      mem_loc_type = MemoryLocationType::STORAGE_MEMORY;
    } else {
      cerr << "Unsupported memory location type: " << mem_loc_type_str << endl;
      exit(1);
    }
  } else {
    std::cout << "No memory location type specified. Defaulting to REMOTE_MEMORY" << std::endl;
    mem_loc_type = MemoryLocationType::REMOTE_MEMORY;
  }

  mem_latency = 0;
  if (j.contains("mem-latency")) {
    mem_latency = j["mem-latency"];
  }

  const bool has_scalar_bandwidth = j.contains("mem-bw");
  const bool has_directional_bandwidth = j.contains("bandwidth-resource");
  if (has_scalar_bandwidth == has_directional_bandwidth) {
    throw invalid_argument(
        "memory configuration must declare exactly one of mem-bw or "
        "bandwidth-resource");
  }
  legacy_scalar_bandwidth = has_scalar_bandwidth;
  if (has_scalar_bandwidth) {
    if (!j["mem-bw"].is_number_unsigned() ||
        j["mem-bw"].get<uint64_t>() == 0 ||
        j["mem-bw"].get<uint64_t>() >
            numeric_limits<uint64_t>::max() / 1'000'000'000ULL) {
      throw invalid_argument("mem-bw must be a positive uint64 GB/s");
    }
    const uint64_t bytes_per_second =
        j["mem-bw"].get<uint64_t>() * 1'000'000'000ULL;
    bandwidth_resource.emplace(AstraSim::BandwidthResourceConfig{
        bytes_per_second,
        bytes_per_second,
        bytes_per_second,
        AstraSim::BandwidthConcurrency::Serialized,
        0,
    });
  } else {
    bandwidth_resource.emplace(AstraSim::parse_bandwidth_resource_config(
        j["bandwidth-resource"], "bandwidth-resource"));
  }

  num_devices = 1;
  if (j.contains("num-devices")) {
    num_devices = j["num-devices"];
  }

  pim_channels = 0; // > 1 if pim enabled
  if (j.contains("pim-channels")) {
    pim_channels = j["pim-channels"];
  }

  if (mem_type != NO_MEMORY_EXPANSION) {
    const size_t ordinary_queue_count =
        static_cast<size_t>(num_devices) * bandwidth_resource->server_count();
    ongoing_transaction.assign(ordinary_queue_count, false);
    pending_requests.resize(ordinary_queue_count);
    last_serialized_operation.resize(num_devices);
    const size_t pim_queue_count =
        static_cast<size_t>(num_devices) * pim_channels;
    pim_ongoing_transaction.assign(pim_queue_count, false);
    pim_pending_requests.resize(pim_queue_count);
  }

  conf_file.close();
}

void AnalyticalMemory::set_sys(int id, Sys* sys) {
  sys_map[id] = sys;
}

void AnalyticalMemory::issue(
    const MemoryRequest& request,
    WorkloadLayerHandlerData* wlhd) {
  if (wlhd == nullptr) {
    throw invalid_argument("memory request handler data must not be null");
  }
  if (!bandwidth_resource.has_value()) {
    throw logic_error("memory bandwidth resource is not initialized");
  }
  wlhd->memory_operation = request.operation;
  int sys_id = wlhd->sys_id;
  int device_id = wlhd->device_id;
  bool pim_enabled = wlhd->pim_enabled;
  // PIM operation
  if (pim_enabled) {
    if (pim_channels == 0) {
      cerr << "PIM operation requested but pim_channels is set to 0" << endl;
      exit(1);
    }
    int pim_channel_id = wlhd->pim_channel_id;
    int queue_idx = num_devices * device_id + pim_channel_id;
    if (pim_ongoing_transaction[queue_idx]) {
      PendingMemoryRequest pmr(request, wlhd);
      pim_pending_requests[queue_idx].push_back(pmr);
    } else {
      uint64_t load_store_time = get_mem_runtime(request);
      uint64_t runtime = wlhd->pim_runtime + load_store_time;

      Sys* sys = sys_map[sys_id];

      sys->register_event(this, EventType::General, wlhd, runtime);

      sys->register_event(
          completion_target(wlhd), EventType::General, wlhd, runtime);

      pim_ongoing_transaction[queue_idx] = true;
    }
    return;
  }
  // Ordinary memory access
  else {
    if (mem_type == NO_MEMORY_EXPANSION) {
      throw invalid_argument(
          "memory access is not supported in NO_MEMORY_EXPANSION");
    }
    if (legacy_scalar_bandwidth &&
        mem_type == PER_NPU_MEMORY_EXPANSION) {
      const uint64_t runtime = get_mem_runtime(request);
      Sys* sys = sys_map.at(sys_id);
      wlhd->memory_operation = request.operation;
      sys->register_event(
          completion_target(wlhd), EventType::General, wlhd, runtime);
      return;
    }
    const size_t request_queue = queue_index(device_id, request.operation);
    if (ongoing_transaction[request_queue]) {
      pending_requests[request_queue].emplace_back(request, wlhd);
    } else {
      start_request(request, wlhd, request_queue);
    }
  }
}

void AnalyticalMemory::call(EventType type, CallData* data) {
  WorkloadLayerHandlerData* wlhd = (WorkloadLayerHandlerData*)data;
  int device_id = wlhd->device_id;
  bool pim_enabled = wlhd->pim_enabled;

  // PIM operation
  if (pim_enabled) {
    int pim_channel_id = wlhd->pim_channel_id;
    int queue_idx = num_devices * device_id + pim_channel_id;
    if (!pim_pending_requests[queue_idx].empty()) {
      PendingMemoryRequest pmr = pim_pending_requests[queue_idx].front();
      pim_pending_requests[queue_idx].pop_front();
      uint64_t load_store_time = get_mem_runtime(pmr.request);
      uint64_t runtime = pmr.wlhd->pim_runtime + load_store_time;
      Sys* sys = sys_map[pmr.wlhd->sys_id];

      sys->register_event(this, EventType::General, pmr.wlhd, runtime);

      sys->register_event(
          completion_target(pmr.wlhd), EventType::General, pmr.wlhd, runtime);

      pim_ongoing_transaction[queue_idx] = true;
    } else {
      pim_ongoing_transaction[queue_idx] = false;
    }
    return;
  }
  // Ordinary memory access
  else {
    const size_t request_queue =
        queue_index(device_id, wlhd->memory_operation);
    if (!pending_requests[request_queue].empty()) {
      PendingMemoryRequest pmr = pending_requests[request_queue].front();
      pending_requests[request_queue].pop_front();
      start_request(pmr.request, pmr.wlhd, request_queue);
    } else {
      ongoing_transaction[request_queue] = false;
    }
  }
}

size_t AnalyticalMemory::queue_index(
    uint32_t device_id,
    MemoryOperation operation) const {
  if (!bandwidth_resource.has_value()) {
    throw logic_error("memory bandwidth resource is not initialized");
  }
  if (device_id >= num_devices) {
    throw out_of_range(
        "memory device_id " + to_string(device_id) +
        " is out of range [0," + to_string(num_devices) + ")");
  }
  return static_cast<size_t>(device_id) * bandwidth_resource->server_count() +
      bandwidth_resource->server_index(operation);
}

void AnalyticalMemory::start_request(
    const MemoryRequest& request,
    WorkloadLayerHandlerData* wlhd,
    size_t queue_idx) {
  uint64_t runtime = get_mem_runtime(request);
  const auto& resource_config = bandwidth_resource->config();
  if (resource_config.concurrency == BandwidthConcurrency::Serialized) {
    auto& previous = last_serialized_operation.at(wlhd->device_id);
    if (previous.has_value()) {
      const uint64_t turnaround = bandwidth_resource->turnaround_delay_ns(
          *previous, request.operation);
      if (runtime > numeric_limits<uint64_t>::max() - turnaround) {
        throw overflow_error("memory runtime plus turnaround exceeds uint64 ns");
      }
      runtime += turnaround;
    }
    previous = request.operation;
  }
  Sys* sys = sys_map.at(wlhd->sys_id);
  wlhd->memory_operation = request.operation;
  sys->register_event(this, EventType::General, wlhd, runtime);
  sys->register_event(
      completion_target(wlhd), EventType::General, wlhd, runtime);
  ongoing_transaction.at(queue_idx) = true;
}

uint64_t AnalyticalMemory::get_mem_runtime(
    const MemoryRequest& request) const {
  if (!bandwidth_resource.has_value()) {
    throw logic_error("memory bandwidth resource is not initialized");
  }
  return bandwidth_resource->service_time_ns(
      request.bytes, request.operation, mem_latency);
}
