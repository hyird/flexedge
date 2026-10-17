import { expect, it } from 'vitest'
import { patchWebsiteOriginRuntime } from './data'
import type { Website } from './types'
import { sortedOriginStates } from './website-display'
import { defaultWebsiteConfig } from './website-form'

it('keeps name and address order as alternating node reports arrive', () => {
  const state = (node_id: string, node_name: string, origin_id: string) => ({
    node_id,
    node_name,
    origin_id,
    status: 'healthy' as const,
    checked_at_unix_millis: 100,
    latency_millis: 1,
  })
  const website = {
    id: 'website',
    config: {
      origins: [
        { id: 'b', protocol: 'http', host: 'b.example', port: 80 },
        { id: 'a', protocol: 'http', host: 'a.example', port: 80 },
      ],
    },
    runtime: {
      origin_sources: ['cd10', 'bj', 'cd2'].map((node_id) => ({
        node_id,
        node_revision: 1,
        reported_at: '2026-10-17T00:00:00Z',
      })),
      origin_states: [
        state('cd10', '成都10', 'a'),
        state('bj', '北京', 'b'),
        state('cd2', '成都2', 'a'),
        state('bj', '北京', 'a'),
      ],
    },
  } as Website
  const ids = (value: Website) =>
    sortedOriginStates(value).map((item) => `${item.node_id}:${item.origin_id}`)
  const expected = ['bj:a', 'bj:b', 'cd2:a', 'cd10:a']
  const original = [...website.runtime.origin_states]
  expect(ids(website)).toEqual(expected)
  let current = website
  for (const node_id of ['cd2', 'bj', 'cd10', 'bj']) {
    current = patchWebsiteOriginRuntime(
      current,
      {
        id: website.id,
        node_id,
        node_revision: 1,
        reported_at: `2026-10-17T00:00:${node_id === 'bj' && current !== website ? '20' : '05'}Z`,
        origin_states: original
          .filter((item) => item.node_id === node_id)
          .reverse(),
      },
      'origin-runtime'
    )!
    expect(ids(current)).toEqual(expected)
  }
  expect(website.runtime.origin_states).toEqual(original)
})

it('defaults new website health checks to sixty seconds', () => {
  expect(defaultWebsiteConfig().health_check_interval_seconds).toBe(60)
})
