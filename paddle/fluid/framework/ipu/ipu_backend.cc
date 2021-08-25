/* Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/framework/ipu/ipu_backend.h"

#include <popart/builder.hpp>
#include <popart/dataflow.hpp>
#include <popart/devicemanager.hpp>
#include <popart/names.hpp>
#include <popart/ndarraywrapper.hpp>
#include <popart/session.hpp>
#include <popart/sessionoptions.hpp>
#include <popart/stepio.hpp>
#include <popart/tensor.hpp>
#include <popart/tensorinfo.hpp>

#include "paddle/fluid/framework/framework.pb.h"
#include "paddle/fluid/framework/ipu/ipu_utils.h"
#include "paddle/fluid/framework/ir/graph.h"
#include "paddle/fluid/framework/ir/graph_helper.h"
#include "paddle/fluid/framework/ir/node.h"
#include "paddle/fluid/platform/enforce.h"

namespace paddle {
namespace framework {
namespace ipu {

std::shared_ptr<IpuBackend> IpuBackend::instance_ = nullptr;

IpuBackend::IpuBackend() {}

void IpuBackend::Compile(ir::Graph* graph,
                         const std::vector<std::string>& feed_list,
                         const std::vector<std::string>& fetch_list) {
  VLOG(1) << "-- in Compile --";
  compiler_ = std::make_shared<Compiler>(ipu_strategy_);
  compiler_->InitInputs(graph, feed_list);
  compiler_->LowerWeights(graph, scope_);
  compiler_->LowerBody(graph);
  compiler_->InitOutputs(fetch_list);
  VLOG(1) << "-- fetch_list --";
  for (const auto& fetch_name : fetch_list) {
    VLOG(1) << fetch_name;
  }
}

std::unique_ptr<popart::Optimizer> IpuBackend::GetPopartOptimizer() {
  // TODO(xiaobingw): change type_ to enum
  PADDLE_ENFORCE_NE(
      optimizer_.type_, "",
      platform::errors::InvalidArgument("Optimizer type have not been set."));
  if (optimizer_.type_ == "sgd") {
    auto optimizer = std::make_unique<popart::SGD>(
        popart::OptimizerValue(GetLRFromScope(), false),
        popart::OptimizerValue(popart::SGD::getUnsetWeightDecay()),
        popart::OptimizerValue(popart::SGD::getUnsetMomentum()),
        popart::OptimizerValue(popart::SGD::getUnsetDampening()),
        popart::OptimizerValue(popart::SGD::getUnsetVelocityScaling()),
        popart::OptimizerValue(popart::SGD::getUnsetLossScaling()));
    return optimizer;
  } else if (optimizer_.type_ == "adam") {
    auto optimizer = std::make_unique<popart::Adam>(
        popart::OptimizerValue(GetLRFromScope(), false),
        popart::OptimizerValue(popart::Adam::getUnsetWeightDecay()),
        popart::OptimizerValue(GetOptimizerAttr("beta1"), false),
        popart::OptimizerValue(GetOptimizerAttr("beta2"), false),
        popart::OptimizerValue(GetOptimizerAttr("epsilon"), false),
        popart::OptimizerValue(popart::Adam::getUnsetLossScaling()),
        popart::AdamMode::Adam, popart::WeightDecayMode::Decay,
        popart::DataType::FLOAT, popart::DataType::FLOAT,
        popart::DataType::FLOAT);
    return optimizer;
  } else {
    PADDLE_THROW(platform::errors::Unimplemented(
        "Optimizer %s is not implemented now.", optimizer_.type_));
  }
}

std::vector<int64_t> IpuBackend::GetTensorShape(const std::string& var_name) {
  auto oshape = compiler_->GetTensorShape(var_name);
  oshape.insert(oshape.begin(), ipu_strategy_->batches_per_step);
  return oshape;
}

void IpuBackend::Prepare() {
  VLOG(1) << "Get ModelProto ...\n";
  auto proto = compiler_->GetModelProto();

  // for onnx graph debug
  // std::ofstream onnxfile("paddle_model_no_check.onnx",
  // std::ios_base::binary);
  // onnxfile.write(proto.data(), proto.size());
  // onnxfile.close();

  VLOG(1) << "Save Model to file paddle_model.onnx ...\n";
  compiler_->SaveModelProto("paddle_model.onnx");

  VLOG(1) << "Constructing DataFlow\n";
  std::vector<popart::TensorId> anchor_ids;
  for (popart::TensorId item : compiler_->GetOutputs()) {
    anchor_ids.push_back(item);
  }
  auto dataFlow = popart::DataFlow(ipu_strategy_->batches_per_step, anchor_ids);

  PADDLE_ENFORCE_NOT_NULL(
      curr_device_,
      platform::errors::Unavailable("IPU device isn't attached, please call "
                                    "IpuBackend::AttachDevice(id) first."));

  if (ipu_strategy_ != nullptr && ipu_strategy_->is_training) {
    VLOG(1) << "Creating TrainingSession from Onnx Model...";
    auto popart_optimizer = GetPopartOptimizer();
    auto tensors = compiler_->GetTensors();
    auto it = tensors.find(optimizer_.loss_);
    PADDLE_ENFORCE_NE(
        it, tensors.end(),
        paddle::platform::errors::InvalidArgument(
            "loss_id = %s doesn't exist in popart graph.", optimizer_.loss_));
    session_ = popart::TrainingSession::createFromOnnxModel(
        proto, dataFlow, it->second, *popart_optimizer, curr_device_,
        popart::InputShapeInfo(), ipu_strategy_->popart_options_,
        popart::Patterns(popart::PatternsLevel::Default));
  } else {
    VLOG(1) << "Creating InferenceSession from Onnx Model...";
    session_ = popart::InferenceSession::createFromOnnxModel(
        proto, dataFlow, curr_device_, popart::InputShapeInfo(),
        ipu_strategy_->popart_options_,
        popart::Patterns(popart::PatternsLevel::Default));
  }
  VLOG(1) << "Creating session from Onnx Model...done";

  VLOG(1) << "Preparing session device...";
  session_->prepareDevice();
  VLOG(1) << "Preparing session device...done";

  VLOG(1) << "Copy weights from host to device...";
  session_->weightsFromHost();
  VLOG(1) << "Copy weights from host to device...done";
}

void IpuBackend::Run(const std::vector<const Tensor*>& inputs,
                     const std::vector<Tensor*>& outputs) {
  if (!is_prepared_) {
    Prepare();
    is_prepared_ = true;
  }

  std::map<popart::TensorId, popart::IArray&> popart_inputs;
  std::map<popart::TensorId, PaddleIArray> input_wrappers;
  auto input_tensors = compiler_->GetInputs();
  for (size_t i = 0; i < inputs.size(); i++) {
    auto tensor_id = input_tensors[i];
    auto tensor = const_cast<Tensor*>(inputs[i]);
    input_wrappers.emplace(tensor_id, PaddleIArray(tensor));
    popart_inputs.emplace(tensor_id, input_wrappers.at(tensor_id));
  }

  std::map<popart::TensorId, popart::IArray&> popart_anchors;
  std::map<popart::TensorId, PaddleIArray> anchor_wrappers;
  auto output_tensors = compiler_->GetOutputs();
  for (size_t i = 0; i < outputs.size(); i++) {
    auto tensor_id = output_tensors[i];
    auto tensor = const_cast<Tensor*>(outputs[i]);
    anchor_wrappers.emplace(tensor_id, PaddleIArray(tensor));
    popart_anchors.emplace(tensor_id, anchor_wrappers.at(tensor_id));
  }

  if (ipu_strategy_ != nullptr && ipu_strategy_->is_training) {
    VLOG(1) << "Update optimizer learning rate...";
    auto popart_optimizer = GetPopartOptimizer();
    auto session = dynamic_cast<popart::TrainingSession*>(session_.get());
    session->updateOptimizerFromHost(popart_optimizer.get());
  }

  popart::StepIO stepio(popart_inputs, popart_anchors);

  VLOG(1) << "Running...";
  session_->run(stepio);
  VLOG(1) << "Running...done";
}

float IpuBackend::GetLRFromScope() {
  auto lr_var = scope_->GetVar(optimizer_.lr_var_name_);
  auto tensor = lr_var->Get<framework::LoDTensor>();

  PADDLE_ENFORCE_EQ(tensor.type(), framework::proto::VarType::FP32,
                    platform::errors::InvalidArgument(
                        "LR requiree float, but got (%s).", tensor.type()));

  return tensor.data<float>()[0];
}

// ipu_num_ must be pow(2,n);
int IpuBackend::UpperIpuNum() {
  PADDLE_ENFORCE_GT(ipu_strategy_->num_ipus, 0,
                    platform::errors::Unavailable(
                        "The ipu num get is wrong, please make sure the "
                        "sharding or pipline parameter is right."));
  int i = 0;
  while (pow(2, i) < ipu_strategy_->num_ipus) {
    i++;
  }
  return pow(2, i);
}

size_t IpuBackend::GetNumDevices() {
  // IpuModel
  bool ipu_model = GetBoolEnv("POPLAR_IPUMODEL");
  if (ipu_model) return 1;
  // Real dev
  size_t num_devices =
      popart::DeviceManager::createDeviceManager().enumerateDevices().size();
  PADDLE_ENFORCE_GT(
      num_devices, 0,
      platform::errors::Unavailable(
          "Do not found any IPU devices, please make "
          "sure Poplar sdk is enabled or enable ENV \"POPLAR_IPUMODEL=1\""));
  return num_devices;
}

std::vector<int> IpuBackend::GetDeviceIds() {
  bool ipu_model = GetBoolEnv("POPLAR_IPUMODEL");
  if (ipu_model) {
    return {0};
  }
  std::vector<int> device_ids;
  auto devices =
      popart::DeviceManager::createDeviceManager().enumerateDevices();
  PADDLE_ENFORCE_GT(
      devices.size(), 0,
      platform::errors::Unavailable("Do not found any IPU devices, please make "
                                    "sure Poplar sdk is enabled."));

  for (auto device : devices) {
    device_ids.push_back(device->getId());
  }

  return device_ids;
}

Device IpuBackend::GetDevice(int id) {
  bool ipu_model = GetBoolEnv("POPLAR_IPUMODEL");
  if (ipu_model) {
    std::map<std::string, std::string> deviceOpts{{"numIPUs", "1 "}};
    curr_device_ =
        popart::DeviceManager::createDeviceManager().createIpuModelDevice(
            deviceOpts);
    Device device(*curr_device_.get());
    return device;
  }
  size_t num_devices = GetNumDevices();
  if (id < 0 || id >= num_devices) {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "device id %d is invalid, number devices is %d", id, num_devices));
  }
  std::shared_ptr<popart::DeviceInfo> popart_device_info =
      popart::DeviceManager::createDeviceManager().getDevice(
          popart::SyncPattern::Full, id);
  Device device(*popart_device_info.get());
  return device;
}

void IpuBackend::AttachDevice(int id) {
  // trick here
  // Compiler ipu is not same as the runtime ipu.
  VLOG(1) << "comile ipu id = " << id;
  bool ipu_model = GetBoolEnv("POPLAR_IPUMODEL");
  if (ipu_model) {
    return;
  }
  curr_device_ =
      popart::DeviceManager::createDeviceManager().acquireAvailableDevice(
          UpperIpuNum());
  PADDLE_ENFORCE_NOT_NULL(
      curr_device_, platform::errors::Unavailable(
                        "Can't attach IPU, ipu_num = %d.", UpperIpuNum()));
}

IpuBackend::~IpuBackend() {
  if (instance_ == nullptr) {
    return;
  }

  // detach device
  if (curr_device_ != nullptr && curr_device_->isAttached()) {
    curr_device_->detach();
  }
}
bool IpuBackend::DeviceIsAttached() { return curr_device_ != nullptr; }

}  // namespace ipu
}  // namespace framework
}  // namespace paddle
