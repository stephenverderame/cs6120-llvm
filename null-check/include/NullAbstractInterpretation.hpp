#pragma once

#include <llvm-17/llvm/Analysis/LazyValueInfo.h>
#include <llvm-17/llvm/Analysis/ScalarEvolution.h>
#include <llvm-17/llvm/IR/DataLayout.h>
#include <llvm-17/llvm/IR/Instruction.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "IntervalAnalysis.hpp"
#include "KnowledgeGraph.hpp"
#include "LinExpr.hpp"
#include "df/DataFlow.hpp"

/**
 * @brief The state of a pointer
 */
enum class NullState : uint8_t {
    /// @brief Potentially null
    MaybeNull = 0,
    /// @brief Definitely not null
    NonNull,
};
class NullAbstractInterpretation;
struct PtrAbstractValue;
/// A mapping from LLVM values to abstract pointer locations
using NullAbstractVals =
    std::unordered_map<const llvm::Value*, std::shared_ptr<PtrAbstractValue>>;

/**
 * @brief Abstract value for a pointer type.
 * This is the representation of a pointer in the abstract domain.
 * TOP: Unknown
 * BOTTOM: Null
 */
struct PtrAbstractValue {
    /// If the value is known to be null.
    NullState IsNull = NullState::MaybeNull;
    /// The size of the data referred to by the pointer, in bytes
    LatticeElem<LinExpr> Size;
    /// The value of the data referred ;to be the pointer, may be null.
    std::shared_ptr<PtrAbstractValue> Data = nullptr;

    /// @brief Constructs a TOP abstract value
    PtrAbstractValue() = default;
    explicit PtrAbstractValue(NullState K) : IsNull(K), Data() {}
    explicit PtrAbstractValue(NullState K, LatticeElem<LinExpr> Size)
        : IsNull(K), Size(Size), Data()
    {
    }

    /**
     * @brief Constructs a new abstract location and abstract value which has
     * the given NullState, known size of data, and no data (not a pointer to a
     * pointer).
     */
    static auto make(NullState N, std::optional<int64_t> Size)
    {
        return std::make_shared<PtrAbstractValue>(
            N, Size.has_value()
                   ? LatticeElem<LinExpr>(LinExpr(AbstractInt(Size.value())))
                   : LatticeElem<LinExpr>::makeTop());
    }

    /**
     * @brief Constructs a new abstract location and abstract value which has
     * the given NullState, known size of data, and no data (not a pointer to a
     * pointer).
     */
    static auto make(NullState N, LinExpr&& Size)
    {
        return std::make_shared<PtrAbstractValue>(
            N, LatticeElem<LinExpr>(std::move(Size)));
    }

    /**
     * @brief Constructs a new abstract location and abstract value which has
     * the given NullState, an unknown size of data, and no data (not a pointer
     * to a pointer).
     */
    static auto make(NullState N)
    {
        return std::make_shared<PtrAbstractValue>(
            N, LatticeElem<LinExpr>::makeTop());
    }

    /**
     * @brief Makes the abstract value a null pointer.
     */
    void nullify()
    {
        IsNull = NullState::MaybeNull;
        Data.reset();
    }

    /**
     * @brief Constructs a deep clone of the current abstract value. If `this`
     * or any of its children have already been cloned, the existing clone is
     * returned from `ClonedVals`. `ClonedVals` is updated with the new clone
     * and its children (if not already present).
     *
     * @param ClonedVals the cache of already cloned pointers so that we
     * don't create two unique memory locations when there should only be one.
     * @return std::shared_ptr<PtrAbstractValue>
     */
    std::shared_ptr<PtrAbstractValue> clone(
        std::unordered_map<const PtrAbstractValue*,
                           std::shared_ptr<PtrAbstractValue>>& ClonedVals)
        const;

    bool operator==(const PtrAbstractValue& Other) const;
};
/// Forward declarations
namespace llvm
{
class Value;
class AllocaInst;
class LoadInst;
class StoreInst;
class CallInst;
class PHINode;
class BranchInst;
class ICmpInst;
class GetElementPtrInst;
}  // namespace llvm

/**
 * @brief Data flow analysis to determine whether a pointer is null, nonnull,
 * or unknown. Based off conditional constant propagation.
 *
 *
 * TOP: Set of all values as unknown (representing unknown value as not in the
 * set, so this is concretely the empty set)
 *
 *
 * BOTTOM: Set of all values as potentially null.
 *
 *
 * MEET: meet of particular value (union, except if both values
 * are specified, then meet of the respective values)
 *
 */
class NullAbstractInterpretation
{
  public:
    using TransferRet = TransferRetType<NullAbstractInterpretation>;

  private:
    /// Mapping from syntactic pointers to abstract pointer locations
    std::unordered_map<const llvm::Value*, std::shared_ptr<PtrAbstractValue>>
        State_;
    /// Mapping from values to their names (ie. "%0") for debugging
    std::unordered_map<const llvm::Value*, std::string> DebugNames_;
    /// @brief Mapping from GEPs which are null to an assignment of variables
    /// which shows why the GEP is null.
    std::unordered_map<const llvm::Value*, QueryResult> FailedRanges_;

    std::reference_wrapper<const DataFlowFacts<IntervalAnalysis>>
        IntervalFacts_;
    std::reference_wrapper<const InequalitySolver> Solver_;

    std::shared_ptr<llvm::DataLayout> DL_;
    std::reference_wrapper<llvm::TargetLibraryInfo> TLI_;

  private:
    // docs in src for private methods

    TransferRet transferAlloca(const llvm::AllocaInst* Alloca) const;
    TransferRet transferLoad(const llvm::LoadInst* Load) const;
    TransferRet transferStore(const llvm::StoreInst* Store) const;
    TransferRet transferCall(const llvm::CallInst* Call) const;
    TransferRet transferPhi(const llvm::PHINode* Phi) const;
    TransferRet transferBranch(
        const llvm::BranchInst* Branch,
        const DataFlowFacts<NullAbstractInterpretation>& Facts) const;
    TransferRet transferGetElemPtr(const llvm::GetElementPtrInst* GEP) const;
    static NullAbstractInterpretation insertIntoRes(
        NullAbstractInterpretation Res, const llvm::Value* Value,
        std::shared_ptr<PtrAbstractValue>&& Ptr);

    std::optional<std::tuple<const llvm::Value*, const llvm::Value*>>
    getNullNonNullPtr(const llvm::Value* LHS, const llvm::Value* RHS) const;

    std::tuple<NullAbstractInterpretation, NullAbstractInterpretation>
    pointerCmp(const llvm::ICmpInst* Cmp, const llvm::Value* LHS,
               const llvm::Value* RHS) const;

    PtrAbstractValue meetVal(const PtrAbstractValue& A,
                             const PtrAbstractValue& B,
                             const NullAbstractInterpretation& ContextB);
    QueryResult inRange(const LinExpr& Idx, const llvm::Instruction* Inst,
                        const LinExpr& Size) const;

  public:
    /// @see Fact::meet
    static NullAbstractInterpretation meet(const NullAbstractInterpretation& A,
                                           const NullAbstractInterpretation& B);

    /// @see Fact::transfer
    TransferRetType<NullAbstractInterpretation> transfer(
        const llvm::Instruction& I,
        const DataFlowFacts<NullAbstractInterpretation>& Facts) const;

    using Dir = Forwards;

    bool operator==(const NullAbstractInterpretation& Other) const;

    /**
     * @brief Get the AbstractValue the pointer refers to.
     * Requires `Value` to be a pointer type.
     *
     * @param Value
     * @return PtrAbstractValue
     */
    PtrAbstractValue getAbstractVal(const llvm::Value* Value) const;

    /**
     * @brief If a value is null due to an indexing, returns the reason why it
     * is null
     * @return QueryResult or none if no reason is known
     */
    std::optional<QueryResult> getFailedRange(const llvm::Value* GEP) const;

    /**
     * @brief Checks if a load or store instruction is safe against overflow.
     *
     * @param I The instruction to check
     * @return QueryResult which indicates success as true if the instruction is
     * safe, or false if it is not safe. If the instruction is not safe
     */
    QueryResult checkMemOverflow(const llvm::Instruction* I) const;

    NullAbstractInterpretation& operator=(
        const NullAbstractInterpretation& Other);
    NullAbstractInterpretation(const NullAbstractInterpretation& Other);
    /**
     * @brief Construct a new Null Abstract Interpretation fact that is the
     * top of the lattice.
     *
     * @param LVA The lazy value analysis
     * @param IntervalFacts The result of the interval analysis
     * @param M The module the function we are analyzing is in
     */
    NullAbstractInterpretation(
        llvm::TargetLibraryInfo& TLInfo,
        const DataFlowFacts<IntervalAnalysis>& IntervalFacts,
        const InequalitySolver& Solver, const llvm::Module& M,
        const llvm::Function& F);
    NullAbstractInterpretation(NullAbstractInterpretation&&) = default;
    NullAbstractInterpretation& operator=(NullAbstractInterpretation&&) =
        default;
    ~NullAbstractInterpretation() = default;
};

static_assert(Fact<NullAbstractInterpretation>);