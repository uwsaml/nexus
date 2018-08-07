#include <chrono>
#include <glog/logging.h>

#include "nexus/backend/backend_server.h"
#include "nexus/backend/model_ins.h"
#include "nexus/backend/worker.h"

namespace nexus {
namespace backend {

Worker::Worker(int index, BackendServer* server,
               BlockPriorityQueue<Task>& task_queue,
               GpuExecutor* gpu_executor) :
    index_(index),
    server_(server),
    task_queue_(task_queue),
    gpu_executor_(gpu_executor),
    running_(false) {}

void Worker::Start() {
  running_ = true;
  thread_ = std::thread(&Worker::Run, this);
}

void Worker::Stop() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Worker::Run() {
  LOG(INFO) << "Worker " << index_ << " started";
  auto timeout = std::chrono::milliseconds(50);
  while (running_) {
    std::shared_ptr<Task> task = task_queue_.pop(timeout);
    if (task == nullptr) {
      continue;
    }
    Process(task);
  }
  LOG(INFO) << "Worker " << index_ << " stopped";
}

void Worker::Process(std::shared_ptr<Task> task) {
  switch (task->stage) {
    case kPreprocess: {
      task->model = server_->GetModelInstance(task->query.model_session_id());
      if (task->model == nullptr) {
        std::stringstream ss;
        ss << "Model session is not loaded: " << task->query.model_session_id();
        task->result.set_status(MODEL_SESSION_NOT_LOADED);
        SendReply(std::move(task));
        break;
      }
      // Increase input counter
      if (task->query.window_size() > 0) {
        task->model->counter()->Increase(task->query.window_size());
      } else {
        task->model->counter()->Increase(1);
      }
      // Preprocess task
      task->model->Preprocess(task);
      if (task->result.status() != CTRL_OK) {
        SendReply(std::move(task));
      } else {
        gpu_executor_->AddTask(std::move(task));
      }
      break;
    }
    case kPostprocess: {
      if (task->result.status() != CTRL_OK) {
        SendReply(std::move(task));
      } else {
        task->model->Postprocess(task);
        SendReply(std::move(task));
      }
      break;
    }
    default:
      LOG(ERROR) << "Wrong task stage: " << task->stage;
  }
}

void Worker::SendReply(std::shared_ptr<Task> task) {
  task->timer.Record("end");
  task->result.set_query_id(task->query.query_id());
  task->result.set_model_session_id(task->query.model_session_id());
  task->result.set_latency_us(task->timer.GetLatencyMicros("begin", "end"));
  task->result.set_queuing_us(task->timer.GetLatencyMicros("begin", "exec"));
  auto msg = std::make_shared<Message>(kBackendReply,
                                       task->result.ByteSizeLong());
  msg->EncodeBody(task->result);
  task->connection->Write(std::move(msg));
}

} // namespace backend
} // namespace nexus
