#include <aaf/timeline/rational.hpp>

#include <format>
#include <limits>
#include <numeric>
#include <optional>

namespace aaf::timeline
{

namespace
{

auto overflow() -> std::unexpected<Error>
{
    return fail(Errc::limit, "rational arithmetic overflowed");
}

constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
constexpr auto kMin = std::numeric_limits<std::int64_t>::min();

auto mul(std::int64_t a, std::int64_t b) -> std::optional<std::int64_t>
{
    if (a == 0 || b == 0)
    {
        return 0;
    }
    bool overflows = false;
    if (a > 0)
    {
        overflows = b > 0 ? a > kMax / b : b < kMin / a;
    }
    else
    {
        overflows = b > 0 ? a < kMin / b : b < kMax / a;
    }
    if (overflows)
    {
        return std::nullopt;
    }
    return a * b;
}

auto addChecked(std::int64_t a, std::int64_t b) -> std::optional<std::int64_t>
{
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b))
    {
        return std::nullopt;
    }
    return a + b;
}

auto floorDiv(std::int64_t a, std::int64_t b) -> std::int64_t
{
    const auto q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/// Compares a/b with c/d (b, d > 0) exactly, without overflow, using the continued-fraction expansion.
auto compareFractions(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d) -> std::strong_ordering
{
    while (true)
    {
        const auto q1 = floorDiv(a, b);
        const auto q2 = floorDiv(c, d);
        if (q1 != q2)
        {
            return q1 <=> q2;
        }
        const auto r1 = a - (q1 * b);
        const auto r2 = c - (q2 * d);
        if (r1 == 0 && r2 == 0)
        {
            return std::strong_ordering::equal;
        }
        if (r1 == 0 || r2 == 0)
        {
            return r1 == 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        a = d;
        c = b;
        b = r2;
        d = r1;
    }
}

auto make(std::int64_t num, std::int64_t den) -> Result<Rational>
{
    if (den == 0)
    {
        return fail(Errc::invalid_argument, "division by zero");
    }
    if (den == kMin || (num == kMin && den < 0))
    {
        return overflow();
    }
    return Rational(num, den);
}

}

Rational::Rational(std::int64_t numerator, std::int64_t denominator)
{
    if (denominator == 0)
    {
        return;
    }
    if (denominator < 0 && numerator != kMin && denominator != kMin)
    {
        numerator = -numerator;
        denominator = -denominator;
    }
    const auto g = std::gcd(numerator, denominator);
    num_ = g == 0 ? 0 : numerator / g;
    den_ = g == 0 ? 1 : denominator / g;
}

auto Rational::toDouble() const noexcept -> double
{
    return static_cast<double>(num_) / static_cast<double>(den_);
}

auto Rational::toString() const -> std::string
{
    return den_ == 1 ? std::to_string(num_) : std::format("{}/{}", num_, den_);
}

auto Rational::add(const Rational& other) const -> Result<Rational>
{
    const auto a = mul(num_, other.den_);
    const auto b = mul(other.num_, den_);
    const auto d = mul(den_, other.den_);
    if (!a || !b || !d)
    {
        return overflow();
    }
    const auto n = addChecked(*a, *b);
    if (!n)
    {
        return overflow();
    }
    return make(*n, *d);
}

auto Rational::subtract(const Rational& other) const -> Result<Rational>
{
    if (other.num_ == kMin)
    {
        return overflow();
    }
    return add(Rational(-other.num_, other.den_));
}

auto Rational::multiply(const Rational& other) const -> Result<Rational>
{
    const auto g1 = std::gcd(num_, other.den_);
    const auto g2 = std::gcd(other.num_, den_);
    const auto n = mul(g1 == 0 ? 0 : num_ / g1, g2 == 0 ? 0 : other.num_ / g2);
    const auto d = mul(g2 == 0 ? den_ : den_ / g2, g1 == 0 ? other.den_ : other.den_ / g1);
    if (!n || !d)
    {
        return overflow();
    }
    return make(*n, *d);
}

auto Rational::divide(const Rational& other) const -> Result<Rational>
{
    if (other.num_ == 0)
    {
        return fail(Errc::invalid_argument, "division by zero");
    }
    return multiply(Rational(other.den_, other.num_));
}

auto Rational::operator<=>(const Rational& other) const -> std::strong_ordering
{
    return compareFractions(num_, den_, other.num_, other.den_);
}

auto convertPosition(std::int64_t position, const Rational& from, const Rational& to) -> Result<std::int64_t>
{
    if (from.isZero() || to.isZero())
    {
        return fail(Errc::invalid_argument, "edit rate is zero");
    }
    auto scaled = Rational(position).multiply(to);
    if (!scaled)
    {
        return std::unexpected(scaled.error());
    }
    auto result = scaled->divide(from);
    if (!result)
    {
        return std::unexpected(result.error());
    }
    return floorDiv(result->numerator(), result->denominator());
}

}
