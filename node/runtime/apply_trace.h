#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

// Longest prefix of `text` within `limit` bytes that does not split a UTF-8 sequence.
[[nodiscard]] inline std::string_view truncate_utf8(std::string_view text, std::size_t limit) noexcept {
    if (text.size() <= limit) {
        return text;
    }
    auto size = limit;
    while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) {
        --size;
    }
    return text.substr(0, size);
}

enum class apply_log_level : std::uint8_t {
    info,
    warning,
    error,
};

// Local steps and log lines of applying one release, reported in ApplyResult.steps/logs.
// Each line carries the phase of the step running when it was written, else of the step
// that finished last (callers log a step's summary after it ends); empty before any step.
// Callers pass only fixed wording, counts and the error text ApplyResult.error already
// carries: never secrets, key or certificate material, object payloads, credentials,
// digests or revision numbers.
class apply_trace final {
public:
    static constexpr std::size_t maximum_steps = 16;
    static constexpr std::size_t maximum_logs = 100;
    static constexpr std::size_t maximum_text_bytes = 512;

    struct step_start final {
        std::string_view phase_;
        std::int64_t started_unix_ms_{};
        std::chrono::steady_clock::time_point started_;
    };

    // Starts a step; log lines written from now on belong to it.
    [[nodiscard]] step_start begin_step(std::string_view phase) {
        current_phase_ = std::string(truncate_utf8(phase, maximum_text_bytes));
        return {.phase_ = phase, .started_unix_ms_ = unix_ms(), .started_ = std::chrono::steady_clock::now()};
    }

    void end_step(const step_start& step, bool ok, std::string_view error = {}) {
        if (steps_.size() == maximum_steps) {
            return;
        }
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - step.started_);
        steps_.push_back({.phase_ = std::string(truncate_utf8(step.phase_, maximum_text_bytes)),
            .started_unix_ms_ = step.started_unix_ms_,
            .duration_ms_ = duration.count(),
            .ok_ = ok,
            .error_ = std::string(truncate_utf8(error, maximum_text_bytes))});
    }

    // Runs one step; a throwing step is recorded as failed with its error, then rethrown.
    template <typename function_type>
    std::invoke_result_t<function_type&> run_step(std::string_view phase, function_type&& function) {
        const auto step = begin_step(phase);
        try {
            if constexpr (std::is_void_v<std::invoke_result_t<function_type&>>) {
                function();
                end_step(step, true);
            } else {
                auto value = function();
                end_step(step, true);
                return value;
            }
        } catch (const std::exception& error) {
            end_step(step, false, error.what());
            throw;
        } catch (...) {
            end_step(step, false, "unknown error");
            throw;
        }
    }

    void log(apply_log_level level, std::string_view message) {
        if (logs_.size() == maximum_logs) {
            ++omitted_logs_;
            return;
        }
        logs_.push_back({.level_ = level,
            .unix_ms_ = unix_ms(),
            .message_ = std::string(truncate_utf8(message, maximum_text_bytes)),
            .phase_ = current_phase_});
    }

    // Replaces the steps and logs of `result`. Past the log cap the last slot reports how
    // many lines were omitted; it names no phase, as the omitted lines may span several.
    void write_to(v2::ApplyResult& result) const {
        result.clear_steps();
        for (const auto& step : steps_) {
            auto& output = *result.add_steps();
            output.set_phase(step.phase_);
            output.set_started_unix_ms(step.started_unix_ms_);
            output.set_duration_ms(step.duration_ms_);
            output.set_ok(step.ok_);
            output.set_error(step.error_);
        }
        result.clear_logs();
        const auto kept = omitted_logs_ == 0 ? logs_.size() : maximum_logs - 1;
        for (std::size_t index = 0; index < kept; ++index) {
            write_log(result, logs_[index].level_, logs_[index].unix_ms_, logs_[index].message_, logs_[index].phase_);
        }
        if (omitted_logs_ != 0) {
            write_log(result, apply_log_level::warning, logs_.back().unix_ms_,
                std::to_string(omitted_logs_ + 1) + " further log lines omitted", {});
        }
    }

private:
    struct recorded_step final {
        std::string phase_;
        std::int64_t started_unix_ms_{};
        std::int64_t duration_ms_{};
        bool ok_{};
        std::string error_;
    };
    struct recorded_log final {
        apply_log_level level_{};
        std::int64_t unix_ms_{};
        std::string message_;
        std::string phase_;
    };

    [[nodiscard]] static std::int64_t unix_ms() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] static std::string_view level_name(apply_log_level level) noexcept {
        switch (level) {
            case apply_log_level::warning:
                return "warning";
            case apply_log_level::error:
                return "error";
            case apply_log_level::info:
                break;
        }
        return "info";
    }

    static void write_log(v2::ApplyResult& result, apply_log_level level, std::int64_t unix_ms,
        std::string_view message, std::string_view phase) {
        auto& output = *result.add_logs();
        output.set_level(std::string(level_name(level)));
        output.set_unix_ms(unix_ms);
        output.set_message(std::string(message));
        output.set_phase(std::string(phase));
    }

    std::vector<recorded_step> steps_;
    std::vector<recorded_log> logs_;
    std::size_t omitted_logs_{};
    std::string current_phase_;
};

}  // namespace flexedge::node
