#pragma once

#include <cstddef>
#include <cstdint>

namespace flexedge::node::log_contract {

inline constexpr std::size_t kMaxDeliveryEvents{512};
inline constexpr std::size_t kMaxDeliveryBytes{1024 * 1024};
inline constexpr std::size_t kMaxWebsiteIdBytes{36};
inline constexpr std::size_t kMaxClientIpBytes{64};
inline constexpr std::size_t kMaxProtocolBytes{16};
inline constexpr std::size_t kMaxMethodBytes{32};
inline constexpr std::size_t kMaxHostBytes{253};
inline constexpr std::size_t kMaxTargetBytes{2048};
inline constexpr std::size_t kMaxUserAgentBytes{512};
inline constexpr std::size_t kMaxRefererBytes{512};
inline constexpr std::size_t kMaxRequestHeadersBytes{32 * 1024};
inline constexpr std::size_t kMaxRequestBodyBytes{512 * 1024};
inline constexpr std::size_t kMaxTlsFingerprintBytes{128};
inline constexpr std::size_t kMaxResponseHeadersBytes{32 * 1024};
inline constexpr std::size_t kMaxQueryStringBytes{2048};
inline constexpr std::size_t kMaxCookiesBytes{16 * 1024};
inline constexpr std::size_t kMaxNodeLevelBytes{16};
inline constexpr std::size_t kMaxNodeCategoryBytes{64};
inline constexpr std::size_t kMaxNodeMessageBytes{2000};
inline constexpr std::uint64_t kMaxResponseBytes{1024ull * 1024ull * 1024ull * 1024ull};
inline constexpr std::uint64_t kMaxDurationMs{24ull * 60ull * 60ull * 1000ull};

} // namespace flexedge::node::log_contract
