import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, describe, expect, it } from 'vitest'
import { api } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  createNode,
  getNodeCredentials,
  nodeQuery,
  nodesQuery,
  resetNodeCredentials,
  updateNode,
  type NodeInput,
} from './data'

const originalAdapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = originalAdapter
  setLiveQueryQueryClient(new QueryClient())
})

const id = '22bc1a68-6db8-4c77-bd29-c11e563fa187'
const agentId = '0123456789abcdef0123456789abcdef'
const secret = 'a'.repeat(64)
const input: NodeInput = {
  cluster_id: id,
  name: 'edge',
  status: 'enabled',
  config: { endpoints: [] },
}

const nodeDto = {
  id,
  revision: 2,
  created_at: '2026-09-01T00:00:00Z',
  updated_at: '2026-09-01T00:00:00Z',
  cluster_id: id,
  cluster_name: 'cluster',
  name: 'edge',
  status: 'enabled',
  node_spec_revision: 1,
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
    class Source extends EventTarget {
      close() {}
    }
    const source = new Source()
    const client = new QueryClient({
      defaultOptions: { queries: { retry: false } },
    })
    setLiveQueryQueryClient(client, undefined, () => source as never)
    const params = { page: 1, page_size: 100 }
    const node = {
      ...nodeDto,
      status: 'disabled',
      runtime: {
        registration_status: 'registered',
        connection_status: 'offline',
        last_heartbeat_at: '2026-09-09T00:00:00Z',
        applied_node_spec_revision: 1,
        cpu_usage: 1,
      },
    }
    const observer = new QueryObserver(client, nodesQuery(params))
    const stop = observer.subscribe(() => undefined)
    const pending = observer.refetch()
    source.dispatchEvent(
      new MessageEvent('snapshot', {
        data: JSON.stringify({
          code: 0,
          message: 'ok',
          data: {
            list: [node],
            total: 1,
            page: 1,
            page_size: 100,
            total_pages: 1,
          },
        }),
      })
    )
    await pending
    const patch = (nodeRevision: number, heartbeat: string, cpu: number) =>
      source.dispatchEvent(
        new MessageEvent('node-runtime', {
          data: JSON.stringify({
            id,
            node_revision: nodeRevision,
            runtime: {
              registration_status: 'registered',
              connection_status: 'online',
              last_heartbeat_at: heartbeat,
              applied_node_spec_revision: 2,
              cpu_usage: cpu,
              cpu_count: 4,
              memory_total_bytes: 8 * 1024 ** 3,
              disk_total_bytes: 100 * 1024 ** 3,
            },
          }),
        })
      )
    patch(1, '2026-09-09T00:02:00Z', 2)
    patch(2, '2026-09-09T00:00:00Z', 3)
    patch(2, '2026-09-09T00:01:00Z', 4)
    await new Promise<void>((resolve) => queueMicrotask(() => resolve()))
    const cached = client.getQueryData<{ list: (typeof node)[] }>(
      nodesQuery(params).queryKey
    )!
    expect(cached.list[0].runtime.cpu_usage).toBe(4)
    expect(cached.list[0].runtime).toMatchObject({
      cpu_count: 4,
      memory_total_bytes: 8 * 1024 ** 3,
      disk_total_bytes: 100 * 1024 ** 3,
    })
    expect(cached.list[0].runtime.connection_status).toBe('offline')
    expect(cached.list[0].runtime.registration_status).toBe('registered')
    stop()
    observer.destroy()
  })
  it('rejects missing or malformed credentials on creation and retrieval', async () => {
    for (const data of [
      undefined,
      null,
      {},
      { node_id: agentId, secret: '', revision: 1 },
      { node_id: agentId, secret: 'short', revision: 1 },
      { node_id: agentId, secret: `${secret} space`, revision: 1 },
      { node_id: id, secret, revision: 1 },
      { node_id: agentId.toUpperCase(), secret, revision: 1 },
      { node_id: agentId, secret, revision: 0 },
      { node_id: agentId, secret, revision: 1.5 },
    ]) {
      respond({ code: 0, message: 'created', data })
      await expect(createNode(input)).rejects.toBeInstanceOf(ApiProtocolError)
      await expect(getNodeCredentials(id)).rejects.toBeInstanceOf(
        ApiProtocolError
      )
    }
  })

  it('returns the created node id with validated credentials', async () => {
    const created = { id, node_id: agentId, secret, revision: 1 }
    respond({ code: 0, message: 'created', data: created })
    expect((await createNode(input)).data).toEqual(created)
    for (const data of [
      { node_id: agentId, secret, revision: 1 },
      { id: agentId, node_id: agentId, secret, revision: 1 },
    ]) {
      respond({ code: 0, message: 'created', data })
      await expect(createNode(input)).rejects.toBeInstanceOf(ApiProtocolError)
    }
  })

  it('retrieves validated credentials from the selected node', async () => {
    const credentials = { node_id: agentId, secret, revision: 3 }
    api.defaults.adapter = async (config) => {
      expect(config.method).toBe('post')
      expect(config.url).toBe(`/nodes/${id}/credentials/reveal`)
      expect(config.headers.get('If-Match')).toBeUndefined()
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

  it('resets credentials with the node revision', async () => {
    const credentials = { node_id: agentId, secret, revision: 8 }
    api.defaults.adapter = async (config) => {
      expect(config.method).toBe('post')
      expect(config.url).toBe(`/nodes/${id}/credentials`)
      expect(config.headers.get('If-Match')).toBe('"7"')
      return {
        config,
        status: 200,
        statusText: 'OK',
        headers: {},
        data: { code: 0, message: 'reset', data: credentials },
      }
    }
    expect(await resetNodeCredentials({ id, revision: 7 })).toEqual({
      message: 'reset',
      data: credentials,
    })
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
    })
  })

  it('does not expose unsolicited update data as credentials', async () => {
    respond({ code: 0, message: 'updated', data: { secret: 'unvalidated' } })
    expect(await updateNode({ id, revision: 7 }, input)).toEqual({
      code: 0,
      message: 'updated',
    })
  })

  it('patches the node detail stream in place without a new snapshot', async () => {
    class Source extends EventTarget {
      close() {}
    }
    const sources: Source[] = []
    const client = new QueryClient({
      defaultOptions: { queries: { retry: false } },
    })
    setLiveQueryQueryClient(client, undefined, (url) => {
      expect(url).toBe(`/api/nodes/${id}/stream`)
      const source = new Source()
      sources.push(source)
      return source as never
    })
    const node = {
      ...nodeDto,
      revision: 4,
      runtime: {
        registration_status: 'registered',
        connection_status: 'online',
        last_heartbeat_at: '2026-09-09T00:00:00Z',
        applied_node_spec_revision: 1,
        cpu_usage: 0.1,
      },
    }
    const observer = new QueryObserver(client, nodeQuery(id))
    const stop = observer.subscribe(() => undefined)
    const pending = observer.refetch()
    sources[0].dispatchEvent(
      new MessageEvent('snapshot', {
        data: JSON.stringify({ code: 0, message: 'ok', data: node }),
      })
    )
    await pending
    const patch = (revision: number, cpu: number) =>
      sources[0].dispatchEvent(
        new MessageEvent('node-runtime', {
          data: JSON.stringify({
            id,
            node_revision: revision,
            runtime: {
              registration_status: 'registered',
              connection_status: 'offline',
              last_heartbeat_at: '2026-09-09T00:01:00Z',
              applied_node_spec_revision: 1,
              cpu_usage: cpu,
            },
          }),
        })
      )
    patch(3, 0.9)
    patch(4, 0.42)
    const cached = client.getQueryData<typeof node>(nodeQuery(id).queryKey)!
    expect(cached.runtime.cpu_usage).toBe(0.42)
    expect(cached.runtime.last_heartbeat_at).toBe('2026-09-09T00:01:00Z')
    expect(cached.runtime.connection_status).toBe('online')
    expect(sources).toHaveLength(1)
    stop()
    observer.destroy()
  })

  it('merges connected_ip_count from patches and keeps it when omitted', async () => {
    class Source extends EventTarget {
      close() {}
    }
    const sources: Source[] = []
    const client = new QueryClient({
      defaultOptions: { queries: { retry: false } },
    })
    setLiveQueryQueryClient(client, undefined, () => {
      const source = new Source()
      sources.push(source)
      return source as never
    })
    const node = {
      ...nodeDto,
      revision: 4,
      runtime: {
        registration_status: 'registered',
        connection_status: 'online',
        last_heartbeat_at: '2026-09-09T00:00:00Z',
        applied_node_spec_revision: 1,
        connected_ip_count: 3,
      },
    }
    const observer = new QueryObserver(client, nodeQuery(id))
    const stop = observer.subscribe(() => undefined)
    const pending = observer.refetch()
    sources[0].dispatchEvent(
      new MessageEvent('snapshot', {
        data: JSON.stringify({ code: 0, message: 'ok', data: node }),
      })
    )
    await pending
    const patch = (heartbeat: string, runtime: object) =>
      sources[0].dispatchEvent(
        new MessageEvent('node-runtime', {
          data: JSON.stringify({
            id,
            node_revision: 4,
            runtime: {
              ...node.runtime,
              connected_ip_count: undefined,
              last_heartbeat_at: heartbeat,
              ...runtime,
            },
          }),
        })
      )
    const cached = () =>
      client.getQueryData<typeof node>(nodeQuery(id).queryKey)!.runtime
    patch('2026-09-09T00:00:05Z', { connected_ip_count: 12 })
    expect(cached().connected_ip_count).toBe(12)
    patch('2026-09-09T00:00:10Z', { cpu_usage: 0.3 })
    expect(cached().connected_ip_count).toBe(12)
    expect(cached().last_heartbeat_at).toBe('2026-09-09T00:00:10Z')
    patch('2026-09-09T00:00:15Z', { connected_ip_count: 0 })
    expect(cached().connected_ip_count).toBe(0)
    stop()
    observer.destroy()
  })
})
