#include <stdexcept>
#include <string>

#include "service/features/log_ingest/envelope.h"

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error("requirement failed: " #condition);                           \
    } while (false)

int main() {
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

    const auto bytes = service::log_ingest::serializeEnvelope(
        "16e29f7c-46c7-48b6-bf6a-880ee2b998af", "0123456789abcdef0123456789abcdef", delivery);
    const auto parsed = service::log_ingest::parseEnvelope(bytes);
    REQUIRE(parsed);
    REQUIRE(parsed->tenant_id() == "16e29f7c-46c7-48b6-bf6a-880ee2b998af");
    REQUIRE(parsed->agent_id() == "0123456789abcdef0123456789abcdef");
    REQUIRE(parsed->delivery().SerializeAsString() == delivery.SerializeAsString());

    auto invalidTenant = *parsed;
    invalidTenant.set_tenant_id("tenant-1");
    REQUIRE(!service::log_ingest::validEnvelope(invalidTenant));
    auto invalidAgent = *parsed;
    invalidAgent.set_agent_id("agent-1");
    REQUIRE(!service::log_ingest::validEnvelope(invalidAgent));
    REQUIRE(!service::log_ingest::parseEnvelope("not-protobuf"));

    access->clear_protocol();
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->set_protocol("HTTP/2");
    access->clear_method();
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->set_method("GET");
    access->clear_host();
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->set_host(std::string(flexedge::node::log_contract::kMaxHostBytes, 'h'));
    REQUIRE(service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->set_host(std::string(flexedge::node::log_contract::kMaxHostBytes + 1, 'h'));
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->set_host("www.example.com");
    access->clear_target();
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->set_target("/asset.js?q=1");
    access->set_referer(std::string(flexedge::node::log_contract::kMaxRefererBytes, 'r'));
    REQUIRE(service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->mutable_referer()->push_back('r');
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));

    flexedge::node::v2::LogDelivery nodeDelivery;
    nodeDelivery.set_node_id(delivery.node_id());
    auto* nodeLog = nodeDelivery.add_events()->mutable_node_log();
    nodeLog->set_id("2ca925d1-17cd-4b78-8d92-85752fe4f573");
    nodeLog->set_occurred_unix_ms(1787769815886);
    nodeLog->set_level("info");
    nodeLog->set_message("worker started");
    REQUIRE(!service::log_ingest::validDelivery(nodeDelivery, nodeDelivery.node_id()));
    nodeLog->set_category("runtime");
    REQUIRE(service::log_ingest::validDelivery(nodeDelivery, nodeDelivery.node_id()));

    access->clear_referer();
    access->set_request_body(std::string(flexedge::node::log_contract::kMaxRequestBodyBytes, 'x'));
    REQUIRE(service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->mutable_request_body()->push_back('x');
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    access->mutable_request_body()->resize(flexedge::node::log_contract::kMaxRequestBodyBytes);
    auto* second = delivery.add_events();
    *second = delivery.events(0);
    second->mutable_access_log()->set_id("2ca925d1-17cd-4b78-8d92-85752fe4f573");
    REQUIRE(delivery.ByteSizeLong() > flexedge::node::log_contract::kMaxDeliveryBytes);
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    delivery.mutable_events()->RemoveLast();
    access = delivery.mutable_events(0)->mutable_access_log();
    access->clear_request_body();
    delivery.mutable_events(0)->mutable_access_log()->set_status_code(99);
    REQUIRE(!service::log_ingest::validDelivery(delivery, delivery.node_id()));
    return 0;
}
