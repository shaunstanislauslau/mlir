#include "mlir/Quantization/FakeQuantSupport.h"
#include "mlir/Quantization/QuantOps.h"

using namespace mlir;
using namespace mlir::quant;

UniformQuantizedType mlir::quant::fakeQuantAttrsToType(Location loc,
                                                       unsigned numBits,
                                                       double rmin, double rmax,
                                                       bool narrowRange,
                                                       Type expressedType) {
  MLIRContext *ctx = expressedType.getContext();
  Type storageType;
  unsigned flags;
  int64_t qmin;
  int64_t qmax;

  // Hard-coded type mapping from TFLite.
  if (numBits <= 8) {
    storageType = IntegerType::get(8, ctx);
    flags = 0;
    qmin = 0;
    qmax = 255;
  } else if (numBits <= 16) {
    storageType = IntegerType::get(16, ctx);
    flags = QuantizationFlags::Signed;
    qmin = -32768;
    qmax = 32767;
  } else {
    ctx->emitError(loc,
                   "unsupported FakeQuant number of bits: " + Twine(numBits));
    return nullptr;
  }

  // Handle narrowRange.
  if (narrowRange) {
    qmin += 1;
  }

  // Range must straddle zero.
  if (rmin > 0.0 || rmax < 0.0) {
    return (ctx->emitError(loc, "FakeQuant range must straddle zero: [" +
                                    Twine(std::to_string(rmin)) + "," +
                                    Twine(std::to_string(rmax)) + "]"),
            nullptr);
  }

  // Special case where min/max is a point. Must be 0.
  if (rmin == rmax) {
    return UniformQuantizedType::getChecked(flags, storageType, expressedType,
                                            0.0, 0, qmin, qmax, loc);
  }

  // Determine the scale.
  const double qminDouble = qmin;
  const double qmaxDouble = qmax;
  const double scale = (rmax - rmin) / (qmaxDouble - qminDouble);

  // Zero point computation.
  // In float, solve the affine equation for any known pair
  // (real value, corresponding quantized value), of which, two such pairs
  // are known: (rmin, qmin), (rmax, qmax).
  // The arithmetic error on the zero point computed from either pair will be
  // roughly machine_epsilon * (sum of absolute values of terms).
  // Use the variant that adds the smaller error.
  const double zeroPointFromMin = qminDouble - rmin / scale;
  const double zeroPointFromMinError =
      std::abs(qminDouble) + std::abs(rmin / scale);
  const double zeroPointFromMax = qmaxDouble - rmax / scale;
  const double zeroPointFromMaxError =
      std::abs(qmaxDouble) + std::abs(rmax / scale);

  const double zeroPointDouble = (zeroPointFromMinError < zeroPointFromMaxError)
                                     ? zeroPointFromMin
                                     : zeroPointFromMax;

  // Now nudge the zero point to be an integer.
  int64_t nudgedZeroPoint = 0;
  if (zeroPointDouble < qminDouble) {
    nudgedZeroPoint = qmin;
  } else if (zeroPointDouble > qmaxDouble) {
    nudgedZeroPoint = qmax;
  } else {
    nudgedZeroPoint = round(zeroPointDouble);
  }

  // By construction, the nudged zero point should always be in range.
  assert(nudgedZeroPoint >= qmin);
  assert(nudgedZeroPoint <= qmax);

  return UniformQuantizedType::getChecked(flags, storageType, expressedType,
                                          scale, nudgedZeroPoint, qmin, qmax,
                                          loc);
}
