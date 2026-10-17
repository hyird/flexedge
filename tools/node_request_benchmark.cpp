#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "node/data/http1_request_reader.h"
#include "node/data/origin_health.h"
#include "node/data/origin_request_codec.h"
#include "node/data/origin_selection.h"
#include "node/data/response_cache.h"
#include "node/data/route_rules.h"
#include "tests/support/cache_directory.h"
#include "tools/node_request_allocations.h"

namespace {
volatile std::uint64_t observed_checksum{};

constexpr std::string_view hostname{"node-benchmark.example.invalid"};
constexpr std::string_view target{"/assets/catalog/2026/archive/resource/file.json?search=two&channel=stable"};
constexpr std::string_view origin_group{"production-origin-group-for-benchmark"};
const std::array<std::pair<std::string_view, std::string_view>, 3> headers{{{"Host", hostname}, {"X-Channel", "stable"}, {"Accept", "application/json"}}};

flexedge::node::v2::Website make_website(std::size_t origin_count, std::size_t rule_count) {
    flexedge::node::v2::Website website;
    website.set_id("website-node-performance-benchmark");
    website.set_default_origin_group(origin_group);
    for (std::size_t index = 0; index != origin_count; ++index) {
        auto* origin = website.add_origins();
        origin->set_id("origin-performance-benchmark-" + std::to_string(index));
        origin->set_group(origin_group);
        origin->set_enabled(true);
        origin->set_role(index < (origin_count * 3 + 3) / 4 ? "primary" : "backup");
        origin->set_weight(index % 4 + 1);
    }
    for (std::size_t index = 0; index != rule_count; ++index) {
        auto* rule = website.add_route_rules();
        rule->set_id("route-performance-benchmark-" + std::to_string(index));
        rule->set_enabled(true);
        rule->set_action("proxy");
        rule->set_match_type("prefix");
        rule->set_path("/assets");
        rule->set_origin_group(origin_group);
        rule->add_hostnames(hostname);
        auto* condition = rule->add_conditions();
        condition->set_source("header");
        condition->set_name("X-Channel");
        condition->set_op("equals");
        condition->set_value("stable");
        auto* request = rule->add_request_headers();
        request->set_name(index % 2 == 0 ? "X-Edge-Request-Context" : "X-Edge-Request-Policy");
        request->set_value("compiled-request-policy-value-" + std::to_string(index));
        auto* response = rule->add_response_headers();
        response->set_name("X-Edge-Response-Policy");
        response->set_value("compiled-response-policy-value-" + std::to_string(index));
    }
    return website;
}

std::shared_ptr<const flexedge::node::origin_health_snapshot> make_health(
    const flexedge::node::v2::Website& website, bool mixed) {
    flexedge::node::origin_health_snapshot::origin_map origins;
    if (mixed) {
        for (int index = 0; index != website.origins_size(); ++index) {
            origins.emplace(flexedge::node::origin_health_key{website.id(), website.origins(index).id()},
                flexedge::node::origin_health_snapshot::origin{.healthy_ = index % 3 != 0});
        }
    }
    return std::make_shared<const flexedge::node::origin_health_snapshot>(std::move(origins));
}

template <typename operation_type>
void measure(std::string_view name, std::size_t iterations, operation_type operation) {
    std::uint64_t checksum{};
    for (std::size_t index = 0; index != 1000; ++index) {
        checksum += operation(index);
    }
    flexedge::node::benchmark::start_allocation_count();
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index != iterations; ++index) {
        checksum += operation(index);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto allocation_count = flexedge::node::benchmark::stop_allocation_count();
    observed_checksum = checksum;
    const auto nanoseconds = std::chrono::duration<double, std::nano>(elapsed).count();
    std::printf("%.*s,%zu,%.3f,%.6f,%llu\n", static_cast<int>(name.size()), name.data(), iterations,
        nanoseconds / static_cast<double>(iterations),
        static_cast<double>(allocation_count) / static_cast<double>(iterations),
        static_cast<unsigned long long>(checksum));
}

void benchmark_route(std::string_view name, std::size_t rules, std::size_t iterations) {
    const auto website = make_website(16, rules);
    const flexedge::node::route_program program(website);
    measure(name, iterations, [&](std::size_t) {
        const auto route = flexedge::node::evaluate_route_rules(program, website, "GET", target, hostname, headers);
        if (!route) {
            throw std::runtime_error("benchmark route evaluation failed");
        }
        return route->id().size() + route->rewritten_target().size() +
               route->request_headers().size() + route->response_headers().size();
    });
}

void benchmark_request(std::string_view name, std::size_t body_size,
    std::size_t field_size, bool upgrade, bool routed, std::size_t iterations) {
    auto website = make_website(16, 16);
    website.set_pass_client_ip(true);
    const flexedge::node::route_program program(website);
    const auto route = flexedge::node::evaluate_route_rules(
        program, website, "GET", target, hostname, headers);
    if (!route) {
        throw std::runtime_error("request benchmark route evaluation failed");
    }
    const std::string body(body_size, 'b');
    const std::string field(field_size, 'v');
    const std::array<ruvia::http_header_view, 10> incoming_headers{{
        {"Host", hostname},
        {"User-Agent", "node-request-performance-benchmark"},
        {"Accept-Encoding", "gzip, br"},
        {"X-Edge-Request-Context", "incoming-request-context-to-be-overridden"},
        {"X-Large", field},
        {"X-Forwarded-For", "198.51.100.2"},
        {"Content-Type", "application/octet-stream"},
        {"X-Channel", "stable"},
        {"Connection", upgrade ? "Upgrade" : "keep-alive"},
        {"Upgrade", upgrade ? "websocket" : ""},
    }};
    const flexedge::node::origin_request_view incoming{
        .method_ = body.empty() ? "GET" : "POST",
        .target_ = target,
        .headers_ = incoming_headers,
        .body_ = body,
        .has_body_ = !body.empty(),
    };
    measure(name, iterations, [&](std::size_t) {
        const auto prepared = flexedge::node::prepare_origin_request(
            incoming, website, hostname, "198.51.100.10", true, upgrade, false,
            true, routed ? &*route : nullptr);
        if (!prepared) {
            throw std::runtime_error("request benchmark preparation failed");
        }
        const auto state = prepared->make_exchange_state();
        (void)state;
        return prepared->bytes_.size() + prepared->target_.size();
    });
}

void benchmark_read(std::string_view name, std::string_view wire,
    std::size_t fragment_size, std::size_t iterations) {
    measure(name, iterations, [&](std::size_t) {
        flexedge::node::http1_request_reader reader(2 * 1024 * 1024);
        std::string buffer;
        buffer.reserve(8192);
        auto status = flexedge::node::http1_request_read_status::need_more;
        for (std::size_t offset = 0; offset < wire.size();) {
            const auto size = std::min(fragment_size, wire.size() - offset);
            buffer.append(wire.substr(offset, size));
            offset += size;
            status = reader.consume(buffer);
            if (status != flexedge::node::http1_request_read_status::need_more) {
                break;
            }
        }
        const auto body = reader.body();
        if (status != flexedge::node::http1_request_read_status::complete || !body) {
            throw std::runtime_error("request reader benchmark failed");
        }
        return reader.request().target().size() + reader.request().headers().size() +
               body->size() + reader.message_bytes() + reader.unconsumed_bytes().size();
    });
}

void benchmark_reads(std::size_t iterations) {
    const auto fields = " " + std::string(target) + " HTTP/1.1\r\nHost: " +
                        std::string(hostname) + "\r\nX-Channel: stable\r\n";
    const auto get = "GET" + fields + "\r\n";
    const auto post4k = "POST" + fields + "Content-Length: 4096\r\n\r\n" + std::string(4096, 'b');
    const auto post64k = "POST" + fields + "Content-Length: 65536\r\n\r\n" + std::string(65536, 'b');
    const auto large = "GET" + fields + "X-Large: " + std::string(20000, 'v') + "\r\n\r\n";
    const auto chunked = "POST" + fields +
                         "Transfer-Encoding: chunked\r\n\r\n4\r\nDATA\r\n5\r\nMORE!\r\n0\r\nX-End: yes\r\n\r\n";
    const auto pipeline = get + get;
    benchmark_read("read_get", get, get.size(), iterations);
    benchmark_read("read_fragmented_get", get, 17, iterations);
    benchmark_read("read_post_4k", post4k, post4k.size(), iterations);
    benchmark_read("read_post_64k_fragmented", post64k, 8192, iterations);
    benchmark_read("read_head_20k", large, large.size(), iterations);
    benchmark_read("read_chunked_fragmented", chunked, 17, iterations);
    benchmark_read("read_pipeline_tail", pipeline, pipeline.size(), iterations);
}

void benchmark_origins(std::string_view name, std::size_t origins, bool mixed,
    bool exhaust, std::size_t iterations) {
    const auto website = make_website(origins, 0);
    const auto health = make_health(website, mixed);
    const flexedge::node::origin_plan plan(website, website.default_origin_group());
    measure(name, iterations, [&](std::size_t sequence) {
        flexedge::node::origin_cursor candidates(plan, sequence, health);
        if (exhaust) {
            std::uint64_t checksum{};
            while (const auto* candidate = candidates.next()) {
                checksum += candidate->weight();
            }
            return checksum;
        }
        return static_cast<std::uint64_t>(candidates.next()->weight());
    });
}

flexedge::node::v2::Website make_cache_website(bool settings, std::size_t rule_count) {
    flexedge::node::v2::Website website;
    website.set_id("cache-benchmark-website-identity");
    website.set_revision(17);
    flexedge::cache::policy policy;
    policy.id_ = "cache-benchmark-policy-identity";
    policy.revision_ = 19;
    policy.storage_ = "memory";
    for (std::size_t index = 0; index != rule_count; ++index) {
        flexedge::cache::rule rule;
        rule.id_ = "cache-benchmark-rule-identity-" + std::to_string(index);
        rule.name_ = "cache benchmark matching rule " + std::to_string(index);
        rule.match_type_ = "prefix";
        rule.patterns_ = {"/assets/cache-performance/"};
        rule.ttl_mode_ = "fixed";
        rule.ttl_seconds_ = 600;
        rule.status_codes_ = {200, 301};
        if (settings) {
            auto& value = rule.settings_.emplace();
            value.enabled_ = {"edge", "key", "status", "ports"};
            value.key_.query_mode_ = "include";
            value.key_.query_parameters_ = {"variant", "download_name_with_long_value"};
            value.key_.headers_ = {"x-cache-benchmark-context-1", "x-cache-benchmark-context-2"};
            value.status_ttl_ = {{200, 299, 600}, {404, 404, 30}};
            value.ports_ = {80, 443, 8080, 8443};
        }
        policy.rules_.push_back(std::move(rule));
    }
    flexedge::cache::to_proto(policy, *website.mutable_response_cache());
    return website;
}

void benchmark_cache_matching(std::string_view name, std::size_t iterations, bool settings, std::size_t rules, bool matched) {
    flexedge::testing::temporary_cache_directory files;
    flexedge::node::response_cache cache(std::make_shared<flexedge::node::response_cache_publication>(files.path_));
    const auto website = make_cache_website(settings, rules);
    const auto request_target = matched ? "/assets/cache-performance/object.css?color=blue&lang=en" : "/not-matched/object.css";
    measure(name, iterations, [&](std::size_t) {
        auto selected = cache.matching(website, request_target, "GET");
        if (!selected) {
            return std::uint64_t{7};
        }
        auto session_rule = std::move(*selected);
        return static_cast<std::uint64_t>(session_rule.id_.size() + session_rule.methods_.size() + session_rule.ttl_seconds_);
    });
}

void benchmark_cache_lookup(std::string_view name, std::size_t iterations, std::string_view request_target, bool ignore_query) {
    flexedge::testing::temporary_cache_directory files;
    flexedge::node::response_cache cache(std::make_shared<flexedge::node::response_cache_publication>(files.path_));
    const auto website = make_cache_website(false, 1);
    auto rule = flexedge::cache::from_proto(website.response_cache()).rules_.front();
    rule.ignore_query_ = ignore_query;
    const std::string address = "http://cache-benchmark.example" + std::string(request_target);
    const auto ticket = cache.lookup(website, rule, address, {});
    if (!ticket.ticket_) {
        throw std::runtime_error("missing initial cache ticket");
    }
    auto fill = cache.begin_fill(website.response_cache(), rule,
        {200, {{"Content-Type", "text/css"}, {"Cache-Control", "public, max-age=600"}}}, {});
    if (!fill || !fill->append("a{color:blue}") || !cache.store(*ticket.ticket_, website.response_cache(), rule, *fill, {})) {
        throw std::runtime_error("cannot populate cache benchmark entry");
    }
    measure(name, iterations, [&](std::size_t) {
        const auto hit = cache.lookup(website, rule, address, {}, false);
        if (!hit.response_ || !hit.response_->body_ || *hit.response_->body_ != "a{color:blue}") {
            throw std::runtime_error("cache benchmark content mismatch");
        }
        return hit.response_->body_bytes_ + hit.key_.size();
    });
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto iterations = argc > 1 ? std::stoull(argv[1]) : 200000;
        if (iterations == 0) {
            throw std::invalid_argument("iterations must be positive");
        }
        std::puts("case,iterations,ns_per_request,cpp_allocations_per_request,checksum");
        benchmark_route("route_default", 0, iterations);
        benchmark_route("route_1_proxy", 1, iterations);
        benchmark_route("route_16_proxy", 16, iterations);
        benchmark_origins("origins_2_first", 2, false, false, iterations);
        benchmark_origins("origins_16_first", 16, false, false, iterations);
        benchmark_origins("origins_64_first", 64, false, false, iterations);
        benchmark_origins("origins_64_mixed_first", 64, true, false, iterations);
        benchmark_origins("origins_64_mixed_exhaust", 64, true, true, iterations);
        benchmark_request("request_get", 0, 32, false, false, iterations);
        benchmark_request("request_routed", 0, 32, false, true, iterations);
        benchmark_request("request_post_4k", 4096, 32, false, true, iterations);
        benchmark_request("request_head_15k", 0, 15000, false, true, iterations);
        benchmark_request("request_head_20k", 0, 20000, false, true, iterations);
        benchmark_request("request_upgrade", 0, 32, true, true, iterations);
        benchmark_reads(iterations);
        benchmark_cache_matching("cache_legacy_1", iterations, false, 1, true);
        benchmark_cache_matching("cache_legacy_8", iterations, false, 8, true);
        benchmark_cache_matching("cache_legacy_unmatched", iterations, false, 1, false);
        benchmark_cache_matching("cache_settings_1", iterations, true, 1, true);
        benchmark_cache_matching("cache_settings_8", iterations, true, 8, true);
        benchmark_cache_matching("cache_settings_unmatched", iterations, true, 1, false);
        benchmark_cache_lookup("cache_hit_path", iterations, "/assets/cache-performance/object-with-a-long-identity.css", false);
        benchmark_cache_lookup("cache_hit_query", iterations, "/assets/cache-performance/object.css?variant=blue&language=english", false);
        benchmark_cache_lookup("cache_hit_ignored_query", iterations, "/assets/cache-performance/object.css?variant=blue&language=english", true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
