// Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/framework/ipu/popart_canonicalization/canonicalization_utils.h"

namespace paddle {
namespace framework {
namespace ipu {

// This avoids the static initialisation order fiasco,
std::unordered_map<std::string, SymbolHandler> &SymbolHandlers() {
  static std::unordered_map<std::string, SymbolHandler> symbol_handlers;
  return symbol_handlers;
}

bool RegisterHandler(const std::string &symbol, const SymbolHandler &handler) {
  if (SymbolHandlers().count(symbol) != 0) {
    LOG(WARNING) << "Trying to register popart handler twice for operator: "
                 << symbol;
    return false;
  }
  bool new_handler = SymbolHandlers().emplace(symbol, handler).second;
  return new_handler;
}

// Return a pointer to a handler if one is registered for this kind of node or
// an empty std::function otherwise.
SymbolHandler GetHandler(const std::string &kind) {
  auto it = SymbolHandlers().find(kind);
  if (it != SymbolHandlers().end()) {
    return it->second;
  }
  return {};
}

void ConnectNodes(ir::Node *first_node, ir::Node *next_node) {
  first_node->outputs.push_back(next_node);
  next_node->inputs.push_back(first_node);
}

void DisConnectNodes(Node *first_node, Node *next_node) {
  auto rm_by_value = [&](std::vector<Node *> &vec, Node *n) {
    vec.erase(std::remove(vec.begin(), vec.end(), n), vec.end());
  };
  rm_by_value(first_node->outputs, next_node);
  rm_by_value(next_node->inputs, first_node);
  rm_by_value(first_node->inputs, next_node);
  rm_by_value(next_node->outputs, first_node);
}

void ClearNode(Node *node) {
  auto rm_by_value = [&](std::vector<Node *> &vec, Node *n) {
    vec.erase(std::remove(vec.begin(), vec.end(), n), vec.end());
  };
  for (auto *node_in : node->inputs) {
    rm_by_value(node_in->outputs, node);
  }
  for (auto *node_out : node->outputs) {
    rm_by_value(node_out->inputs, node);
  }
}

void CopyOpAttr(const std::string &attr_name, OpDesc *op, OpDesc *new_op,
                bool override) {
  if (new_op->HasAttr(attr_name) && !override) {
    return;
  }
  if (op->HasAttr(attr_name)) {
    new_op->SetAttr(attr_name, op->GetAttr(attr_name));
    new_op->Flush();
  }
}

const int VarType2OnnxDtype(const int type) {
  auto dtype = static_cast<proto::VarType::Type>(type);
  switch (dtype) {
    case proto::VarType::BOOL:
      return static_cast<int>(ONNXDataType::BOOL);
    case proto::VarType::INT16:
      return static_cast<int>(ONNXDataType::INT16);
    case proto::VarType::INT32:
      return static_cast<int>(ONNXDataType::INT32);
    case proto::VarType::INT64:
      return static_cast<int>(ONNXDataType::INT64);
    case proto::VarType::FP16:
      return static_cast<int>(ONNXDataType::FLOAT16);
    case proto::VarType::FP32:
      return static_cast<int>(ONNXDataType::FLOAT);
    case proto::VarType::FP64:
      return static_cast<int>(ONNXDataType::DOUBLE);
    case proto::VarType::UINT8:
      return static_cast<int>(ONNXDataType::UINT8);
    case proto::VarType::INT8:
      return static_cast<int>(ONNXDataType::INT8);
    case proto::VarType::BF16:
      return static_cast<int>(ONNXDataType::BFLOAT16);
    case proto::VarType::COMPLEX64:
      return static_cast<int>(ONNXDataType::COMPLEX64);
    case proto::VarType::COMPLEX128:
      return static_cast<int>(ONNXDataType::COMPLEX128);
    default:
      PADDLE_THROW(
          platform::errors::Unimplemented("Unsupported data type: %d.", dtype));
  }
}

const std::string VarType2PopStr(const int type) {
  auto dtype = static_cast<proto::VarType::Type>(type);
  switch (dtype) {
    case proto::VarType::UINT8:
      return "UINT8";
    case proto::VarType::INT8:
      return "INT8";
    case proto::VarType::INT16:
      return "INT16";
    case proto::VarType::INT32:
      return "INT32";
    case proto::VarType::INT64:
      return "INT64";
    case proto::VarType::BOOL:
      return "BOOL";
    case proto::VarType::FP64:
      return "DOUBLE";
    case proto::VarType::FP32:
      return "FLOAT";
    case proto::VarType::FP16:
      return "FLOAT16";
    default:
      PADDLE_THROW(
          paddle::platform::errors::Unavailable("Unsupported data type."));
  }
}

Node *GetInputNode(const std::string &name, const Node *node, const int id) {
  auto node_name = node->Op()->Input(name).at(id);
  for (auto *n : node->inputs) {
    if (n->Name() == node_name) {
      return n;
    }
  }
  return nullptr;
}

Node *GetOutputNode(const std::string &name, const Node *node, const int id) {
  auto node_name = node->Op()->Output(name).at(id);
  for (auto *n : node->outputs) {
    if (n->Name() == node_name) {
      return n;
    }
  }
  return nullptr;
}

std::vector<int64_t> GetInputNodeShape(const std::string &name,
                                       const Node *op_node, const int id) {
  auto input_node = GetInputNode(name, op_node, id);
  return op_node->Op()->Block()->FindVar(input_node->Name())->GetShape();
}

std::vector<int64_t> GetOutputNodeShape(const std::string &name,
                                        const Node *op_node, const int id) {
  auto output_node = GetOutputNode(name, op_node, id);
  return op_node->Op()->Block()->FindVar(output_node->Name())->GetShape();
}

}  // namespace ipu
}  // namespace framework
}  // namespace paddle
