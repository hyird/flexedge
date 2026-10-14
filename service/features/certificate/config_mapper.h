#pragma once

#include <ruvia/web/model_json.h>

#include <optional>
#include <string_view>

#include "service/features/certificate/config_transport.h"
#include "service/features/certificate/model.h"

namespace service::certificate_issuance {

[[nodiscard]] inline std::optional<certificate_config_data>
normalize(const certificate_config_input& input) {
    const auto& auto_renew = input.get<"auto_renew">();
    if (!auto_renew) {
        return std::nullopt;
    }
    return certificate_config_data{.auto_renew_ = auto_renew->value_};
}

[[nodiscard]] inline std::optional<certificate_config_data>
parse_config_stored(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<certificate_config_input> input =
        ruvia::from_json<certificate_config_input>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline certificate_config_output to_output(const certificate_config_data& input,
    ruvia::model_options options = {}) {
    certificate_config_output output(options);
    output.set<"auto_renew">(input.auto_renew_);
    return output;
}
}  // namespace service::certificate_issuance
