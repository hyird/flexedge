import { AxiosError, AxiosHeaders } from 'axios'
import { afterEach, describe, expect, it } from 'vitest'
import { ApiBusinessError, ApiProtocolError } from './api-response'
import { createAppQueryClient } from './query-client'

const clients: ReturnType<typeof createAppQueryClient>[] = []
afterEach(() => {
  clients.splice(0).forEach((client) => client.clear())
})

function setup() {
  const messages: string[] = []
  const client = createAppQueryClient((message) => {
    messages.push(message)
  })
  // Exercise the real retry policy without its production backoff delay.
  client.setDefaultOptions({
    ...client.getDefaultOptions(),
    queries: { ...client.getDefaultOptions().queries, retryDelay: 0 },
  })
  clients.push(client)
  return { client, messages }
}

describe('application query policy', () => {
  it('does not retry deterministic response contract or business errors', async () => {
    for (const error of [
      new ApiProtocolError(),
      new ApiBusinessError(10001, 'invalid'),
    ]) {
      const { client } = setup()
      let attempts = 0
      await expect(
        client.fetchQuery({
          queryKey: ['resource'],
          queryFn: async () => {
            attempts++
            throw error
          },
        })
      ).rejects.toBe(error)
      expect(attempts).toBe(1)
    }
  })
  it('retries transient reads, then notifies once when initial loading fails', async () => {
    const { client, messages } = setup()
    let attempts = 0
    await expect(
      client.fetchQuery({
        queryKey: ['resource'],
        queryFn: async () => {
          attempts += 1
          throw new Error('unavailable')
        },
      })
    ).rejects.toThrow('unavailable')
    expect(attempts).toBe(3)
    expect(messages).toEqual(['unavailable'])
  })

  it('does not retry authentication or permission failures', async () => {
    for (const status of [401, 403]) {
      const { client, messages } = setup()
      const config = { headers: new AxiosHeaders() }
      const error = new AxiosError('denied', undefined, config, undefined, {
        status,
        statusText: 'Denied',
        headers: {},
        config,
        data: { message: 'access denied' },
      })
      let attempts = 0
      await expect(
        client.fetchQuery({
          queryKey: ['protected'],
          queryFn: async () => {
            attempts += 1
            throw error
          },
        })
      ).rejects.toBe(error)
      expect(attempts).toBe(1)
      expect(messages).toEqual(['access denied'])
    }
  })

  it('keeps cached data and leaves refresh error reporting to the refresh owner', async () => {
    const { client, messages } = setup()
    client.setQueryData(['resource'], ['previous'])
    await client.invalidateQueries({
      queryKey: ['resource'],
      refetchType: 'none',
    })
    await expect(
      client.fetchQuery({
        queryKey: ['resource'],
        retry: false,
        queryFn: async () => {
          throw new Error('refresh failed')
        },
      })
    ).rejects.toThrow('refresh failed')
    expect(client.getQueryData(['resource'])).toEqual(['previous'])
    expect(messages).toEqual([])
  })

  it('serves fresh cached data until an explicit invalidation', async () => {
    const { client } = setup()
    let reads = 0
    const options = { queryKey: ['resource'], queryFn: async () => ++reads }
    expect(await client.fetchQuery(options)).toBe(1)
    expect(await client.fetchQuery(options)).toBe(1)
    await client.invalidateQueries({
      queryKey: ['resource'],
      refetchType: 'none',
    })
    expect(await client.fetchQuery(options)).toBe(2)
  })

  it('reports mutation failures through the injected notifier', async () => {
    const { client, messages } = setup()
    const mutation = client.getMutationCache().build(client, {
      mutationFn: async () => {
        throw new Error('write failed')
      },
    })
    await expect(mutation.execute(undefined)).rejects.toThrow('write failed')
    expect(messages).toEqual(['write failed'])
  })
})
