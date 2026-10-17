#pragma once
#include "common/cache_settings.h"
namespace flexedge::cache {
template <typename proto_type>
void settings_to_proto(const status_ttl& input, proto_type& output) {
    output.set_from(input.from_);
    output.set_to(input.to_);
    output.set_seconds(input.seconds_);
}
template <typename proto_type>
status_ttl status_ttl_from_proto(const proto_type& input) {
    status_ttl result;
    result.from_ = input.from();
    result.to_ = input.to();
    result.seconds_ = input.seconds();
    return result;
}
template <typename proto_type>
void settings_to_proto(const header_key& input, proto_type& output) {
    output.set_name(input.name_);
    for (const auto& value : input.values_) {
        output.add_values(value);
    }
}
template <typename proto_type>
header_key header_key_from_proto(const proto_type& input) {
    header_key result;
    result.name_ = input.name();
    result.values_ = {input.values().begin(), input.values().end()};
    return result;
}
template <typename proto_type>
void settings_to_proto(const cache_key& input, proto_type& output) {
    output.set_query_mode(input.query_mode_);
    for (const auto& value : input.query_parameters_) {
        output.add_query_parameters(value);
    }
    for (const auto& value : input.headers_) {
        output.add_headers(value);
    }
    for (const auto& value : input.header_presence_) {
        output.add_header_presence(value);
    }
    for (const auto& value : input.header_values_) {
        settings_to_proto(value, *output.add_header_values());
    }
    for (const auto& value : input.cookies_) {
        output.add_cookies(value);
    }
    for (const auto& value : input.cookie_presence_) {
        output.add_cookie_presence(value);
    }
    output.set_include_origin(input.include_origin_);
    output.set_host(input.host_);
    output.set_country(input.country_);
    output.set_language(input.language_);
}
template <typename proto_type>
cache_key cache_key_from_proto(const proto_type& input) {
    cache_key result;
    result.query_mode_ = input.query_mode();
    result.query_parameters_ = {input.query_parameters().begin(), input.query_parameters().end()};
    result.headers_ = {input.headers().begin(), input.headers().end()};
    result.header_presence_ = {input.header_presence().begin(), input.header_presence().end()};
    for (const auto& value : input.header_values()) {
        result.header_values_.push_back(header_key_from_proto(value));
    }
    result.cookies_ = {input.cookies().begin(), input.cookies().end()};
    result.cookie_presence_ = {input.cookie_presence().begin(), input.cookie_presence().end()};
    result.include_origin_ = input.include_origin();
    result.host_ = input.host();
    result.country_ = input.country();
    result.language_ = input.language();
    return result;
}
template <typename proto_type>
void settings_to_proto(const vary_header& input, proto_type& output) {
    output.set_name(input.name_);
    output.set_action(input.action_);
    for (const auto& value : input.values_) {
        output.add_values(value);
    }
}
template <typename proto_type>
vary_header vary_header_from_proto(const proto_type& input) {
    vary_header result;
    result.name_ = input.name();
    result.action_ = input.action();
    result.values_ = {input.values().begin(), input.values().end()};
    return result;
}
template <typename proto_type>
void settings_to_proto(const rule_settings& input, proto_type& output) {
    for (const auto& value : input.enabled_) {
        output.add_enabled(value);
    }
    for (const auto& value : input.status_ttl_) {
        settings_to_proto(value, *output.add_status_ttl());
    }
    settings_to_proto(input.key_, *output.mutable_key());
    output.set_armor(input.armor_);
    output.set_device(input.device_);
    output.set_sort_query(input.sort_query_);
    output.set_serve_stale(input.serve_stale_);
    output.set_strong_etags(input.strong_etags_);
    output.set_origin_control(input.origin_control_);
    output.set_vary_default(input.vary_default_);
    for (const auto& value : input.vary_headers_) {
        settings_to_proto(value, *output.add_vary_headers());
    }
    output.set_origin_range(input.origin_range_);
    for (const auto& value : input.ports_) {
        output.add_ports(value);
    }
    output.set_read_timeout(input.read_timeout_);
    output.set_error_passthrough(input.error_passthrough_);
}
template <typename proto_type>
rule_settings rule_settings_from_proto(const proto_type& input) {
    rule_settings result;
    result.enabled_ = {input.enabled().begin(), input.enabled().end()};
    for (const auto& value : input.status_ttl()) {
        result.status_ttl_.push_back(status_ttl_from_proto(value));
    }
    result.key_ = cache_key_from_proto(input.key());
    result.armor_ = input.armor();
    result.device_ = input.device();
    result.sort_query_ = input.sort_query();
    result.serve_stale_ = input.serve_stale();
    result.strong_etags_ = input.strong_etags();
    result.origin_control_ = input.origin_control();
    result.vary_default_ = input.vary_default();
    for (const auto& value : input.vary_headers()) {
        result.vary_headers_.push_back(vary_header_from_proto(value));
    }
    result.origin_range_ = input.origin_range();
    result.ports_ = {input.ports().begin(), input.ports().end()};
    result.read_timeout_ = input.read_timeout();
    result.error_passthrough_ = input.error_passthrough();
    return result;
}
}  // namespace flexedge::cache
