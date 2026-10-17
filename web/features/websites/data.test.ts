import { QueryClient } from '@tanstack/react-query'
import { afterEach, expect, it, vi } from 'vitest'
import { api } from '@/lib/api'
import { ApiBusinessError, ApiProtocolError } from '@/lib/api-response'
import { queryKeys } from '@/lib/query-keys'
import {
  afterWebsiteWrite,
  createWebsite,
  isWebsiteNotFound,
  patchWebsiteOriginRuntime,
  updateWebsite,
  websiteSnapshotKey,
  websitesSnapshotKey,
} from './data'
import type { Website } from './types'
import { defaultWebsiteConfig } from './website-form'

const originalAdapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = originalAdapter
})

it('applies current origin runtime per node and ignores stale updates', () => {
  const website = {
    id: 'website',
    runtime: {
      origin_sources: [
        {
          node_id: 'node',
          node_revision: 1,
          reported_at: '2026-09-08T00:00:00Z',
        },
      ],
      origin_states: [
        {
          node_id: 'node',
          node_name: 'N',
          origin_id: 'origin-old',
          status: 'healthy',
          checked_at_unix_millis: 200,
        },
        {
          node_id: 'other',
          node_name: 'O',
          origin_id: 'origin-other',
          status: 'healthy',
          checked_at_unix_millis: 1,
        },
      ],
    },
  } as Website
  const update = {
    id: 'website',
    node_id: 'node',
    node_revision: 1,
    reported_at: '2026-09-08T00:01:00Z',
    origin_states: [
      {
        node_id: 'node',
        node_name: 'N',
        origin_id: 'origin-new',
        status: 'unhealthy',
        checked_at_unix_millis: 300,
        latency_millis: 12,
      },
    ],
  }
  const event = 'origin-runtime'
  const changed = patchWebsiteOriginRuntime(website, update, event)!
  expect(changed.runtime.origin_states.map((state) => state.origin_id)).toEqual(
    ['origin-other', 'origin-new']
  )
  expect(
    patchWebsiteOriginRuntime(
      changed,
      {
        ...update,
        reported_at: '2026-09-08T00:00:30Z',
        origin_states: [
          { ...update.origin_states[0], checked_at_unix_millis: 100 },
        ],
      },
      event
    )
  ).toBe(changed)
  const removed = patchWebsiteOriginRuntime(
    changed,
    { ...update, reported_at: '2026-09-08T00:02:00Z', origin_states: [] },
    event
  )!
  expect(removed.runtime.origin_states).toEqual([
    changed.runtime.origin_states[0],
  ])
  expect(
    patchWebsiteOriginRuntime(
      removed,
      { ...update, reported_at: '2026-09-08T00:01:30Z' },
      event
    )
  ).toBe(removed)
})

it('sends cluster selection separately from the website configuration', async () => {
  const config = defaultWebsiteConfig()
  const id = '00000000-0000-4000-8000-000000000999'
  api.defaults.adapter = async (request) => {
    expect(request.method).toBe('post')
    expect(request.url).toBe('/websites?cluster_id=cluster-one')
    expect(JSON.parse(request.data)).toEqual({ status: 'enabled', config })
    expect(request.headers.has('If-Match')).toBe(false)
    return {
      config: request,
      status: 200,
      statusText: 'OK',
      headers: {},
      data: { code: 0, message: 'created', data: { id, revision: 1 } },
    }
  }
  await expect(
    createWebsite({ cluster_id: 'cluster-one', status: 'enabled', config })
  ).resolves.toEqual({ id, revision: 1 })
})

it('rejects a create response without the new website id', async () => {
  api.defaults.adapter = async (request) => ({
    config: request,
    status: 200,
    statusText: 'OK',
    headers: {},
    data: { code: 0, message: 'created', data: { revision: 1 } },
  })
  await expect(
    createWebsite({
      cluster_id: 'cluster-one',
      status: 'enabled',
      config: defaultWebsiteConfig(),
    })
  ).rejects.toBeInstanceOf(ApiProtocolError)
})

it('uses the selected revision when updating an existing website', async () => {
  api.defaults.adapter = async (request) => {
    expect(request.method).toBe('put')
    expect(request.url).toBe('/websites/site-one?cluster_id=cluster-two')
    expect(request.headers.get('If-Match')).toBe('"7"')
    return {
      config: request,
      status: 200,
      statusText: 'OK',
      headers: {},
      data: { code: 0, message: 'updated', data: null },
    }
  }
  const response = await updateWebsite(
    {
      cluster_id: 'cluster-two',
      status: 'disabled',
      config: defaultWebsiteConfig(),
    },
    { id: 'site-one', revision: 7 }
  )
  expect(response.message).toBe('updated')
})

it('recognises the website not-found stream error', () => {
  expect(isWebsiteNotFound(new ApiBusinessError(16601, '网站不存在'))).toBe(
    true
  )
  expect(isWebsiteNotFound(new ApiBusinessError(40001, '资源不存在'))).toBe(
    false
  )
  expect(isWebsiteNotFound(new Error('网站不存在'))).toBe(false)
})

it('invalidates dependent caches after a website write', async () => {
  const client = new QueryClient()
  const spy = vi.spyOn(client, 'invalidateQueries')
  await afterWebsiteWrite(client, 'site-1')
  expect(spy.mock.calls.map(([arg]) => arg?.queryKey)).toEqual([
    websitesSnapshotKey,
    queryKeys.certificates,
    queryKeys.dnsZones,
    queryKeys.clusters,
    [...websiteSnapshotKey, 'site-1'],
  ])
})
