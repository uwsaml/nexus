#include <boost/filesystem.hpp>
#include <glog/logging.h>
#include <unordered_set>

#include "nexus/common/config.h"
#include "nexus/common/model_db.h"
#include "nexus/scheduler/scheduler.h"

namespace fs = boost::filesystem;

DEFINE_bool(epoch_schedule, true, "Enable epoch scheduling");
DEFINE_bool(prefix_batch, true, "Enable prefix batching");
DEFINE_int32(beacon, 2, "beacon interval in seconds");
DEFINE_int32(epoch, 10, "epoch scheduling interval in seconds");

namespace nexus {
namespace scheduler {

INSTANTIATE_RPC_CALL(AsyncService, Register, RegisterRequest, RegisterReply);
INSTANTIATE_RPC_CALL(AsyncService, Unregister, UnregisterRequest, RpcReply);
INSTANTIATE_RPC_CALL(AsyncService, LoadModel, LoadModelRequest, LoadModelReply);
INSTANTIATE_RPC_CALL(AsyncService, UpdateBackendStats, BackendStatsProto,
                     RpcReply);
INSTANTIATE_RPC_CALL(AsyncService, KeepAlive, KeepAliveRequest, RpcReply);

Scheduler::Scheduler(std::string port, size_t nthreads,
                     std::string model_db_root) :
    AsyncRpcServiceBase(port, nthreads),
    beacon_interval_sec_(FLAGS_beacon),
    epoch_interval_sec_(FLAGS_epoch),
    enable_epoch_schedule_(FLAGS_epoch_schedule),
    enable_prefix_batch_(FLAGS_prefix_batch) {
  min_history_len_ = (epoch_interval_sec_ + beacon_interval_sec_ - 1) /
                     beacon_interval_sec_;
  history_len_ = min_history_len_ * 2;
  if (!enable_epoch_schedule_) {
    LOG(INFO) << "Epoch scheduling is off";
  }
  if (!enable_prefix_batch_) {
    LOG(INFO) << "Prefix batching is off";
  }
  ModelDatabase::Singleton().Init(model_db_root);
}

void Scheduler::LoadWorkloadFile(const std::string& workload_file) {
  LOG(INFO) << "Load workload file from " << workload_file;
  YAML::Node config = YAML::LoadFile(workload_file);
  // Load static workload configuration
  for (uint i = 0; i < config.size(); ++i) {
    const YAML::Node& backend_workload = config[i];
    LOG(INFO) << "Backend " << i << ":";
    std::vector<YAML::Node> models;
    for (uint j = 0; j < backend_workload.size(); ++j) {
      LOG(INFO) << "- " <<backend_workload[j];
      models.push_back(backend_workload[j]);
    }
    static_workloads_.push_back(models);
  }
}

void Scheduler::Run() {
  // Start RPC service first
  Start();
  // main scheduler login
  uint64_t elapse_sec = 0;
  uint64_t last_beacon = 0;
  uint64_t last_epoch = 0;
  while (running_) {
    auto now = std::chrono::system_clock::now();
    if (elapse_sec > 0 && elapse_sec % beacon_interval_sec_ == 0) {
      last_beacon = elapse_sec;
      BeaconCheck();
    }
    if (elapse_sec > 0 && elapse_sec % epoch_interval_sec_ == 0) {
      last_epoch = elapse_sec;
      if (enable_epoch_schedule_) {
        EpochSchedule();
      }
    }
    int next_sec = std::min(last_beacon + beacon_interval_sec_,
                            last_epoch + epoch_interval_sec_);
    std::this_thread::sleep_until(
        now + std::chrono::seconds(next_sec - elapse_sec));
    elapse_sec = next_sec;
  }
}

void Scheduler::Register(const grpc::ServerContext& ctx,
                         const RegisterRequest& request, RegisterReply* reply) {
  std::vector<std::string> tokens;
  SplitString(ctx.peer(), ':', &tokens);
  std::string ip = tokens[1];
  //std::string server_addr = tokens[1] + ':' + request.server_port();
  //std::string rpc_addr = tokens[1] + ':' + request.rpc_port();
  LOG(INFO) << "Register server: " << request.DebugString();
  if (request.node_type() == BACKEND_NODE) {
    auto backend = std::make_shared<BackendDelegate>(
        request.node_id(), ip, request.server_port(), request.rpc_port(),
        request.gpu_device_name(), request.gpu_available_memory(),
        beacon_interval_sec_, epoch_interval_sec_);
    RegisterBackend(std::move(backend), reply);
  } else { // FRONTEND_NODE
    auto frontend = std::make_shared<FrontendDelegate>(
        request.node_id(), ip, request.server_port(), request.rpc_port(),
        beacon_interval_sec_);
    RegisterFrontend(std::move(frontend), reply);
  }
}

void Scheduler::Unregister(const grpc::ServerContext& ctx,
                           const UnregisterRequest& request, RpcReply* reply) {
  LOG(INFO) << "Unregister " << NodeType_Name(request.node_type()) << " " <<
      request.node_id();
  if (request.node_type() == BACKEND_NODE) {
    UnregisterBackend(request.node_id());
  } else { // FRONTEND_NODE
    UnregisterFrontend(request.node_id());
  }
  reply->set_status(CTRL_OK);
}

void Scheduler::LoadModel(const grpc::ServerContext& ctx,
                          const LoadModelRequest& request,
                          LoadModelReply* reply) {
  ModelSession model_sess(request.model_session());
  auto info = ModelDatabase::Singleton().GetModelInfo(
      ModelSessionToModelID(model_sess));
  if (info == nullptr) {
    reply->set_status(MODEL_NOT_FOUND);
    return;
  }
  if ((*info)["resizable"] && (*info)["resizable"].as<bool>()) {
    if (model_sess.image_height() == 0) {
      // Set default image size for resizable CNN
      model_sess.set_image_height((*info)["image_height"].as<uint32_t>());
      model_sess.set_image_width((*info)["image_width"].as<uint32_t>());
    }
  }
  std::string model_sess_id = ModelSessionToString(model_sess);
  float workload = request.estimate_workload();

  std::lock_guard<std::mutex> lock(mutex_);
  auto frontend = GetFrontend(request.node_id());
  if (frontend == nullptr) {
    reply->set_status(CTRL_SERVER_NOT_REGISTERED);
    return;
  }
  if (session_table_.find(model_sess_id) != session_table_.end()) {
    // TODO: For now, if model session is already loaded, don't allocate
    // new backends, just rely on epoch scheduling
    reply->set_status(CTRL_OK);
    GetModelRoute(model_sess_id, reply->mutable_model_route());
    frontend->SubscribeModel(model_sess_id);
    if (session_subscribers_.count(model_sess_id) == 0) {
      session_subscribers_.emplace(model_sess_id, ServerList{request.node_id()});
    } else {
      session_subscribers_.at(model_sess_id).insert(request.node_id());
    }
    return;
  }
  
  // Check prefix batching first
  if (enable_prefix_batch_) {
    // Prefix batching is enabled
    std::string model_id = ModelSessionToModelID(model_sess);
    auto share_prefixes = ModelDatabase::Singleton().GetPrefixShareModels(
        model_id);
    if (!share_prefixes.empty()) {
      std::shared_ptr<SessionInfo> share_session_info = nullptr;
      ModelSession share_model_sess;
      share_model_sess.set_image_height(model_sess.image_height());
      share_model_sess.set_image_width(model_sess.image_width());
      // TODO: currently only support prefix batching with same latency sla
      share_model_sess.set_latency_sla(model_sess.latency_sla());
      // Find if there are model sessions that can share prefix with
      for (auto share_model_id : share_prefixes) {
        ParseModelID(share_model_id, &share_model_sess);
        std::string share_model_sess_id = ModelSessionToString(
            share_model_sess);
        auto iter = session_table_.find(share_model_sess_id);
        if (iter != session_table_.end()) {
          share_session_info = iter->second;
          break;
        }
      }
      if (share_session_info != nullptr) {
        // Find shared model session
        LOG(INFO) << "Model session " << model_sess_id << " shares prefix "
            "with session " << ModelSessionToString(share_model_sess);
        for (auto iter : share_session_info->backend_throughputs) {
          auto backend = GetBackend(iter.first);
          backend->LoadPrefixModel(model_sess, share_model_sess);
          backend->UpdateModelTableRpc();
        }
        share_session_info->model_sessions.push_back(model_sess);
        session_table_.emplace(model_sess_id, share_session_info);
        frontend->SubscribeModel(model_sess_id);
        session_subscribers_.emplace(model_sess_id,
                                     ServerList{request.node_id()});
        // Fill route table in the reply
        reply->set_status(CTRL_OK);
        GetModelRoute(model_sess_id, reply->mutable_model_route());
        return;
      }
    }
  }

  // Find best-fit backends to serve the workload
  std::vector<std::pair<BackendDelegatePtr, InstanceInfo> > assign_backends;
  std::unordered_set<uint32_t> used;
  if (workload == 0) {
    BackendDelegatePtr backend;
    InstanceInfo inst_info;
    FindBestBackend(model_sess, workload, used, &backend, &inst_info);
    if (backend == nullptr) {
      reply->set_status(NOT_ENOUGH_BACKENDS);
      return;
    }
    assign_backends.emplace_back(backend, inst_info);
  } else {
    while (workload > 0) {
      BackendDelegatePtr backend;
      InstanceInfo inst_info;
      FindBestBackend(model_sess, workload, used, &backend, &inst_info);
      if (backend == nullptr) {
        reply->set_status(NOT_ENOUGH_BACKENDS);
        return;
      }
      assign_backends.emplace_back(backend, inst_info);
      used.insert(backend->node_id());
      workload -= inst_info.throughput;
    }
  }

  // Load models
  auto session_info = std::make_shared<SessionInfo>();
  for (auto iter : assign_backends) {
    auto backend = iter.first;
    auto const& inst_info = iter.second;
    backend->LoadModel(inst_info);
    backend->UpdateModelTableRpc();
    session_info->backend_throughputs.emplace(backend->node_id(),
                                              inst_info.throughput);
  }
  session_info->model_sessions.push_back(model_sess);
  session_table_.emplace(model_sess_id, session_info);
  frontend->SubscribeModel(model_sess_id);
  session_subscribers_.emplace(model_sess_id, ServerList{request.node_id()});
  
  // Fill route table in the reply
  reply->set_status(CTRL_OK);
  GetModelRoute(model_sess_id, reply->mutable_model_route());
}

void Scheduler::UpdateBackendStats(const grpc::ServerContext& ctx,
                                   const BackendStatsProto& request,
                                   RpcReply* reply) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto backend = GetBackend(request.node_id());
  if (backend == nullptr) {
    reply->set_status(CTRL_SERVER_NOT_REGISTERED);
    return;
  }
  backend->UpdateStats(request);
  reply->set_status(CTRL_OK);
}

void Scheduler::KeepAlive(const grpc::ServerContext& ctx,
                          const KeepAliveRequest& request, RpcReply* reply) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto frontend = GetFrontend(request.node_id());
  if (frontend == nullptr) {
    reply->set_status(CTRL_SERVER_NOT_REGISTERED);
    return;
  }
  frontend->Tick();
  reply->set_status(CTRL_OK);
}

void Scheduler::HandleRpcs() {
  using namespace std::placeholders;
  new Register_Call(&service_, cq_.get(),
                    std::bind(&Scheduler::Register, this, _1, _2, _3));
  new Unregister_Call(&service_, cq_.get(),
                      std::bind(&Scheduler::Unregister, this, _1, _2, _3));
  new LoadModel_Call(&service_, cq_.get(),
                     std::bind(&Scheduler::LoadModel, this, _1, _2, _3));
  new UpdateBackendStats_Call(
      &service_, cq_.get(),
      std::bind(&Scheduler::UpdateBackendStats, this, _1, _2, _3));
  new KeepAlive_Call(&service_, cq_.get(),
                     std::bind(&Scheduler::KeepAlive, this, _1, _2, _3));
  void* tag;
  bool ok;
  while (running_) {
    cq_->Next(&tag, &ok);
    if (ok) {
      static_cast<RpcCallBase*>(tag)->Proceed();
    }
  }
}

void Scheduler::RegisterFrontend(FrontendDelegatePtr frontend,
                                 RegisterReply* reply) {
  // lock protected
  std::lock_guard<std::mutex> lock(mutex_);
  if (frontends_.find(frontend->node_id()) != frontends_.end()) {
    reply->set_status(CTRL_FRONTEND_NODE_ID_CONFLICT);
    return;
  }
  // add the frontend client in the frontend map
  frontends_[frontend->node_id()] = frontend;
  reply->set_status(CTRL_OK);
  reply->set_beacon_interval_sec(BEACON_INTERVAL_SEC);
}

void Scheduler::RegisterBackend(BackendDelegatePtr backend,
                                RegisterReply* reply) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backends_.find(backend->node_id()) != backends_.end()) {
    reply->set_status(CTRL_BACKEND_NODE_ID_CONFLICT);
    return;
  }
  // Add the backend client in the backend map
  backends_[backend->node_id()] = backend;
  reply->set_status(CTRL_OK);
  reply->set_beacon_interval_sec(BEACON_INTERVAL_SEC);

  // Update workload to new backend
  AddBackend(backend);
}

void Scheduler::UnregisterFrontend(uint32_t node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto frontend = GetFrontend(node_id);
  if (frontend == nullptr) {
    LOG(ERROR) << "Cannot find frontend " << node_id;
    return;
  }
  frontends_.erase(frontend->node_id());
  LOG(INFO) << "Remove frontend " << node_id;
  RemoveFrontend(frontend);
}

void Scheduler::UnregisterBackend(uint32_t node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto backend = GetBackend(node_id);
  if (backend == nullptr) {
    LOG(ERROR) << "Cannot find backend " << node_id;
    return;
  }
  backends_.erase(backend->node_id());
  LOG(INFO) << "Remove backend " << node_id;
  RemoveBackend(backend);
}

void Scheduler::AddBackend(BackendDelegatePtr backend) {
  std::unordered_set<SessionInfoPtr> changed_sessions;
  std::unordered_set<BackendDelegatePtr> changed_backends;

  // 1. Check if there is any static configured workload to assign
  int assign_load_id = -1;
  for (uint id = 0; id < static_workloads_.size(); ++id) {
    if (assigned_static_workloads_.find(id) ==
        assigned_static_workloads_.end()) {
      assign_load_id = id;
      assigned_static_workloads_[id] = backend->node_id();
      break;
    }
  }
  if (assign_load_id >= 0) {
    LOG(INFO) << "Assign workload " << assign_load_id << " to backend " <<
        backend->node_id();
    auto workload = static_workloads_[assign_load_id];
    for (auto session_info : workload) {
      backend->LoadModel(session_info);
    }
    backend->set_workload_id(assign_load_id);
    changed_backends.insert(backend);

    // Update session info
    for (auto& model_sess_id : backend->GetModelSessions()) {
      if (session_table_.find(model_sess_id) == session_table_.end()) {
        auto session_info = std::make_shared<SessionInfo>();
        session_info->has_static_workload = true;
        ModelSession model_sess;
        ParseModelSession(model_sess_id, &model_sess);
        session_info->model_sessions.push_back(model_sess);
        session_table_.emplace(model_sess_id, session_info);
      }
      auto session_info = session_table_.at(model_sess_id);
      session_info->backend_throughputs.emplace(
          backend->node_id(), backend->GetModelThroughput(model_sess_id));
      changed_sessions.insert(session_info);
    }
    // Add backup model to session info
    for (auto& model_sess_id : backend->GetBackupModelSessions()) {
      LOG(INFO) << "backup model session: " << model_sess_id;
      auto iter = session_table_.find(model_sess_id);
      if (iter == session_table_.end()) {
        LOG(ERROR) << "Cannot find backup model session " << model_sess_id <<
            " in the session table";
        continue;
      }
      auto session_info = iter->second;
      if (session_info->backup_backends.count(backend->node_id()) > 0) {
        continue;
      }
      session_info->backup_backends.insert(backend->node_id());
      LOG(INFO) << "a";
      BackendInfo info;
      backend->GetInfo(&info);
      for (auto iter : session_info->backend_throughputs) {
        auto b = GetBackend(iter.first);
        if (b == nullptr) {
          continue;
        }
        b->AddBackupForModel(model_sess_id, info);
        changed_backends.insert(b);
      }
      LOG(INFO) << "b";
    }
  } else {
    // 2. Check if there are unassigned workloads
    AllocateUnassignedWorkloads(&changed_sessions, &changed_backends);
    for (auto session : changed_sessions) {
      LOG(INFO) << "Changed session: " <<
          ModelSessionToString(session->model_sessions[0]);
    }
  }

  // 3. Update backend model table
  for (auto b : changed_backends) {
    b->UpdateModelTableRpc();
  }
  
  // 4. Update model info and route
  UpdateModelRoutes(changed_sessions);
}

void Scheduler::RemoveBackend(BackendDelegatePtr backend) {
  if (backend->IsIdle()) {
    return;
  }
  std::unordered_set<SessionInfoPtr> changed_sessions;
  std::unordered_set<BackendDelegatePtr> changed_backends;

  // 1. Remove backend from ModelInfo
  std::vector<std::string> model_sessions = backend->GetModelSessions();
  for (auto& model_sess_id : model_sessions) {
    if (session_table_.count(model_sess_id) == 0) {
      continue;
    }
    auto session_info = session_table_.at(model_sess_id);
    // Because shared prefix models could share the same session_info,
    // it's necessary to check whether session_info is already in the changed list
    if (changed_sessions.count(session_info) == 0) {
      session_info->backend_throughputs.erase(backend->node_id());
      changed_sessions.insert(session_info);
    }
  }
  
  // 2. Try to re-assign backend's workload to another idle one
  BackendDelegatePtr assigned;
  for (auto iter : backends_) {
    if (iter.second->IsIdle() && iter.second->Assign(*backend)) {
      assigned = iter.second;
      break;
    }
  }
  if (assigned != nullptr) {
    for (auto& model_sess_id : model_sessions) {
      session_table_.at(model_sess_id)->backend_throughputs.emplace(
          assigned->node_id(), assigned->GetModelThroughput(model_sess_id));
    }
    if (assigned->workload_id()) {
      assigned_static_workloads_.emplace(
          assigned->workload_id(), assigned->node_id());
      LOG(INFO) << "Reassign workload " << assigned->workload_id() <<
          " to backend " << assigned->node_id();
    }
    changed_backends.insert(assigned);
    // Update backup models to session info
    for (auto& model_sess_id : backend->GetBackupModelSessions()) {
      auto session_info = session_table_.at(model_sess_id);
      bool remove = false;
      bool insert = false;
      if (session_info->backup_backends.erase(backend->node_id()) > 0) {
        remove = true;
      }
      if (session_info->backup_backends.count(assigned->node_id()) == 0) {
        session_info->backup_backends.insert(assigned->node_id());
        insert = true;
      }
      if (!remove && !insert) {
        continue;
      }
      BackendInfo info;
      assigned->GetInfo(&info);
      for (auto iter : session_info->backend_throughputs) {
        auto b = GetBackend(iter.first);
        if (b != nullptr) {
          b->RemoveBackupForModel(model_sess_id, backend->node_id());
          b->AddBackupForModel(model_sess_id, info);
          changed_backends.insert(b);
        }
      }
    }
  } else { // assigned == nullptr
    // Remove backup models to session info
    for (auto& model_sess_id : backend->GetBackupModelSessions()) {
      auto session_info = session_table_.at(model_sess_id);
      if (session_info->backup_backends.erase(backend->node_id()) == 0) {
        continue;
      }
      for (auto iter : session_info->backend_throughputs) {
        auto b = GetBackend(iter.first);
        if (b != nullptr) {
          b->RemoveBackupForModel(model_sess_id, backend->node_id());
          changed_backends.insert(b);
        }
      }
    }
    if (backend->workload_id() >= 0) {
      assigned_static_workloads_.erase(backend->workload_id());
      LOG(INFO) << "Remove workload " << backend->workload_id();
    } else {
      // 3. If it's not static configured workload, try to allocate model
      // instances to other backends
      for (auto& model_sess_id : model_sessions) {
        float tp = backend->GetModelThroughput(model_sess_id);
        session_table_.at(model_sess_id)->unassigned_workload += tp;
      }
      AllocateUnassignedWorkloads(&changed_sessions, &changed_backends);
      // TODO: assign backup models to other backends
    }
  }

  // 4. Update backend model table
  for (auto b : changed_backends) {
    b->UpdateModelTableRpc();
  }
  
  // 5. Update changed routes;
  UpdateModelRoutes(changed_sessions);
}

void Scheduler::RemoveFrontend(FrontendDelegatePtr frontend) {
  // Update subscribed model sessions
  std::unordered_set<BackendDelegatePtr> update_backends;
  for (auto model_sess_id : frontend->subscribe_models()) {
    ServerList& subs = session_subscribers_.at(model_sess_id);
    subs.erase(frontend->node_id());
    if (subs.empty()) {
      auto session_info = session_table_.at(model_sess_id);
      if (session_info->has_static_workload) {
        continue;
      }
      LOG(INFO) << "Remove model session: " << model_sess_id;
      RemoveFromSessionGroup(&session_info->model_sessions, model_sess_id);
      for (auto iter : session_info->backend_throughputs) {
        auto backend = GetBackend(iter.first);
        backend->UnloadModel(model_sess_id);
        update_backends.insert(backend);
      }
      session_table_.erase(model_sess_id);
    }
  }
  // Remove model sessions
  for (auto backend : update_backends) {
    backend->UpdateModelTableRpc();
  }
}

BackendDelegatePtr Scheduler::GetBackend(uint32_t node_id) {
  auto iter = backends_.find(node_id);
  if (iter == backends_.end()) {
    LOG(ERROR) << "Cannot find backend " << node_id;
    return nullptr;
  }
  return iter->second;
}

FrontendDelegatePtr Scheduler::GetFrontend(uint32_t node_id) {
  auto iter = frontends_.find(node_id);
  if (iter == frontends_.end()) {
    LOG(ERROR) << "Cannot find frontend " << node_id;
    return nullptr;
  }
  return iter->second;
}

void Scheduler::GetModelRoute(const std::string& model_sess_id,
                              ModelRouteProto* route) {
  route->set_model_session_id(model_sess_id);
  for (auto iter : session_table_.at(model_sess_id)->backend_throughputs) {
    auto backend_rate = route->add_backend_rate();
    backends_.at(iter.first)->GetInfo(backend_rate->mutable_info());
    backend_rate->set_throughput(iter.second);
  }
}

void Scheduler::FindBestBackend(
    const ModelSession& model_sess, float request_rate,
    const std::unordered_set<uint32_t>& skips,
    BackendDelegatePtr* best_backend, InstanceInfo* inst_info) {
  using ModelLoad = std::tuple<BackendDelegatePtr, InstanceInfo, float>;
  ModelLoad max_tp_load;
  ModelLoad max_occ_load;
  for (auto iter : backends_) {
    auto backend = iter.second;
    if (skips.find(backend->node_id()) != skips.end()) {
      continue;
    }
    if (!backend->IsAlive() || backend->workload_id() >= 0) {
      continue;
    }
    if (request_rate == 0 && !backend->IsIdle()) {
      continue;
    }
    InstanceInfo tmp_info;
    float occupancy;
    bool ret = backend->PrepareLoadModel(model_sess, request_rate, &tmp_info,
                                         &occupancy);
    if (!ret) {
      continue;
    }
    if (std::get<0>(max_tp_load) == nullptr ||
        tmp_info.throughput > std::get<1>(max_tp_load).throughput) {
      max_tp_load = std::make_tuple(backend, tmp_info, occupancy);
    }
    if (std::get<0>(max_occ_load) == nullptr ||
        occupancy > std::get<2>(max_occ_load)) {
      max_occ_load = std::make_tuple(backend, tmp_info, occupancy);
    }
  }
  if (request_rate == 0) {
    // for request rate = 0, return backend that provides highest throughput
    *best_backend = std::get<0>(max_tp_load);
    *inst_info = std::get<1>(max_tp_load);
  } else if (std::get<1>(max_tp_load).throughput < request_rate) {
    // If no backend can achieve request rate, return backend that provides
    // highest throughput
    *best_backend = std::get<0>(max_tp_load);
    *inst_info = std::get<1>(max_tp_load);
  } else {
    // Otherwise, return backend that has highest occupancy
    *best_backend = std::get<0>(max_occ_load);
    *inst_info = std::get<1>(max_occ_load);
  }
}

void Scheduler::BeaconCheck() {
  std::lock_guard<std::mutex> lock(mutex_);
  // 1. Remove dead frontends
  std::vector<FrontendDelegatePtr> dead_frontends;
  for (auto iter : frontends_) {
    auto frontend = iter.second;
    if (frontend->IsAlive()) {
      continue;
    }
    dead_frontends.push_back(frontend);
  }
  for (auto frontend : dead_frontends) {
    frontends_.erase(frontend->node_id());
    std::time_t last_time = frontend->LastAliveTime();
    LOG(INFO) << "Remove frontend " << frontend->node_id() <<
        ", last alive time: " << std::ctime(&last_time);
    RemoveFrontend(frontend);
  }
  
  // 2. Aggregate model session rps
  for (auto iter : session_table_) {
    const auto& model_sess_id = iter.first;
    auto session_info = iter.second;
    double rps = 0.;
    for (auto backend_iter : session_info->backend_throughputs) {
      auto backend = backends_.at(backend_iter.first);
      rps += backend->GetModelRps(model_sess_id);
    }
    if (session_info->rps_history.size() > 0 || rps > 0) {
      // Don't push 0 in the begining
      session_info->rps_history.push_back(rps);
    }
    if (session_info->rps_history.size() > history_len_) {
      session_info->rps_history.pop_front();
    }
    VLOG(2) << "Model " << model_sess_id << " rps: " << rps <<
        " req/s (avg over " << epoch_interval_sec_ << " seconds)";
    for (auto backend_iter : session_info->backend_throughputs) {
      auto backend = backends_.at(backend_iter.first);
      VLOG(2) << "- backend " << backend->node_id() << ": " <<
          backend->GetModelRps(model_sess_id);
    }
  }
  
  // 3. Remove dead backend
  std::vector<BackendDelegatePtr> dead_backends;
  for (auto iter : backends_) {
    auto backend = iter.second;
    if (backend->IsAlive()) {
      continue;
    }
    dead_backends.push_back(backend);
  }
  for (auto backend : dead_backends) {
    std::time_t last_time = backend->LastAliveTime();
    LOG(INFO) << "Remove backend " << backend->node_id() <<
        ", last alive time: " << std::ctime(&last_time);
    backends_.erase(backend->node_id());
  }
  // Reassign workload of dead backends
  for (auto backend : dead_backends) {
    RemoveBackend(backend);
  }
}

void Scheduler::EpochSchedule() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::unordered_set<std::shared_ptr<SessionInfo> > visited;
  std::unordered_set<std::shared_ptr<SessionInfo> > changed_sessions;
  std::vector<BackendDelegatePtr> overload_backends;

  VLOG(1) << "Epoch schedule";
  // 1. Adjust the GPU allocation based on the workload
  for (auto iter : session_table_) {
    auto const& model_sess_id = iter.first;
    auto session_info = iter.second;
    if (visited.count(session_info) > 0) {
      continue;
    }
    visited.insert(session_info);
    double throughput = session_info->total_throughput();
    // Compute the workload mean and std
    uint32_t n = session_info->rps_history.size();
    if (n < min_history_len_) {
      continue;
    }
    double rps_mean = 0., rps_std = 0.;
    for (double rps : session_info->rps_history) {
      rps_mean += rps;
    }
    rps_mean /= n;
    for (double rps : session_info->rps_history) {
      rps_std += (rps - rps_mean) * (rps - rps_mean);
    }
    rps_std = sqrt(rps_std / (n - 1));
    double estimate_rps = std::max(
        session_info->rps_history[n - 1] + rps_std, 0.1);
    session_info->unassigned_workload = std::max(0., estimate_rps - throughput);
    VLOG(1) << model_sess_id << " estimate rps: " << estimate_rps <<
        " (last: " << session_info->rps_history[n - 1] << ", mean: " <<
        rps_mean << ", std: " << rps_std << "), throughput: " << throughput;

    if (estimate_rps < throughput * 0.97) {
      // Workload is smaller than throughput, can release some GPUs
      std::vector<std::pair<uint32_t, double> > adjust_backends;
      // Backends with static configured workload are still fixed
      for (auto iter : session_info->backend_throughputs) {
        auto backend = backends_.at(iter.first);
        if (backend->workload_id() >= 0) {
          estimate_rps -= iter.second;
        } else {
          adjust_backends.push_back(iter);
        }
      }
      // Sort the backends based on throughput
      std::sort(adjust_backends.begin(), adjust_backends.end(),
                [](std::pair<uint32_t, double> a,
                   std::pair<uint32_t, double> b) {
                  return a.second > b.second;
                });
      for (auto iter : adjust_backends) {
        if (estimate_rps <= 0) {
          auto backend = backends_.at(iter.first);
          backend->UnloadModel(model_sess_id);
          session_info->backend_throughputs.erase(iter.first);
        } else if (iter.second > estimate_rps) {
          auto backend = backends_.at(iter.first);
          float new_tp = backend->UpdateModelThroughput(model_sess_id,
                                                        estimate_rps);
          session_info->backend_throughputs[iter.first] = new_tp;
          estimate_rps -= new_tp;
        } else {
          estimate_rps -= iter.second;
        }
      }
      changed_sessions.insert(session_info);
    } else if (estimate_rps > throughput) {
      // Workload is larger than throughput, need to allocate more gpus
      std::vector<std::pair<uint32_t, double> > adjust_backends;
      // Backends with static configured workload are still fix
      for (auto iter : session_info->backend_throughputs) {
        auto backend = backends_.at(iter.first);
        if (backend->workload_id() >= 0) {
          estimate_rps -= iter.second;
        } else {
          adjust_backends.push_back(iter);
        }
      }
      // Second sort the backends based on throughput
      std::sort(adjust_backends.begin(), adjust_backends.end(),
                [](std::pair<uint32_t, double> a,
                   std::pair<uint32_t, double> b) {
                  return a.second > b.second;
                });
      for (auto iter : adjust_backends) {
        auto backend = backends_.at(iter.first);
        float new_tp = backend->UpdateModelThroughput(model_sess_id,
                                                      estimate_rps);
        session_info->backend_throughputs[iter.first] = new_tp;
        estimate_rps -= new_tp;
        if (backend->overload()) {
          overload_backends.push_back(backend);
        }
      }
      if (estimate_rps > 0) {
        session_info->unassigned_workload = estimate_rps;
      }
      changed_sessions.insert(session_info);
    }
  }

  // 2. Adjust overloaded backends
  for (auto backend : overload_backends) {
    std::vector<std::pair<SessionGroup, float> > spillout;
    backend->SpillOutWorkload(&spillout);
    for (auto iter : spillout) {
      auto model_sess_id = ModelSessionToString(iter.first[0]);
      auto session_info = session_table_.at(model_sess_id);
      session_info->backend_throughputs.erase(backend->node_id());
      session_info->unassigned_workload += iter.second;
    }
  }
  
  // 3. Allocate the unassigned workloads to backends that still have space
  AllocateUnassignedWorkloads(&changed_sessions);

  // 4. Update model table to backends and model routes to frontends
  for (auto iter : backends_) {
    iter.second->UpdateModelTableRpc();
  }
  UpdateModelRoutes(changed_sessions);

  DisplayModelTable();
}

void Scheduler::AllocateUnassignedWorkloads(
    std::unordered_set<SessionInfoPtr>* changed_sessions,
    std::unordered_set<BackendDelegatePtr>* changed_backends) {
  // Sort unassigned workloads by request rate
  std::vector<SessionInfoPtr> unassigned_workloads;
  std::unordered_set<SessionInfoPtr> visited;
  for (auto iter : session_table_) {
    auto session_info = iter.second;
    if (visited.count(session_info) > 0) {
      continue;
    }
    visited.insert(session_info);
    if (session_info->unassigned_workload > 0) {
      unassigned_workloads.emplace_back(session_info);
    }
  }
  if (unassigned_workloads.empty()) {
    return;
  }
  std::sort(unassigned_workloads.begin(), unassigned_workloads.end(),
            [](SessionInfoPtr a, SessionInfoPtr b) {
              return a->unassigned_workload > b->unassigned_workload;
            });
  for (auto session_info : unassigned_workloads) {
    float request_rate = session_info->unassigned_workload;
    auto const& sessions = session_info->model_sessions;
    // LOG(INFO) << "Try to assign workload " << model_sess_id << ", " <<
    //     request_rate << " req/s";
    while (request_rate > 0) {
      BackendDelegatePtr backend;
      InstanceInfo inst_info;
      FindBestBackend(sessions[0], request_rate, {}, &backend, &inst_info);
      if (backend == nullptr) {
        std::string model_sess_id = ModelSessionToString(sessions[0]);
        LOG(INFO) << "Unassigned workload " << model_sess_id << ", " <<
            request_rate << " req/s";
        break;
      }
      request_rate -= inst_info.throughput;
      backend->LoadModel(inst_info);
      for (int i = 1; i < sessions.size(); ++i) {
        backend->LoadPrefixModel(sessions[i], sessions[0]);
      }
      session_info->backend_throughputs.emplace(backend->node_id(),
                                                inst_info.throughput);
      changed_sessions->insert(session_info);
      if (changed_backends != nullptr) {
        changed_backends->insert(backend);
      }
    }
    session_info->unassigned_workload = std::max(0.f, request_rate);
  }
}

void Scheduler::UpdateModelRoutes(std::unordered_set<SessionInfoPtr> sessions) {
  std::unordered_map<uint32_t, ModelRouteUpdates> frontend_updates;
  for (auto session_info : sessions) {
    for (auto& model_sess : session_info->model_sessions) {
      std::string model_sess_id = ModelSessionToString(model_sess);
      if (session_subscribers_.count(model_sess_id) == 0) {
        continue;
      }
      for (auto frontend_id : session_subscribers_.at(model_sess_id)) {
        if (frontend_updates.find(frontend_id) == frontend_updates.end()) {
          frontend_updates.emplace(frontend_id, ModelRouteUpdates());
        }
        GetModelRoute(model_sess_id,
                      frontend_updates.at(frontend_id).add_model_route());
      }
    }
  }
  for (auto iter : frontend_updates) {
    auto frontend = frontends_.at(iter.first);
    frontend->UpdateModelRoutesRpc(iter.second);
  }
}

void Scheduler::DisplayModelTable() {
  std::stringstream ss;
  for (auto iter : session_table_) {
    auto const& model_sess_id = iter.first;
    auto session_info = iter.second;
    ss << model_sess_id << ":";
    for (auto backend_iter : session_info->backend_throughputs) {
      auto backend = GetBackend(backend_iter.first);
      double throughput = backend_iter.second;
      auto info = backend->GetInstanceInfo(model_sess_id);
      ss << " " << backend_iter.first << "/" << throughput << "/" <<
          info->batch;
    }
    ss << "\n";
  }
  VLOG(1) << "Model table: \n" << ss.str();
}

} // namespace scheduler
} // namespace nexus
