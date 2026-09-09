import { AxiosError, AxiosHeaders } from 'axios'
import { afterEach, expect, it } from 'vitest'
import { createAppQueryClient } from '@/lib/query-client'
import { sessionQueryOptions } from './data'

const clients: ReturnType<typeof createAppQueryClient>[] = []
afterEach(() => clients.splice(0).forEach((client) => client.clear()))
function setup() {
  const messages: string[] = []
  const client = createAppQueryClient((message) => messages.push(message))
  clients.push(client)
  return { client, messages }
}

it('keeps signed-out session probes quiet while reporting other failures', async () => {
  for (const status of [401, 503]) {
    const { client, messages } = setup()
    const config = { headers: new AxiosHeaders() }
    const error = new AxiosError('denied', undefined, config, undefined, {
      status,
      statusText: 'Denied',
      headers: {},
      config,
      data: { message: status === 401 ? '未登录' : '服务不可用' },
    })
    await expect(
      client.fetchQuery({
        ...sessionQueryOptions,
        queryFn: async () => {
          throw error
        },
      })
    ).rejects.toBe(error)
    expect(messages).toEqual(status === 401 ? [] : ['服务不可用'])
    if (status === 401) {
      const mutation = client.getMutationCache().build(client, {
        mutationFn: async () => {
          throw error
        },
      })
      await expect(mutation.execute(undefined)).rejects.toBe(error)
      expect(messages).toEqual(['未登录'])
    }
  }
})
