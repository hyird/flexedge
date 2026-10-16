#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/websocket.h>

#include <chrono>
#include <string>

#include "node/proto/control_protocol.h"
#include "service/features/node_channel/session.h"
#include "service/features/node_release/catalog.h"

namespace service::agent {

class control_protocol_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next) {
        if (c.req().header("Sec-WebSocket-Protocol").value_or("") !=
            flexedge::node::control_subprotocol) {
            auto response = c.error({.status_ = ruvia::http_status::upgrade_required,
                .code_ = "control_protocol_required",
                .message_ = "unsupported node control protocol"});
            response.header("Upgrade", "websocket");
            response.header("Sec-WebSocket-Protocol", flexedge::node::control_subprotocol);
            c.respond(std::move(response));
            co_return;
        }
        co_await next();
    }
};

class agent_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/agent")
    RUVIA_ROUTES_BEGIN(agent_controller)
    RUVIA_GET("/install-node.sh", install_node_script);
    RUVIA_GET("/node", download_node);
    const auto control_options = ruvia::websocket_route_config{
        .subprotocols_ = {std::string(flexedge::node::control_subprotocol)},
        .lifecycle_ =
            {
                .heartbeat_ =
                    {
                        .ping_interval_ = std::chrono::seconds(30),
                        .pong_timeout_ = std::chrono::seconds(15),
                    },
                .close_handshake_timeout_ = std::chrono::seconds(5),
            },
    };
    RUVIA_GET_WS_OPTIONS("/connect", connect, control_options, control_protocol_middleware);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> install_node_script(ruvia::context& c) {
        const auto& installer = service::node_release::current(c)->installer();
        c.header("cache-control", "no-store");
        c.header("content-disposition", "inline; filename=install-node.sh");
        c.header("x-content-type-options", "nosniff");
        co_return c.file(
            {.path_ = installer.path(), .content_type_ = "text/x-shellscript; charset=utf-8"});
    }

    ruvia::task<ruvia::http_response> download_node(ruvia::context& c) {
        const auto& snapshot = service::node_release::current(c);
        const auto& release = snapshot->binary();
        c.header("cache-control", "no-cache");
        c.header("content-disposition", "attachment; filename=flexedge-node");
        c.header("x-flexedge-node-version", snapshot->version());
        c.header("x-flexedge-node-sha256", release.digest());
        c.header("etag", release.entity_tag());
        if (c.req().header("If-None-Match").value_or("") == release.entity_tag()) {
            c.status(ruvia::http_status::not_modified);
            co_return c.body(nullptr);
        }
        co_return c.file({.path_ = release.path(), .content_type_ = "application/octet-stream"});
    }

    ruvia::task<void> connect(ruvia::context& c) {
        co_await service::node_channel::session::serve(c);
    }
};

}  // namespace service::agent
