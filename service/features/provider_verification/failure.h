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

class VerificationError final : public std::runtime_error {
  public:
    VerificationError(std::string_view message, bool permanent)
        : std::runtime_error(std::string(message)), permanent_(permanent) {}

    [[nodiscard]] bool permanent() const noexcept { return permanent_; }

  private:
    bool permanent_;
};

struct TaskFailure final {
    std::string message;
    bool permanent{};
};

inline TaskFailure classifyTaskFailure(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        if (const auto* cloudflare = dynamic_cast<const service::dns::CloudflareError*>(&error)) {
            return {
                .message = service::sync_runtime::boundedError(cloudflare->what()),
                .permanent =
                    cloudflare->code() == service::dns::CloudflareErrorCode::credentialInvalid ||
                    cloudflare->code() == service::dns::CloudflareErrorCode::authorizationFailed,
            };
        }
        if (const auto* aliyun = dynamic_cast<const service::dns::AliyunError*>(&error)) {
            return {
                .message = service::sync_runtime::boundedError(aliyun->what()),
                .permanent = aliyun->code() == service::dns::AliyunErrorCode::credentialInvalid ||
                             aliyun->code() == service::dns::AliyunErrorCode::authorizationFailed,
            };
        }
        if (const auto* certificate =
                dynamic_cast<const service::certificate_issuance::CertificateProviderClientError*>(
                    &error)) {
            return {
                .message = service::sync_runtime::boundedError(certificate->what()),
                .permanent = !certificate->retryable(),
            };
        }
        if (const auto* verification = dynamic_cast<const VerificationError*>(&error)) {
            return {
                .message = service::sync_runtime::boundedError(verification->what()),
                .permanent = verification->permanent(),
            };
        }
        if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
            return {.message = service::sync_runtime::boundedError(error.what()),
                    .permanent = true};
        }
        return {.message = service::sync_runtime::boundedError(error.what()), .permanent = false};
    } catch (...) {
        return {.message = "供应商检测发生未知错误", .permanent = false};
    }
}

} // namespace service::provider_verification::detail
