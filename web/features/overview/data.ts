import { z } from 'zod'
import { queryOptions } from '@tanstack/react-query'
import { streamPath, type PageData } from '@/lib/api'
import { formatDate, ratioOf } from '@/lib/format'
import { readLiveEvent } from '@/lib/live-query'
import { applyNodeRuntimePagePatch } from '@/features/nodes/data'
import type { Node } from '@/features/nodes/types'

const count = z.number().int().nonnegative()
const amount = z.number().nonnegative()

// service/domains/overview/overview_types.h
export const overviewSchema = z.object({
  resources: z.object({
    website_count: count,
    domain_count: count,
    certificate_count: count,
    cluster_count: count,
    node_count: count,
    online_node_count: count,
  }),
  issues: z.object({
    dns_zone_issue_count: count,
    certificate_expiring_count: count,
    certificate_failed_count: count,
  }),
})
export type Overview = z.infer<typeof overviewSchema>

// redesign-spec §8.3: one combined SSE connection, `/api/overview/stream`,
// pushes `snapshot` (counts and issues), `traffic`, `nodes` and
// `node-runtime` patches. Each event feeds its own query; all three share the
// page's single EventSource, released when the last of them unmounts. Tasks
// are a separate one-shot read (`recentOperationsQuery`).
const overviewStream = streamPath('/overview')

export const overviewQuery = queryOptions({
  queryKey: ['overview', 'summary'],
  retry: false,
  // Rendered inline by QueryNotice / DataTable on the overview page.
  meta: { inlineError: true },
  queryFn: ({ queryKey, signal }) =>
    readLiveEvent(overviewStream, 'snapshot', queryKey, signal, (value) =>
      overviewSchema.parse(value)
    ),
})

// `traffic` event. Every figure is aggregated from access logs of websites
// with logging enabled.
export const overviewTrafficSchema = z.object({
  window_hours: count,
  summary: z.object({
    request_count: count,
    response_bytes: amount,
    peak_bandwidth_bps: amount,
    status_5xx_count: count,
    unique_client_ips: count,
  }),
  hourly: z.array(
    z.object({
      timestamp: z.string(),
      request_count: count,
      response_bytes: amount,
      bandwidth_bps: amount,
    })
  ),
  status_classes: z.array(
    z.object({ label: z.string(), request_count: count })
  ),
  top_websites: z.array(
    z.object({
      website_id: z.string(),
      name: z.string(),
      request_count: count,
      response_bytes: amount,
    })
  ),
  coverage: z.object({
    logging_website_count: count,
    website_count: count,
  }),
})
export type OverviewTraffic = z.infer<typeof overviewTrafficSchema>

export const overviewTrafficQuery = queryOptions({
  queryKey: ['overview', 'traffic'],
  retry: false,
  meta: { inlineError: true },
  queryFn: ({ queryKey, signal }) =>
    readLiveEvent(overviewStream, 'traffic', queryKey, signal, (value) =>
      overviewTrafficSchema.parse(value)
    ),
})

/** Page size of the `nodes` event (same page shape as `/api/nodes/stream`). */
export const overviewNodePageSize = 100

/** `nodes` event: first 100 nodes, kept current by `node-runtime` patches. */
export const overviewNodesQuery = queryOptions({
  queryKey: ['overview', 'nodes'],
  retry: false,
  meta: { inlineError: true },
  queryFn: ({ queryKey, signal }) =>
    readLiveEvent<PageData<Node>>(
      overviewStream,
      'nodes',
      queryKey,
      signal,
      undefined,
      applyNodeRuntimePagePatch
    ),
})

export type TrafficPoint = {
  timestamp: string
  label: string
  bandwidth_bps: number
  request_count: number
}

/** Chart points labelled with the console hour (Asia/Shanghai, `HH:00`). */
export function trafficSeries(hourly: OverviewTraffic['hourly']) {
  return hourly.map((point): TrafficPoint => ({
    timestamp: point.timestamp,
    label: formatDate(point.timestamp).slice(11, 16),
    bandwidth_bps: point.bandwidth_bps,
    request_count: point.request_count,
  }))
}

const statusClassColors: Record<string, string> = {
  '1xx': 'var(--color-muted)',
  '2xx': 'var(--color-success)',
  '3xx': 'var(--color-accent)',
  '4xx': 'var(--color-warning)',
  '5xx': 'var(--color-danger)',
}

export type StatusClassItem = {
  label: string
  request_count: number
  /** Share of all logged requests, 0–1. */
  share: number
  /** Semantic chart color (CSS variable). */
  color: string
}

/** Status classes with their share and chart color (semantic tokens). */
export function statusDistribution(classes: OverviewTraffic['status_classes']) {
  const total = classes.reduce((sum, item) => sum + item.request_count, 0)
  const items = classes.map((item): StatusClassItem => ({
    label: item.label,
    request_count: item.request_count,
    share: ratioOf(item.request_count, total),
    color: statusClassColors[item.label] ?? 'var(--color-muted)',
  }))
  return { total, items }
}

export type TopWebsiteRow = OverviewTraffic['top_websites'][number] & {
  /** Share of all logged response bytes, 0–1. */
  share: number
}

/** Top websites with their share of all logged response bytes. */
export function topWebsiteRows(traffic: OverviewTraffic) {
  return traffic.top_websites.map((site): TopWebsiteRow => ({
    ...site,
    share: ratioOf(site.response_bytes, traffic.summary.response_bytes),
  }))
}

type CoverageState = 'no_websites' | 'no_logging' | 'no_traffic' | 'ok'

/** Why traffic statistics may be empty: no websites at all, no website with
 * access logging, or simply no logged requests in the window. */
export function coverageState(traffic: OverviewTraffic): CoverageState {
  if (traffic.coverage.website_count === 0) return 'no_websites'
  if (traffic.coverage.logging_website_count === 0) return 'no_logging'
  if (traffic.summary.request_count === 0) return 'no_traffic'
  return 'ok'
}

const connectionRank: Record<string, number> = {
  offline: 0,
  unregistered: 1,
  online: 2,
}

/** Health of the loaded node page (first `overviewNodeParams.page_size`
 * nodes); tenant-wide node counts come from the overview stream. */
export type NodeHealth = {
  /** Sum of `traffic_out_bps` over online nodes, bit/s. */
  egressBps: number
  ordered: Node[]
}

/** Node health summary: current egress is the sum over online nodes; the list
 * puts offline nodes first, then unregistered, then online by CPU load. */
export function nodeHealth(nodes: Node[]): NodeHealth {
  const online = nodes.filter(
    (node) => node.runtime.connection_status === 'online'
  )
  return {
    egressBps: online.reduce(
      (sum, node) => sum + (node.runtime.traffic_out_bps ?? 0),
      0
    ),
    ordered: [...nodes].sort(
      (left, right) =>
        (connectionRank[left.runtime.connection_status] ?? 1) -
          (connectionRank[right.runtime.connection_status] ?? 1) ||
        (right.runtime.cpu_usage ?? 0) - (left.runtime.cpu_usage ?? 0) ||
        left.name.localeCompare(right.name)
    ),
  }
}

type AttentionItem = {
  id: string
  label: string
  description: string
  count: number
  href: string
  tone: 'danger' | 'warning'
}

/** "需要关注" rows, each linking to the filtered list that shows them. Rows
 * with nothing to report are dropped. `attentionTasks` is the task list's
 * `attention` count (retrying, failed, partially failed or needing action). */
export function attentionItems(
  overview: Overview,
  attentionTasks: number
): AttentionItem[] {
  const items: AttentionItem[] = [
    {
      id: 'dns',
      label: 'DNS 同步异常',
      description: '同步失败或存在冲突的域名',
      count: overview.issues.dns_zone_issue_count,
      href: '/dns-zones?sync_status=issue',
      tone: 'danger',
    },
    {
      id: 'certificate-failed',
      label: '证书异常',
      description: '签发失败、已过期或分发出错',
      count: overview.issues.certificate_failed_count,
      href: '/certificates?abnormal=1',
      tone: 'danger',
    },
    {
      id: 'certificate-expiring',
      label: '证书即将到期',
      description: '30 天内到期的证书',
      count: overview.issues.certificate_expiring_count,
      href: '/certificates?expiring=1',
      tone: 'warning',
    },
    {
      id: 'task-attention',
      label: '需关注任务',
      description: '重试中、失败或需处理的任务',
      count: attentionTasks,
      href: '/tasks?status=attention',
      tone: 'warning',
    },
    {
      id: 'node-not-online',
      label: '未在线节点（离线或未接入）',
      description: '没有在线心跳的节点',
      count:
        overview.resources.node_count - overview.resources.online_node_count,
      href: '/nodes?connection_status=offline',
      tone: 'danger',
    },
  ]
  return items.filter((item) => item.count > 0)
}
