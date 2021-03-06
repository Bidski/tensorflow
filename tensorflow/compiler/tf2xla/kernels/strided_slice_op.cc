/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/util/strided_slice_op.h"

#include <vector>

#include "absl/algorithm/container.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/tf2xla/literal_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/tf2xla/xla_helpers.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/compiler/xla/client/lib/constants.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/ops_util.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/mem.h"

namespace tensorflow {
namespace {
using errors::InvalidArgument;

class StridedSliceOp : public XlaOpKernel {
 public:
  explicit StridedSliceOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("begin_mask", &begin_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("end_mask", &end_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("ellipsis_mask", &ellipsis_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("new_axis_mask", &new_axis_mask_));
    OP_REQUIRES_OK(ctx,
                   ctx->GetAttribute("shrink_axis_mask", &shrink_axis_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("Index", &index_type_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    const TensorShape input_shape = ctx->InputShape(0);
    const TensorShape begin_shape = ctx->InputShape("begin");
    OP_REQUIRES(
        ctx, begin_shape.dims() == 1,
        errors::InvalidArgument("'begin' input has to be a rank 1 vector"));

    absl::InlinedVector<int64, 4> begin;
    absl::InlinedVector<int64, 4> end;
    absl::InlinedVector<int64, 4> strides;

    xla::Literal begin_literal, end_literal, strides_literal;
    bool begin_is_constant = ctx->ConstantInput(1, &begin_literal).ok();
    bool end_is_constant = ctx->ConstantInput(2, &end_literal).ok();

    OP_REQUIRES_OK(ctx, ctx->ConstantInput(3, &strides_literal));

    Tensor begin_tensor, end_tensor, strides_tensor;
    if (begin_is_constant) {
      OP_REQUIRES_OK(
          ctx, LiteralToHostTensor(begin_literal, index_type_, &begin_tensor));
    }
    if (end_is_constant) {
      OP_REQUIRES_OK(
          ctx, LiteralToHostTensor(end_literal, index_type_, &end_tensor));
    }
    OP_REQUIRES_OK(ctx, LiteralToHostTensor(strides_literal, index_type_,
                                            &strides_tensor));

    TensorShape final_shape;
    PartialTensorShape dummy_processing_shape, partial_final_shape;
    bool dummy = false;
    absl::InlinedVector<int64, 4> output_to_sparse_mapping;
    absl::InlinedVector<int64, 4> output_to_processing_mapping;
    OP_REQUIRES_OK(
        ctx,
        ValidateStridedSliceOp(
            begin_is_constant ? &begin_tensor : nullptr,
            end_is_constant ? &end_tensor : nullptr, strides_tensor,
            input_shape, begin_mask_, end_mask_, ellipsis_mask_, new_axis_mask_,
            shrink_axis_mask_, &dummy_processing_shape, &partial_final_shape,
            &dummy, &dummy, &dummy, &begin, &end, &strides,
            &output_to_sparse_mapping, &output_to_processing_mapping));

    OP_REQUIRES(
        ctx, partial_final_shape.AsTensorShape(&final_shape),
        InvalidArgument("XLA can't deduce compile time constant output "
                        "shape for strided slice: ",
                        partial_final_shape.DebugString(),
                        ", output shape must be a compile-time constant"));

    xla::XlaOp slice = ctx->Input(0);
    if (begin_is_constant && end_is_constant) {
      absl::InlinedVector<int64, 4> dimensions_to_reverse;
      absl::InlinedVector<int64, 4> slice_begin, slice_end, slice_strides;
      for (int i = 0; i < begin.size(); ++i) {
        if (strides[i] > 0) {
          slice_begin.push_back(begin[i]);
          slice_end.push_back(std::max(end[i], begin[i]));
          slice_strides.push_back(strides[i]);
        } else {
          // Negative stride: swap begin and end, add 1 because the interval
          // is semi-open, and mark the dimension to be reversed.
          slice_begin.push_back(input_shape.dim_size(i) - begin[i] - 1);
          slice_end.push_back(std::max(input_shape.dim_size(i) - end[i] - 1,
                                       input_shape.dim_size(i) - begin[i] - 1));
          slice_strides.push_back(-strides[i]);
          dimensions_to_reverse.push_back(i);
        }
      }
      if (!dimensions_to_reverse.empty()) {
        slice = xla::Rev(slice, dimensions_to_reverse);
      }
      slice = xla::Slice(slice, slice_begin, slice_end, slice_strides);
      auto operand_shape_or = ctx->builder()->GetShape(ctx->Input(0));
      OP_REQUIRES_OK(ctx, operand_shape_or.status());
      xla::Shape xla_shape = operand_shape_or.ValueOrDie();
      std::vector<bool> begins_are_dynamic;
      OP_REQUIRES_OK(
          ctx, ctx->ResolveInputDynamismIntoPredVector(1, &begins_are_dynamic));
      std::vector<bool> ends_are_dynamic;
      OP_REQUIRES_OK(
          ctx, ctx->ResolveInputDynamismIntoPredVector(2, &ends_are_dynamic));
      bool begins_are_static = absl::c_all_of(
          begins_are_dynamic, [](bool dynamic) { return !dynamic; });
      OP_REQUIRES(ctx, begins_are_static,
                  errors::InvalidArgument(
                      "XLA can't use dynamic begin values for slice."));
      bool ends_are_static = absl::c_all_of(
          ends_are_dynamic, [](bool dynamic) { return !dynamic; });
      // Static output shape, return a static slice.
      slice = xla::Reshape(slice, final_shape.dim_sizes());
      if (xla_shape.is_static() && ends_are_static) {
        ctx->SetOutput(0, slice);
        return;
      }

      for (int64 i = 0; i < final_shape.dims(); ++i) {
        int64 input_index = output_to_processing_mapping[i];
        if (input_index == -1) {
          continue;
        }
        bool input_is_dynamic = xla_shape.is_dynamic_dimension(input_index);

        int64 sparse_index = output_to_sparse_mapping[i];
        bool end_is_dynamic =
            sparse_index == -1 ? false : ends_are_dynamic[sparse_index];
        bool backward_slice = sparse_index == -1
                                  ? false
                                  : end_literal.Get<int32>({sparse_index}) < 0;
        if ((input_is_dynamic && backward_slice) || end_is_dynamic) {
          OP_REQUIRES(
              ctx, strides[input_index] == 1,
              errors::InvalidArgument("XLA has not implemented dynamic "
                                      "sized slice with non-trival stride yet. "
                                      "Please file a bug against XLA"));
          // If there is a dynamic dimension, properly set dimension size of
          // the result.
          auto operand_size = xla::GetDimensionSize(ctx->Input(0), input_index);
          if (backward_slice) {
            // We consider slicing a dynamic tensor t with negative indices as
            // a dynamic sized slice. E.g., t[: -n], the result length is
            // shape(t) - n.
            OP_REQUIRES(ctx, !end_is_dynamic,
                        errors::InvalidArgument(
                            "XLA has not implemented dynamic "
                            "sized slice with dynamic negative index %lld. "));
            operand_size = xla::Add(
                operand_size,
                xla::ConstantR0<int32>(ctx->builder(),
                                       end_literal.Get<int32>({sparse_index})));
          } else {
            // The end of slice with dynamic slice size is the min of operand
            // shape and slice size. E.g., t[:end_size], result size is
            // min(shape(t), end_size).
            xla::XlaOp end_size;
            if (end_is_dynamic) {
              end_size = xla::Reshape(xla::Slice(ctx->Input(2), {sparse_index},
                                                 {sparse_index + 1}, {1}),
                                      {});
            } else {
              end_size =
                  xla::ConstantR0<int32>(ctx->builder(), end[input_index]);
            }
            operand_size = xla::Min(operand_size, end_size);
          }
          slice = xla::SetDimensionSize(
              slice,
              xla::Sub(operand_size, xla::ConstantR0<int32>(
                                         ctx->builder(), begin[input_index])),
              i);
        }
      }
      ctx->SetOutput(0, slice);
      return;
    } else {
      // When output shape is fully defined, it must be a size one slice:
      //
      // 1. The number of output elements has to be equal to the number of input
      // elements that are sliced.
      // 2. The stride of the slice dimensions must be exact one.
      int64 output_elements = final_shape.num_elements();

      int64 input_elements_sliced = 1;
      int64 slicing_dim_size = begin_shape.dim_size(0);
      // We only support slicing major dimensions, so minor dimensions after
      // slicing dimension are all sliced with their full sizes.
      for (int64 d = slicing_dim_size; d < input_shape.dims(); ++d) {
        input_elements_sliced *= input_shape.dim_size(d);
      }

      OP_REQUIRES(ctx, output_elements == input_elements_sliced,
                  errors::InvalidArgument(
                      "Dynamic indices of strided_slice_op have to be leading "
                      "dimensions in the indices list."));

      for (int64 i = 0; i < ctx->InputShape("begin").dims(); ++i) {
        OP_REQUIRES(
            ctx, strides[i] == 1,
            errors::InvalidArgument(
                "Strides have to be one when inputs are not constant."));
      }

      // When inputs are not compile time constants, shape inference can only
      // inference size 1 slice.
      std::vector<int64> slice_sizes(slicing_dim_size, 1);
      std::vector<xla::XlaOp> start_indices;
      auto zero = xla::Zero(ctx->builder(), ctx->InputXlaType("begin"));
      for (int64 d = 0; d < slicing_dim_size; ++d) {
        auto index = xla::Slice(ctx->Input("begin"), {d}, {d + 1}, {1});
        // Convert index to scalar.
        index = xla::Reshape(index, {});
        // Negative index: wrap it around with dimension size.
        auto index_negative = xla::Lt(index, zero);
        auto dim_size = xla::ConvertElementType(
            xla::ConstantR0<int32>(ctx->builder(), input_shape.dim_size(d)),
            ctx->InputXlaType("begin"));
        auto wrapped_index = xla::Add(dim_size, index);
        index = xla::Select(index_negative, wrapped_index, index);
        start_indices.push_back(index);
      }

      for (int64 d = slicing_dim_size; d < input_shape.dims(); ++d) {
        // For non-slice dims, naturally we get the full slice starting from 0.
        slice_sizes.push_back(input_shape.dim_size(d));
        start_indices.push_back(zero);
      }

      std::vector<int64> output_shape_dim_sizes;
      slice = xla::DynamicSlice(slice, start_indices, slice_sizes);
      slice = xla::Reshape(slice, final_shape.dim_sizes());
      ctx->SetOutput(0, slice);
    }
  }

 private:
  int32 begin_mask_, end_mask_;
  int32 ellipsis_mask_, new_axis_mask_, shrink_axis_mask_;
  DataType index_type_;
};

REGISTER_XLA_OP(Name("StridedSlice")
                    .CompileTimeConstantInput("begin")
                    .CompileTimeConstantInput("end")
                    .CompileTimeConstantInput("strides"),
                StridedSliceOp);

class StridedSliceGradOp : public XlaOpKernel {
 public:
  explicit StridedSliceGradOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("begin_mask", &begin_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("end_mask", &end_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("ellipsis_mask", &ellipsis_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("new_axis_mask", &new_axis_mask_));
    OP_REQUIRES_OK(ctx,
                   ctx->GetAttribute("shrink_axis_mask", &shrink_axis_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("Index", &index_type_));
  }

  // When the begin / end is unknown, compile the gradient into dynamic update
  // slice into a broadcasted 0s.
  //
  //    Broadcasted 0
  // +----------------------+
  // |         +----+       |
  // |<-begin->|grad|<-end->| <== Dynamic update grad into 0s.
  // |         +----+       |
  // +----------------------+
  void CompileAsDynamicUpdateSlice(XlaOpKernelContext* ctx,
                                   const TensorShape& input_shape,
                                   const xla::Literal& strides_literal) {
    bool dummy = false;
    Tensor strides_tensor;
    PartialTensorShape processing_shape, final_shape;
    absl::InlinedVector<int64, 4> begin;
    absl::InlinedVector<int64, 4> end;
    absl::InlinedVector<int64, 4> strides;

    absl::InlinedVector<int64, 4> output_to_sparse_mapping;
    absl::InlinedVector<int64, 4> output_to_processing_mapping;

    OP_REQUIRES_OK(ctx, LiteralToHostTensor(strides_literal, index_type_,
                                            &strides_tensor));
    OP_REQUIRES_OK(
        ctx, ValidateStridedSliceOp(
                 nullptr, nullptr, strides_tensor, input_shape, begin_mask_,
                 end_mask_, ellipsis_mask_, new_axis_mask_, shrink_axis_mask_,
                 &processing_shape, &final_shape, &dummy, &dummy, &dummy,
                 &begin, &end, &strides, &output_to_sparse_mapping,
                 &output_to_processing_mapping));
    for (int64 i = 0; i < processing_shape.dims(); ++i) {
      OP_REQUIRES(
          ctx, strides[i] == 1,
          errors::InvalidArgument("Strides in strided slice grad have to be "
                                  "one when inputs are not constant."));
    }

    auto zero = XlaHelpers::Zero(ctx->builder(), ctx->expected_output_dtype(0));
    zero = xla::Broadcast(zero, input_shape.dim_sizes());
    xla::XlaOp grad = ctx->Input(4);
    xla::Shape grad_shape = ctx->InputXlaShape(4).ValueOrDie();
    // Undo any new/shrink axes.
    VLOG(1) << "xla grad shape" << grad_shape;
    VLOG(1) << "input_shape" << input_shape.DebugString();
    std::vector<xla::XlaOp> begins(processing_shape.dims(),
                                   xla::Zero(ctx->builder(), xla::S32));
    for (int64 i = 0; i < grad_shape.rank(); ++i) {
      // Use grad shape, which is known, to update unknown processing shape.
      // Grad shape is the output of the ValidateStridedSliceOp function in
      // forward pass, thus we use output_to_processing_mapping.
      if (output_to_processing_mapping[i] != -1) {
        processing_shape.set_dim(output_to_processing_mapping[i],
                                 grad_shape.dimensions(i));
      }

      // Similarly, use output_to_sparse_mapping to find out corresponding
      // begin dim of the output, as indices for dynamic update slice.
      int64 begin_dim = output_to_sparse_mapping[i];
      if (begin_dim != -1) {
        auto begin_index =
            xla::Slice(ctx->Input(1), {begin_dim}, {begin_dim + 1}, {1});
        auto begin_index_scalar = xla::Reshape(
            xla::ShapeUtil::MakeScalarShape(xla::S32), begin_index);
        begins[output_to_sparse_mapping[i]] = begin_index_scalar;
      }
    }
    VLOG(1) << "processing_shape" << processing_shape.DebugString();
    TensorShape full_processing_shape;
    OP_REQUIRES(ctx, processing_shape.AsTensorShape(&full_processing_shape),
                errors::InvalidArgument(
                    "Processing shape ", processing_shape.DebugString(),
                    " can't be fully inferred from grad shape"));
    grad = xla::Reshape(grad, full_processing_shape.dim_sizes());
    grad = xla::DynamicUpdateSlice(zero, grad, begins);
    ctx->SetOutput(0, grad);
  }
  void Compile(XlaOpKernelContext* ctx) override {
    TensorShape processing_shape, final_shape;
    absl::InlinedVector<int64, 4> begin;
    absl::InlinedVector<int64, 4> end;
    absl::InlinedVector<int64, 4> strides;

    TensorShape input_shape;
    OP_REQUIRES_OK(ctx, ctx->ConstantInputAsShape(0, &input_shape));
    xla::Literal begin_literal, end_literal, strides_literal;

    bool begin_is_constant = ctx->ConstantInput(1, &begin_literal).ok();
    bool end_is_constant = ctx->ConstantInput(2, &end_literal).ok();
    OP_REQUIRES_OK(ctx, ctx->ConstantInput(3, &strides_literal));
    if (!(begin_is_constant && end_is_constant)) {
      CompileAsDynamicUpdateSlice(ctx, input_shape, strides_literal);
      return;
    }
    Tensor begin_tensor, end_tensor, strides_tensor;
    OP_REQUIRES_OK(
        ctx, LiteralToHostTensor(begin_literal, index_type_, &begin_tensor));
    OP_REQUIRES_OK(ctx,
                   LiteralToHostTensor(end_literal, index_type_, &end_tensor));
    OP_REQUIRES_OK(ctx, LiteralToHostTensor(strides_literal, index_type_,
                                            &strides_tensor));

    bool dummy = false;
    OP_REQUIRES_OK(
        ctx, ValidateStridedSliceOp(
                 &begin_tensor, &end_tensor, strides_tensor, input_shape,
                 begin_mask_, end_mask_, ellipsis_mask_, new_axis_mask_,
                 shrink_axis_mask_, &processing_shape, &final_shape, &dummy,
                 &dummy, &dummy, &begin, &end, &strides));

    // Check to make sure dy is consistent with the original slice
    const TensorShape dy_shape = ctx->InputShape(4);
    OP_REQUIRES(
        ctx, final_shape == dy_shape,
        errors::InvalidArgument("shape of dy was ", dy_shape.DebugString(),
                                " instead of ", final_shape.DebugString()));

    OP_REQUIRES(
        ctx, input_shape.dims() == processing_shape.dims(),
        errors::Internal(
            "input shape and processing shape must have same number of dims"));

    auto zero = XlaHelpers::Zero(ctx->builder(), ctx->expected_output_dtype(0));

    xla::XlaOp grad = ctx->Input(4);

    // Undo any new/shrink axes.
    grad = xla::Reshape(grad, processing_shape.dim_sizes());

    // Pad the input gradients.
    absl::InlinedVector<int64, 4> dimensions_to_reverse;
    xla::PaddingConfig padding_config;

    for (int i = 0; i < processing_shape.dims(); ++i) {
      auto* dims = padding_config.add_dimensions();
      if (strides[i] > 0) {
        dims->set_edge_padding_low(begin[i]);
        dims->set_interior_padding(strides[i] - 1);

        // Pad the upper dimension up to the expected input shape. (It's
        // not sufficient simply to use "end[i]" to compute the padding in
        // cases where the stride does not divide evenly into the interval
        // between begin[i] and end[i].)
        int64 size =
            dims->edge_padding_low() + processing_shape.dim_size(i) +
            (processing_shape.dim_size(i) - 1) * dims->interior_padding();
        dims->set_edge_padding_high(input_shape.dim_size(i) - size);
      } else {
        dimensions_to_reverse.push_back(i);
        dims->set_edge_padding_high(input_shape.dim_size(i) - begin[i] - 1);
        dims->set_interior_padding(-strides[i] - 1);

        // Pad the lower dimension up to the expected input shape.
        int64 size =
            dims->edge_padding_high() + processing_shape.dim_size(i) +
            (processing_shape.dim_size(i) - 1) * dims->interior_padding();
        dims->set_edge_padding_low(input_shape.dim_size(i) - size);
      }
    }
    if (!dimensions_to_reverse.empty()) {
      grad = xla::Rev(grad, dimensions_to_reverse);
    }
    grad = xla::Pad(grad, zero, padding_config);

    xla::XlaOp dynamic_shape = ctx->Input(0);
    xla::Shape grad_shape = ctx->builder()->GetShape(grad).ValueOrDie();
    ctx->set_dynamic_dimension_is_minus_one(true);
    std::vector<int64> dynamic_size;
    OP_REQUIRES_OK(ctx, ctx->ConstantInputAsIntVector(0, &dynamic_size));
    // Input of strided_slice_op has to have the same shape as output.
    DCHECK_EQ(grad_shape.rank(), input_shape.dims());
    for (int64 dim = 0; dim < input_shape.dims(); ++dim) {
      DCHECK_EQ(grad_shape.dimensions(dim), input_shape.dim_size(dim));
      if (dynamic_size[dim] == -1) {
        // Input is a dynamic dimension, set the same dynamic dimension size in
        // the output.
        auto dim_size = xla::Slice(dynamic_shape, {dim}, {dim + 1}, {1});
        auto dim_size_scalar =
            xla::Reshape(xla::ShapeUtil::MakeScalarShape(xla::S32), dim_size);
        grad = xla::SetDimensionSize(grad, dim_size_scalar, dim);
      } else if (grad_shape.is_dynamic_dimension(dim)) {
        // Input is static but output is dynamic, respect input and remove any
        // dynamic dim in the output.
        grad = xla::RemoveDynamicDimension(grad, dim);
      }
    }

    ctx->SetOutput(0, grad);
  }

 private:
  int32 begin_mask_, end_mask_;
  int32 ellipsis_mask_, new_axis_mask_, shrink_axis_mask_;
  DataType index_type_;
};

REGISTER_XLA_OP(Name("StridedSliceGrad")
                    .CompileTimeConstantInput("shape")
                    .CompileTimeConstantInput("begin")
                    .CompileTimeConstantInput("end")
                    .CompileTimeConstantInput("strides"),
                StridedSliceGradOp);

class StridedSliceAssignOp : public XlaOpKernel {
 public:
  explicit StridedSliceAssignOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("begin_mask", &begin_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("end_mask", &end_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("ellipsis_mask", &ellipsis_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("new_axis_mask", &new_axis_mask_));
    OP_REQUIRES_OK(ctx,
                   ctx->GetAttribute("shrink_axis_mask", &shrink_axis_mask_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("Index", &index_type_));
    OP_REQUIRES_OK(ctx, ctx->GetAttribute("T", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    TensorShape final_shape;
    absl::InlinedVector<int64, 4> begin;
    absl::InlinedVector<int64, 4> end;
    absl::InlinedVector<int64, 4> strides;

    xla::Literal begin_literal, end_literal, strides_literal;
    OP_REQUIRES_OK(ctx, ctx->ConstantInput(1, &begin_literal));
    OP_REQUIRES_OK(ctx, ctx->ConstantInput(2, &end_literal));
    OP_REQUIRES_OK(ctx, ctx->ConstantInput(3, &strides_literal));

    Tensor begin_tensor, end_tensor, strides_tensor;
    OP_REQUIRES_OK(
        ctx, LiteralToHostTensor(begin_literal, index_type_, &begin_tensor));
    OP_REQUIRES_OK(ctx,
                   LiteralToHostTensor(end_literal, index_type_, &end_tensor));
    OP_REQUIRES_OK(ctx, LiteralToHostTensor(strides_literal, index_type_,
                                            &strides_tensor));

    TensorShape lhs_shape;
    xla::XlaOp lhs;
    if (ctx->input_type(0) == DT_RESOURCE) {
      OP_REQUIRES_OK(ctx, ctx->ReadVariableInput(0, dtype_, &lhs_shape, &lhs));
    } else {
      lhs_shape = ctx->InputShape(0);
      lhs = ctx->Input(0);
    }

    const TensorShape rhs_shape = ctx->InputShape(4);

    TensorShape dummy_processing_shape;
    bool dummy = false;
    OP_REQUIRES_OK(ctx,
                   ValidateStridedSliceOp(
                       &begin_tensor, &end_tensor, strides_tensor, lhs_shape,
                       begin_mask_, end_mask_, ellipsis_mask_, new_axis_mask_,
                       shrink_axis_mask_, &dummy_processing_shape, &final_shape,
                       &dummy, &dummy, &dummy, &begin, &end, &strides));

    if (final_shape.num_elements() == 0 && rhs_shape.num_elements() == 0) {
      // DynamicUpdateSlice does not allow 0-element updates. We should probably
      // check that rhs_shape can be broadcast to final_shape, but that is
      // probably better handled when implementing broadcasting more generally.
      return;
    }

    // TODO(aselle): This check is too strong, we only should need
    // input_shape to be broadcastable to final_shape
    OP_REQUIRES(ctx, final_shape == rhs_shape,
                errors::Unimplemented(
                    "sliced l-value shape ", final_shape.DebugString(),
                    " does not match r-value shape ", rhs_shape.DebugString(),
                    ". Automatic broadcasting not yet implemented."));

    xla::XlaOp rhs = ctx->Input(4);

    absl::InlinedVector<int64, 4> dimensions_to_reverse;
    absl::InlinedVector<xla::XlaOp, 4> slice_begin;
    absl::InlinedVector<int64, 4> slice_dims;
    for (int i = 0; i < begin.size(); ++i) {
      // TODO(b/121179231): implement strides != 1
      OP_REQUIRES(
          ctx, strides[i] == 1 || strides[i] == -1,
          errors::Unimplemented("Strides != 1 or -1 are not yet implemented"));
      if (strides[i] > 0) {
        slice_begin.push_back(xla::ConstantR0<int64>(ctx->builder(), begin[i]));
        slice_dims.push_back(end[i] - begin[i]);
      } else {
        // Negative stride: swap begin and end, add 1 because the interval
        // is semi-open, and mark the dimension to be reversed.
        slice_begin.push_back(
            xla::ConstantR0<int64>(ctx->builder(), end[i] + 1));
        slice_dims.push_back(begin[i] - end[i]);
        dimensions_to_reverse.push_back(i);
      }
    }

    if (!dimensions_to_reverse.empty()) {
      rhs = xla::Rev(rhs, dimensions_to_reverse);
    }
    rhs = xla::Reshape(rhs, slice_dims);

    lhs = xla::DynamicUpdateSlice(lhs, rhs, slice_begin);

    if (ctx->input_type(0) == DT_RESOURCE) {
      OP_REQUIRES_OK(ctx, ctx->AssignVariable(0, dtype_, lhs));
    } else {
      ctx->SetOutput(0, lhs);
    }
  }

 private:
  int32 begin_mask_, end_mask_;
  int32 ellipsis_mask_, new_axis_mask_, shrink_axis_mask_;
  DataType index_type_;
  DataType dtype_;
};

REGISTER_XLA_OP(Name("ResourceStridedSliceAssign")
                    .CompileTimeConstantInput("begin")
                    .CompileTimeConstantInput("end")
                    .CompileTimeConstantInput("strides"),
                StridedSliceAssignOp);

REGISTER_XLA_OP(Name("TensorStridedSliceUpdate")
                    .CompileTimeConstantInput("begin")
                    .CompileTimeConstantInput("end")
                    .CompileTimeConstantInput("strides"),
                StridedSliceAssignOp);

}  // namespace
}  // namespace tensorflow
