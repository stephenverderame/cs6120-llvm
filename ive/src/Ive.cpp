#include <llvm-17/llvm/ADT/APInt.h>
#include <llvm-17/llvm/IR/IRBuilder.h>
#include <llvm-17/llvm/IR/InstrTypes.h>
#include <llvm-17/llvm/IR/Instruction.h>
#include <llvm-17/llvm/IR/PassManager.h>
#include <llvm-17/llvm/IR/Value.h>
#include <llvm-17/llvm/Passes/PassBuilder.h>
#include <llvm-17/llvm/Passes/PassPlugin.h>
#include <llvm-17/llvm/Support/Casting.h>
#include <llvm-17/llvm/Support/CommandLine.h>
#include <llvm-17/llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Analysis/LazyValueInfo.h>
#include <llvm/IR/Dominators.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "df/DataFlow.hpp"

// NOLINTNEXTLINE
using namespace llvm;

static cl::opt<bool> EnablePrint(
    "ive-print", cl::desc("Enable printing of detected induction variables"),
    cl::init(false));

static cl::opt<bool> EnableDebug(
    "ive-debug", cl::desc("Enable printing of debug information"),
    cl::init(false));

namespace
{

/**
 * @brief A basic induction variable of the form `i += e` or `i -= e`
 */
struct BasicIV {
    /// The PHI node / value of the basic IV at the start of the loop
    PHINode* Phi;
    /// The binary operator that increments the basic IV / the value of the
    /// basic IV at the end of the loop
    BinaryOperator* BinOp;
    /// The value that is added to the basic IV (`e`)
    Value* Addend;
    /// True if BinOp is a subtraction (`i -= e`)
    bool IsSub;
    /// Initial value of the basic IV before the loop
    Value* InitialVal;

    BasicIV(PHINode* Phi, BinaryOperator* BinOp)
        : Phi(Phi),
          BinOp(BinOp),
          Addend(BinOp->getOperand(0) == Phi ? BinOp->getOperand(1)
                                             : BinOp->getOperand(0)),
          IsSub(BinOp->getOpcode() == Instruction::Sub),
          InitialVal(Phi->getIncomingValue(0) == BinOp
                         ? Phi->getIncomingValue(1)
                         : Phi->getIncomingValue(0))
    {
        // only 2 incoming values bc we're in canonical form (single preheader)
        assert(Phi->getNumIncomingValues() == 2);
    }
};

using BasicIVMap = std::unordered_map<Value*, std::shared_ptr<BasicIV>>;

/**
 * @brief If `BinOp` is a basic induction variable, then insert it into
 * `BasicIVs` and return the updated map. Otherwise, return `BasicIVs`.
 *
 * @param BasicIVs mapping of values to `BasicIV` structs
 * @param BinOp Binary operator instruction
 * @param L loop
 * @return updated mapping of values to `BasicIV` structs
 */
auto insertBasicIvIntoMap(BasicIVMap&& BasicIVs, BinaryOperator* BinOp,
                          const Loop* L)
{
    if (BinOp->getOpcode() != Instruction::Add &&
        BinOp->getOpcode() != Instruction::Sub) {
        return BasicIVs;
    }
    const auto IsAdd = BinOp->getOpcode() == Instruction::Add;
    Value* LHS = BinOp->getOperand(0);
    Value* RHS = BinOp->getOperand(1);
    auto* Phi = dyn_cast<PHINode>(RHS) == nullptr ? dyn_cast<PHINode>(LHS)
                                                  : dyn_cast<PHINode>(RHS);

    if (Phi != nullptr && (L->isLoopInvariant(LHS) && RHS == Phi && IsAdd) ||
        (L->isLoopInvariant(RHS) && LHS == Phi)) {
        for (auto& IncomingVal : Phi->incoming_values()) {
            if (IncomingVal == BinOp) {
                const auto Basic = std::make_shared<BasicIV>(Phi, BinOp);
                BasicIVs.emplace(Phi, Basic);
                BasicIVs.emplace(BinOp, Basic);
                break;
            }
        }
    }
    return BasicIVs;
}

/**
 * @brief Get a set of basic induction variables. The set will contain two
 * entries for each basic induction variable. One for the phi node and one for
 * the result of the addition or subtraction.
 *
 * @param L the loop
 * @return the set of basic induction variables
 */
auto getBasicIVs(const Loop* L, BasicIVMap& InnerBasics)
{
    BasicIVMap BasicIVs;
    for (llvm::BasicBlock* BB : L->getBlocks()) {
        for (llvm::Instruction& I : *BB) {
            // outs() << "Instruction: " << I << "\n";
            if (auto* BinOp = dyn_cast<BinaryOperator>(&I);
                BinOp != nullptr && !InnerBasics.contains(BinOp)) {
                BasicIVs = insertBasicIvIntoMap(std::move(BasicIVs), BinOp, L);
            }
        }
    }
    for (auto& [Val, Basic] : BasicIVs) {
        InnerBasics.emplace(Val, Basic);
    }
    return BasicIVs;
}

/**
 * @brief A derived induction variable of the form `BasicIV * Factor + Addend`.
 * A basic IV is a Derived IV with `Factor` and `Addend` as `nullptr`.
 */
class IndVar
{
  private:
    /// The value of the basic induction variable this is derived from
    // (either the phi node or the result of the addition/subtraction)
    Value* Base = nullptr;
    /// The basic induction variable that this is derived from
    std::shared_ptr<BasicIV> BasicIv = nullptr;
    /// True if this is a basic induction variable
    bool IsBasic = false;

  public:
    /// Nullopt if Factor is 1
    std::optional<Value*> Factor = {};
    /// Nullopt if Addend is 0
    std::optional<Value*> Addend = {};
    /// True if Addend should be negative (`j = i * c - d`)
    bool Sub = false;
    /// The PHI node for this derived induction variable
    std::optional<PHINode*> DerivedPhi = {};
    /// The final binary operator for the update of this derived induction
    /// variable
    std::optional<Value*> DerivedIncr = {};

    IndVar(Value* Base, const std::shared_ptr<BasicIV>& BasicIv, Value* Factor)
        : Base(Base), BasicIv(BasicIv), Factor(Factor)
    {
        assert(Factor != nullptr);
    }

    /// @brief Constructs a basic induction variable (Derived of the form `<i,
    /// 1, 0>`)
    /// @param Base the value of the basic induction variable
    /// @param BasicIv the basic induction variable
    IndVar(Value* Base, const std::shared_ptr<BasicIV>& BasicIv)
        : Base(Base), BasicIv(BasicIv), IsBasic(true)
    {
    }

    IndVar(const IndVar& Other, Value* Addend, bool Sub)
        : Base(Other.Base),
          BasicIv(Other.BasicIv),
          Factor(Other.Factor),
          Addend(Addend),
          Sub(Sub)
    {
        assert(Addend != nullptr);
    }

    inline bool isBasic() const { return IsBasic; }

    inline auto* getBase() const { return Base; }
    inline auto getBasic() const { return BasicIv; }
};

auto prettyPrint(const Instruction* I)
{
    std::string Str;
    raw_string_ostream Stream(Str);
    I->print(Stream);
    return Str;
}

/**
 * @brief Inserts `BinOp` into `DerivedIVs` if it is a derived induction
 * variable. Otherwise, does nothing.
 *
 * @param DerivedIVs Current mapping of values to `DerivedIV` structs
 * @param BinOp Binary operator instruction
 * @param BasicIVs Mapping of values to basic induction variables
 * @param L loop
 * @return updated mapping of values to `DerivedIV` structs
 */
auto insertDerivedIV(std::unordered_map<Value*, IndVar>&& DerivedIVs,
                     BinaryOperator* BinOp, const BasicIVMap& BasicIVs,
                     const Loop* L)
{
    const auto Name = getDebugName(BinOp);
    const auto Instr = prettyPrint(BinOp);
    auto* LHS = BinOp->getOperand(0);
    auto* RHS = BinOp->getOperand(1);
    if (BinOp->getOpcode() == Instruction::Mul) {
        if (BasicIVs.contains(LHS) && L->isLoopInvariant(RHS)) {
            DerivedIVs.emplace(BinOp, IndVar{LHS, BasicIVs.at(LHS), RHS});
        } else if (BasicIVs.contains(RHS) && L->isLoopInvariant(LHS)) {
            DerivedIVs.emplace(BinOp, IndVar{RHS, BasicIVs.at(RHS), LHS});
        }
    } else if (BinOp->getOpcode() == Instruction::Add ||
               BinOp->getOpcode() == Instruction::Sub) {
        const auto IsSub = BinOp->getOpcode() == Instruction::Sub;
        if (DerivedIVs.contains(LHS) && L->isLoopInvariant(RHS) &&
            !DerivedIVs.at(LHS).Addend.has_value()) {
            DerivedIVs.emplace(BinOp, IndVar{DerivedIVs.at(LHS), RHS, IsSub});
        } else if (DerivedIVs.contains(RHS) && L->isLoopInvariant(LHS) &&
                   !IsSub && !DerivedIVs.at(RHS).Addend.has_value()) {
            // we could allow subtraction and just negate the factor
            DerivedIVs.emplace(BinOp, IndVar{DerivedIVs.at(RHS), LHS, IsSub});
        }
    }
    return DerivedIVs;
}

/**
 * @brief Get the Derived IVs object
 *
 * @param L loop
 * @param BasicIVs mapping of induction variable value pointers to `BasicIV`
 * @return mapping from derived induction variable value pointers to `DerivedIV`
 * struct
 */
auto getDerivedIVs(const Loop* L, const BasicIVMap& BasicIVs)
{
    std::unordered_map<Value*, IndVar> DerivedIVs;
    for (auto& [Val, Basic] : BasicIVs) {
        DerivedIVs.emplace(Val, IndVar{Val, Basic});
    }
    // Basic IVs contains the phi nodes and the resulting value of performing
    // the basic iv addition or subtraction on that phi node
    // Convert those additions and subtraction into IndVar structs
    for (auto& [V, BIV] : DerivedIVs) {
        if (const auto BinOp = dyn_cast<BinaryOperator>(V); BinOp != nullptr) {
            for (auto Idx = 0; Idx < 2; ++Idx) {
                auto* Operand = BinOp->getOperand(Idx);
                if (DerivedIVs.contains(Operand)) {
                    BIV.Addend = BinOp->getOperand(1 - Idx);
                    BIV.Sub = BinOp->getOpcode() == Instruction::Sub;
                }
            }
        }
    }
    for (auto* BB : L->getBlocks()) {
        for (auto& I : *BB) {
            if (auto BinOp = dyn_cast<BinaryOperator>(&I); BinOp != nullptr) {
                DerivedIVs =
                    insertDerivedIV(std::move(DerivedIVs), BinOp, BasicIVs, L);
            }
        }
    }
    return DerivedIVs;
}

/**
 * @brief Constructs the new derived initialization instruction and adds it to
 * the end of the pre-header.
 *
 * @param LoopPreHeader
 * @param DerivedIV
 * @param IVPhi
 */
[[nodiscard]] Value* addInitDerived(BasicBlock* LoopPreHeader,
                                    const IndVar& DerivedIV)
{
    // add instruction to the end of the pre-header
    IRBuilder<> Builder(LoopPreHeader);
    if (auto LPTerminator = LoopPreHeader->getTerminator();
        LPTerminator != nullptr) {
        Builder.SetInsertPoint(LPTerminator);
    }
    const auto& BasicIV = *DerivedIV.getBasic();
    auto* Initial =
        DerivedIV.Factor.has_value()
            ? Builder.CreateMul(DerivedIV.Factor.value(), BasicIV.InitialVal)
            : BasicIV.InitialVal;
    if (!DerivedIV.Addend.has_value()) {
        return Initial;
    }
    if (DerivedIV.Sub) {
        return Builder.CreateSub(Initial, DerivedIV.Addend.value());
    } else {
        return Builder.CreateAdd(Initial, DerivedIV.Addend.value());
    }
}

/**
 * @brief Constructs the new affine increment instruction(s) for the derived
 * induction variables. Inserts this immediately after the basic induction
 * variable's increment
 *
 * @param IndVarVal the value of the original induction variable
 * @param IV the derived induction variable
 * @param DerivedPhi the derived PHI node
 * @param DT dominator tree
 * @return New increment instruction
 */
[[nodiscard]] Value* addDerivedIncrement(const Value* IndVarVal,
                                         const IndVar& IV, PHINode* DerivedPhi)
{
    auto* BasicIVIncr = IV.getBasic()->BinOp;
    Value* BasicIVStride = IV.getBasic()->Addend;

    IRBuilder<> Builder(BasicIVIncr);
    Value* IndMul = IV.Factor.has_value()
                        ? Builder.CreateMul(IV.Factor.value(), BasicIVStride)
                        : BasicIVStride;

    if (BasicIVIncr->getOpcode() == Instruction::Sub) {
        return Builder.CreateSub(DerivedPhi, IndMul);
    } else {
        return Builder.CreateAdd(DerivedPhi, IndMul);
    }
}

/**
 * @brief Replaces uses of the original derived induction variable with the
 * result of the derived increment. If the use is before the increment of the
 * basic induction variable, then replace it with the derived PHI node.
 *
 * @param IndVarVal
 * @param DerivedIncr
 * @param DerivedPhi
 * @param DT
 * @return auto
 */
auto replaceUses(Value* OrigIndVar, Value* DerivedIncr, PHINode* DerivedPhi,
                 const DominatorTree& DT)
{
    // replace uses of original IndVar with new increment
    const auto OrigIndVarName = getDebugName(OrigIndVar);
    std::vector<Use*> Uses;
    std::transform(OrigIndVar->uses().begin(), OrigIndVar->uses().end(),
                   std::back_inserter(Uses), [](auto& U) { return &U; });

    // somehow, and I don't know why, but the following keeps adding
    // uses such that it loops forever if we just loop over
    // `OrigIndVar->uses()` directly
    for (auto& U : Uses) {
        auto* InstrUser = U->getUser();
        // debug variables
        const auto UseName = getDebugName(cast<Instruction>(U));
        const auto DebugName = getDebugName(InstrUser);
        const auto DerivedIncrDebugName = getDebugName(DerivedIncr);
        const auto DerivedPhiDebugName = getDebugName(DerivedPhi);
        // if the use of the derived is after the increment of the basic IV,
        // then replace it with the result of the derived increment.
        // Otherwise replace it with the derived PHI node
        if (DT.dominates(DerivedIncr, *U)) {
            U->set(DerivedIncr);
        } else {
            assert(DT.dominates(DerivedPhi, *U));
            U->set(DerivedPhi);
        }
    }
}

/**
 * @brief Asserts that `V` dominates all of its uses, and if not,
 * prints the uses and the value and asserts.
 */
void assertDominatesUses(const Value* V, const DominatorTree& DT,
                         const std::string& OrigIndVarName)
{
    for (const auto& U : V->uses()) {
        if (!DT.dominates(V, U)) {
            const auto UseName = getDebugName(U);
            const auto VName = getDebugName(V);
            errs() << VName << " does not dominate "
                   << getDebugName(U.getUser()) << "\n";
            errs() << "Value: " << *V << "\n";
            errs() << "User:" << *U.getUser() << "\n";
            errs() << "OrigIndVarName: " << OrigIndVarName << "\n";
            assert(DT.dominates(V, U));
        }
    }
}

/**
 * @brief Transforms the given derived induction variable by adding an
 * initialization to the loop pre-head and increment instruction to the loop
 * body.
 *
 * @param Loop the loop
 * @param IV the derived induction variable
 * @param F the function
 */
void transformDerived(Loop* Loop, std::pair<Value* const, IndVar>& IV,
                      Function& F)
{
    BasicBlock* LoopHeader = Loop->getHeader();
    BasicBlock* LoopPreHeader = Loop->getLoopPreheader();
    BasicBlock* LoopLatch = Loop->getLoopLatch();
    if (LoopPreHeader == nullptr || LoopLatch == nullptr) {
        // Assume loop-simplify has been run
        assert(false &&
               "Loop preheader or latch is null, did you run "
               "loop-simplify first?");
        return;
    }

    auto& [OrigIvInstr, IndVar] = IV;
    if (OrigIvInstr->getNumUses() == 0) {
        return;
    }
    const auto OrigName = getDebugName(OrigIvInstr);

    if (EnableDebug) {
        outs() << "Transforming derived: " << *OrigIvInstr << "\n\n\n";
    }

    IRBuilder Builder(LoopHeader);
    Builder.SetInsertPoint(LoopHeader->getFirstNonPHI());
    // Create a Phi instruction in the current basic block
    PHINode* PhiNode = Builder.CreatePHI(OrigIvInstr->getType(), 2);

    if (EnableDebug) {
        outs() << F << "\n\n--------\n\nAdded Phi\n";
        outs().flush();
    }

    auto* DerivedInit = addInitDerived(LoopPreHeader, IndVar);
    PhiNode->addIncoming(DerivedInit, LoopPreHeader);

    if (EnableDebug) {
        outs() << F << "\n\n--------\n\nAdded Derived Init\n";
        outs().flush();
    }

    auto* DerivedIncr = addDerivedIncrement(OrigIvInstr, IndVar, PhiNode);
    PhiNode->addIncoming(DerivedIncr, LoopLatch);

    if (EnableDebug) {
        outs() << F << "\n\n--------\n\nAdded Derived Incr\n";
        outs().flush();
    }

    DominatorTree DT;
    DT.recalculate(F);
    replaceUses(OrigIvInstr, DerivedIncr, PhiNode, DT);

    if (EnableDebug) {
        outs() << F << "\n\n--------\n\nFinished\n\n\n\n";
        outs().flush();
    }

    assertDominatesUses(PhiNode, DT, OrigName);
    assertDominatesUses(DerivedIncr, DT, OrigName);

    IndVar.DerivedIncr = DerivedIncr;
    IndVar.DerivedPhi = PhiNode;
}

/**
 * @brief Prints the given derived induction variables
 *
 * @param DerivedIVs
 */
void printIVs(const std::unordered_map<Value*, IndVar>& DerivedIVs,
              std::unordered_set<const Value*>& DisplayedIVs)
{
    // sorted for deterministic output
    std::map<std::string, std::tuple<const Value*, IndVar>> SortedIVs;
    for (const auto [Val, DerivedIV] : DerivedIVs) {
        if (DisplayedIVs.contains(Val)) {
            continue;
        }
        DisplayedIVs.insert(Val);
        SortedIVs.emplace(getDebugName(Val), std::make_tuple(Val, DerivedIV));
    }
    for (const auto& [ValStr, DerivedTuple] : SortedIVs) {
        const auto [Val, DerivedIV] = DerivedTuple;
        if (DerivedIV.isBasic()) {
            outs() << "Basic IV: " << *Val << "\n";
        } else {
            outs() << "Derived IV: " << getDebugName(Val) << " = ";
            outs() << getDebugName(DerivedIV.getBase());
            if (DerivedIV.Factor.has_value()) {
                outs() << " * " << getDebugName(DerivedIV.Factor.value());
            }
            if (DerivedIV.Addend.has_value()) {
                if (DerivedIV.Sub) {
                    outs() << " - ";
                } else {
                    outs() << " + ";
                }
                outs() << getDebugName(DerivedIV.Addend.value());
            }
            outs() << "\n";
        }
    }
    outs().flush();
}

/**
 * @brief Replaces the uses of the basic induction which `Derived` is derived
 * from with the derived induction variable for uses which are comparisons.
 *
 * @param L loop
 * @param Derived derived induction variable
 * @param DT dominator tree
 * @param LVI lazy value info
 * @return true if any changes were made
 */
bool replaceCmp(Loop* L, const IndVar& Derived, const DominatorTree& DT,
                LazyValueInfo& LVI)
{
    std::vector<Use*> Uses;
    std::transform(Derived.getBasic()->Phi->use_begin(),
                   Derived.getBasic()->Phi->use_end(), std::back_inserter(Uses),
                   [](auto& U) { return &U; });
    bool AnyChange = false;
    for (auto* U : Uses) {
        if (auto* Cmp = dyn_cast<CmpInst>(U->getUser())) {
            assert(Cmp->getNumOperands() == 2);
            auto* Other = Cmp->getOperand(1 - U->getOperandNo());
            if (!L->isLoopInvariant(Other)) {
                // no sense in adding extra computation if it cannot be put in
                // the preheader
                continue;
            }
            if (Derived.Factor.has_value() &&
                (LVI.getConstantRange(Other, Cmp).contains(APInt(8, 0)) ||
                 !LVI.getConstantRange(Derived.Factor.value(), Cmp)
                      .isAllNonNegative())) {
                // we only support positive factors
                continue;
            }
            // insert right before the cmp instruction bc the expression might
            // not be loop invariant
            IRBuilder Builder(Cmp);
            auto* Mul = Derived.Factor.has_value()
                            ? Builder.CreateMul(Other, Derived.Factor.value())
                            : Other;
            if (Derived.Addend.has_value()) {
                auto* Add =
                    Derived.Sub
                        ? Builder.CreateSub(Mul, Derived.Addend.value())
                        : Builder.CreateAdd(Mul, Derived.Addend.value());
                Cmp->setOperand(1 - U->getOperandNo(), Add);
            } else {
                Cmp->setOperand(1 - U->getOperandNo(), Mul);
            }
            if (DT.dominates(Derived.DerivedIncr.value(), Cmp)) {
                U->set(Derived.DerivedIncr.value());
            } else {
                assert(DT.dominates(Derived.DerivedPhi.value(), Cmp));
                U->set(Derived.DerivedPhi.value());
            }

            AnyChange = true;
        }
    }
    return AnyChange;
}

/**
 * @brief Replaces comparisons of the basic induction variable with a loop
 * invariant expression to that of the derived induction variable if there
 * exists a derived IV.
 *
 * @param L loop
 * @param Derived mapping of all induction variables
 * @param F function
 * @param LVI lazy value info
 */
void replaceComparisons(Loop* L,
                        const std::unordered_map<Value*, IndVar>& Derived,
                        Function& F, LazyValueInfo& LVI)
{
    std::unordered_set<Value*> Transformed;
    for (const auto& [Val, IndVar] : Derived) {
        if (IndVar.isBasic() || Transformed.contains(IndVar.getBase())) {
            continue;
        }
        const auto& Basic = IndVar.getBasic();
        bool DoReplace = true;
        for (const auto& U : Val->uses()) {
            if (Derived.contains(U.getUser())) {
                // not "the most derived" induction variable, so we won't
                // replace basic comparison with this one bc it might end up
                // being dead
                DoReplace = false;
                break;
            }
        }
        for (const auto& U : Basic->Phi->uses()) {
            if (!Derived.contains(U.getUser()) && U.getUser() != Basic->BinOp &&
                dyn_cast<CmpInst>(U.getUser()) == nullptr) {
                // there is a use of the basic induction variable that isn't a
                // compare, a derived induction variable, or the increment of
                // the basic induction variable. So we can't eliminate the basic
                // induction variable and might as well not replace the compare
                DoReplace = false;
                break;
            }
        }
        if (DoReplace && IndVar.DerivedIncr.has_value() &&
            IndVar.DerivedPhi.has_value()) {
            DominatorTree DT;
            DT.recalculate(F);
            if (replaceCmp(L, IndVar, DT, LVI)) {
                Transformed.insert(IndVar.getBase());
            }
        }
    }
}

struct InductionVariableElimination
    : public PassInfoMixin<InductionVariableElimination> {
    PreservedAnalyses run(Module& M, ModuleAnalysisManager& AM)
    {
        FunctionAnalysisManager& FAM =
            AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
        for (auto& F : M) {
            // skip empty functions
            if (F.getName().starts_with("llvm.dbg.") ||
                F.getInstructionCount() == 0) {
                continue;
            }
            llvm::LoopInfo* LI = &FAM.getResult<llvm::LoopAnalysis>(F);
            std::unordered_set<const Value*> DisplayedIVs;
            llvm::LazyValueInfo* LVI =
                &FAM.getResult<llvm::LazyValueAnalysis>(F);

            const auto Name = F.getName();
            BasicIVMap UsedBasics;
            for (llvm::Loop* L : LI->getTopLevelLoops()) {
                runLoop(L, DisplayedIVs, F, UsedBasics, *LVI);
            }
        }

        return PreservedAnalyses::none();
    };

    /**
     * @brief Recursive function to perform induction variable eleimination on a
     * loop and all subloops in preorder (subloops before parent loops).
     *
     * @param L loop
     * @param DisplayedIVs set of induction variables that have already been
     * printed. Only used when EnablePrint is true.
     * @param F function
     * @param UsedBasics mapping of values to basic induction variables that
     * have already been used. Used to avoid reusing basic induction variables
     * that have already been used in a subloop.
     */
    void runLoop(Loop* L, std::unordered_set<const Value*>& DisplayedIVs,
                 Function& F, BasicIVMap& UsedBasics, LazyValueInfo& LVI)
    {
        for (const auto SubLoop : L->getSubLoops()) {
            runLoop(SubLoop, DisplayedIVs, F, UsedBasics, LVI);
        }
        const auto BasicIVs = getBasicIVs(L, UsedBasics);
        auto DerivedIVs = getDerivedIVs(L, BasicIVs);

        if (EnablePrint) {
            outs() << "Loop: " << *L->getHeader() << "\n";
            printIVs(DerivedIVs, DisplayedIVs);
        }

        // filter out basic and call this on
        //    mappings
        for (auto& P : DerivedIVs | std::views::filter([](auto& P) {
                           return !P.second.isBasic();
                       })) {
            transformDerived(L, P, F);
        }

        replaceComparisons(L, DerivedIVs, F, LVI);
    }
};

}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo()
{
    return {.APIVersion = LLVM_PLUGIN_API_VERSION,
            .PluginName = "ive",
            .PluginVersion = "v0.1",
            .RegisterPassBuilderCallbacks = [](PassBuilder& PB) {
                // for usage with opt
                PB.registerPipelineParsingCallback(
                    [](auto Name, ModulePassManager& PM,
                       auto /* PipelineElement*/) {
                        if (Name == "ive") {
                            PM.addPass(InductionVariableElimination{});
                            return true;
                        }
                        return false;
                    });
                // for usage with clang
                PB.registerOptimizerEarlyEPCallback(
                    [](ModulePassManager& PM, OptimizationLevel /* Level */) {
                        PM.addPass(InductionVariableElimination{});
                    });
            }};
}
