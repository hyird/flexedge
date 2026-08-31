#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/MoveOnlyFunction.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/web/HttpClient.h>
#include <ruvia/web/db/DbClient.h>

#include "service/config/outbound.h"

namespace service::background {

class WorkerPool;

class WorkerContext final {
  public:
    ~WorkerContext();

    WorkerContext(const WorkerContext&) = delete;
    WorkerContext& operator=(const WorkerContext&) = delete;
    WorkerContext(WorkerContext&&) = delete;
    WorkerContext& operator=(WorkerContext&&) = delete;

    [[nodiscard]] ruvia::DbClient& db() const noexcept;
    [[nodiscard]] ruvia::HttpClient& httpClient(std::string_view alias) const;
    [[nodiscard]] const ruvia::WorkerHandle& worker() const noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] ruvia::StopToken stopToken() const noexcept;
    [[nodiscard]] std::string_view leaseOwner() const noexcept;

  private:
    struct Impl;

    WorkerContext(ruvia::EventLoop loop, ruvia::DbConfig database,
                  const service::config::OutboundOrigins& origins, std::string workerName,
                  std::span<const std::string_view> outboundAliases);

    void close() noexcept;

    std::unique_ptr<Impl> impl_;
    friend class WorkerPool;
};

struct WorkerDefinition final {
    std::string name;
    std::vector<std::string_view> outboundAliases;
    ruvia::MoveOnlyFunction<ruvia::Task<void>(WorkerContext&)> run;
};

class WorkerPool final {
  public:
    WorkerPool(const ruvia::DbConfig& database, const service::config::OutboundOrigins& origins,
               std::vector<WorkerDefinition> workers);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] std::size_t workerCount() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace service::background
