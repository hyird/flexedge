import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, describe, expect, test } from 'vitest'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import type { Node } from '@/features/nodes/types'
import {
  attentionItems,
  coverageState,
  overviewNodesQuery,
  overviewQuery,
  overviewSchema,
  nodeHealth,
  overviewTrafficQuery,
  overviewTrafficSchema,
  statusDistribution,
  topWebsiteRows,
  trafficSeries,
  type Overview,
  type OverviewTraffic,
} from './data'

const traffic: OverviewTraffic = {
  window_hours: 24,
  summary: {
    request_count: 1000,
    response_bytes: 4096,
    peak_bandwidth_bps: 8000,
    status_5xx_count: 5,
    unique_client_ips: 12,
  },
  hourly: [
    {
      timestamp: '2026-10-14T01:00:00+00:00',
      request_count: 600,
      response_bytes: 3072,
      bandwidth_bps: 7,
    },
    {
      timestamp: '2026-10-14T02:00:00+00:00',
      request_count: 400,
      response_bytes: 1024,
      bandwidth_bps: 2,
    },
  ],
  status_classes: [
    { label: '1xx', request_count: 0 },
    { label: '2xx', request_count: 900 },
    { label: '3xx', request_count: 50 },
    { label: '4xx', request_count: 45 },
    { label: '5xx', request_count: 5 },
  ],
  top_websites: [
    { website_id: 'w1', name: 'a', request_count: 700, response_bytes: 3072 },
    { website_id: 'w2', name: 'b', request_count: 300, response_bytes: 1024 },
  ],
  coverage: { logging_website_count: 2, website_count: 3 },
}

function node(
  id: string,
  connection_status: string,
  runtime: Partial<Node['runtime']> = {}
): Node {
  return {
    id,
    revision: 1,
    created_at: '',
    updated_at: '',
    cluster_id: 'c1',
    cluster_name: 'cluster',
    name: id,
    status: 'enabled',
    node_spec_revision: 1,
    config: { endpoints: [] },
    runtime: {
      registration_status:
        connection_status === 'unregistered' ? 'pending' : 'registered',
      connection_status,
      applied_node_spec_revision: 1,
      ...runtime,
    },
  } as Node
}

describe('overview traffic', () => {
  test('schema accepts the §8 contract and rejects negative counts', () => {
    expect(overviewTrafficSchema.parse(traffic)).toEqual(traffic)
    expect(() =>
      overviewTrafficSchema.parse({
        ...traffic,
        summary: { ...traffic.summary, request_count: -1 },
      })
    ).toThrow()
  })

  test('labels hourly points in console time', () => {
    expect(trafficSeries(traffic.hourly).map((point) => point.label)).toEqual([
      '09:00',
      '10:00',
    ])
  })

  test('status distribution shares and colors', () => {
    const { total, items } = statusDistribution(traffic.status_classes)
    expect(total).toBe(1000)
    expect(items[1]).toMatchObject({ label: '2xx', share: 0.9 })
    expect(items[4]?.color).toBe('var(--color-danger)')
    expect(statusDistribution([]).total).toBe(0)
  })

  test('top websites share of logged bytes', () => {
    expect(topWebsiteRows(traffic).map((site) => site.share)).toEqual([
      0.75, 0.25,
    ])
    expect(
      topWebsiteRows({
        ...traffic,
        summary: { ...traffic.summary, response_bytes: 0 },
      })[0]?.share
    ).toBe(0)
  })

  test('coverage state explains empty statistics', () => {
    expect(coverageState(traffic)).toBe('ok')
    expect(
      coverageState({
        ...traffic,
        coverage: { logging_website_count: 0, website_count: 0 },
      })
    ).toBe('no_websites')
    expect(
      coverageState({
        ...traffic,
        coverage: { logging_website_count: 0, website_count: 4 },
      })
    ).toBe('no_logging')
    expect(
      coverageState({
        ...traffic,
        summary: { ...traffic.summary, request_count: 0 },
      })
    ).toBe('no_traffic')
  })
})

describe('node health', () => {
  test('sums online egress (bit/s) and orders offline first', () => {
    const health = nodeHealth([
      node('busy', 'online', { cpu_usage: 0.9, traffic_out_bps: 1000 }),
      node('idle', 'online', { cpu_usage: 0.1, traffic_out_bps: 500 }),
      node('new', 'unregistered'),
      node('down', 'offline', { traffic_out_bps: 9999 }),
    ])
    expect(health.egressBps).toBe(1500)
    expect(health.ordered.map((item) => item.id)).toEqual([
      'down',
      'new',
      'busy',
      'idle',
    ])
  })
})

describe('attention items', () => {
  const overview: Overview = {
    resources: {
      website_count: 1,
      domain_count: 1,
      certificate_count: 1,
      cluster_count: 1,
      node_count: 5,
      online_node_count: 3,
    },
    issues: {
      dns_zone_issue_count: 2,
      certificate_expiring_count: 0,
      certificate_failed_count: 1,
      active_marker_count: 3,
      retry_marker_count: 1,
    },
    recent_markers: [],
  }

  test('overview schema requires tenant-wide node counts', () => {
    expect(overviewSchema.parse(overview)).toEqual(overview)
    const { node_count: _omitted, ...resources } = overview.resources
    expect(() => overviewSchema.parse({ ...overview, resources })).toThrow()
  })

  test('links non-zero issues to filtered lists', () => {
    expect(
      attentionItems(overview).map((item) => [item.href, item.count])
    ).toEqual([
      ['/dns-zones?sync_status=issue', 2],
      ['/certificates?abnormal=1', 1],
      ['/tasks?status=retrying', 1],
      ['/nodes?connection_status=offline', 2],
    ])
  })

  test('is empty when nothing needs attention', () => {
    expect(
      attentionItems({
        ...overview,
        resources: { ...overview.resources, online_node_count: 5 },
        issues: {
          dns_zone_issue_count: 0,
          certificate_expiring_count: 0,
          certificate_failed_count: 0,
          active_marker_count: 2,
          retry_marker_count: 0,
        },
      })
    ).toEqual([])
  })
})

describe('combined overview stream', () => {
  class Source extends EventTarget {
    closed = false
    constructor(readonly url: string) {
      super()
    }
    close() {
      this.closed = true
    }
    frame(name: string, data: unknown) {
      this.dispatchEvent(new MessageEvent(name, { data: JSON.stringify(data) }))
    }
  }
  // Releases are scheduled with queueMicrotask; drain the microtask queue.
  const flushMicrotasks = async () => {
    for (let index = 0; index < 10; index += 1) await Promise.resolve()
  }
  const cleanups: Array<() => void> = []
  afterEach(() => {
    cleanups.splice(0).forEach((cleanup) => cleanup())
    setLiveQueryQueryClient(new QueryClient())
  })

  test('summary, traffic and nodes share one connection and node-runtime patches nodes', async () => {
    const client = new QueryClient()
    const sources: Source[] = []
    setLiveQueryQueryClient(client, undefined, (url) => {
      const source = new Source(url)
      sources.push(source)
      return source as never
    })
    const observers = [
      overviewQuery,
      overviewTrafficQuery,
      overviewNodesQuery,
    ].map((options) => {
      const observer = new QueryObserver(client, options as never)
      const unsubscribe = observer.subscribe(() => undefined)
      cleanups.push(() => observer.destroy())
      return { observer, unsubscribe }
    })
    await flushMicrotasks()
    expect(sources.map((source) => source.url)).toEqual([
      '/api/overview/stream',
    ])
    const summary: Overview = {
      resources: {
        website_count: 0,
        domain_count: 0,
        certificate_count: 0,
        cluster_count: 0,
        node_count: 1,
        online_node_count: 1,
      },
      issues: {
        dns_zone_issue_count: 0,
        certificate_expiring_count: 0,
        certificate_failed_count: 0,
        active_marker_count: 0,
        retry_marker_count: 0,
      },
      recent_markers: [],
    }
    const online = node('n1', 'online', { cpu_usage: 0.2 })
    const page = { list: [online], total: 1, page: 1, page_size: 100 }
    const ok = (data: unknown) => ({ code: 0, message: 'ok', data })
    sources[0].frame('snapshot', ok(summary))
    sources[0].frame('traffic', ok(traffic))
    sources[0].frame('nodes', ok(page))
    await Promise.all(
      [overviewQuery, overviewTrafficQuery, overviewNodesQuery].map(
        (options) =>
          client.getQueryCache().find({ queryKey: options.queryKey })?.promise
      )
    )
    expect(client.getQueryData(overviewQuery.queryKey)).toEqual(summary)
    expect(client.getQueryData(overviewTrafficQuery.queryKey)).toEqual(traffic)
    expect(client.getQueryData(overviewNodesQuery.queryKey)).toEqual(page)
    sources[0].frame('node-runtime', {
      id: 'n1',
      node_revision: online.revision,
      runtime: { ...online.runtime, cpu_usage: 0.7 },
    })
    await flushMicrotasks()
    expect(
      client.getQueryData(overviewNodesQuery.queryKey)?.list[0]?.runtime
        .cpu_usage
    ).toBe(0.7)
    expect(client.getQueryData(overviewTrafficQuery.queryKey)).toEqual(traffic)
    observers.forEach((item) => item.unsubscribe())
    await flushMicrotasks()
    expect(sources).toHaveLength(1)
    expect(sources[0].closed).toBe(true)
  })
})
