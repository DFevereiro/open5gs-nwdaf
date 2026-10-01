#include "ml/holt_forecaster.hpp"
#include <algorithm>
#include <cmath>

std::optional<NwdafForecast> HoltForecaster::forecast(const std::vector<std::pair<double, double>>& series,
                                                      double from, double to, const Params& p) {
    if (series.empty()) return std::nullopt;
    const size_t n = series.size();

    // Level from the first sample, trend from the first two; one-step errors
    // are measured from the third sample on, once both are initialised.
    double level = series[0].second;
    double trend = n > 1 ? series[1].second - series[0].second : 0.0;
    double sq_err = 0.0;
    size_t errors = 0;
    for (size_t i = 1; i < n; ++i) {
        const double v = series[i].second;
        if (i >= 2) {
            const double e = v - (level + trend);
            sq_err += e * e;
            ++errors;
        }
        const double prev = level;
        level = p.alpha * v + (1 - p.alpha) * (level + trend);
        trend = i == 1 ? trend : p.beta * (level - prev) + (1 - p.beta) * trend;
    }

    const double last = series.back().first;
    const double mid = (std::max(from, last) + std::max(to, last)) / 2;
    const double steps = p.step_seconds > 0 ? (mid - last) / p.step_seconds : 0;
    const double value = std::clamp(level + trend * steps, p.lo, p.hi);

    int confidence = 0;
    if (n >= p.min_samples) {
        const double sd = (errors ? std::sqrt(sq_err / errors) : 0.0) * std::sqrt(std::max(1.0, steps));
        confidence = sd <= 1e-9 ? 100
                                : static_cast<int>(std::lround(100 * std::erf(p.tolerance / (sd * std::sqrt(2.0)))));
    }
    return NwdafForecast{value, std::clamp(confidence, 0, 100), n};
}
