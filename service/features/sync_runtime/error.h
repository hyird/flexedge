#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "service/features/attempt_trace/recorder.h"
#include "service/features/attempt_trace/redaction.h"
#include "service/features/logging/logger.h"

namespace service::sync_runtime {

inline constexpr std::size_t max_error_message_length{1000};

// Keeps at most max_error_message_length bytes without splitting a UTF-8
// character: PostgreSQL rejects the whole statement on a truncated sequence.
[[nodiscard]] inline std::string bounded_error(std::string_view value) {
    if (value.size() <= max_error_message_length) {
        return std::string(value);
    }
    auto length = max_error_message_length;
    while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80) {
        --length;
    }
    return std::string(value.substr(0, length));
}

// The error of a failed attempt as stored (task error, sys_sync_event.error,
// the resource's last error) and logged: the attempt's credentials and the
// credential patterns are redacted before the text is bounded, so a cut
// never leaves part of a secret behind.
[[nodiscard]] inline std::string attempt_error(service::attempt_trace::recorder& trace, std::string_view error) {
    return bounded_error(trace.redact_error(error));
}

// A failure that escaped every attempt, as a worker loop logs it: no attempt
// credentials are in scope, so only the credential patterns are redacted.
[[nodiscard]] inline std::string worker_error(std::string_view error) {
    return bounded_error(service::attempt_trace::redact_patterns(error));
}

// The server log line of a failed attempt; `message` comes from
// attempt_error(). Building the line may fail on allocation; the attempt's
// result is already recorded, so the line is then skipped.
inline void log_attempt_failure(std::string_view subject, std::string_view id, std::string_view message) noexcept {
    try {
        service::logging::error(std::string(subject) + " " + std::string(id) + " failed: " + std::string(message));
    } catch (...) {
        return;
    }
}

}  // namespace service::sync_runtime
