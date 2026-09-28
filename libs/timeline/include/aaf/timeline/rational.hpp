#pragma once

#include <aaf/error.hpp>

#include <compare>
#include <cstdint>
#include <string>

namespace aaf::timeline
{

/// An exact rational number with a positive denominator, always in lowest terms.
class Rational
{
public:
    constexpr Rational() = default;
    /// Creates `numerator/denominator`; a zero denominator yields 0/1.
    Rational(std::int64_t numerator, std::int64_t denominator = 1);

    [[nodiscard]] auto numerator() const noexcept -> std::int64_t { return num_; }
    [[nodiscard]] auto denominator() const noexcept -> std::int64_t { return den_; }
    [[nodiscard]] auto isZero() const noexcept -> bool { return num_ == 0; }
    [[nodiscard]] auto toDouble() const noexcept -> double;
    [[nodiscard]] auto toString() const -> std::string;

    /// Arithmetic fails with `Errc::limit` on 64-bit overflow.
    [[nodiscard]] auto add(const Rational& other) const -> Result<Rational>;
    [[nodiscard]] auto subtract(const Rational& other) const -> Result<Rational>;
    [[nodiscard]] auto multiply(const Rational& other) const -> Result<Rational>;
    [[nodiscard]] auto divide(const Rational& other) const -> Result<Rational>;

    auto operator==(const Rational&) const -> bool = default;
    auto operator<=>(const Rational& other) const -> std::strong_ordering;

private:
    std::int64_t num_ = 0;
    std::int64_t den_ = 1;
};

/// Converts a position counted in edit units of `from` into edit units of `to`, rounding toward negative infinity.
[[nodiscard]] auto convertPosition(std::int64_t position, const Rational& from, const Rational& to) -> Result<std::int64_t>;

}
