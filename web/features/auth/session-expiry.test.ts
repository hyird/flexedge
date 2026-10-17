import { AxiosError, AxiosHeaders } from 'axios'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { createAppQueryClient } from '@/lib/query-client'
import { queryKeys } from '@/lib/query-keys'
import {
  createSessionExpiryHandler,
  recoverStreamSession,
} from './session-expiry'

const clients: ReturnType<typeof createAppQueryClient>[] = []
afterEach(() => clients.splice(0).forEach((client) => client.clear()))

function denied(status: number) {
  const config = { headers: new AxiosHeaders() }
  return new AxiosError('denied', undefined, config, undefined, {
    config,
    status,
    statusText: '',
    headers: {},
    data: {},
  })
}

test('stream recovery keeps the session on transient transport failure', async () => {
  const client = createAppQueryClient(() => undefined)
  clients.push(client)
  client.setQueryData(queryKeys.session, { id: 'admin' })
  const original = api.defaults.adapter
  api.defaults.adapter = async () => {
    throw new Error('network down')
  }
  try {
    await expect(recoverStreamSession(client)).resolves.toBe(true)
    expect(client.getQueryData(queryKeys.session)).toEqual({ id: 'admin' })
  } finally {
    api.defaults.adapter = original
  }
})

test('a final query 401 tears down cached resources before navigation', async () => {
  const errors: unknown[] = []
  const client = createAppQueryClient(
    (message) => errors.push(message),
    (error): boolean => expire(error)
  )
  clients.push(client)
  client.setQueryData(queryKeys.session, { id: 'admin' })
  client.setQueryData(['nodes'], ['old'])
  let navigated!: () => void
  const navigation = new Promise<void>((resolve) => {
    navigated = resolve
  })
  const expire = createSessionExpiryHandler(
    client,
    () => {
      expect(client.getQueryCache().getAll()).toHaveLength(0)
      navigated()
    },
    (error) => errors.push(error)
  )
  await client
    .fetchQuery({
      queryKey: ['nodes'],
      staleTime: 0,
      queryFn: async () => {
        throw denied(401)
      },
    })
    .catch(() => undefined)
  await navigation
  expect(errors).toEqual([])
})

test('mutation 401 clears the session before navigation', async () => {
  const client = createAppQueryClient(
    () => undefined,
    (error): boolean => expire(error)
  )
  clients.push(client)
  client.setQueryData(queryKeys.session, { id: 'admin' })
  let navigated!: () => void
  const navigation = new Promise<void>((resolve) => {
    navigated = resolve
  })
  let finishNavigation!: () => void
  const finished = new Promise<void>((resolve) => {
    finishNavigation = resolve
  })
  const expire = createSessionExpiryHandler(
    client,
    async () => {
      navigated()
      await finished
    },
    () => undefined
  )
  const mutation = client.getMutationCache().build(client, {
    mutationFn: async () => {
      throw denied(401)
    },
  })
  const request = mutation.execute(undefined).catch(() => undefined)
  await navigation
  await request
  expect(client.getQueryData(queryKeys.session)).toBeUndefined()
  finishNavigation()
})
