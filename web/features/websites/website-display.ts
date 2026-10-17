import {
  enablementStatuses,
  statusReserve,
  type StatusDictionary,
} from '@/lib/status'
import type { Website, WebsiteOrigin } from './types'

/** Labels of the website `status` (`enablementStatuses`). */
export const websiteStatusReserve = statusReserve(enablementStatuses)

/** `runtime.deploy_status`, derived from the node counts
 * (service/domains/website/website_runtime_mapper.h). */
export const deployStatuses = {
  applied: { label: '已生效', tone: 'success' },
  pending: { label: '等待中', tone: 'warning' },
  partial: { label: '部分完成', tone: 'warning' },
  no_nodes: { label: '无节点', tone: 'default' },
} as const satisfies StatusDictionary<
  'applied' | 'pending' | 'partial' | 'no_nodes'
>
export const deployStatusReserve = statusReserve(deployStatuses)

/** `runtime.origin_states[].status`, one node's health check of one origin
 * (node/data/origin_health.h, accepted by service/features/node_channel). */
export const originHealthStatuses = {
  healthy: { label: '健康', tone: 'success' },
  unhealthy: { label: '不健康', tone: 'danger' },
  unknown: { label: '未知', tone: 'default' },
} as const satisfies StatusDictionary<'healthy' | 'unhealthy' | 'unknown'>
export const originHealthReserve = statusReserve(originHealthStatuses)

/** `runtime.domain_states[].resolution_status`: the DNS probe's CNAME check
 * (service/features/website_dns/runtime.h); a domain without a probe result
 * is `unverified` (website_runtime_mapper.h). */
export const domainResolutionStatuses = {
  verified: { label: '已验证', tone: 'success' },
  unverified: { label: '未验证', tone: 'warning' },
  invalid: { label: '无效', tone: 'danger' },
} as const satisfies StatusDictionary<'verified' | 'unverified' | 'invalid'>
export const domainResolutionReserve = statusReserve(domainResolutionStatuses)

/** Display name used in titles, menus and confirmations. */
export function websiteName(
  website: Pick<Website, 'config' | 'access_domain'>
) {
  return (
    website.config.name ||
    website.config.domains[0]?.hostname ||
    website.access_domain ||
    '未命名网站'
  )
}

/** Primary origin of the default origin group, falling back to its first member. */
export function defaultOrigin(website: Pick<Website, 'config'>) {
  const group = website.config.origins.filter(
    (origin) => origin.group === website.config.default_origin_group
  )
  return group.find((origin) => origin.role === 'primary') ?? group[0]
}

export function originAddress(
  origin: Pick<WebsiteOrigin, 'protocol' | 'host' | 'port'>
) {
  return `${origin.protocol}://${origin.host}:${origin.port}`
}

export function originGroupName(group: string) {
  return group === 'default' ? '默认' : group
}

/** Origin groups in configuration order. */
export function originGroups(website: Pick<Website, 'config'>) {
  return [...new Set(website.config.origins.map((origin) => origin.group))]
}

/** Health reports by status: `healthy` counts as up, anything else as down. */
export function originHealth(website: Pick<Website, 'runtime'>) {
  const states = website.runtime.origin_states
  return {
    total: states.length,
    healthy: states.filter((state) => state.status === 'healthy').length,
  }
}

/** Stable presentation order across initial snapshots and per-node SSE reports. */
export function sortedOriginStates(
  website: Pick<Website, 'runtime' | 'config'>
) {
  const compare = new Intl.Collator('zh-CN', { numeric: true }).compare
  const address = new Map(
    website.config.origins.map((origin) => [origin.id, originAddress(origin)])
  )
  return [...website.runtime.origin_states].sort(
    (left, right) =>
      compare(left.node_name, right.node_name) ||
      compare(
        address.get(left.origin_id) ?? left.origin_id,
        address.get(right.origin_id) ?? right.origin_id
      ) ||
      compare(left.node_id, right.node_id) ||
      compare(left.origin_id, right.origin_id)
  )
}
