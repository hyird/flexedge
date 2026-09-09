import { AxiosError, AxiosHeaders } from 'axios'
import { afterEach, expect, test } from 'vitest'
import { createAppQueryClient } from '@/lib/query-client'
import { queryKeys } from '@/lib/query-keys'
import { createSessionExpiryHandler, recoverStreamSession } from './session-expiry'
import { registerSessionCleanup } from './session-lifecycle'
import { api } from '@/lib/api'

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
  api.defaults.adapter = async () => { throw new Error('network down') }
  try {
    await expect(recoverStreamSession(client)).resolves.toBe(true)
    expect(client.getQueryData(queryKeys.session)).toEqual({ id: 'admin' })
  } finally { api.defaults.adapter = original }
})

test('a final query 401 tears down cached resources and streams before navigation', async () => {
  const errors: unknown[] = []
  const client = createAppQueryClient(
    (message) => errors.push(message),
    (error): boolean => expire(error)
  )
  clients.push(client)
  client.setQueryData(queryKeys.session, { id: 'admin' })
  client.setQueryData(['nodes'], ['old'])
  let stopped = false
  registerSessionCleanup(client, () => {
    stopped = true
  })
  let navigated!: () => void
  const navigation = new Promise<void>((resolve) => {
    navigated = resolve
  })
  const expire = createSessionExpiryHandler(
    client,
    () => {
      expect(stopped).toBe(true)
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

test('mutation 401 uses the same teardown and concurrent failures navigate once', async () => {
  const client = createAppQueryClient(
    () => undefined,
    (error): boolean => expire(error)
  )
  clients.push(client)
  client.setQueryData(queryKeys.session, { id: 'admin' })
  let calls = 0
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
      calls++
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
  expect(calls).toBe(1)
  expect(client.getQueryData(queryKeys.session)).toBeUndefined()
  expect(expire(denied(401))).toBe(true)
  expect(calls).toBe(1)
  finishNavigation()
})

test('permission and initial authentication failures do not clear resource state', () => {
  const client = createAppQueryClient(() => undefined)
  clients.push(client)
  let navigations = 0
  const expire = createSessionExpiryHandler(
    client,
    () => {
      navigations++
    },
    () => undefined
  )
  expect(expire(denied(401))).toBe(false)
  client.setQueryData(queryKeys.session, { id: 'admin' })
  client.setQueryData(['nodes'], ['existing'])
  expect(expire(denied(403))).toBe(false)
  expect(expire(new Error('offline'))).toBe(false)
  expect(client.getQueryData(['nodes'])).toEqual(['existing'])
  expect(navigations).toBe(0)
})
