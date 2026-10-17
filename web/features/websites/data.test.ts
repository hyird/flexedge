import { afterEach, expect, it } from 'vitest'
import { api } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import { createWebsite, updateWebsite } from './data'
import { defaultWebsiteConfig } from './website-form'

const originalAdapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = originalAdapter
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
