#pragma once
#include <variant>

#include "external/bigint.h"
namespace bound
{
/**
 * @brief A bound on a range. Contains a value if the bound is known, otherwise,
 * the bound can be +inf or -inf.
 */
class Bound
{
    enum class Inf : uint8_t { NegInf, PosInf };
    std::variant<bigint, Inf> V_;

  public:
    // NOLINTNEXTLINE(bugprone-unchecked-*)
    inline auto& value() { return std::get<bigint>(V_); }
    // NOLINTNEXTLINE(bugprone-unchecked-*)
    inline const auto& value() const { return std::get<bigint>(V_); }
    inline bool hasValue() const { return std::holds_alternative<bigint>(V_); }

    friend Bound operator+(const Bound& A, const Bound& B);
    friend Bound operator-(const Bound& A, const Bound& B);
    friend Bound operator*(const Bound& A, const Bound& B);
    friend Bound operator/(const Bound& A, const Bound& B);
    friend Bound pow(const Bound& A, const Bound& B);
    friend Bound abs(const Bound& A);
    friend Bound min(const Bound& A, const Bound& B);
    friend Bound max(const Bound& A, const Bound& B);
    friend bool operator<(const Bound& A, const Bound& B);
    friend bool operator<=(const Bound& A, const Bound& B);
    friend bool operator>(const Bound& A, const Bound& B);
    friend bool operator>=(const Bound& A, const Bound& B);

    bool operator==(const Bound& Other) const = default;

    explicit Bound(bigint&& V) : V_(std::move(V)) {}
    explicit Bound(const bigint& V) : V_(V) {}
    explicit Bound(Inf I) : V_(I) {}
    explicit Bound(int64_t I) : V_(bigint(I)) {}
    explicit Bound(uint64_t I) : V_(bigint(I)) {}
    explicit Bound(int32_t I) : V_(bigint(I)) {}
    explicit Bound(uint32_t I) : V_(bigint(I)) {}

    static Bound makeNegInf() { return Bound(Inf::NegInf); }
    static Bound makePosInf() { return Bound(Inf::PosInf); }

    inline bool isNegInf() const
    {
        return std::holds_alternative<Inf>(V_) &&
               std::get<Inf>(V_) == Inf::NegInf;
    }

    inline bool isPosInf() const
    {
        return std::holds_alternative<Inf>(V_) &&
               std::get<Inf>(V_) == Inf::PosInf;
    }

    inline bool isNegative() const
    {
        return isNegInf() || (hasValue() && value() < bigint(0));
    }

    inline bool isPositive() const
    {
        return isPosInf() || (hasValue() && value() > bigint(0));
    }
};

Bound pow(int n, const Bound& B);
Bound operator+(const Bound& A, const Bound& B);
Bound operator-(const Bound& A, const Bound& B);
Bound operator*(const Bound& A, const Bound& B);
Bound operator/(const Bound& A, const Bound& B);
Bound pow(const Bound& A, const Bound& B);
Bound abs(const Bound& A);
Bound min(const Bound& A, const Bound& B);
Bound max(const Bound& A, const Bound& B);
bool operator<(const Bound& A, const Bound& B);
bool operator<=(const Bound& A, const Bound& B);
bool operator>(const Bound& A, const Bound& B);
bool operator>=(const Bound& A, const Bound& B);
}  // namespace bound