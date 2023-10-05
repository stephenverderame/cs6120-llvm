#pragma once
#include <llvm-17/llvm/IR/Instructions.h>

#include "df/LatticeElem.hpp"
#include "external/bigint.h"
/**
 * @brief Whether a range is monotonic increasing or decreasing.
 */
enum class Monotonic : uint8_t {
    Increasing,
    Decreasing,
};

/**
 * @brief An inclusive integral range. We use bigints for arbitrary precision
 * since LLVM has arbitrary precision integers, and to prevent worrying
 * about overflow cases with things like signed and unsigned integers.
 *
 * Technically speaking, we can be less conservative by exploting
 * undefined behavior and attribute information, but this is simpler
 * and should be correct.
 *
 */
struct IntRange {
    bigint Lower;
    bigint Upper;
    LatticeElem<Monotonic> Monotonicity;

    /**
     * Computes the greatest lower bound of two ranges.
     * To be conservative for things like loops, if either of the bounds are
     * unequal, we set that bound to be inf or -inf, for the upper and lower
     * bounds respectively.
     */
    [[nodiscard]] static IntRange meet(const IntRange& A, const IntRange& B);
    /** Computes the least upper bound of two ranges (range intersection) */
    [[nodiscard]] static IntRange join(const IntRange& A, const IntRange& B);
    bool operator==(const IntRange& Other) const = default;

    /// @brief Create a range from a single integer.
    explicit IntRange(bigint&& I) : Lower(I), Upper(std::move(I)) {}
    IntRange() = default;
    IntRange(bigint&& Lower, bigint&& Upper)
        : Lower(std::move(Lower)), Upper(std::move(Upper))
    {
    }

    /// Converts this range to a signed range with the given bit width.
    [[nodiscard]] IntRange toSigned(unsigned int BitWidth) const;
    /// Converts this range to an unsigned range with the given bit width.
    [[nodiscard]] IntRange toUnsigned(unsigned int BitWidth) const;
    /// Computes `this ** A`
    [[nodiscard]] IntRange pow(bigint&& Exponent) const;
    /// Computes `Base ** this`
    [[nodiscard]] IntRange exponentiate(bigint&& Base) const;

    [[nodiscard]] IntRange remainder(const IntRange& Other,
                                     bool Signed = true) const;
    [[nodiscard]] IntRange unsignedRemainder(const IntRange& Other) const
    {
        return remainder(Other, false);
    }

    /// Returns true if the entire range is non-negative.
    [[nodiscard]] inline bool isNonNegative() const
    {
        return Lower >= bigint(0);
    }
    /// Returns true if the entire range is positive.
    [[nodiscard]] inline bool isPositive() const { return Lower > bigint(0); }
    /// Returns true if the entire range is negative.
    [[nodiscard]] inline bool isNegative() const { return Upper < bigint(0); }
    /// Returns true if the entire range is non-positive.
    [[nodiscard]] inline bool isNonPositive() const
    {
        return Upper <= bigint(0);
    }

    /**
     * If the lower bound has been adjusted, adjusts the upper bound to
     * prevent inconsistencies.
     *
     * Ex. (10, 0) -> (10, 10)
     */
    [[nodiscard]] IntRange fixUpperBound() const;

    /**
     * If the upper bound has been adjusted, adjusts the lower bound to
     * prevent inconsistencies.
     *
     * Ex. (10, 0) -> (0, 0)
     */
    [[nodiscard]] IntRange fixLowerBound() const;
};

[[nodiscard]] IntRange operator*(const IntRange& A, const IntRange& B);
[[nodiscard]] IntRange operator+(const IntRange& A, const IntRange& B);
[[nodiscard]] IntRange operator-(const IntRange& A, const IntRange& B);
[[nodiscard]] IntRange operator/(const IntRange& A, const IntRange& B);
[[nodiscard]] IntRange operator<<(const IntRange& A, const IntRange& B);

/**
 * @brief Returns the stricter (smaller distance between upper and lower bounds)
 * of the two ranges. If there is a tie, the first argument is returned.
 *
 * @param A
 * @param B
 * @return IntRange
 */
[[nodiscard]] IntRange smallerRange(const IntRange& A, const IntRange& B);

/**
 * @brief Returns a new lattice element that can be assumed when
 * `LHS Cond RHS` is true. The returned lattice element will be with
 * respect to the `LHS` argument, that is if `RHS` does not have a value,
 * the returned lattice element will be `LHS`.
 *
 * @param LHS the lattice element for the left hand side of the comparison
 * @param RHS the lattice element for the right hand side of the comparison
 * @param BitWidth the bit width of the operands
 * @param Cond the condition of the comparison
 * @return LatticeElem<IntRange>
 */
[[nodiscard]] LatticeElem<IntRange> adjustForCondition(
    const LatticeElem<IntRange>& LHS, const LatticeElem<IntRange>& RHS,
    uint64_t BitWidth, llvm::ICmpInst::Predicate Cond);