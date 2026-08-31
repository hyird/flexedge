#pragma once

#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include <ruvia/core/EventLoopPool.h>

#include "node/data/http_listener.h"
#include "node/data/health_supervisor.h"
#include "node/data/https_listener.h"
#include "node/runtime/compiled_config.h"
#include "node/runtime/log_buffer.h"

namespace flexedge::node {

class PreparedDataPlaneReload final {
  public:
    PreparedDataPlaneReload(PreparedDataPlaneReload&&) noexcept = default;
    PreparedDataPlaneReload& operator=(PreparedDataPlaneReload&&) noexcept = default;
    PreparedDataPlaneReload(const PreparedDataPlaneReload&) = delete;
    PreparedDataPlaneReload& operator=(const PreparedDataPlaneReload&) = delete;

  private:
    friend class DataPlane;

    PreparedDataPlaneReload() = default;

    std::unordered_map<std::string, std::shared_ptr<HttpListener>> listeners;
    std::vector<std::pair<std::string, std::shared_ptr<HttpListener>>> additions;
    std::unordered_map<std::string, std::shared_ptr<HttpsListener>> httpsListeners;
    std::vector<std::pair<std::string, std::shared_ptr<HttpsListener>>> httpsAdditions;
    std::vector<std::shared_ptr<HttpListener>> retired;
    std::vector<std::shared_ptr<HttpsListener>> httpsRetired;
    OriginHealthRegistry::KeySet originKeys;
    std::shared_ptr<const TlsContextSet> tlsContexts;
    std::shared_ptr<ListenerActivationGate> activationGate;
    std::shared_ptr<const CompiledConfig> config;
    bool primed{};
};

class DataPlane final {
  public:
    DataPlane(ruvia::EventLoopPool& loops, ruvia::EventLoop owner, RuntimeState& runtime,
              NodeLogBuffer& logs)
        : loops_(loops), owner_(std::move(owner)), runtime_(runtime), logs_(logs),
          healthSupervisor_(owner_, runtime_, health_, healthProbeTls_),
          stopRegistration_(owner_.onStop([this] { stopOnOwner(); })) {
        workers_.reserve(loops_.loopCount());
        for (std::size_t index = 0; index < loops_.loopCount(); ++index) {
            workers_.push_back(std::make_unique<WorkerState>(loops_.loop(index).executor(),
                                                             logs_.workerProducer(index)));
        }
    }

    ~DataPlane() { stopOnOwner(); }

    void start() { healthSupervisor_.requestStart(); }

    [[nodiscard]] bool healthy() const { return health_.allHealthy(); }

    [[nodiscard]] std::vector<OriginHealthRegistry::Snapshot> originHealthReports() const {
        return health_.reports();
    }

    [[nodiscard]] RuntimeMetricsSnapshot metrics() {
        RuntimeMetricsSnapshot result;
        for (const auto& worker : workers_) {
            const auto sample = worker->metrics.sample();
            result.cpuUsage += sample.cpuUsage;
            result.memoryUsage += sample.memoryUsage;
            result.trafficOutBps += sample.trafficOutBps;
            result.connectionCount += sample.connectionCount;
            result.load1m += sample.load1m;
        }
        if (!workers_.empty()) {
            result.cpuUsage /= static_cast<double>(workers_.size());
            result.memoryUsage /= static_cast<double>(workers_.size());
            result.load1m /= static_cast<double>(workers_.size());
        }
        return result;
    }

    [[nodiscard]] bool drained() const noexcept {
        return std::ranges::all_of(
            workers_, [](const auto& worker) { return worker->metrics.connectionCount() == 0; });
    }

    void requestStopAccepting() noexcept {
        if (owner_.isCurrent()) {
            stopOnOwner();
            return;
        }
        if (!owner_.post([this] { stopOnOwner(); }).accepted()) {
            stopOnOwner();
        }
    }

    [[nodiscard]] PreparedDataPlaneReload prepare(std::shared_ptr<const CompiledConfig> config) {
        if (!owner_.isCurrent()) {
            throw std::logic_error("data plane reload must run on its owner worker");
        }
        if (!config) {
            throw std::invalid_argument("data plane config cannot be empty");
        }
        PreparedDataPlaneReload reload;
        reload.config = std::move(config);
        reload.tlsContexts = std::make_shared<const TlsContextSet>(*reload.config);
        reload.activationGate = std::make_shared<ListenerActivationGate>();
        if (!reload.config->enabled()) {
            return finishPrepare(std::move(reload));
        }
        for (const auto& endpoint : reload.config->endpoints()) {
            for (std::size_t workerIndex = 0; workerIndex < loops_.loopCount(); ++workerIndex) {
                auto key = listenerKey(endpoint, workerIndex);
                const auto prepared = reload.listeners.find(key);
                const auto existing = listeners_.find(key);
                if (prepared != reload.listeners.end()) {
                    // Multiple advertised addresses of the same family share the wildcard
                    // listener for this port.
                } else if (existing != listeners_.end()) {
                    reload.listeners.emplace(std::move(key), existing->second);
                } else {
                    reload.additions.emplace_back(
                        key, std::make_shared<HttpListener>(
                                 loops_.loop(workerIndex), runtime_, health_,
                                 workers_[workerIndex]->metrics, workers_[workerIndex]->logs,
                                 workers_[workerIndex]->originConnections, requestBuffers_,
                                 responseBuffers_,
                                 asio::ip::tcp::endpoint(
                                     bindAddress(endpoint.ip_address()),
                                     static_cast<std::uint16_t>(endpoint.http_port()))));
                    reload.listeners.emplace(key, reload.additions.back().second);
                }
                if (reload.tlsContexts->empty()) {
                    continue;
                }
                auto httpsKey =
                    listenerKey(endpoint.ip_address(), endpoint.https_port(), workerIndex);
                const auto preparedHttps = reload.httpsListeners.find(httpsKey);
                const auto existingHttps = httpsListeners_.find(httpsKey);
                if (preparedHttps != reload.httpsListeners.end()) {
                    // See the HTTP listener case above.
                } else if (existingHttps != httpsListeners_.end()) {
                    reload.httpsListeners.emplace(std::move(httpsKey), existingHttps->second);
                } else {
                    reload.httpsAdditions.emplace_back(
                        httpsKey, std::make_shared<HttpsListener>(
                                      loops_.loop(workerIndex), runtime_, health_, tlsContexts_,
                                      workers_[workerIndex]->metrics, workers_[workerIndex]->logs,
                                      workers_[workerIndex]->originConnections, requestBuffers_,
                                      responseBuffers_,
                                      asio::ip::tcp::endpoint(
                                          bindAddress(endpoint.ip_address()),
                                          static_cast<std::uint16_t>(endpoint.https_port()))));
                    reload.httpsListeners.emplace(httpsKey, reload.httpsAdditions.back().second);
                }
            }
        }
        return finishPrepare(std::move(reload));
    }

    void prime(PreparedDataPlaneReload& reload) {
        if (!owner_.isCurrent()) {
            throw std::logic_error("data plane reload must run on its owner worker");
        }
        if (reload.primed) {
            throw std::logic_error("data plane reload is already primed");
        }
        try {
            for (const auto& [_, listener] : reload.additions) {
                listener->requestStart(reload.activationGate);
            }
            for (const auto& [_, listener] : reload.httpsAdditions) {
                listener->requestStart(reload.activationGate);
            }
            health_.retain(reload.originKeys);
            reload.primed = true;
        } catch (...) {
            abort(reload);
            throw;
        }
    }

    void activate(PreparedDataPlaneReload reload) noexcept {
        tlsContexts_.publish(std::move(reload.tlsContexts));
        runtime_.publish(std::move(reload.config));
        listeners_.swap(reload.listeners);
        httpsListeners_.swap(reload.httpsListeners);
        reload.activationGate->activate();
        for (const auto& listener : reload.retired) {
            listener->requestStop();
        }
        for (const auto& listener : reload.httpsRetired) {
            listener->requestStop();
        }
    }

    void abort(PreparedDataPlaneReload& reload) noexcept {
        for (const auto& [_, listener] : reload.additions) {
            listener->requestStop();
        }
        for (const auto& [_, listener] : reload.httpsAdditions) {
            listener->requestStop();
        }
        reload.primed = false;
    }

  private:
    struct WorkerState final {
        WorkerState(asio::any_io_executor executor, NodeLogBuffer::Producer producer)
            : originConnections(std::move(executor), originTls), logs(producer) {}

        RuntimeMetrics metrics;
        OriginTlsContext originTls;
        OriginConnectionPool originConnections;
        NodeLogBuffer::Producer logs;
    };

    [[nodiscard]] PreparedDataPlaneReload finishPrepare(PreparedDataPlaneReload reload) const {
        for (const auto& [key, listener] : listeners_) {
            if (!reload.listeners.contains(key)) {
                reload.retired.push_back(listener);
            }
        }
        for (const auto& [key, listener] : httpsListeners_) {
            if (!reload.httpsListeners.contains(key)) {
                reload.httpsRetired.push_back(listener);
            }
        }
        for (const auto* website : reload.config->websites()) {
            for (const auto& origin : website->origins()) {
                reload.originKeys.emplace(OriginHealthRegistry::key(website->id(), origin.id()));
            }
        }
        return reload;
    }

    static std::string listenerKey(const v2::Endpoint& endpoint, std::size_t workerIndex) {
        return listenerKey(endpoint.ip_address(), endpoint.http_port(), workerIndex);
    }

    static std::string listenerKey(std::string_view address, std::uint32_t port,
                                   std::size_t workerIndex) {
        const auto parsed = asio::ip::make_address(address);
        return std::string(parsed.is_v6() ? "v6:" : "v4:") + std::to_string(port) + '#' +
               std::to_string(workerIndex);
    }

    static asio::ip::address bindAddress(std::string_view advertisedAddress) {
        return asio::ip::make_address(advertisedAddress).is_v6()
                   ? asio::ip::address{asio::ip::address_v6::any()}
                   : asio::ip::address{asio::ip::address_v4::any()};
    }

    void stopOnOwner() noexcept {
        for (const auto& [_, listener] : listeners_) {
            listener->requestStop();
        }
        listeners_.clear();
        for (const auto& [_, listener] : httpsListeners_) {
            listener->requestStop();
        }
        httpsListeners_.clear();
    }

    ruvia::EventLoopPool& loops_;
    ruvia::EventLoop owner_;
    RuntimeState& runtime_;
    NodeLogBuffer& logs_;
    OriginHealthRegistry health_;
    OriginTlsContext healthProbeTls_;
    TlsContextRegistry tlsContexts_;
    OriginHealthSupervisor healthSupervisor_;
    BufferedBytesBudget requestBuffers_{kMaximumRequestBufferBytes};
    BufferedBytesBudget responseBuffers_{kMaximumResponseBufferBytes};
    ruvia::EventLoopStopRegistration stopRegistration_;
    std::vector<std::unique_ptr<WorkerState>> workers_;
    std::unordered_map<std::string, std::shared_ptr<HttpListener>> listeners_;
    std::unordered_map<std::string, std::shared_ptr<HttpsListener>> httpsListeners_;
};

} // namespace flexedge::node
