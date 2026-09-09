import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, describe, expect, it } from 'vitest'
import { api } from '@/lib/api'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import { ApiProtocolError } from '@/lib/api-response'
import {
  createNode,
  getNodeCredentials,
  updateNode,
  nodesQuery,
  type NodeInput,
} from './data'

const originalAdapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = originalAdapter
  setLiveQueryQueryClient(new QueryClient())
})

const id = '22bc1a68-6db8-4c77-bd29-c11e563fa187'
const input: NodeInput = {
  cluster_id: id,
  name: 'edge',
  status: 'enabled',
  config: { endpoints: [] },
}

function respond(data: unknown) {
  api.defaults.adapter = async (config) => ({
    config,
    status: 200,
    statusText: 'OK',
    headers: {},
    data,
  })
}

describe('node API contracts', () => {
  it('applies only newer matching node runtime patches', async () => {
    class Source extends EventTarget { close() {} }
    const source = new Source()
    const client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
    setLiveQueryQueryClient(client, undefined, () => source as never)
    const params = { page: 1, page_size: 100 }
    const node = {
      id, revision: 2, cluster_id: id, cluster_name: 'cluster', name: 'edge', status: 'disabled',
      node_spec_revision: 1, config: { endpoints: [] },
      runtime: { registration_status: 'registered', connection_status: 'offline', last_heartbeat_at: '2026-09-09T00:00:00Z', applied_node_spec_revision: 1, cpu_usage: 1 },
    }
    const observer = new QueryObserver(client, nodesQuery(params))
    const stop = observer.subscribe(() => undefined)
    const pending = observer.refetch()
    source.dispatchEvent(new MessageEvent('snapshot', { data: JSON.stringify({ code: 0, message: 'ok', data: { list: [node], total: 1, page: 1, page_size: 100, total_pages: 1 } }) }))
    await pending
    const patch = (nodeRevision: number, heartbeat: string, cpu: number) =>
      source.dispatchEvent(new MessageEvent('node-runtime', { data: JSON.stringify({ id, node_revision: nodeRevision, runtime: { registration_status: 'registered', connection_status: 'online', last_heartbeat_at: heartbeat, applied_node_spec_revision: 2, cpu_usage: cpu } }) }))
    patch(1, '2026-09-09T00:02:00Z', 2)
    patch(2, '2026-09-09T00:00:00Z', 3)
    patch(2, '2026-09-09T00:01:00Z', 4)
    await new Promise<void>((resolve) => queueMicrotask(() => resolve()))
    const cached = client.getQueryData<{ list: typeof node[] }>(nodesQuery(params).queryKey)!
    expect(cached.list[0].runtime.cpu_usage).toBe(4)
    expect(cached.list[0].runtime.connection_status).toBe('offline')
    expect(cached.list[0].runtime.registration_status).toBe('registered')
    stop(); observer.destroy()
  })
  it('rejects missing or malformed credentials on creation and retrieval', async () => {
    for (const data of [
      undefined,
      null,
      {},
      { node_id: id, secret: '', revision: 1 },
      { node_id: 'invalid', secret: 'token', revision: 1 },
      { node_id: id, secret: 'token', revision: 0 },
      { node_id: id, secret: 'token', revision: 1.5 },
    ]) {
      respond({ code: 0, message: 'created', data })
      await expect(createNode(input)).rejects.toBeInstanceOf(ApiProtocolError)
      await expect(getNodeCredentials(id)).rejects.toBeInstanceOf(
        ApiProtocolError
      )
    }
  })

  it('returns validated credentials on creation', async () => {
    const credentials = { node_id: id, secret: 'token', revision: 1 }
    respond({ code: 0, message: 'created', data: credentials })
    expect((await createNode(input)).data).toEqual(credentials)
  })

  it('retrieves validated credentials from the selected node', async () => {
    const credentials = { node_id: id, secret: 'token', revision: 3 }
    api.defaults.adapter = async (config) => {
      expect(config.method).toBe('post')
      expect(config.url).toBe(`/nodes/${id}/credentials/reveal`)
      return {
        config,
        status: 200,
        statusText: 'OK',
        headers: {},
        data: { code: 0, message: '', data: credentials },
      }
    }
    expect(await getNodeCredentials(id)).toEqual(credentials)
  })

  it('accepts update acknowledgements and sends the original revision', async () => {
    api.defaults.adapter = async (config) => {
      expect(config.method).toBe('put')
      expect(config.url).toBe(`/nodes/${id}`)
      expect(config.headers.get('If-Match')).toBe('"7"')
      expect(JSON.parse(config.data)).toEqual(input)
      return {
        config,
        status: 200,
        statusText: 'OK',
        headers: {},
        data: { code: 0, message: 'updated' },
      }
    }
    expect(await updateNode({ id, revision: 7 }, input)).toEqual({
      code: 0,
      message: 'updated',
      data: undefined,
    })
  })

  it('does not expose unsolicited update data as credentials', async () => {
    respond({ code: 0, message: 'updated', data: { secret: 'unvalidated' } })
    expect((await updateNode({ id, revision: 7 }, input)).data).toBeUndefined()
  })
})
