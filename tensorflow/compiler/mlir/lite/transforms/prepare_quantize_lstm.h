/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

// Transform pass for LSTMs.

#ifndef TENSORFLOW_COMPILER_MLIR_LITE_TRANSFORMS_PREPARE_QUANTIZE_LSTM
#define TENSORFLOW_COMPILER_MLIR_LITE_TRANSFORMS_PREPARE_QUANTIZE_LSTM

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "llvm/Support/Casting.h"
#include "llvm/Support/MathExtras.h"
#include "mlir/Dialect/Quant/FakeQuantSupport.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantTypes.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_config.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/tools/optimize/operator_property.h"

//===----------------------------------------------------------------------===//
// The prepare-quantize Pass for LSTM.
//
namespace mlir {
namespace TFL {

// Calculates the minimum power of two that is not less than the value.
inline double power_of_two_bound(double value) {
  return std::pow(2, std::ceil(std::log2(value)));
}

constexpr double power_of_two_scale = 32768.0;

namespace operator_property = ::tflite::optimize::operator_property;
using Q = quant::QuantizeCastOp;
using DQ = quant::DequantizeCastOp;

// Quantize recurrent input of LSTM with 16 bits.
template <typename SourceOp>
struct ConvertLstmStatsToQDQs : public OpRewritePattern<SourceOp> {
 public:
  ConvertLstmStatsToQDQs(MLIRContext* context,
                         const QuantizationSpecs& quant_specs)

      : OpRewritePattern<SourceOp>(context, /*benefit=*/2),
        quant_specs(quant_specs) {}
  LogicalResult matchAndRewrite(SourceOp op,
                                PatternRewriter& rewriter) const override {
    operator_property::OpVariant lstm_variant;
    if (llvm::isa<TFL::LSTMOp>(op.getOperation())) {
      lstm_variant.op_code = tflite::BuiltinOperator_LSTM;
    } else if (llvm::isa<TFL::UnidirectionalSequenceLSTMOp>(
                   op.getOperation())) {
      lstm_variant.op_code =
          tflite::BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_LSTM;
    } else {
      op.emitError("ConvertLstmStatsToQDQs pass only supports LSTMs.");
      return failure();
    }
    lstm_variant.use_projection =
        !op.projection_weights().getType().template isa<NoneType>();
    lstm_variant.use_peephole =
        !op.cell_to_output_weights().getType().template isa<NoneType>();
    lstm_variant.use_peephole =
        !op.cell_to_output_weights().getType().template isa<NoneType>();
    lstm_variant.use_layer_norm =
        !op.forget_layer_norm_coefficients().getType().template isa<NoneType>();

    const auto lstm_property =
        operator_property::GetOperatorProperty(lstm_variant);

    // TODO(b/172517537): use same scale for input 18 and output
    if (failed(processIntermediates(op, lstm_variant, lstm_property)) ||
        failed(processInputs(op, lstm_variant, lstm_property, rewriter))) {
      return failure();
    }

    return success();
  }

 private:
  QuantizationSpecs quant_specs;
  // Same with the ordering of //tensorflow/compiler/mlir/lite/ir/tfl_ops.td
  const std::vector<std::string> intermediate_attributes = {
      "input_to_input_intermediate", "input_to_forget_intermediate",
      "input_to_cell_intermediate", "input_to_output_intermediate",
      "effective_hidden_scale_intermediate"};

  QuantizedType getIntermediateType(SourceOp op, int intermediate_index) const {
    TypeAttr attr = op.template getAttrOfType<TypeAttr>(
        intermediate_attributes[intermediate_index]);
    if (!attr) {
      return nullptr;
    }
    return QuantizedType::getQuantizedElementType(attr.getValue());
  }

  LogicalResult getDerivedScale(
      SourceOp op, int input_index,
      const operator_property::TensorProperty& tensor_property,
      double& scale) const {
    scale = 1.0;
    for (int tensor_index : tensor_property.derived_scale.input_tensors) {
      auto dequantize_op = llvm::dyn_cast_or_null<DQ>(
          op.getOperand(tensor_index).getDefiningOp());

      if (!dequantize_op) {
        return failure();  // Wait for other scales to be calculated.
      }
      auto quant_type = QuantizedType::getQuantizedElementType(
          dequantize_op.getOperand().getType());
      if (!quant_type ||
          !quant_type.template isa<quant::UniformQuantizedType>()) {
        dequantize_op.emitError("Expected UniformQuantizedType.");
        return failure();
      }
      scale *= quant_type.template dyn_cast<quant::UniformQuantizedType>()
                   .getScale();
    }
    for (int tensor_index :
         tensor_property.derived_scale.intermediate_tensors) {
      auto quant_type = getIntermediateType(op, tensor_index);
      if (!quant_type ||
          !quant_type.template isa<quant::UniformQuantizedType>()) {
        op.emitError() << "While processing derived scale for input "
                       << input_index << ": "
                       << intermediate_attributes[tensor_index]
                       << " is not quantized.";
        return failure();
      }
      scale *= quant_type.template dyn_cast<quant::UniformQuantizedType>()
                   .getScale();
    }
    for (float factor : tensor_property.derived_scale.factors) {
      scale *= factor;
    }
    return success();
  }

  LogicalResult processIntermediates(
      SourceOp op, const operator_property::OpVariant& lstm_variant,
      const operator_property::OperatorProperty& lstm_property) const {
    for (auto& enumerated_intermediates : lstm_property.intermediates) {
      int index = enumerated_intermediates.first;
      auto& tensor_property = enumerated_intermediates.second;
      // intermediate tensors 0, 1, 2, 3 are only used with layer normalization.
      if (!lstm_variant.use_layer_norm && index != 4) {
        continue;
      }

      TypeAttr attr =
          op.template getAttrOfType<TypeAttr>(intermediate_attributes[index]);
      auto quant_type = getIntermediateType(op, index);
      if (!quant_type) {
        // intermediate tensor 4 is optional, unless the LSTM uses projection.
        if (index == 4 && !lstm_variant.use_projection) {
          return success();
        }
        op.emitError() << intermediate_attributes[index]
                       << " is not quantized.";
        return failure();
      }
      auto calibrated_type =
          quant_type.template dyn_cast<quant::CalibratedQuantizedType>();
      if (!calibrated_type) {
        int num_storage_bits = quant_type.getStorageTypeIntegralWidth();
        if (tensor_property.number_of_bits != num_storage_bits) {
          op.emitError() << intermediate_attributes[index]
                         << " is expected to be quantized with "
                         << tensor_property.number_of_bits << " bits, but got "
                         << num_storage_bits << " bits instead.";
          return failure();
        }
        continue;  // skip if it is already quantized.
      }
      quant::UniformQuantizedType qtype;
      if (tensor_property.number_of_bits == 8) {
        qtype = quant::fakeQuantAttrsToType(
            op.getLoc(), tensor_property.number_of_bits,
            calibrated_type.getMin(), calibrated_type.getMax(),
            /*narrowRange=*/false, calibrated_type.getExpressedType(),
            /*isSigned=*/quant_specs.IsSignedInferenceType());
      } else if (tensor_property.number_of_bits == 16) {
        double max = std::max(std::abs(calibrated_type.getMin()),
                              std::abs(calibrated_type.getMax()));
        qtype = quant::fakeQuantAttrsToType(
            op.getLoc(), tensor_property.number_of_bits, -max, max,
            /*narrowRange=*/true, calibrated_type.getExpressedType(),
            /*isSigned=*/true);
      } else {
        op.emitError() << "Unsupported quantization bits: "
                       << tensor_property.number_of_bits;
        return failure();
      }

      op.setAttr(intermediate_attributes[index],
                 TypeAttr::get(qtype.castFromExpressedType(
                     qtype.castToExpressedType(attr.getValue()))));
    }
    return success();
  }

  LogicalResult processInputs(
      SourceOp op, const operator_property::OpVariant& lstm_variant,
      const operator_property::OperatorProperty& lstm_property,
      PatternRewriter& rewriter) const {
    for (auto& enumerated_inputs : lstm_property.inputs) {
      int index = enumerated_inputs.first;
      auto& tensor_property = enumerated_inputs.second;

      Value input = op.getOperand(index);

      if (input.getDefiningOp() == nullptr) continue;

      // TODO(b/172517537): make this work with non-PTQ case.
      if (llvm::isa<ConstantOp, TFL::ConstOp>(input.getDefiningOp())) {
        if (failed(processConstantOp(op, input.getDefiningOp(), index,
                                     tensor_property, rewriter))) {
          return failure();
        }
      } else {
        if (auto stats_op =
                llvm::dyn_cast<quant::StatisticsOp>(input.getDefiningOp())) {
          if (failed(replaceStatsOp(op, stats_op, index, tensor_property,
                                    rewriter))) {
            return failure();
          }
          // Continue if StatisticsOp is already converted to Q-DQ pair.
        } else if (!llvm::isa<DQ>(input.getDefiningOp())) {
          // TODO(b/172517537): make this work with non-PTQ case.
          op.emitError() << "Input " << index
                         << " should be from DequantizeCast "
                            "or Statistics op.";
          input.getDefiningOp()->emitError();
          return failure();
        }
      }
    }
    return success();
  }

  LogicalResult processConstantOp(
      SourceOp op, Operation* const_op, int input_index,
      const operator_property::TensorProperty& tensor_property,
      PatternRewriter& rewriter) const {
    // Non-float tensors are neither weights nor require quantization.
    auto type = const_op->getResult(0).getType().dyn_cast<ShapedType>();
    if (!type || !type.getElementType().isa<FloatType>()) return success();

    DenseFPElementsAttr attr;
    if (!matchPattern(const_op->getResult(0), m_Constant(&attr))) {
      const_op->emitError("Not a constant op.");
      return failure();
    }

    UniformQuantizedType quant_type = nullptr;
    const int64_t storage_min = llvm::minIntN(tensor_property.number_of_bits);
    const int64_t storage_max = llvm::maxIntN(tensor_property.number_of_bits);
    const IntegerType storage_type =
        rewriter.getIntegerType(tensor_property.number_of_bits);
    const Type expressed_type =
        getElementTypeOrSelf(const_op->getResult(0).getType());

    if (tensor_property.use_derived_scale) {
      // Biases use derived scale from other tensors.
      // input 12~15: gate biases, input 17: projection bias
      if (tensor_property.number_of_bits != 32) {
        op.emitError() << "Derived scale is only supported for 32-bit "
                       << "quantization. Got " << tensor_property.number_of_bits
                       << " bits in input index " << input_index;
        return failure();
      }
      double scale;
      if (failed(getDerivedScale(op, input_index, tensor_property, scale))) {
        return failure();
      }
      quant_type = UniformQuantizedType::getChecked(
          quant::QuantizationFlags::Signed, storage_type, expressed_type, scale,
          /*zeroPoint=*/0, storage_min, storage_max, const_op->getLoc());
    } else {
      // For weights, use quantization scale directly inferred from the
      // values.
      //
      // input 1~4: input to gate weights
      // input 5~8: recurrent to gate weights
      // input 9~11: peephole weights, input 16: projection weight
      // input 20~23: normalization weights
      quant_type =
          quant::GetUniformQuantizedTypeForWeight(
              attr, /*symmetric=*/true,
              /*num_bits=*/tensor_property.number_of_bits, /*is_signed=*/true,
              /*narrow_range=*/true)
              .template dyn_cast<quant::UniformQuantizedType>();
    }

    if (!quant_type) {
      const_op->emitError("Failed to get quantized type");
      return failure();
    }

    // TODO(b/172517537): duplicate the constant when the bias is shared.
    Type cast_type =
        quant_type.castFromExpressedType(const_op->getResult(0).getType());
    rewriter.setInsertionPointAfter(const_op);
    auto q = rewriter.create<Q>(const_op->getLoc(), cast_type,
                                const_op->getResult(0));
    auto dq = rewriter.create<DQ>(const_op->getLoc(), cast_type, q);
    op.setOperand(input_index, dq.getResult());
    return success();
  }

  LogicalResult replaceStatsOp(
      SourceOp op, quant::StatisticsOp stats_op, int input_index,
      const operator_property::TensorProperty& tensor_property,
      PatternRewriter& rewriter) const {
    if (tensor_property.state_tensor && !stats_op.getResult().hasOneUse()) {
      // TODO(b/172517537): check if other tensors should go through this
      // check too.
      op.emitError() << "Input tensor [" << input_index
                     << "] is a state tensor, but has more than one use.";
      return failure();
    }
    auto stats = stats_op.layerStats().dyn_cast<DenseFPElementsAttr>();
    if (!stats || stats.getNumElements() != 2) {
      stats_op.emitError("Stats should have 2 values.");
      return failure();
    }
    quant::QuantizedType quant_type;
    double min = FloatAttr::getValueAsDouble(stats.getValue<APFloat>({0}));
    double max = FloatAttr::getValueAsDouble(stats.getValue<APFloat>({1}));
    Type expressed = getElementTypeOrSelf(stats_op.getType());

    if (tensor_property.extend_to_power_of_two) {
      if (tensor_property.number_of_bits != 16) {
        op.emitError(
            "extended power of 2 scale is only supported for 16-bit"
            " quantization.");
        return failure();
      }

      double bound = power_of_two_bound(std::max(std::abs(min), std::abs(max)));
      // Set flags to 1 for signed type.
      quant_type = UniformQuantizedType::getChecked(
          quant::QuantizationFlags::Signed,
          rewriter.getIntegerType(tensor_property.number_of_bits), expressed,
          /*scale=*/bound / -llvm::minIntN(tensor_property.number_of_bits),
          /*zeroPoint=*/0, llvm::minIntN(tensor_property.number_of_bits),
          llvm::maxIntN(tensor_property.number_of_bits), op.getLoc());
    } else {
      quant_type = quant::fakeQuantAttrsToType(
          op.getLoc(), tensor_property.number_of_bits, min, max,
          /*narrowRange=*/false, expressed,
          /*isSigned=*/true);
    }
    rewriter.setInsertionPointAfter(stats_op);
    Type result_type = quant_type.castFromExpressedType(stats_op.getType());
    auto q = rewriter.create<Q>(stats_op.getLoc(), result_type, stats_op.arg());
    rewriter.replaceOpWithNewOp<DQ>(stats_op, stats_op.getType(), q);
    return success();
  }
};

}  // namespace TFL
}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_LITE_TRANSFORMS_PREPARE_QUANTIZE_LSTM
