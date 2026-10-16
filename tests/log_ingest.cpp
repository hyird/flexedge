#include <stdexcept>
#include <string>

#include "service/features/log_ingest/envelope.h"
#include "service/features/log_ingest/log_mapper.h"
#include "service/features/log_ingest/tail_cursor.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

int main() {
    const auto cursor = service::log_ingest::parse_tail_cursor(
        "123:01234567-89AB-CDEF-0123-456789ABCDEF");
    REQUIRE(cursor && cursor->ingested_unix_micros_ == 123);
    REQUIRE(service::log_ingest::encode_tail_cursor(cursor->ingested_unix_micros_, cursor->id_) ==
            "123:01234567-89ab-cdef-0123-456789abcdef");
    for (const auto invalid : {"", "0:01234567-89ab-cdef-0123-456789abcdef",
             "-1:01234567-89ab-cdef-0123-456789abcdef",
             "9223372036854775808:01234567-89ab-cdef-0123-456789abcdef",
             "123:not-a-uuid", "123x:01234567-89ab-cdef-0123-456789abcdef"}) {
        REQUIRE(!service::log_ingest::parse_tail_cursor(invalid));
    }
    flexedge::node::v2::LogDelivery delivery;
    delivery.set_node_id("920dc14b-cc26-49e4-9937-93a7637bef79");
    auto* access = delivery.add_events()->mutable_access_log();
    access->set_id("b71e4ffc-56df-4e47-b947-b9a60e40996d");
    access->set_occurred_unix_ms(1787769815886);
    access->set_website_id("b2d6e77d-4a0c-4796-93d5-755d3c6d3837");
    access->set_client_ip("192.0.2.1");
    access->set_protocol("HTTP/2");
    access->set_method("GET");
    access->set_host("www.example.com");
    access->set_target("/asset.js?q=1");
    access->set_status_code(200);
    access->set_response_bytes(4096);
    access->set_duration_ms(12);

    const auto bytes = service::log_ingest::serialize_envelope(
        "16e29f7c-46c7-48b6-bf6a-880ee2b998af", "0123456789abcdef0123456789abcdef", delivery);
    const auto parsed = service::log_ingest::parse_envelope(bytes);
    REQUIRE(parsed);
    REQUIRE(parsed->tenant_id() == "16e29f7c-46c7-48b6-bf6a-880ee2b998af");
    REQUIRE(parsed->agent_id() == "0123456789abcdef0123456789abcdef");
    REQUIRE(parsed->delivery().SerializeAsString() == delivery.SerializeAsString());

    auto mutable_source = *parsed;
    const auto rows = service::log_ingest::to_log_rows(mutable_source, 1787769816000);
    REQUIRE(rows.access_.size() == 1 && rows.node_.empty());
    REQUIRE(rows.websites_ == std::vector<std::string>{access->website_id()});
    REQUIRE(rows.access_[0].get<"occurred_at">() == "2026-08-26T18:43:35.886Z");
    REQUIRE(rows.access_[0].get<"response_bytes">() == 4096);
    REQUIRE(rows.access_[0].get<"duration_ms">() == 12);
    REQUIRE(!rows.access_[0].is_set<"created_at">());
    mutable_source.mutable_delivery()->mutable_events(0)->mutable_access_log()->set_target("/changed");
    REQUIRE(rows.access_[0].get<"target">() == "/asset.js?q=1");
    REQUIRE(service::log_ingest::occurred_timestamp(1787770116001, 1787769816000) == "2026-08-26T18:43:36.000Z");
    REQUIRE(service::log_ingest::occurred_timestamp(1787770116000, 1787769816000) == "2026-08-26T18:48:36.000Z");

    auto invalid_tenant = *parsed;
    invalid_tenant.set_tenant_id("tenant-1");
    REQUIRE(!service::log_ingest::valid_envelope(invalid_tenant));
    auto invalid_agent = *parsed;
    invalid_agent.set_agent_id("agent-1");
    REQUIRE(!service::log_ingest::valid_envelope(invalid_agent));
    REQUIRE(!service::log_ingest::parse_envelope("not-protobuf"));

    access->clear_protocol();
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->set_protocol("HTTP/2");
    access->clear_method();
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->set_method("GET");
    access->clear_host();
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->set_host(std::string(flexedge::node::log_contract::max_host_bytes, 'h'));
    REQUIRE(service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->set_host(std::string(flexedge::node::log_contract::max_host_bytes + 1, 'h'));
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->set_host("www.example.com");
    access->clear_target();
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->set_target("/asset.js?q=1");
    access->set_referer(std::string(flexedge::node::log_contract::max_referer_bytes, 'r'));
    REQUIRE(service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->mutable_referer()->push_back('r');
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));

    flexedge::node::v2::LogDelivery node_delivery;
    node_delivery.set_node_id(delivery.node_id());
    auto* node_log = node_delivery.add_events()->mutable_node_log();
    node_log->set_id("2ca925d1-17cd-4b78-8d92-85752fe4f573");
    node_log->set_occurred_unix_ms(1787769815886);
    node_log->set_level("info");
    node_log->set_message("worker started");
    REQUIRE(!service::log_ingest::valid_delivery(node_delivery, node_delivery.node_id()));
    node_log->set_category("runtime");
    REQUIRE(service::log_ingest::valid_delivery(node_delivery, node_delivery.node_id()));

    access->clear_referer();
    access->set_request_body(std::string(flexedge::node::log_contract::max_request_body_bytes, 'x'));
    REQUIRE(service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->mutable_request_body()->push_back('x');
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    access->mutable_request_body()->resize(flexedge::node::log_contract::max_request_body_bytes);
    auto* second = delivery.add_events();
    *second = delivery.events(0);
    second->mutable_access_log()->set_id("2ca925d1-17cd-4b78-8d92-85752fe4f573");
    REQUIRE(delivery.ByteSizeLong() > flexedge::node::log_contract::max_delivery_bytes);
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    delivery.mutable_events()->RemoveLast();
    access = delivery.mutable_events(0)->mutable_access_log();
    access->clear_request_body();
    delivery.mutable_events(0)->mutable_access_log()->set_status_code(99);
    REQUIRE(!service::log_ingest::valid_delivery(delivery, delivery.node_id()));
    return 0;
}
