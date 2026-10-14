#pragma once

#include <ruvia/web/model_json.h>

#include <optional>
#include <string>
#include <string_view>

#include "ruvia/web/model.h"

namespace service::certificate_material {

RUVIA_MODEL(
    certificate_material_input,
    RUVIA_OPTIONAL_FIELD_NAME("certificate_chain_pem", certificate_chain_pem, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("private_key_envelope", private_key_envelope, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("not_before", not_before, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("serial_number", serial_number, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("fingerprint_sha256", fingerprint_sha256, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_issued_at", last_issued_at, ruvia::string));
RUVIA_MODEL(
    certificate_material_output,
    RUVIA_REQUIRED_FIELD_NAME("certificate_chain_pem", certificate_chain_pem, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("private_key_envelope", private_key_envelope, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("not_before", not_before, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("serial_number", serial_number, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("fingerprint_sha256", fingerprint_sha256, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("last_issued_at", last_issued_at, ruvia::string));

struct certificate_material_data final {
    std::optional<std::string> certificate_chain_pem_;
    std::optional<std::string> private_key_envelope_;
    std::optional<std::string> not_before_;
    std::optional<std::string> serial_number_;
    std::optional<std::string> fingerprint_sha256_;
    std::optional<std::string> last_issued_at_;
};

[[nodiscard]] inline certificate_material_data normalize(const certificate_material_input& input) {
    certificate_material_data result;
    if (const auto& value = input.get<"certificate_chain_pem">()) {
        result.certificate_chain_pem_ = std::string(value->view());
    }
    if (const auto& value = input.get<"private_key_envelope">()) {
        result.private_key_envelope_ = std::string(value->view());
    }
    if (const auto& value = input.get<"not_before">()) {
        result.not_before_ = std::string(value->view());
    }
    if (const auto& value = input.get<"serial_number">()) {
        result.serial_number_ = std::string(value->view());
    }
    if (const auto& value = input.get<"fingerprint_sha256">()) {
        result.fingerprint_sha256_ = std::string(value->view());
    }
    if (const auto& value = input.get<"last_issued_at">()) {
        result.last_issued_at_ = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline std::optional<certificate_material_data>
parse_stored(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<certificate_material_input> input =
        ruvia::from_json<certificate_material_input>(json, options);
    return input ? std::optional<certificate_material_data>{normalize(*input)} : std::nullopt;
}

}  // namespace service::certificate_material
