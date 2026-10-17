#pragma once
#include <ruvia/web/context.h>

#include "service/domains/security/security_types.h"

namespace service::security {
class security_read_service final {
public:
    static ruvia::task<ruvia::array<website_dto>> websites(auto& c, const std::string& tenant) {
        const auto rows = co_await c.db().query("SELECT id,COALESCE(NULLIF(config->>'name',''),config#>>'{domains,0,hostname}',id::text),COALESCE(security_policy_id::text,'') FROM sys_website WHERE tenant_id=$1 AND deleted_at IS NULL ORDER BY sort DESC", tenant);
        ruvia::array<website_dto> result({.resource_ = c.pool()});
        for (const auto& row : rows) {
            auto& item = result.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"id">(row[0].value().value());
            item.template set<"name">(row[1].value().value());
            item.template set<"policy_id">(row[2].value().value());
        }
        co_return result;
    }
    static ruvia::task<telemetry_dto> telemetry(auto& c, const std::string& tenant) {
        telemetry_dto result({.resource_ = c.pool()});
        const auto requests = co_await c.db().query("SELECT count(*) FROM sys_website_access_log WHERE tenant_id=$1 AND occurred_at >= date_trunc('hour',NOW())-interval '23 hours'", tenant);
        result.template set<"requests">(requests.front()[0].template as<std::int64_t>().value());
        // JSON parsing is guarded: ordinary node logs are never interpreted as security events.
        const auto rows = co_await c.db().query(R"SQL(
WITH events AS (
 SELECT log.id,log.occurred_at,log.tenant_id,log.node_id,
 CASE WHEN log.message IS JSON OBJECT THEN log.message::jsonb ELSE '{}'::jsonb END AS data
 FROM sys_node_log log WHERE log.tenant_id=$1 AND log.category='security'
 AND log.occurred_at>=date_trunc('hour',NOW())-interval '23 hours'
)
SELECT events.id,to_char(events.occurred_at AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),events.data::text,COALESCE(NULLIF(site.config->>'name',''),site.config#>>'{domains,0,hostname}',site.id::text)
FROM events JOIN sys_website site ON site.tenant_id=events.tenant_id AND site.id::text=events.data->>'website_id' AND site.deleted_at IS NULL
JOIN sys_node node ON node.tenant_id=events.tenant_id AND node.id=events.node_id AND node.cluster_id=site.cluster_id
WHERE events.data->>'action' IN ('block','challenge','log','skip') AND events.data->>'stage' IN ('custom','rate','managed')
ORDER BY events.occurred_at DESC,events.id DESC LIMIT 5000
)SQL",
            tenant);
        auto& events = result.template ensure<"events">();
        auto& distribution = result.template ensure<"distribution">();
        distribution.template set<"block">(0);
        distribution.template set<"challenge">(0);
        distribution.template set<"log">(0);
        distribution.template set<"skip">(0);
        // The bounded event sample backs the event table and sampled breakdowns in the UI.
        for (const auto& row : rows) {
            auto parsed = ruvia::from_json<event_dto>(row[2].value().value(), {.resource_ = c.pool()});
            if (!parsed) {
                continue;
            }
            parsed->template set<"id">(row[0].value().value());
            parsed->template set<"time">(row[1].value().value());
            parsed->template set<"website">(row[3].value().value());
            events.push_back(std::move(*parsed));
        }
        const auto hours = co_await c.db().query(R"SQL(
WITH events AS (
 SELECT log.occurred_at,log.tenant_id,log.node_id,
 CASE WHEN log.message IS JSON OBJECT THEN log.message::jsonb ELSE '{}'::jsonb END AS data
 FROM sys_node_log log WHERE log.tenant_id=$1 AND log.category='security'
 AND log.occurred_at>=date_trunc('hour',NOW())-interval '23 hours'
), totals AS (
 SELECT date_trunc('hour',events.occurred_at) AS hour,
 count(*) FILTER(WHERE data->>'action'='block') AS blocked,
 count(*) FILTER(WHERE data->>'action'='challenge') AS challenged,
 count(*) FILTER(WHERE data->>'action'='log') AS logged,
 count(*) FILTER(WHERE data->>'action'='skip') AS skipped
 FROM events JOIN sys_website site ON site.tenant_id=events.tenant_id AND site.id::text=data->>'website_id' AND site.deleted_at IS NULL
 JOIN sys_node node ON node.tenant_id=events.tenant_id AND node.id=events.node_id AND node.cluster_id=site.cluster_id
 WHERE data->>'stage' IN ('custom','rate','managed')
 GROUP BY date_trunc('hour',events.occurred_at)
)
SELECT to_char(hours.hour AT TIME ZONE 'UTC','HH24:00'),COALESCE(blocked,0),COALESCE(challenged,0),COALESCE(logged,0),COALESCE(skipped,0)
FROM generate_series(date_trunc('hour',NOW())-interval '23 hours',date_trunc('hour',NOW()),interval '1 hour') AS hours(hour)
LEFT JOIN totals USING(hour) ORDER BY hours.hour
)SQL",
            tenant);
        auto& points = result.template ensure<"hourly">();
        for (const auto& row : hours) {
            auto& item = points.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            const auto label = row[0].value().value();
            item.template set<"label">(label);
            const auto blocks = row[1].template as<std::int64_t>().value(), challenges = row[2].template as<std::int64_t>().value();
            distribution.template set<"block">(distribution.template get<"block">().value_ + blocks);
            distribution.template set<"challenge">(distribution.template get<"challenge">().value_ + challenges);
            distribution.template set<"log">(distribution.template get<"log">().value_ + row[3].template as<std::int64_t>().value());
            distribution.template set<"skip">(distribution.template get<"skip">().value_ + row[4].template as<std::int64_t>().value());
            item.template set<"block">(blocks);
            item.template set<"challenge">(challenges);
        }
        co_return result;
    }
};
}  // namespace service::security
