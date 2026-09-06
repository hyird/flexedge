#pragma once

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>

#include "service/features/dns/aliyun.h"
#include "service/features/dns/cloudflare.h"

namespace service::dns_sync::detail {

struct TaskFailure final {
    std::string message;
    bool permanent{};
};

inline std::string boundedError(std::string_view value) {
    constexpr std::size_t limit{1000};
    return std::string(value.substr(0, std::min(value.size(), limit)));
}

inline TaskFailure classifyTaskFailure(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        if (const auto* cloudflare = dynamic_cast<const service::dns::CloudflareError*>(&error)) {
            return {
                .message = boundedError(cloudflare->what()),
                .permanent =
                    cloudflare->code() == service::dns::CloudflareErrorCode::authorizationFailed ||
                    cloudflare->code() == service::dns::CloudflareErrorCode::credentialInvalid ||
                    cloudflare->code() == service::dns::CloudflareErrorCode::recordConflict,
            };
        }
        if (const auto* aliyun = dynamic_cast<const service::dns::AliyunError*>(&error)) {
            return {
                .message = boundedError(aliyun->what()),
                .permanent = aliyun->code() == service::dns::AliyunErrorCode::authorizationFailed ||
                             aliyun->code() == service::dns::AliyunErrorCode::credentialInvalid ||
                             aliyun->code() == service::dns::AliyunErrorCode::recordConflict ||
                             aliyun->code() == service::dns::AliyunErrorCode::domainNotFound,
            };
        }
        return {.message = boundedError(error.what()), .permanent = false};
    } catch (...) {
        return {.message = "同步任务发生未知错误", .permanent = false};
    }
}

} // namespace service::dns_sync::detail
