#include <gflags/gflags.h>
#include <glog/logging.h>
#include <iostream>
#include <string>
#include <vector>

#include "nexus/common/config.h"
#include "nexus/common/util.h"
#include "nexus/backend/backend_server.h"

using namespace nexus;
using namespace nexus::backend;

DEFINE_string(port, std::to_string(BACKEND_DEFAULT_PORT), "server port");
DEFINE_string(rpc_port, std::to_string(BACKEND_DEFAULT_RPC_PORT), "RPC port");
DEFINE_string(sch_addr, "127.0.0.1", "scheduler IP address "
              "(use default port 10001 if no port specified)");
DEFINE_int32(gpu, 0, "gpu device ID (default: 0)");
DEFINE_uint64(num_workers, 4, "number of workers (default: 4)");
DEFINE_string(cores, "", "Specify cores to use, e.g., \"0-4\", or \"0-3,5\"");

std::vector<int> ParseCores(std::string s) {
  std::vector<int> cores;
  std::vector<std::string> segs;
  SplitString(s, ',', &segs);
  for (auto seg : segs) {
    if (seg.find('-') == std::string::npos) {
      cores.push_back(std::stoi(seg));
    } else {
      std::vector<std::string> range;
      SplitString(seg, '-', &range);
      CHECK_EQ(range.size(), 2) << "Wrong format of cores";
      int beg = std::stoi(range[0]);
      int end = std::stoi(range[1]);
      for (int i = beg; i <= end; ++i) {
        cores.push_back(i);
      }
    }
  }
  for (auto i : cores) {
    LOG(INFO) << "Core " << i;
  }
  return cores;
}

int main(int argc, char** argv) {
  // log to stderr
  FLAGS_logtostderr = 1;
  // Init glog
  google::InitGoogleLogging(argv[0]);
  // Parse command line flags
  google::ParseCommandLineFlags(&argc, &argv, true);
  // Setup backtrace on segfault
  google::InstallFailureSignalHandler();
  // Decide server IP address
  LOG(INFO) << "Backend server: port " << FLAGS_port << ", rpc port " <<
      FLAGS_rpc_port << ", workers " << FLAGS_num_workers << ", gpu " <<
      FLAGS_gpu;
  // Create the backend server
  std::vector<int> cores = ParseCores(FLAGS_cores);
  BackendServer server(FLAGS_port, FLAGS_rpc_port, FLAGS_sch_addr,
                       FLAGS_gpu, FLAGS_num_workers, cores);
  server.Run();
  return 0;
}
