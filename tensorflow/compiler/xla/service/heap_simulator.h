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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_HEAP_SIMULATOR_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_HEAP_SIMULATOR_H_

#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "tensorflow/compiler/xla/service/buffer_value.h"
#include "tensorflow/compiler/xla/service/buffer_value_containers.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
#include "tensorflow/compiler/xla/service/hlo_alias_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_buffer.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_dataflow_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_ordering.h"
#include "tensorflow/compiler/xla/service/hlo_schedule.h"
#include "tensorflow/compiler/xla/service/tuple_points_to_analysis.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {

// Forward declare classes defined below.
class HeapAlgorithm;
class NoFragmentationStatsHeap;

// HeapSimulator assigns buffer offsets by running a simulation of a regular
// memory heap with Alloc and Free calls.  It only works for completely
// sequential instruction sequences.  Unlike regular heaps, we have the
// advantage that the sequence of Alloc and Free calls is known up-front; we
// don't need to return the assignment of buffer offsets until the very end.
class HeapSimulator {
 public:
  // Chunk represents a contiguous piece of memory.  Each BufferValue will be
  // associated with a chunk in the assignment result.
  struct Chunk {
    int64 offset;
    int64 size;

    int64 chunk_end() const { return offset + size; }
  };

  // Result represents the result of the heap simulation.
  struct Result {
    // The assignment of buffers to chunks.
    absl::flat_hash_map<const BufferValue*, Chunk> chunk_map;

    // The total size in bytes of the heap, containing all assigned chunks.
    int64 heap_size = 0;

    // The total size in bytes of heap fragmentation.
    int64 fragmentation_size = 0;

    // A trace of heap simulation events.
    HeapSimulatorTrace debug_trace;
  };

  // The different options to be passed to the Run() APIs.
  struct Options {
    Options()
        : may_reuse_operand_buffers(true),
          alloc_constants(false),
          buffers_to_assign(nullptr) {}

    // Whether a buffer about to be Free()-ed, can be recycled for a new born
    // one, hence collapsing Free()+Alloc() calls (default true).
    bool may_reuse_operand_buffers;
    // Whether to issue Alloc() and Free() calls for constants (default false).
    bool alloc_constants;
    // If 'buffers_to_assign' is provided, only those buffers are assigned
    // offsets, otherwise all buffers defined by the instructions are assigned.
    const BufferValueFlatSet* buffers_to_assign;
    // A vector of multiple buffer value sets. Each set enforces a must-alias
    // relationship for all buffers inside them.
    std::vector<BufferValueFlatSet> must_alias_sets;
  };

  // Returns the minimum memory required to compute an HLO module where all
  // computations have been scheduled (represented by the given
  // schedule), assuming no fragmentation.
  static StatusOr<int64> MinimumMemoryForModule(
      const HloSchedule& schedule,
      const LogicalBuffer::SizeFunction& size_function);

  // Returns the minimum memory required to compute the given computation,
  // assuming no fragmentation.
  static StatusOr<int64> MinimumMemoryForComputation(
      const HloComputation& computation, const HloInstructionSequence& sequence,
      const HloAliasAnalysis& alias_analysis,
      const LogicalBuffer::SizeFunction& size_function,
      const absl::flat_hash_map<const HloComputation*, int64>*
          memory_by_computation = nullptr);

  static StatusOr<int64> MinimumMemoryForComputation(
      const HloComputation& computation, const HloInstructionSequence& sequence,
      const HloAliasAnalysis& alias_analysis,
      const LogicalBuffer::SizeFunction& size_function,
      const HloSchedule* schedule);

  // Run the heap simulation with the given algorithm, assuming the given
  // schedule, which must contain a topologically-consistent total
  // ordering of all instructions within each computation. The result is invalid
  // if instructions are not run in exactly this sequence.
  //
  // Running heap simulation on the whole module tends to save memory, compared
  // to running on a per-computation basis, since we can re-use buffer space for
  // called sub-computations.
  //
  static StatusOr<Result> Run(std::unique_ptr<HeapAlgorithm> algorithm,
                              const HloModule& module,
                              const HloSchedule& schedule,
                              const HloAliasAnalysis& alias_analysis,
                              const BufferValue::SizeFunction& size_fn,
                              const Options& options = Options());

  // Same as above, but runs on a single computation. The 'instruction_sequence'
  // must contain a topologically-consistent total ordering of all instructions
  // in the computation. The result is invalid if instructions are not run in
  // exactly this sequence.
  static StatusOr<Result> Run(
      std::unique_ptr<HeapAlgorithm> algorithm,
      const HloComputation& computation,
      const HloInstructionSequence& instruction_sequence,
      const HloAliasAnalysis& alias_analysis,
      const BufferValue::SizeFunction& size_fn,
      const Options& options = Options(),
      const absl::flat_hash_map<const HloComputation*, int64>*
          memory_by_computation = nullptr);

  // Same as above, but runs on with a schedule that covers all nested
  // computations.
  static StatusOr<Result> Run(
      std::unique_ptr<HeapAlgorithm> algorithm,
      const HloComputation& computation,
      const HloInstructionSequence& instruction_sequence,
      const HloAliasAnalysis& alias_analysis,
      const BufferValue::SizeFunction& size_fn, const HloSchedule* schedule,
      const Options& options = Options());

 private:
  // If 'schedule' is non-null, it is used to find kCall and kWhile
  // sub-computations, and the heap simulation for those sub-computations will
  // be run recursively. I.e. the simulation is run over the whole module.
  HeapSimulator(std::unique_ptr<HeapAlgorithm> algorithm,
                const BufferValue::SizeFunction& size_fn,
                const Options& options, const HloSchedule* schedule = nullptr,
                const absl::flat_hash_map<const HloComputation*, int64>*
                    memory_by_computation = nullptr);
  ~HeapSimulator();

  Status RunComputation(const HloComputation& computation,
                        const HloInstructionSequence& instruction_sequence,
                        const HloAliasAnalysis& alias_analysis);

  bool IgnoreBuffer(const BufferValue* buffer) const;
  void Alloc(const BufferValue* buffer, const HloInstruction* instruction);
  void Free(const BufferValue* buffer, const HloInstruction* instruction);
  // ShareBuffer indicates that a new buffer is defined and it has to be the
  // same address as the shared one.
  void ShareBuffer(const BufferValue* buffer, const BufferValue* shared,
                   const HloInstruction* instruction);

  // Returns true if:
  //  Two buffers belong to the same shared group.
  //  Eight of the buffer has no shared group assigned.
  bool InSameSharedGroup(const BufferValue* left, const BufferValue* right);
  Result Finish();

  void FillDebugTrace(HeapSimulatorTrace::Event::Kind kind,
                      const BufferValue* buffer,
                      const HloInstruction* instruction,
                      const BufferValue* share_with_canonical);

  // Counterintuitive: the algorithm_ itself can be a NoFragmentationStatsHeap,
  // in which case we are calculating the same allocs/frees twice in the
  // simulation.
  const std::unique_ptr<NoFragmentationStatsHeap> no_fragmentation_stats_;
  const std::unique_ptr<HeapAlgorithm> algorithm_;
  const BufferValue::SizeFunction size_fn_;
  const Options options_;
  // schedule_ is set by buffer assignment, and memory_by_computation_ is
  // set by hlo scheduling. Then, in RunComputation, we check both in order to
  // handle subcomputations. It would be good to unify the handling of
  // subcomputations, but it's not clear how.
  const HloSchedule* schedule_;
  const absl::flat_hash_map<const HloComputation*, int64>*
      memory_by_computation_;

  // In addition to Alloc and Free, the heap simulator exposes a concept of
  // buffer sharing.  When ShareBuffer is called, instead of allocating new
  // space for the buffer, it associates the buffer with a previously allocated
  // (or shared) buffer.  Each group of mutually-shared buffers points to a
  // single SharedGroup instance, which is a shared control block.
  //
  // This forced buffer sharing is hidden from the underlying heap algorithm,
  // which only sees a regular Alloc call on the canonical buffer.  The
  // corresponding Free call is delayed until the liveness of all shared buffers
  // in the group has expired, which is tracked via the refcount.  The results
  // are post-processed in Finish to add chunks for shared buffers.
  //
  // The shared_buffers_ map associates each shared buffer (including the
  // canonical) to its SharedGroup control block.
  struct SharedGroup {
    const BufferValue* canonical = nullptr;
    int64 refcount = 0;
  };
  absl::flat_hash_map<const BufferValue*, std::shared_ptr<SharedGroup>>
      shared_buffers_;

  // Hold some sets for error-checking the sequence of Alloc and Free calls.
  absl::flat_hash_set<const BufferValue*> allocated_buffers_;
  absl::flat_hash_set<const BufferValue*> freed_buffers_;

  // Debugging information filled in while the heap simulator runs.
  HeapSimulatorTrace debug_trace_;
};

// Abstract base class describing a heap simulation algorithm that assigns
// offsets to buffers.  A sequence of Alloc / Free calls will be made, with the
// same semantics as a regular memory heap.  Finish will be called at the end to
// collect the simulation results.
class HeapAlgorithm {
 public:
  using Chunk = HeapSimulator::Chunk;
  using Result = HeapSimulator::Result;

  virtual ~HeapAlgorithm() = default;

  // Alloc allocates a buffer of 'size' bytes.
  virtual void Alloc(const BufferValue* buffer, int64 size) = 0;

  // Takes memory usage of subcomputations into account when calculating the
  // memory usage of a computation. Currently, we don't handle buffer aliasing
  // between computations entirely correctly. We are careful to not double count
  // for the output buffers of whiles/conds/calls. But we don't take into
  // account other aliases, such as for the while init. A more thorough solution
  // would require something like BufferAssignment::BuildColocatedBufferSets.
  // TODO(b/65835246):
  // Since TuplePointsToAnalysis is being replaced with a module-aware alias
  // analysis, it's not worth making major changes to HeapSimulator now.
  virtual void AccountForSubcomputationMemory(
      const HloInstruction* instruction,
      // The total number of bytes allocated by instruction.
      int64 alloc_size_by_instruction,
      const absl::flat_hash_map<const HloComputation*, int64>&
          memory_by_computation) {}

  // Free de-allocates a previously allocated buffer.
  virtual void Free(const BufferValue* buffer, int64 size) = 0;

  // Indicates that a buffer has to be collocated with another buffer.
  virtual void ShareWith(const BufferValue* buffer,
                         const BufferValue* share_with, int64 size) {
    Alloc(buffer, size);
  }

  // Finish collects the buffer offset assignment results.  Free may only be
  // called once, after the Alloc and Free calls.
  virtual Result Finish() = 0;
};

// NoFragmentationStatsHeap computes the heap size assuming no fragmentation;
// this is the absolute minimum size for a given instruction sequence.  The
// result.chunk_map returned in Finish is always empty, since we only collect
// stats, and don't actually compute chunk assignments.
class NoFragmentationStatsHeap : public HeapAlgorithm {
 public:
  NoFragmentationStatsHeap() = default;
  ~NoFragmentationStatsHeap() override = default;

  void Alloc(const BufferValue* buffer, int64 size) override;

  void AccountForSubcomputationMemory(
      const HloInstruction* instruction, int64 alloc_size_by_instruction,
      const absl::flat_hash_map<const HloComputation*, int64>&
          memory_by_computation) override;

  void Free(const BufferValue* buffer, int64 size) override;

  Result Finish() override;

 private:
  int64 current_heap_size_ = 0;
  int64 max_heap_size_ = 0;
};

// GlobalDecreasingSizeBestFitHeap collects the live intervals of all buffers,
// then allocates them in decreasing spatial or temporal size regardless of the
// alloc/free time. It internally tracks the allocated buffers and their live
// intervals; when allocating a buffer, it finds the best-fit free chunk during
// its live interval.
class GlobalDecreasingSizeBestFitHeap : public HeapAlgorithm {
 public:
  enum Type {
    kSpatial = 0,
    kTemporal,
  };

  explicit GlobalDecreasingSizeBestFitHeap(int64 alignment,
                                           Type type = kSpatial)
      : alignment_(alignment), type_(type) {}
  ~GlobalDecreasingSizeBestFitHeap() override {}

  void Alloc(const BufferValue* buffer, int64 size) override;
  void Free(const BufferValue* buffer, int64 size) override;

  void ShareWith(const BufferValue* buffer, const BufferValue* share_with,
                 int64 size) override;

  Result Finish() override;

 private:
  int64 alignment_;
  Result result_;
  Type type_;

  // The current time represented as an integer. It increments by 1 at each
  // Alloc or Free call.
  int64 current_time_ = 0;

  // BufferInterval stores a buffer's size and time interval.
  struct BufferInterval {
    const BufferValue* buffer;
    int64 size;
    // Alloc time of the buffer.
    int64 start;
    // Free time of the buffer.
    int64 end;

    // Colocation buffers that need to be collocated with this one.
    std::vector<const BufferValue*> colocations;

    // True if this buffer needs an allocation. False if it is collocated with
    // other buffer.
    bool need_allocation;
  };

  // Returns all transitive colocated buffers of this buffer interval. I.e., If
  // a buffer A is colocated with B and B is colocated with C, this function
  // returns all three of them.
  absl::flat_hash_set<const BufferValue*> GetTransitiveColocations(
      const BufferInterval& interval);
  absl::flat_hash_map<const BufferValue*, BufferInterval> buffer_intervals_;
};

// A heap algorithm that chooses the best results from other algorithms added to
// it.
class ChooseBestHeapAlgorithm : public HeapAlgorithm {
 public:
  ChooseBestHeapAlgorithm(
      std::unique_ptr<std::vector<std::unique_ptr<HeapAlgorithm>>> algorithms)
      : algorithms_(std::move(*algorithms)) {}
  ~ChooseBestHeapAlgorithm() override {}

  void Alloc(const BufferValue* buffer, int64 size) override {
    for (auto& algorithm : algorithms_) {
      algorithm->Alloc(buffer, size);
    }
  }

  void ShareWith(const BufferValue* buffer, const BufferValue* share_with,
                 int64 size) override {
    for (auto& algorithm : algorithms_) {
      algorithm->ShareWith(buffer, share_with, size);
    }
  }

  void Free(const BufferValue* buffer, int64 size) override {
    for (auto& algorithm : algorithms_) {
      algorithm->Free(buffer, size);
    }
  }

  Result Finish() override;

 private:
  std::vector<std::unique_ptr<HeapAlgorithm>> algorithms_;
};

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_HEAP_SIMULATOR_H_
