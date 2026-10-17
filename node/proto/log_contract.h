#pragma once

#include <cstddef>
#include <cstdint>

namespace flexedge::node::log_contract {

inline constexpr std::size_t max_delivery_events{512};
inline constexpr std::size_t max_delivery_bytes{1024 * 1024};
inline constexpr std::size_t max_website_id_bytes{36};
inline constexpr std::size_t max_client_ip_bytes{64};
inline constexpr std::size_t max_protocol_bytes{16};
inline constexpr std::size_t max_method_bytes{32};
inline constexpr std::size_t max_host_bytes{253};
inline constexpr std::size_t max_target_bytes{2048};
inline constexpr std::size_t max_user_agent_bytes{512};
inline constexpr std::size_t max_referer_bytes{512};
inline constexpr std::size_t max_request_headers_bytes{32 * 1024};
inline constexpr std::size_t max_request_body_bytes{512 * 1024};
inline constexpr std::size_t max_tls_fingerprint_bytes{128};
inline constexpr std::size_t max_response_headers_bytes{32 * 1024};
inline constexpr std::size_t max_query_string_bytes{2048};
inline constexpr std::size_t max_cookies_bytes{16 * 1024};
inline constexpr std::size_t max_node_level_bytes{16};
inline constexpr std::size_t max_node_category_bytes{64};
inline constexpr std::size_t max_node_message_bytes{2000};
inline constexpr std::uint64_t max_request_bytes{2ull * 1024ull * 1024ull};
inline constexpr std::uint64_t max_response_bytes{1024ull * 1024ull * 1024ull * 1024ull};
inline constexpr std::uint64_t max_duration_ms{24ull * 60ull * 60ull * 1000ull};
inline constexpr std::uint64_t max_connection_duration_ms{365ull * 24ull * 60ull * 60ull * 1000ull};
inline constexpr std::uint64_t max_connection_bytes{1024ull * 1024ull * 1024ull * 1024ull};

}  // namespace flexedge::node::log_contract
