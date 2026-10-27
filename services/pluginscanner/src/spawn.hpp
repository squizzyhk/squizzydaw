// pluginscanner: child-process runner used for the out-of-process scan.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sq::pscan {

struct SpawnResult {
  bool started = false;
  bool timedOut = false;  // killed after the timeout
  bool crashed = false;   // died from an exception / signal
  int64_t exitCode = -1;
};

// argv[0] = executable path. Blocks until the child exits or timeoutMs elapsed (then kills it).
SpawnResult runProcess(const std::vector<std::string>& argv, uint32_t timeoutMs);

}  // namespace sq::pscan
