// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Runtime overrides for implementation-specific PHY acceleration paths.

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ocudu {

/// Returns true if \c value is one of the accepted false-like environment flag values.
inline bool phy_acceleration_env_flag_value_is_false(const char* value)
{
  return (std::strcmp(value, "0") == 0) || (std::strcmp(value, "false") == 0) || (std::strcmp(value, "False") == 0) ||
         (std::strcmp(value, "FALSE") == 0) || (std::strcmp(value, "off") == 0) || (std::strcmp(value, "OFF") == 0) ||
         (std::strcmp(value, "no") == 0) || (std::strcmp(value, "NO") == 0);
}

/// Returns true if \c value is one of the accepted true-like environment flag values.
inline bool phy_acceleration_env_flag_value_is_true(const char* value)
{
  return (std::strcmp(value, "1") == 0) || (std::strcmp(value, "true") == 0) || (std::strcmp(value, "True") == 0) ||
         (std::strcmp(value, "TRUE") == 0) || (std::strcmp(value, "on") == 0) || (std::strcmp(value, "ON") == 0) ||
         (std::strcmp(value, "yes") == 0) || (std::strcmp(value, "YES") == 0);
}

/// Reads a boolean environment flag with the accepted true/false string set.
inline bool phy_acceleration_env_flag_enabled(const char* name, bool default_value = false)
{
  const char* value = std::getenv(name);
  if ((value == nullptr) || (*value == '\0')) {
    return default_value;
  }

  if (phy_acceleration_env_flag_value_is_true(value)) {
    return true;
  }
  if (phy_acceleration_env_flag_value_is_false(value)) {
    return false;
  }

  return default_value;
}

/// Reads an unsigned integer environment override, returning \c default_value for unset or malformed values.
inline unsigned phy_acceleration_env_unsigned(const char* name, unsigned default_value)
{
  const char* value = std::getenv(name);
  if ((value == nullptr) || (*value == '\0')) {
    return default_value;
  }
  if (*value == '-') {
    return default_value;
  }

  char* end_ptr        = nullptr;
  errno                = 0;
  unsigned long parsed = std::strtoul(value, &end_ptr, 10);
  if ((end_ptr == value) || (*end_ptr != '\0') || (errno == ERANGE) ||
      (parsed > std::numeric_limits<unsigned>::max())) {
    return default_value;
  }

  return static_cast<unsigned>(parsed);
}

/// Returns true when a grid allocation mode string requests managed memory.
/// \brief Gets the longest a host thread waits for GPU work before treating it as failed.
///
/// A healthy wait lasts at most a few slots; reaching this bound means the GPU work is stuck. Set it with
/// OCUDU_CUDA_GPU_WAIT_TIMEOUT_US.
inline std::chrono::microseconds phy_acceleration_gpu_wait_timeout()
{
  // Zero would fail every wait at once, so it selects the default like a missing or malformed value. The lower PHY
  // reads the same variable with the same rules through ocudu_cuda_gpu_wait_timeout().
  static constexpr unsigned default_timeout_us = 100000;
  static const unsigned     timeout_us         = phy_acceleration_env_unsigned("OCUDU_CUDA_GPU_WAIT_TIMEOUT_US", 0);
  return std::chrono::microseconds((timeout_us != 0) ? timeout_us : default_timeout_us);
}

inline bool phy_acceleration_env_mode_is_managed(const char* value)
{
  return (value != nullptr) && (std::strcmp(value, "managed") == 0);
}

/// Returns the CUDA-visible grid mode, preferring an optional direction-specific override.
inline const char* phy_acceleration_cuda_visible_grid_mode(const char* direction_env_name = nullptr)
{
  const char* mode = nullptr;
  if (direction_env_name != nullptr) {
    mode = std::getenv(direction_env_name);
  }
  if (mode == nullptr) {
    mode = std::getenv("OCUDU_CUDA_VISIBLE_GRID");
  }
  return mode;
}

/// Returns true when CUDA-visible resource grids should use managed memory.
inline bool phy_acceleration_cuda_visible_grid_managed_requested(const char* direction_env_name = nullptr)
{
  return phy_acceleration_env_mode_is_managed(phy_acceleration_cuda_visible_grid_mode(direction_env_name));
}

/// Returns true when the PDSCH auto-enable override has been configured.
inline bool phy_acceleration_pdsch_auto_enable_discrete_configured()
{
  const char* value = std::getenv("OCUDU_PDSCH_AUTO_ENABLE_DISCRETE");
  return (value != nullptr) && (*value != '\0');
}

/// Returns true when PDSCH auto-enable for discrete GPUs is requested.
inline bool phy_acceleration_pdsch_auto_enable_discrete_requested()
{
  return phy_acceleration_env_flag_enabled("OCUDU_PDSCH_AUTO_ENABLE_DISCRETE");
}

/// Capability-derived defaults for a CUDA-visible managed resource grid.
///
/// Inputs are the three CUDA device attributes that actually differ across the
/// boxes we run on. Product names are only examples; the policy never keys off
/// them.
///
/// | class | CMA | host PT | CPU→GPU-resident | examples | default |
/// |---|---|---|---|---|---|
/// | fully coherent | 1 | 1 | 1 | GH200, Grace-Hopper | no prefetch; concurrent DL writes; prefer device |
/// | GPU sees host pages, CPU cannot read GPU-resident | 1 | 1 | 0 | GB10, many Jetson | prefetch both ways; no
/// concurrent writes | | migrate-on-fault / no ATS | 0 or host PT 0 | * | 0 | discrete, older Jetson | same as the row
/// above if managed is forced (auto uses pinned) |
///
/// `cudaMemPrefetchAsync` returning NotSupported is a later no-op, so turning
/// prefetch on by default is safe on parts that cannot migrate.
struct cuda_visible_managed_access_policy {
  bool host_reads_device_resident_directly = false;
  bool prefetch_by_default                 = false;
  bool prefer_device_memory                = false;
  bool allow_concurrent_downlink_writes    = false;
};

/// True when CPU and GPU can touch the same managed pages with no migration.
inline bool cuda_visible_managed_access_is_fully_coherent(bool concurrent_managed_access,
                                                          bool host_uses_host_page_tables,
                                                          bool direct_managed_access_from_host)
{
  return concurrent_managed_access && host_uses_host_page_tables && direct_managed_access_from_host;
}

/// \brief Automatic placement of a CUDA-visible resource grid: true for managed memory, false for pinned host memory.
///
/// Managed memory wherever the CPU and the GPU share it without page migrations: integrated GPUs (one physical memory,
/// e.g. DGX Spark GB10, Jetson Thor) and fully coherent CPU-GPU links (e.g. GH200 / GB200 NVLink-C2C). There the GPU
/// stages work on the grid in place -- PDSCH maps into the downlink grid, O-RAN compression reads it and decompression
/// writes the uplink grid, PUSCH / SRS read it -- and the host stages (PDCCH, DMRS, SSB, CSI-RS writes; PUCCH and host
/// PUSCH reads, through the uplink host mirror) need no copies. Discrete PCIe GPUs get pinned host grids: the host
/// stages would fault device-resident managed pages back over PCIe every slot, and the GPU stages copy instead.
inline bool cuda_visible_grid_auto_uses_managed_memory(bool integrated,
                                                       bool concurrent_managed_access,
                                                       bool host_uses_host_page_tables,
                                                       bool direct_managed_access_from_host)
{
  return integrated || cuda_visible_managed_access_is_fully_coherent(
                           concurrent_managed_access, host_uses_host_page_tables, direct_managed_access_from_host);
}

inline cuda_visible_managed_access_policy
make_cuda_visible_managed_access_policy(bool concurrent_managed_access,
                                        bool host_uses_host_page_tables,
                                        bool direct_managed_access_from_host,
                                        bool enable_concurrent_downlink_writes = false)
{
  const bool fully_coherent = cuda_visible_managed_access_is_fully_coherent(
      concurrent_managed_access, host_uses_host_page_tables, direct_managed_access_from_host);

  cuda_visible_managed_access_policy policy;
  policy.host_reads_device_resident_directly = direct_managed_access_from_host;
  policy.prefetch_by_default                 = !fully_coherent;
  policy.prefer_device_memory                = fully_coherent;
  policy.allow_concurrent_downlink_writes    = enable_concurrent_downlink_writes && fully_coherent;
  return policy;
}

/// \brief Decides whether forced CUDA PDSCH must supply its own codeblock executor.
///
/// A sequential lower PHY (for example the ZMQ radio) deliberately supplies no codeblock executor,
/// because it requires synchronous PHY processing. Forced CUDA PDSCH already processes codeblock
/// batches synchronously, so an inline executor satisfies the flexible PDSCH processor's dependency
/// without introducing a task queue or breaking the sequential contract.
///
/// \param[in] acceleration_requested      True when pdsch_acceleration_mode is "enabled".
/// \param[in] flexible_processor_selected True when the PDSCH processor configuration is flexible.
/// \param[in] codeblock_executor_valid    True when the deployment supplied a codeblock executor.
/// \return True when the factory must substitute an inline codeblock executor.
inline bool pdsch_acceleration_requires_inline_codeblock_executor(bool acceleration_requested,
                                                                  bool flexible_processor_selected,
                                                                  bool codeblock_executor_valid)
{
  return acceleration_requested && flexible_processor_selected && !codeblock_executor_valid;
}

} // namespace ocudu
