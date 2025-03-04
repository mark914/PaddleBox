/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>  // NOLINT
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "paddle/fluid/framework/data_feed.h"
#include "paddle/fluid/framework/data_set.h"
#include "paddle/fluid/framework/device_worker.h"
#include "paddle/fluid/framework/fleet/heter_context.h"
#include "paddle/fluid/framework/heter_util.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/program_desc.h"
#include "paddle/fluid/framework/reader.h"
#include "paddle/fluid/framework/trainer_desc.pb.h"
#include "paddle/fluid/framework/variable_helper.h"
#include "paddle/fluid/operators/reader/blocking_queue.h"
#include "paddle/phi/backends/dynload/port.h"

#ifdef PADDLE_WITH_PSLIB
#include "proto/the_one_ps.pb.h"
#endif

namespace paddle {
namespace framework {

class Dataset;
class ProgramDesc;
class PullDenseWorker;
class Scope;
class VarDesc;
class DeviceWorker;
class HeterWrapper;
class HeterRequest;
class HeterResponse;

template <class T>
class ChannelObject;

class TrainerBase {
 public:
  TrainerBase() {}
  virtual ~TrainerBase() {}
  // model memory are hosted in root_scope
  void SetScope(Scope* root_scope);
  void SetDebug(const bool debug) { debug_ = debug; }
  void SetDataset(Dataset* dataset_ptr) { dataset_ptr_ = dataset_ptr; }
  virtual void Initialize(const TrainerDesc& trainer_desc,
                          Dataset* data_set) = 0;
  virtual void InitTrainerEnv(const ProgramDesc& main_program,
                              const platform::Place& place) = 0;
  virtual void InitOtherEnv(const ProgramDesc& main_program) = 0;
  virtual void Run() = 0;
  virtual void Finalize() = 0;
  virtual Scope* GetWorkerScope(int thread_id) = 0;
  virtual void InitDumpEnv() = 0;
  virtual void DumpWork(int tid);
  virtual void ResetDataset(Dataset* dataset_ptr) {}

 protected:
  virtual std::string GetDumpPath(int tid) = 0;
  virtual void ParseDumpConfig(const TrainerDesc& trainer_desc);
  virtual void FinalizeDumpEnv();

  Scope* root_scope_;
  bool debug_;
  Dataset* dataset_ptr_;
  TrainerDesc trainer_desc_;

  // For dump param or field
  bool need_dump_field_ = false;
  std::string user_define_dump_filename_;
  bool need_dump_param_ = false;
  std::string dump_fields_path_;
  std::string dump_converter_;
  std::vector<std::string> dump_param_;
  std::vector<std::string> dump_fields_;
  int dump_thread_num_;
  std::vector<std::thread> dump_thread_;
  std::shared_ptr<paddle::framework::ChannelObject<std::string>> queue_;
};

// general trainer for async execution
// local trainer and distributed trainer are supported
// depends on the assigned device_worker
class MultiTrainer : public TrainerBase {
 public:
  MultiTrainer() {}
  virtual ~MultiTrainer() {}
  virtual void Initialize(const TrainerDesc& trainer_desc, Dataset* data_set);
  virtual void InitTrainerEnv(const ProgramDesc& main_program,
                              const platform::Place& place);
  virtual void InitOtherEnv(const ProgramDesc& main_program);
  virtual void Run();
  virtual void Finalize();
  virtual void InitDumpEnv();
  virtual Scope* GetWorkerScope(int thread_id);
  virtual std::string GetDumpPath(int tid);

  template <typename T>
  void MergeToRootScope(LoDTensor* root_tensor, LoDTensor* thread_tensor);
#ifdef PADDLE_WITH_HETERPS

  void MergeDenseParam();
#endif

 protected:
  int thread_num_;
  std::vector<std::thread> threads_;
  std::vector<DataFeed*> readers_;
  std::vector<std::shared_ptr<DeviceWorker>> workers_;
  std::vector<std::string> need_merge_var_names_;
  std::vector<std::string> trainable_param_;
#ifdef PADDLE_WITH_HETERPS
  std::vector<platform::Place> places_;
#endif
  int mpi_rank_;
  int mpi_size_;
  int dump_file_num_;
};

class DistMultiTrainer : public MultiTrainer {
 public:
  DistMultiTrainer() {}
  virtual ~DistMultiTrainer() {}
  virtual void Initialize(const TrainerDesc& trainer_desc, Dataset* data_set);
  virtual void InitTrainerEnv(const ProgramDesc& main_program,
                              const platform::Place& place);
  virtual void InitOtherEnv(const ProgramDesc& main_program);
  virtual void Run();
  virtual void Finalize();
  template <typename T>
  void MergeToRootScope(LoDTensor* root_tensor, LoDTensor* thread_tensor);
  virtual void InitDumpEnv();
  virtual Scope* GetWorkerScope(int thread_id);
  virtual void RegisterHeterCallback();

 protected:
  std::shared_ptr<paddle::framework::PullDenseWorker> pull_dense_worker_;
};

#if (defined PADDLE_WITH_CUDA || defined PADDLE_WITH_HIP || \
     defined PADDLE_WITH_XPU) &&                            \
    (defined PADDLE_WITH_PSLIB) && (!defined(PADDLE_WITH_HETERPS))
class HeterServiceContext {
 public:
  HeterServiceContext() {}
  virtual ~HeterServiceContext() {
    for (OperatorBase* op : ops_) {
      delete op;
    }
    std::vector<OperatorBase*>().swap(ops_);
  }
  void Reset() { push_dense_status_.clear(); }
  int place_num_;
  Scope* scope_{nullptr};

#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
  gpuEvent_t event_;
#endif
  std::vector<OperatorBase*> ops_;
  std::vector<::std::future<int32_t>> push_dense_status_;
};

class HeterXpuTrainer : public TrainerBase {
 public:
  HeterXpuTrainer() {}
  virtual ~HeterXpuTrainer() {
    for (OperatorBase* op : ops_) {
      delete op;
    }
    std::vector<OperatorBase*>().swap(ops_);
  }
  virtual void Initialize(const TrainerDesc& trainer_desc, Dataset* data_set);
  virtual void InitTrainerEnv(const ProgramDesc& main_program,
                              const platform::Place& place);
  virtual void InitOtherEnv(const ProgramDesc& main_program);
  virtual void Run();
  virtual void Finalize();
  virtual void DumpWork(int tid);
  virtual void RegisterServiceHandler();
  virtual int RunTask(const HeterRequest* request, HeterResponse* response);
  virtual Scope* GetWorkerScope(int thread_id);
  virtual void CacheProgram(const ProgramDesc& main_program) {
    new (&program_) ProgramDesc(main_program);
  }
  virtual std::string GetDumpPath(int tid) { return ""; }
  virtual void InitDumpEnv() {}
  template <typename T>
#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
  void HeterMemCpy(LoDTensor* tensor,
                   LoDTensor* root_tensor,
                   const paddle::platform::Place& thread_place,
                   gpuStream_t stream);
#endif
#ifdef PADDLE_WITH_XPU
  void HeterMemCpy(LoDTensor* thread_tensor,
                   LoDTensor* root_tensor,
                   const paddle::platform::Place& thread_place);
#endif
  void CreateThreadParam(const ProgramDesc& program, int num);
  template <typename T>
  void MergeToRootScope(LoDTensor* root_tensor, LoDTensor* thread_tensor);
  int EndPass(const HeterRequest* request, HeterResponse* response);
  int StopService(const HeterRequest* request, HeterResponse* response);

 protected:
  DownpourWorkerParameter param_;
  std::map<uint64_t, std::vector<std::string>> dense_grad_names_;
  std::vector<std::string> need_merge_var_names_;
  float scale_datanorm_;
  int xpu_begin_op_index_;
  int xpu_end_op_index_;
  bool running_;
  paddle::platform::Place place_;
  std::mutex mutex_;
  ProgramDesc program_;
  std::condition_variable cond_;
  std::shared_ptr<paddle::framework::FleetWrapper> fleet_ptr_;
  std::shared_ptr<paddle::framework::HeterWrapper> heter_ptr_;
  std::shared_ptr<paddle::framework::PullDenseWorker> pull_dense_worker_;
  std::vector<OperatorBase*> ops_;
  std::vector<std::string> op_names_;
  std::vector<Scope*> place_scopes_;
  BtObjectPool<HeterServiceContext> object_pool_;
  std::vector<platform::Place> places_;
#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
  std::vector<gpuStream_t> copy_streams_;
  std::vector<gpuEvent_t> events_;
#endif
};

#endif

#if (defined PADDLE_WITH_NCCL || defined PADDLE_WITH_RCCL || \
     defined PADDLE_WITH_XPU_BKCL) &&                        \
    (defined PADDLE_WITH_PSLIB)
class PSGPUTrainer : public TrainerBase {
 public:
  PSGPUTrainer() {}
  virtual ~PSGPUTrainer() {}
  virtual void Initialize(const TrainerDesc& trainer_desc, Dataset* data_set);
  virtual void InitTrainerEnv(const ProgramDesc& main_program,
                              const platform::Place& place);
  virtual void InitOtherEnv(const ProgramDesc& main_program);
  virtual void Run();
  virtual void Finalize();
  virtual void RegisterHeterCallback();
  virtual Scope* GetWorkerScope(int thread_id);
  virtual void CacheProgram(const ProgramDesc& main_program) {
    new (&program_) ProgramDesc(main_program);
  }
  virtual std::string GetDumpPath(int tid);
  void InitDumpEnv() override;
  virtual void MergeDenseParam();

  template <typename T>
  void MergeToRootScope(LoDTensor* root_tensor, LoDTensor* thread_tensor);
  void InitializeGPUServer(const TrainerDesc& trainer_desc);

 protected:
  Dataset* dataset_;
  DownpourWorkerParameter param_;
  std::map<uint64_t, std::vector<std::string>> dense_grad_names_;
  std::vector<std::string> need_merge_var_names_;
  std::vector<std::string> trainable_param_;
  float scale_datanorm_;
  paddle::platform::Place place_;
  ProgramDesc program_;
  std::shared_ptr<paddle::framework::PullDenseWorker> pull_dense_worker_;
  std::vector<std::shared_ptr<DeviceWorker>> workers_;
  std::vector<platform::Place> places_;
  // ps-gpu
  std::vector<std::thread> threads_;
  int use_ps_gpu_;
  int thread_num_;
  int mpi_rank_;
  int mpi_size_;
  int dump_file_num_;

  // _ps_param for gpups optimizer config
  ::paddle::PSParameter _ps_param;
};
#endif

#if defined(PADDLE_WITH_NCCL) || defined(PADDLE_WITH_RCCL) || \
    defined(PADDLE_WITH_ASCEND_CL)
class PipelineTrainer : public TrainerBase {
 public:
  PipelineTrainer() {}
  ~PipelineTrainer() override {}
  void Initialize(const TrainerDesc& trainer_desc, Dataset* data_set) override;
  void InitTrainerEnv(const ProgramDesc& main_program,
                      const platform::Place& place) override;
  void InitOtherEnv(const ProgramDesc& main_program) override;
  void Run() override;
  void Finalize() override;
  virtual Scope* GetWorkerScope(int thread_id);
  void InitDumpEnv() override;
  virtual std::string GetDumpPath(int tid);
  void GetSkipVars(const ProgramDesc& main_program);

 protected:
  int num_microbatches_;
  platform::Place place_;
  std::vector<std::string> skip_vars_;
  TrainerDesc trainer_desc_;

  std::future<void> section_thread_;
  std::shared_ptr<paddle::framework::DeviceWorker> worker_;
  Scope* minibatch_scope_;
  // microbatch_scopes_: [microbatch_id]
  std::vector<Scope*> microbatch_scopes_;

  void CopyParameters(int microbatch_id,
                      const ProgramDesc& program,
                      const platform::Place& place);
};
#endif

#if defined(PADDLE_WITH_PSCORE) && defined(PADDLE_WITH_DISTRIBUTE)
class HeterPipelineTrainer : public TrainerBase {
 public:
  HeterPipelineTrainer() {}
  ~HeterPipelineTrainer() override {}
  void Initialize(const TrainerDesc& trainer_desc, Dataset* data_set) override;
  void InitTrainerEnv(const ProgramDesc& main_program,
                      const platform::Place& place) override;
  void InitOtherEnv(const ProgramDesc& main_program) override;
  void Run() override;
  void Finalize() override;
  Scope* GetWorkerScope(int thread_id) override;
  void InitDumpEnv() override;
  std::string GetDumpPath(int tid) override;
  void ResetDataset(Dataset* dataset_ptr) override;

 protected:
  int trainer_id_;             // stage_trainer_id
  std::vector<int> trainers_;  //  std::vector<int> trainers
  int thread_num_;
  std::vector<std::thread> threads_;

  int num_microbatches_;
  platform::Place place_;
  TrainerDesc trainer_desc_;

  int num_pipeline_stages_;
  int pipeline_stage_;
  std::unordered_map<int, std::shared_ptr<paddle::framework::DeviceWorker>>
      workers_;

  std::shared_ptr<
      std::unordered_map<int,
                         std::shared_ptr<::paddle::framework::BlockingQueue<
                             std::pair<std::string, int>>>>>
      task_queue_;

  platform::DeviceContext* dev_ctx_ = nullptr;

  std::shared_ptr<std::unordered_map<int, Scope*>> mini_scopes_;
  std::shared_ptr<std::unordered_map<int, std::shared_ptr<std::vector<Scope*>>>>
      micro_scopes_;

  std::unique_ptr<std::thread> listen_ptr_ = nullptr;
};
#endif

#ifdef PADDLE_WITH_BOX_PS
class BoxPSTrainer : public TrainerBase {
 public:
  BoxPSTrainer() {}
  ~BoxPSTrainer() override {}
  void Initialize(const TrainerDesc& trainer_desc, Dataset* data_set) override;
  void InitTrainerEnv(const ProgramDesc& main_program,
                      const platform::Place& place) override;
  void InitOtherEnv(const ProgramDesc& main_program) override;
  void Run() override;
  void Finalize() override;
  virtual Scope* GetWorkerScope(int thread_id);
  void InitDumpEnv() override;
  virtual std::string GetDumpPath(int tid);
  virtual void DumpWork(int tid);
  virtual void FinalizeDumpEnv();
  void RemoveOtherDeviceVars(const ProgramDesc& main_program,
                             Scope* root_scope);

 protected:
  int thread_num_;
  BoxPSWorkerParameter param_config_;

  std::vector<std::shared_ptr<paddle::framework::DeviceWorker>> workers_;
  //  std::vector<std::thread> worker_threads_;
  std::vector<std::future<void>> wait_futures_;
  std::vector<DataFeed*> readers_;
  std::vector<std::future<void>> dump_futures_;

  std::shared_ptr<std::vector<std::string>> param_need_sync_;
  std::vector<std::string> persistable_vars_;
  std::set<std::string> async_grad_name_;

  bool async_mode_ = false;
  std::shared_ptr<BoxPSAsynDenseTable> dense_table_ = nullptr;
};
#endif

}  // namespace framework
}  // namespace paddle
