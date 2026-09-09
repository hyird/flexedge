import { afterEach, expect, it } from 'vitest'
import { api } from '@/lib/api'
import { patchWebsiteOriginRuntime, saveWebsite } from './data'
import type { Website } from './types'
import { defaultWebsiteConfig } from './website-form'

const originalAdapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = originalAdapter
})

it('applies current origin runtime per node and ignores stale updates', () => {
  const website = {
    id: 'website',
    runtime: { origin_sources: [{ node_id: 'node', node_revision: 1, reported_at: '2026-09-08T00:00:00Z' }], origin_states: [
      { node_id: 'node', node_name: 'N', origin_id: 'origin-old', status: 'healthy', checked_at_unix_millis: 200 },
      { node_id: 'other', node_name: 'O', origin_id: 'origin-other', status: 'healthy', checked_at_unix_millis: 1 },
    ] },
  } as Website
  const update = { id: 'website', node_id: 'node', node_revision: 1, reported_at: '2026-09-08T00:01:00Z', origin_states: [
    { node_id: 'node', node_name: 'N', origin_id: 'origin-new', status: 'unhealthy', checked_at_unix_millis: 300 },
  ] }
  const changed = patchWebsiteOriginRuntime(website, update)!
  expect(changed.runtime.origin_states.map((state) => state.origin_id)).toEqual(['origin-other', 'origin-new'])
  expect(patchWebsiteOriginRuntime(changed, { ...update, reported_at: '2026-09-08T00:00:30Z', origin_states: [{ ...update.origin_states[0], checked_at_unix_millis: 100 }] })).toBe(changed)
  const removed = patchWebsiteOriginRuntime(changed, { ...update, reported_at: '2026-09-08T00:02:00Z', origin_states: [] })!
  expect(removed.runtime.origin_states).toEqual([changed.runtime.origin_states[0]])
  expect(patchWebsiteOriginRuntime(removed, { ...update, reported_at: '2026-09-08T00:01:30Z' })).toBe(removed)
})

it('sends cluster selection separately from the website configuration', async () => {
  const config = defaultWebsiteConfig()
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
      data: { code: 0, message: 'created', data: null },
    }
  }
  const response = await saveWebsite({
    cluster_id: 'cluster-one',
    status: 'enabled',
    config,
  })
  expect(response.message).toBe('created')
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
  const response = await saveWebsite(
    {
      cluster_id: 'cluster-two',
      status: 'disabled',
      config: defaultWebsiteConfig(),
    },
    { id: 'site-one', revision: 7 }
  )
  expect(response.message).toBe('updated')
})
