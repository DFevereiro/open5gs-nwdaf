#pragma once
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

// H1.7: short-horizon predictions for the 3GPP interfaces (interpretation
// I-13). Holt's linear-trend exponential smoothing over a regularly sampled
// series, with a confidence that is a probability (TS 23.288 V18.13.0
// §6.1.3: "probability assertion, i.e. confidence in the prediction").
struct NwdafForecast {
    double value;        // the predicted mean over the period, clamped to [lo, hi]
    int    confidence;   // 0–100
    size_t samples;      // series length the forecast used
};

class HoltForecaster {
public:
    struct Params {
        double alpha = 0.3;          // level smoothing
        double beta  = 0.1;          // trend smoothing
        double step_seconds = 10;    // the sampling interval
        double tolerance = 10;       // |error| counted as correct, in the series' unit
        size_t min_samples = 10;     // fewer: confidence 0 (TS 29.520: "shall return a zero confidence")
        double lo = 0, hi = 100;
    };

    // `series`: (time in seconds, value), oldest first. The forecast is the
    // mean of the fitted trend over [from, to] (seconds, at or after the last
    // sample), i.e. its value at the midpoint. The confidence is the
    // probability that the actual mean is within `tolerance`, for a normal
    // error whose spread is the fit's one-step error grown with the square
    // root of the steps ahead. nullopt for an empty series.
    static std::optional<NwdafForecast> forecast(const std::vector<std::pair<double, double>>& series,
                                                 double from, double to, const Params& p);
};
