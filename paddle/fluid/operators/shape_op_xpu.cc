/* Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License. */

#ifdef PADDLE_WITH_XPU
#include <algorithm>

#include "paddle/fluid/framework/op_registry.h"

namespace paddle {
namespace operators {

using Tensor = framework::Tensor;
using LoDTensor = framework::LoDTensor;
using SelectedRows = phi::SelectedRows;

template <typename T>
class ShapeXPUKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    auto* in_var = ctx.InputVar("Input");
    framework::DDim in_dims;
    if (in_var->IsType<phi::SelectedRows>()) {
      in_dims = in_var->Get<phi::SelectedRows>().value().dims();
    } else {
      in_dims = in_var->Get<LoDTensor>().dims();
    }
    auto* out_t = ctx.Output<Tensor>("Out");
    out_t->Resize({in_dims.size()});
    auto out_data = out_t->mutable_data<int32_t>(platform::CPUPlace());
    for (int i = 0; i < in_dims.size(); ++i) {
      out_data[i] = in_dims[i];
    }
  }
};
}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;
REGISTER_OP_XPU_KERNEL(shape,
                       ops::ShapeXPUKernel<bool>,
                       ops::ShapeXPUKernel<int>,
                       ops::ShapeXPUKernel<int64_t>,
                       ops::ShapeXPUKernel<float>,
                       ops::ShapeXPUKernel<double>);

#endif
