#pragma once

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/certificate/provider.h"
#include "service/features/dns/aliyun.h"
#include "service/features/dns/cloudflare.h"
#include "service/features/sync_runtime/error.h"

namespace service::provider_verification::detail {

class verification_error final : public std::runtime_error {
public:
    verification_error(std::string_view message, bool permanent)
        : std::runtime_error(std::string(message)),
          permanent_(permanent) {}

    [[nodiscard]] bool permanent() const noexcept {
        return permanent_;
    }

private:
    bool permanent_;
};

struct task_failure final {
    std::string message_;
    bool permanent_{};
};

inline task_failure classify_task_failure(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        if (const auto* cloudflare = dynamic_cast<const service::dns::cloudflare_error*>(&error)) {
            return {
                .message_ = service::sync_runtime::bounded_error(cloudflare->what()),
                .permanent_ =
                    cloudflare->code() == service::dns::cloudflare_error_code::credential_invalid ||
                    cloudflare->code() == service::dns::cloudflare_error_code::authorization_failed,
            };
        }
        if (const auto* aliyun = dynamic_cast<const service::dns::aliyun_error*>(&error)) {
            return {
                .message_ = service::sync_runtime::bounded_error(aliyun->what()),
                .permanent_ = aliyun->code() == service::dns::aliyun_error_code::credential_invalid ||
                              aliyun->code() == service::dns::aliyun_error_code::authorization_failed,
            };
        }
        if (const auto* certificate =
                dynamic_cast<const service::certificate_issuance::certificate_provider_client_error*>(
                    &error)) {
            return {
                .message_ = service::sync_runtime::bounded_error(certificate->what()),
                .permanent_ = !certificate->retryable(),
            };
        }
        if (const auto* verification = dynamic_cast<const verification_error*>(&error)) {
            return {
                .message_ = service::sync_runtime::bounded_error(verification->what()),
                .permanent_ = verification->permanent(),
            };
        }
        if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
            return {.message_ = service::sync_runtime::bounded_error(error.what()),
                .permanent_ = true};
        }
        return {.message_ = service::sync_runtime::bounded_error(error.what()), .permanent_ = false};
    } catch (...) {
        return {.message_ = "供应商检测发生未知错误", .permanent_ = false};
    }
}

}  // namespace service::provider_verification::detail
