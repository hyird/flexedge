#pragma once

#include <exception>
#include <string>
#include <string_view>

#include "service/features/dns/aliyun.h"
#include "service/features/dns/cloudflare.h"

namespace service::dns_sync::detail {

// The raw error of a failed attempt; fail_dns_task redacts, then bounds it.
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
                .message_ = cloudflare->what(),
                .permanent_ =
                    cloudflare->code() == service::dns::cloudflare_error_code::authorization_failed ||
                    cloudflare->code() == service::dns::cloudflare_error_code::credential_invalid ||
                    cloudflare->code() == service::dns::cloudflare_error_code::record_conflict,
            };
        }
        if (const auto* aliyun = dynamic_cast<const service::dns::aliyun_error*>(&error)) {
            return {
                .message_ = aliyun->what(),
                .permanent_ = aliyun->code() == service::dns::aliyun_error_code::authorization_failed ||
                              aliyun->code() == service::dns::aliyun_error_code::credential_invalid ||
                              aliyun->code() == service::dns::aliyun_error_code::record_conflict ||
                              aliyun->code() == service::dns::aliyun_error_code::domain_not_found,
            };
        }
        return {.message_ = error.what(), .permanent_ = false};
    } catch (...) {
        return {.message_ = "同步任务发生未知错误", .permanent_ = false};
    }
}

}  // namespace service::dns_sync::detail
