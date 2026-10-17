import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, describe, expect, test } from 'vitest'
import { ApiProtocolError } from '@/lib/api-response'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import type { Node } from '@/features/nodes/types'
import {
  overviewNodesQuery,
  overviewQuery,
  overviewTrafficQuery,
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
      },
    }
    const online = node('n1', 'online', { cpu_usage: 0.2 })
    const page = {
      list: [online],
      total: 1,
      page: 1,
      page_size: 100,
      total_pages: 1,
    }
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
    // The `nodes` frame is parsed with the node schema.
    sources[0].frame('nodes', ok({ ...page, list: [{ id: 'broken' }] }))
    await flushMicrotasks()
    const nodes = client
      .getQueryCache()
      .find({ queryKey: overviewNodesQuery.queryKey })?.state
    expect(nodes?.error).toBeInstanceOf(ApiProtocolError)
    expect(
      client.getQueryData(overviewNodesQuery.queryKey)?.list[0]?.runtime
        .cpu_usage
    ).toBe(0.7)
    observers.forEach((item) => item.unsubscribe())
    await flushMicrotasks()
    expect(sources).toHaveLength(1)
    expect(sources[0].closed).toBe(true)
  })
})
