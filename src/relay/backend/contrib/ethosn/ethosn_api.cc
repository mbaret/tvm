/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "ethosn_api.h"

#include <tvm/relay/attrs/nn.h>
#include <tvm/relay/expr.h>
#include <tvm/relay/expr_functor.h>
#include <tvm/relay/transform.h>
#include <tvm/tir/analysis.h>

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ethosn_support_library/Support.hpp"
#include "ethosn_support_library/SupportQueries.hpp"

namespace tvm {
namespace relay {
namespace contrib {
namespace ethosn {

EthosnError EthosnAPI::QnnConv2d(const Expr& expr, ConvolutionParams* params) {
  Call requantize = Downcast<Call>(expr);
  Call bias_add = Downcast<Call>(requantize->args[0]);
  Call conv = Downcast<Call>(bias_add->args[0]);
  Call pad;
  if (conv->args[0]->IsInstance<CallNode>() &&
      Downcast<Call>(conv->args[0])->op == Op::Get("nn.pad"))
    pad = Downcast<Call>(conv->args[0]);
  const auto& conv_attr = conv->attrs.as<Conv2DAttrs>();

  // Extract the quantization params from the arguments
  int input_zero_point;
  int kernel_zero_point;
  int output_zero_point;
  float input_scale;
  float kernel_scale;
  float output_scale;
  EthosnError err = AsConstant<int>(conv->args[2], &input_zero_point);
  err += AsConstant<int>(conv->args[3], &kernel_zero_point);
  err += AsConstant<int>(requantize->args[4], &output_zero_point);
  err += AsConstant<float>(conv->args[4], &input_scale);
  err += AsConstant<float>(conv->args[5], &kernel_scale);
  err += AsConstant<float>(requantize->args[3], &output_scale);

  // Convert quantization params
  sl::QuantizationInfo data_q_info;
  sl::QuantizationInfo weights_q_info;
  sl::QuantizationInfo bias_q_info;
  sl::QuantizationInfo output_q_info;
  err += Tvm2Npu(input_zero_point, input_scale, &data_q_info);
  err += Tvm2Npu(kernel_zero_point, kernel_scale, &weights_q_info);
  err += Tvm2Npu(0, data_q_info.m_Scale * weights_q_info.m_Scale, &bias_q_info);
  err += Tvm2Npu(output_zero_point, output_scale, &output_q_info);

  // Convert convolution attributes
  sl::Padding padding;
  if (pad.defined()) {
    Tvm2Npu(conv_attr->padding, &padding);
    // Don't support both standalone operator padding and attribute defined padding
    if (padding != sl::Padding({0, 0, 0, 0})) {
      err += EthosnError(
          ErrStrm() << "both op and attr padding exist, must be either op/attr only or no padding");
    }
    err += Tvm2Npu(pad->attrs.as<PadAttrs>()->pad_width, &padding);
  } else {
    err += Tvm2Npu(conv_attr->padding, &padding);
  }
  sl::Stride stride;
  err += Tvm2Npu(conv_attr->strides, &stride);
  // Dilation is not supported
  std::array<uint32_t, 4> dilation = {1, 1, 1, 1};
  AsArray(conv_attr->dilation, &dilation);
  if (conv_attr->dilation.size() != 2 || dilation[0] != 1 || dilation[1] != 1) {
    err +=
        EthosnError(ErrStrm() << "dilation=" << conv_attr->dilation << ", dilation must = [1, 1]");
  }
  // Create convolution info
  params->conv_info = sl::ConvolutionInfo(padding, stride, output_q_info);

  // Create data info
  const TensorTypeNode* data_dtype;
  if (pad.defined()) {
    data_dtype = pad->args[0]->checked_type().as<TensorTypeNode>();
  } else {
    data_dtype = conv->args[0]->checked_type().as<TensorTypeNode>();
  }
  sl::TensorShape activation_tensor_shape;
  sl::DataType activation_data_type;
  err += Tvm2Npu(data_dtype->shape, &activation_tensor_shape);
  err += Tvm2Npu(data_dtype->dtype, &activation_data_type);
  params->activation_info = sl::TensorInfo(activation_tensor_shape, activation_data_type,
                                           sl::DataFormat::NHWC, data_q_info);

  // Create weights info
  params->is_depthwise = conv_attr->channels.defined() &&
                         tvm::tir::ExprDeepEqual()(conv_attr->channels, conv_attr->groups) &&
                         conv_attr->groups != 1;

  const auto* weights_dtype = conv->args[1]->checked_type().as<TensorTypeNode>();
  sl::TensorShape weights_tensor_shape;
  sl::DataType weights_data_type;
  sl::DataFormat weights_data_format;
  // Ignore the error here because weights don't have a batch axis
  Tvm2Npu(weights_dtype->shape, &weights_tensor_shape);
  err += Tvm2Npu(weights_dtype->dtype, &weights_data_type);
  err += Tvm2Npu(params->is_depthwise ? "HWIM" : "HWIO", &weights_data_format);
  params->weights_info =
      sl::TensorInfo(weights_tensor_shape, weights_data_type, weights_data_format, weights_q_info);
  params->raw_weights = conv->args[1].as<ConstantNode>()->data->data;

  // Create bias info
  params->bias_info = sl::TensorInfo(
      {1, 1, 1, params->is_depthwise ? weights_tensor_shape[2] : weights_tensor_shape[3]},
      sl::DataType::INT32_QUANTIZED, sl::DataFormat::NHWC, bias_q_info);
  params->raw_bias = bias_add->args[1].as<ConstantNode>()->data->data;

  return err;
}

EthosnError EthosnAPI::Concatenate(const Expr& expr, ConcatenateParams* params) {
  Call call = Downcast<Call>(expr);
  const auto& attrs = call->attrs.as<ConcatenateAttrs>();
  params->concat_info.m_Axis = attrs->axis;

  float output_s;
  int output_zp;
  EthosnError err = AsConstant<float>(call->args[3], &output_s);
  err += AsConstant<int>(call->args[4], &output_zp);
  params->concat_info.m_OutputQuantizationInfo = sl::QuantizationInfo(output_zp, output_s);

  auto input_scales = call->args[1].as<TupleNode>()->fields;
  auto input_zero_points = call->args[2].as<TupleNode>()->fields;
  auto input_tensors = call->args[0]->checked_type().as<TupleTypeNode>()->fields;

  int index = 0;
  for (auto input_scale : input_scales) {
    auto input_dtype = input_tensors[index].as<TensorTypeNode>();
    auto input_zero_point = input_zero_points[index];
    float scale;
    int zp;
    err += AsConstant<float>(input_scale, &scale);
    err += AsConstant<int>(input_zero_point, &zp);
    sl::TensorShape input_tensor_shape = {1, 1, 1, 1};
    sl::DataType input_data_type;
    err += Tvm2Npu(input_dtype->shape, &input_tensor_shape);
    err += Tvm2Npu(input_dtype->dtype, &input_data_type);
    params->input_infos.emplace_back(sl::TensorInfo(input_tensor_shape, input_data_type,
                                                    sl::DataFormat::NHWC,
                                                    sl::QuantizationInfo(zp, scale)));
    index++;
  }
  return err;
}

EthosnError EthosnAPI::Split(const Expr& expr, SplitParams* params) {
  Call call = Downcast<Call>(expr);
  const auto* input_tensor_type = call->args[0]->checked_type().as<TensorTypeNode>();
  const auto& attrs = call->attrs.as<SplitAttrs>();

  sl::TensorShape input_tensor_shape = {1, 1, 1, 1};
  sl::DataType input_data_type;
  EthosnError err = Tvm2Npu(input_tensor_type->shape, &input_tensor_shape);
  err += Tvm2Npu(input_tensor_type->dtype, &input_data_type);
  params->input_info =
      sl::TensorInfo(input_tensor_shape, input_data_type, params->input_info.m_DataFormat,
                     params->input_info.m_QuantizationInfo);
  params->split_info.m_Axis = attrs->axis;
  if (attrs->indices_or_sections->IsInstance<IntImmNode>()) {
    auto sections = Downcast<IntImm>(attrs->indices_or_sections)->value;
    int size = input_tensor_shape[attrs->axis] / sections;
    for (int i = 0; i < sections; i++) {
      params->split_info.m_Sizes.push_back(size);
    }
  } else {
    auto indices = Downcast<tvm::Array<Integer>>(attrs->indices_or_sections);
    int last_index = 0;
    for (const auto& i : indices) {
      params->split_info.m_Sizes.push_back(i->value - last_index);
      last_index = i->value;
    }
    int axis_size = input_tensor_shape[attrs->axis];
    params->split_info.m_Sizes.push_back(axis_size - last_index);
  }
  return err;
}

EthosnError EthosnAPI::Tvm2Npu(const Array<IndexExpr>& padding, sl::Padding* npu_padding) {
  std::array<uint32_t, 4> dim;
  if (EthosnError err = AsArray<IndexExpr, uint32_t>(padding, &dim)) {
    return err;
  }
  switch (padding.size()) {
    case 1:
      *npu_padding = sl::Padding(dim[0], dim[0], dim[0], dim[0]);
      break;
    case 2:
      // Height, width -> top, bottom, left, right
      *npu_padding = sl::Padding(dim[0], dim[0], dim[1], dim[1]);
      break;
    case 4:
      // Top, left, bottom, right -> top, bottom, left, right
      *npu_padding = sl::Padding(dim[0], dim[2], dim[1], dim[3]);
      break;
    default:
      return EthosnError(ErrStrm() << "padding tuple size=" << padding.size()
                                   << ", padding tuple size must be {1, 2, 4}");
  }
  return EthosnError();
}

EthosnError EthosnAPI::Tvm2Npu(const Array<IndexExpr>& strides, sl::Stride* npu_stride) {
  if (strides.size() != 2) {
    return EthosnError(ErrStrm() << "stride size=" << strides.size() << ", stride size must = 2");
  }
  std::array<uint32_t, 4> dim;
  if (EthosnError err = AsArray<IndexExpr, uint32_t>(strides, &dim)) {
    return err;
  }
  *npu_stride = sl::Stride(dim[1], dim[0]);
  return EthosnError();
}

EthosnError EthosnAPI::Tvm2Npu(const std::string& dformat, sl::DataFormat* data_format) {
  if (dformat == "NCHW") {
    *data_format = sl::DataFormat::NCHW;
    return EthosnError();
  } else if (dformat == "NHWC") {
    *data_format = sl::DataFormat::NHWC;
    return EthosnError();
  } else if (dformat == "HWIO") {
    *data_format = sl::DataFormat::HWIO;
    return EthosnError();
  } else if (dformat == "HWIM") {
    *data_format = sl::DataFormat::HWIM;
    return EthosnError();
  }
  return EthosnError(ErrStrm() << "format=" << dformat
                               << ", format must be {NCHW, NHWC, HWIO, HWIM}");
}

EthosnError EthosnAPI::Tvm2Npu(const Array<IndexExpr>& shape, sl::TensorShape* npu_shape) {
  EthosnError err = AsArray<IndexExpr, uint32_t>(shape, npu_shape);
  if (npu_shape->front() != 1) {
    err += EthosnError(ErrStrm() << "batch size=" << npu_shape->front() << ", batch size must = 1");
  }
  return err;
}

EthosnError EthosnAPI::Tvm2Npu(const tvm::DataType& dtype, sl::DataType* data_type) {
  if (dtype.is_scalar() == 1) {
    if (dtype.is_uint() && dtype.bits() == 8) {
      *data_type = sl::DataType::UINT8_QUANTIZED;
      return EthosnError();
    } else if (dtype.is_int() && dtype.bits() == 32) {
      *data_type = sl::DataType::INT32_QUANTIZED;
      return EthosnError();
    }
  }
  return EthosnError(ErrStrm() << "dtype=\'" << dtype << "\', dtype must be either uint8 or int32");
}

EthosnError EthosnAPI::Tvm2Npu(int32_t zero_point, float scale, sl::QuantizationInfo* npu_qinfo) {
  *npu_qinfo = sl::QuantizationInfo(zero_point, scale);
  return EthosnError();
}

EthosnError EthosnAPI::Tvm2Npu(const Array<Array<IndexExpr>>& padding, sl::Padding* npu_padding) {
  if (padding.size() != 4) {
    return EthosnError(ErrStrm() << "padding tuple size=" << padding.size()
                                 << ", padding tuple size must = 4");
  }
  Array<IndexExpr> reduced_padding;
  reduced_padding.push_back(padding[1][0]);
  reduced_padding.push_back(padding[1][1]);
  reduced_padding.push_back(padding[2][0]);
  reduced_padding.push_back(padding[2][1]);
  std::array<uint32_t, 4> dim;
  if (EthosnError err = AsArray<IndexExpr, uint32_t>(reduced_padding, &dim)) {
    return err;
  }
  *npu_padding = sl::Padding(dim[0], dim[1], dim[2], dim[3]);
  return EthosnError();
}

// Convert an array of IntImmNodes into ValueT
// IndexT type of Array indexing variable
// ValueT type of resulting value
template <typename IndexT, typename ValueT>
EthosnError EthosnAPI::AsArray(const Array<IndexT>& arr, std::array<ValueT, 4>* v) {
  if (arr.size() > 4)
    return EthosnError(ErrStrm() << "dimensions=" << arr.size() << ", dimensions must be <= 4");
  for (size_t i = 0; i < std::min(arr.size(), 4ul); i++) {
    const PrimExpr& a = arr[i];
    const auto* intImm = a.as<IntImmNode>();
    if (intImm->value > std::numeric_limits<ValueT>::max()) {
      return EthosnError(ErrStrm() << "axis size=" << intImm->value << ", axis size must be <= "
                                   << std::numeric_limits<ValueT>::max());
    }
    (*v)[i] = static_cast<ValueT>(intImm->value);
  }
  return EthosnError();
}

// Get a T from a constant represented by a NDArray.
template <typename T>
EthosnError EthosnAPI::AsConstant(const Expr& expr, T* out) {
  if (!expr->IsInstance<ConstantNode>()) {
    return EthosnError("expected constant data");
  }
  runtime::NDArray data = Downcast<Constant>(expr)->data;
  *out = *static_cast<T*>(data->data);
  return EthosnError();
}

TVM_REGISTER_GLOBAL("relay.ethos-n.support.conv2d")
    .set_body([](tvm::TVMArgs args, tvm::TVMRetValue* rv) {
      Call call = args[0];
      ConvolutionParams params;
      auto err = EthosnAPI::QnnConv2d(call, &params);
      if (params.is_depthwise) {
        *rv = !err && sl::IsDepthwiseConvolutionSupported(params.bias_info, params.weights_info,
                                                          params.conv_info, params.activation_info);
      } else {
        *rv = !err && sl::IsConvolutionSupported(params.bias_info, params.weights_info,
                                                 params.conv_info, params.activation_info);
      }
    });

TVM_REGISTER_GLOBAL("relay.ethos-n.support.concatenate")
    .set_body([](tvm::TVMArgs args, tvm::TVMRetValue* rv) {
      Call call = args[0];
      ConcatenateParams params;
      auto err = EthosnAPI::Concatenate(call, &params);
      *rv = !err && sl::IsConcatenationSupported(params.input_infos, params.concat_info);
    });

TVM_REGISTER_GLOBAL("relay.ethos-n.support.split")
    .set_body([](tvm::TVMArgs args, tvm::TVMRetValue* rv) {
      Call call = args[0];
      SplitParams params;
      auto err = EthosnAPI::Split(call, &params);
      *rv = !err && sl::IsSplitSupported(params.input_info, params.split_info);
    });

TVM_REGISTER_GLOBAL("relay.ethos-n.query").set_body([](tvm::TVMArgs args, tvm::TVMRetValue* rv) {
#if defined ETHOSN_HW
  *rv = true;
#else
  *rv = false;
#endif
});

}  // namespace ethosn
}  // namespace contrib
}  // namespace relay
}  // namespace tvm
