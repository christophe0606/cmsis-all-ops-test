// Copyright 2026 Arm Limited and/or its affiliates.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.
//
/**
 * @file main.cpp
 * @brief Bare-metal all-operators ExecuTorch test runner.
 *
 * This executable is built entirely from the CMSIS Pack's ExecuTorch runtime
 * and kernel components.
 *
 * The runner has two purposes:
 * - Build/link coverage: all_ops.cproject.yml selects every operator
 *   component, so linking this image exercises every operator's generated
 *   registration, forward declaration, and kernel source.
 * - Execution coverage: every model produced by generate_test_models.py is
 *   embedded directly into the ELF (.rodata) via embedded_models.S. The image
 *   can therefore self-test on bare metal without semihosting access to the
 *   host filesystem.
 *
 * For each embedded model, the runner executes model.pte, loads its generated
 * input_* methods, compares the output tensors against expected_* methods, and
 * prints a per-operator PASS/FAIL result plus a final aggregate summary.
 *
 * API usage mirrors examples/arm/executor_runner/arm_executor_runner.cpp.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <executorch/runtime/core/data_loader.h>
#include <executorch/runtime/core/error.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/exec_aten/exec_aten.h>
#include <executorch/runtime/core/hierarchical_allocator.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/core/result.h>
#include <executorch/runtime/core/span.h>
#include <executorch/runtime/executor/memory_manager.h>
#include <executorch/runtime/executor/method.h>
#include <executorch/runtime/executor/program.h>
#include <executorch/runtime/platform/runtime.h>
#include <executorch/extension/tensor/tensor_ptr.h>


#include "embedded_models.h"
#include "arm_embedded_module.hpp"
#include "arm_memory_allocator.h"
#include "container.h"

using executorch::aten::Tensor;
using executorch::runtime::DataLoader;
using executorch::runtime::Error;
using executorch::runtime::EValue;
using executorch::runtime::FreeableBuffer;
using executorch::runtime::HierarchicalAllocator;
using executorch::runtime::MemoryAllocator;
using executorch::runtime::MemoryManager;
using executorch::runtime::Method;
using executorch::runtime::MethodMeta;
using executorch::runtime::Program;
using executorch::runtime::Result;
using executorch::runtime::Span;

using namespace arm::embedded;

namespace
{

  /** @brief Static arena used by ExecuTorch for method lifetime allocations. */
  constexpr size_t kMethodPoolSize = 16 * 1024 * 1024;

  /** @brief Static arena used by ExecuTorch for temporary execution memory. */
  constexpr size_t kTempPoolSize = 4 * 1024 * 1024;

  /** @brief Backing storage for the method allocator. */
  alignas(16) uint8_t g_method_pool[kMethodPoolSize];

  /** @brief Backing storage for the temporary allocator. */
  alignas(16) uint8_t g_temp_pool[kTempPoolSize];

  /**
   * @brief Returns a reference to a memory-mapped 32-bit hardware register.
   *
   * Used for CoreDebug and DWT cycle-counter registers. The return type is
   * volatile so reads and writes are emitted exactly where requested.
   *
   * @param addr Register address.
   * @return Reference to the register at @p addr.
   */
  inline volatile uint32_t &reg32(uintptr_t addr)
  {
    return *reinterpret_cast<volatile uint32_t *>(addr);
  }

  /** @brief CoreDebug DEMCR register; bit 24 enables trace/DWT access. */
  constexpr uintptr_t kDemcr = 0xE000EDFC;   // CoreDebug DEMCR, TRCENA = bit 24

  /** @brief DWT_CTRL register; bit 0 enables CYCCNT. */
  constexpr uintptr_t kDwtCtrl = 0xE0001000; // DWT_CTRL, CYCCNTENA = bit 0

  /** @brief DWT cycle-count register sampled around model execution. */
  constexpr uintptr_t kDwtCyccnt = 0xE0001004;

  /**
   * @brief Per-model result row printed in the final summary table.
   */
  struct RowStat
  {
    /** @brief Embedded model metadata for the tested operator. */
    const EmbeddedModel *m;

    /** @brief True when all outputs matched their expected tensors. */
    bool pass;

    /** @brief Input-byte summary column for this model row. */
    size_t in_bytes;

    /** @brief Output-byte summary column for this model row. */
    size_t out_bytes;

    /** @brief DWT cycles spent in the model forward pass. */
    uint32_t cycles;
  };

  /** @brief Maximum number of per-model rows retained for final reporting. */
  constexpr size_t kMaxRows = 512;

  /** @brief Summary rows accumulated as embedded models are executed. */
  RowStat g_rows[kMaxRows];

  /**
   * @brief ExecuTorch DataLoader backed by a read-only in-memory buffer.
   *
   * The CMSIS Pack ships runtime and kernel components, but not the extension/
   * filesystem helpers. This loader lets Program read embedded .pte bytes
   * directly from flash/rodata.
   */
  class BufferLoader final : public DataLoader
  {
  public:
    /**
     * @brief Creates a loader over a contiguous model buffer.
     *
     * @param data Pointer to the first byte of the embedded .pte.
     * @param size Number of bytes available at @p data.
     */
    BufferLoader(const void *data, size_t size) : data_(data), size_(size) {}

    /**
     * @brief Returns a view over a requested model segment.
     *
     * ExecuTorch calls this while parsing the program. No ownership transfer is
     * needed because the backing storage is embedded in the image for the whole
     * process lifetime.
     *
     * @param offset First byte requested within the model buffer.
     * @param size Number of bytes requested.
     * @return FreeableBuffer view, or Error::InvalidArgument if out of range.
     */
    Result<FreeableBuffer> load(
        size_t offset,
        size_t size,
        const DataLoader::SegmentInfo &) const override
    {
      if (offset + size > size_)
      {
        return Error::InvalidArgument;
      }
      return FreeableBuffer(
          static_cast<const uint8_t *>(data_) + offset, size, nullptr);
    }

    /** @brief Returns the total size of the embedded program buffer. */
    Result<size_t> size() const override
    {
      return size_;
    }

  private:
    const void *data_;
    size_t size_;
  };

  inline bool is_channels_last_tensor(const Tensor& tensor) {
  if (tensor.dim() != 4) {
    return false;
  }

  // When channels or spatial dims are 1 the layout information is ambiguous.
  if (tensor.size(1) == 1 || (tensor.size(2) == 1 && tensor.size(3) == 1)) {
    return true;
  }

  constexpr executorch::aten::DimOrderType kChannelsLastDimOrder[] = {
      0, 2, 3, 1};
  executorch::aten::ArrayRef<executorch::aten::DimOrderType>
      channels_last_order(kChannelsLastDimOrder, 4);

  return tensor.dim_order() == channels_last_order;
}

  /**
   * @brief Prints a tensor's shape and scalar values for manual debugging.
   *
   * This helper is intentionally unused in normal PASS/FAIL logs because some
   * generated test tensors are large. It is useful when locally uncommenting
   * calls in the comparison helpers.
   *
   * @param t Tensor to print.
   */
void print_tensor(const Tensor& t) {
  printf("shape=[");
  for (int i = 0; i < t.dim(); ++i) {
    if (i) printf(", ");
    printf("%d", t.size(i));
  }
  printf("] values=[");

  switch (t.scalar_type()) {
    case executorch::aten::ScalarType::Float: {
      const float* p = t.const_data_ptr<float>();
      for (size_t i = 0; i < t.numel(); ++i) {
        if (i) printf(", ");
        printf("%f", p[i]);
      }
      break;
    }

    case executorch::aten::ScalarType::Int: {
      const int32_t* p = t.const_data_ptr<int32_t>();
      for (size_t i = 0; i < t.numel(); ++i) {
        if (i) printf(", ");
        printf("%d", p[i]);
      }
      break;
    }

    case executorch::aten::ScalarType::Bool: {
      const bool* p = t.const_data_ptr<bool>();
      for (size_t i = 0; i < t.numel(); ++i) {
        if (i) printf(", ");
        printf("%s", p[i] ? "true" : "false");
      }
      break;
    }

    default:
      printf("<unsupported dtype>");
      break;
  }

  printf("]\n");
}

  /**
   * @brief Compares two floating-point tensors using absolute/relative error.
   *
   * Shape and dtype checks are performed by the caller; this routine treats the
   * tensor storage as float data and reports the largest observed error to make
   * mismatches easier to diagnose from a serial log.
   *
   * @param a Actual tensor produced by the model.
   * @param b Expected tensor embedded with the model.
   * @param rtol Relative tolerance.
   * @param atol Absolute tolerance.
   * @return true when every element is within tolerance.
   */
  bool tensor_allclose(
      const Tensor &a,
      const Tensor &b,
      double rtol = 1e-6,
      double atol = 1e-6)
  {

    //print_tensor(a);
    //print_tensor(b);
    int a_bytes = a.numel() * a.element_size();
    int b_bytes = b.numel() * b.element_size();

    if (a_bytes != b_bytes)
    {
      printf(
          "  size mismatch: got %u vs expected %u bytes\n",
          static_cast<unsigned>(a_bytes),
          static_cast<unsigned>(b_bytes));
      return false;
    }

    const float *pa = a.const_data_ptr<float>();
    const float *pb = b.const_data_ptr<float>();

    float max_abs = 0.f;
    size_t worst = 0;

    bool ok = true;
    size_t n = a.numel();
    for (size_t i = 0; i < n; ++i)
    {
      float d = std::fabs(pa[i] - pb[i]);
      if (d > max_abs)
      {
        max_abs = d;
        worst = i;
      }
      if (d > atol + rtol * std::abs(pb[i]))
      {
        ok = false;
      }
    }
    printf(
        "  [cmp] %u float(s): max|err|=%g at [%u] (got %f vs exp %f),"
        " atol=%g rtol=%g -> %s\n",
        static_cast<unsigned>(n),
        max_abs,
        static_cast<unsigned>(worst),
        pa[worst],
        pb[worst],
        atol,
        rtol,
        ok ? "within tol" : "OUT OF TOL");
    return ok;
  }

  /**
   * @brief Compares two non-floating tensors for exact dtype, shape, and bytes.
   *
   * Integer, boolean, and quantized outputs are deterministic for these tests,
   * so byte-for-byte comparison gives the most useful result.
   *
   * @param a Actual tensor produced by the model.
   * @param b Expected tensor embedded with the model.
   * @return true when dtype, shape, and payload bytes all match.
   */
  bool tensor_equal(
      const Tensor &a,
      const Tensor &b)
  {

    //print_tensor(a);
    //print_tensor(b);
    if (a.scalar_type() != b.scalar_type())
      return false;

    if (a.dim() != b.dim())
      return false;

    for (int i = 0; i < a.dim(); ++i)
    {
      if (a.size(i) != b.size(i))
        return false;
    }

    size_t nbytes = a.nbytes(); // if available
    bool ok = std::memcmp(a.const_data_ptr(), b.const_data_ptr(), nbytes) == 0;
    printf(
        "  [cmp] %u byte(s) exact compare -> %s\n",
        static_cast<unsigned>(nbytes),
        ok ? "match" : "MISMATCH");
    return ok;
  }

  /**
   * @brief Dispatches tensor comparison based on scalar type.
   *
   * Floating-point outputs use model-specific tolerances. All other scalar
   * types are compared exactly.
   *
   * @param got Tensor returned by the model forward pass.
   * @param expected Tensor returned by the generated expected-output method.
   * @param atol Absolute tolerance for floating-point outputs.
   * @param rtol Relative tolerance for floating-point outputs.
   * @return true when @p got matches @p expected.
   */
  bool tensors_match(
      const Tensor &got,
      const Tensor &expected,
      float atol,
      float rtol)
  {

    const auto dtype = got.scalar_type();
    if (dtype == executorch::aten::ScalarType::Float)
    {
      return tensor_allclose(got, expected, rtol, atol);
    }
    else
    {
      return tensor_equal(got, expected);
    }
  }


using executorch::aten::DimOrderType;
using executorch::aten::SizesType;
using executorch::aten::StridesType;
using executorch::aten::Tensor;
using executorch::extension::TensorPtr;
using executorch::extension::make_tensor_ptr;

TensorPtr to_channels_last_4d_float(const Tensor& in) {
  ET_CHECK(in.dim() == 4);

  const SizesType N = in.size(0);
  const SizesType C = in.size(1);
  const SizesType H = in.size(2);
  const SizesType W = in.size(3);

  std::vector<SizesType> sizes = {N, C, H, W};

  std::vector<DimOrderType> dim_order = {
      static_cast<DimOrderType>(0),
      static_cast<DimOrderType>(2),
      static_cast<DimOrderType>(3),
      static_cast<DimOrderType>(1),
  };

  std::vector<StridesType> strides = {
      static_cast<StridesType>(H * W * C),
      static_cast<StridesType>(1),
      static_cast<StridesType>(W * C),
      static_cast<StridesType>(C),
  };

  std::vector<float> out_data(
      static_cast<size_t>(N) *
      static_cast<size_t>(C) *
      static_cast<size_t>(H) *
      static_cast<size_t>(W));

  const float* in_data = in.const_data_ptr<float>();

  // in.strides() returns ArrayRef<StridesType>, not a pointer.
  const auto in_strides = in.strides();

  for (SizesType n = 0; n < N; ++n) {
    for (SizesType c = 0; c < C; ++c) {
      for (SizesType h = 0; h < H; ++h) {
        for (SizesType w = 0; w < W; ++w) {
          const auto in_offset =
              n * in_strides[0] +
              c * in_strides[1] +
              h * in_strides[2] +
              w * in_strides[3];

          const auto out_offset =
              n * strides[0] +
              c * strides[1] +
              h * strides[2] +
              w * strides[3];

          out_data[static_cast<size_t>(out_offset)] =
              in_data[static_cast<size_t>(in_offset)];
        }
      }
    }
  }

  return make_tensor_ptr<float>(
      std::move(sizes),
      std::move(out_data),
      std::move(dim_order),
      std::move(strides));
}

  /**
   * @brief Runs one embedded model end-to-end and records its result.
   *
   * The generated .pte contains small helper methods named nb_inputs,
   * nb_outputs, input_N, output_N, atol, and rtol. This function executes those
   * helpers to discover the test shape, populate model inputs, retrieve the
   * expected outputs, and choose comparison tolerances.
   *
   * The log format intentionally includes "Test_result: <op> PASS/FAIL" lines
   * so a host-side parser can aggregate results across a full bare-metal run.
   *
   * @param pte_data Scratch buffer to copy the embedded .pte from flash/rodata.
   * @param m Embedded model and metadata to execute.
   * @param row Output summary row populated for the final table.
   * @return true when every model output matched its expected tensor.
   */
  bool run_one_model(uint8_t *pte_data, const EmbeddedModel &m, RowStat &row)
  {
    row.m = &m;
    row.pass = false;
    row.in_bytes = 0;
    row.out_bytes = 0;
    row.cycles = 0;

    auto method_allocator = std::make_unique<ArmMemoryAllocator>(kMethodPoolSize,
                                                              g_method_pool);
    auto temp_allocator = std::make_unique<ArmMemoryAllocator>(kTempPoolSize,
                                                            g_temp_pool);

    const uint8_t *container_pte_data = static_cast<const uint8_t *>(get_object_pointer(m.object_index));
    size_t pte_size = get_object_length(m.object_index);
    std::memcpy(pte_data, container_pte_data, pte_size);
    auto loader = std::make_unique<BufferLoader>(pte_data, pte_size);
    EmbeddedModule module_(pte_data,
                           pte_size,
                           std::move(loader),
                           std::move(method_allocator),
                           std::move(temp_allocator));

    size_t num_inputs = 0;
    auto nb_inputs_ = module_.execute("nb_inputs");
    if (nb_inputs_.ok())
      num_inputs = nb_inputs_.get()[0].toInt();

    size_t num_outputs = 0;
    auto num_outputs_ = module_.execute("nb_outputs");
    if (!num_outputs_.ok())
      return false;

    num_outputs = num_outputs_.get()[0].toInt();

    bool channel_last = false;
    auto channel_last_ = module_.execute("channel_last");
    if (channel_last_.ok())
      channel_last = channel_last_.get()[0].toBool();
    printf("channel_last=%s\n", channel_last ? "true" : "false");


    printf(
        "Test_exec: %s (dir=%s) pte=%u bytes, %u input(s), %u expected\n",
        m.op,
        m.dir,
        static_cast<unsigned>(pte_size),
        static_cast<unsigned>(num_inputs),
        static_cast<unsigned>(num_outputs));

    char method_name[256];

    for (size_t i = 0; i < num_inputs; ++i)
    {
      sprintf(method_name, "input_%d", i);
      auto input_ = module_.execute(method_name);
      if (input_.ok())
      {
        auto input = input_.get()[0].toTensor();
        //print_tensor(input);
        TensorPtr input_ptr = make_tensor_ptr(input);
        if (channel_last) 
        {
           input_ptr = to_channels_last_4d_float(input);
        };

        
        auto error = module_.set_input(*input_ptr, i);
        if (error != Error::Ok)
        {
          printf("  input %u FAIL (set_input)\n", static_cast<unsigned>(i));
          return false;
        }
        
      }
    }

    printf("  [run] %s execute() ...\n", m.op);
    uint32_t c0 = reg32(kDwtCyccnt);
    const auto result = module_.forward();
    row.cycles = reg32(kDwtCyccnt) - c0;
    if (!result.ok())
    {
      printf("Test_result: %s FAIL (execute)\n", m.op);
      return false;
    }

    printf(
        "  [run] %s execute() ok, %u output(s)\n",
        m.op,
        static_cast<unsigned>(num_outputs));
    bool pass = true;

    float atol = 0;
    auto atol_ = module_.execute("atol");
    if (!atol_.ok())
      return false;
    atol = static_cast<float>(atol_.get()[0].toDouble());

    float rtol = 0;
    auto rtol_ = module_.execute("rtol");
    if (!rtol_.ok())
      return false;
    rtol = static_cast<float>(rtol_.get()[0].toDouble());

    for (size_t i = 0; i < num_outputs; ++i)
    {
      const auto got = result->at(i).toTensor();
      //print_tensor(got);
      
      sprintf(method_name, "output_%d", i);
      auto output_ = module_.execute(method_name);
      if (!output_.ok())
      {
        printf("  output %u FAIL (get_output)\n", static_cast<unsigned>(i));
        pass = false;
        continue;
      }
     
      const auto expected = output_.get()[0].toTensor();
      //print_tensor(expected);
      TensorPtr expected_ptr = make_tensor_ptr(expected);
      if (channel_last)
      {
        expected_ptr = to_channels_last_4d_float(expected);
      };

      if (!tensors_match(got, *expected_ptr, atol, rtol))
      {
        printf("  output %u mismatch\n", static_cast<unsigned>(i));
        pass = false;
      }
      
    }

    row.pass = pass;
    printf("Test_result: %s %s\n", m.op, pass ? "PASS" : "FAIL");
    return pass;
  }

} // namespace

/**
 * @brief Program entry point for the all-operators test image.
 *
 * Initializes ExecuTorch, enables the Cortex-M DWT cycle counter, executes each
 * embedded model in sequence, and prints both per-operator and aggregate test
 * results. A zero return value means every embedded model passed.
 */
int main(void)
{
  // Unbuffered stdout: the bare-metal startup may loop after main() returns
  // (never calling exit()), so block-buffered output would never flush. Make
  // every printf reach the semihosting console immediately.
  setvbuf(stdout, nullptr, _IONBF, 0);

  executorch::runtime::runtime_init();

  // Enable the DWT cycle counter so run_one_model can time each inference.
  reg32(kDemcr) |= (1u << 24);
  reg32(kDwtCyccnt) = 0;
  reg32(kDwtCtrl) |= 1u;

  uint32_t max_object_size = get_max_object_size();
  /* Create a buffer to copy the pte file from container */
  uint8_t *pte_temp_buffer_ptr = static_cast<uint8_t *>(std::aligned_alloc(MODEL_ALIGNMENT, max_object_size));

  size_t passed = 0;
  const size_t total = g_embedded_models_count;
  for (size_t mi = 0; mi < total; ++mi)
  {
    RowStat &row = g_rows[mi < kMaxRows ? mi : kMaxRows - 1];
    if (run_one_model(pte_temp_buffer_ptr,g_embedded_models[mi], row))
    {
      ++passed;
    }
  }

  printf(
      "\n==== Per-op results (%u models) ====\n", static_cast<unsigned>(total));
  printf(
      "%-30s %-6s %10s %8s %8s %12s\n",
      "op",
      "result",
      "model(B)",
      "in(B)",
      "out(B)",
      "cycles");
  for (size_t mi = 0; mi < total && mi < kMaxRows; ++mi)
  {
    const RowStat &r = g_rows[mi];
    size_t pte_size = get_object_length(r.m->object_index);
    printf(
        "%-30s %-6s %10u %8u %8u %12u\n",
        r.m->op,
        r.pass ? "PASS" : "FAIL",
        static_cast<unsigned>(pte_size),
        static_cast<unsigned>(r.in_bytes),
        static_cast<unsigned>(r.out_bytes),
        static_cast<unsigned>(r.cycles));
  }

  printf(
      "Test_result: SUMMARY %u/%u PASS\n",
      static_cast<unsigned>(passed),
      static_cast<unsigned>(total));
  free(pte_temp_buffer_ptr);
  exit(passed == total ? 0 : 1);
}
