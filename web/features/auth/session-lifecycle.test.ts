import { QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { readLiveQuery, setLiveQueryQueryClient } from '@/lib/live-query'
import { createAppQueryClient } from '@/lib/query-client'
import { ensureAuthenticatedSession, login, logout } from './data'
import { replaceSession } from './session-lifecycle'

const cleanups: Array<() => void> = []
const originalAdapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = originalAdapter
  cleanups
    .splice(0)
    .reverse()
    .forEach((cleanup) => cleanup())
})
const user = { id: 'admin', username: 'same-admin', status: 'enabled' }

test('an obsolete session check cannot authenticate after logout', async () => {
  const client = setup()
  const check = ensureAuthenticatedSession(client)
  await replaceSession(client)
  await expect(check).rejects.toThrow('登录会话已变更，请重试')
})

function setup() {
  const client = createAppQueryClient(() => undefined)
  client.setQueryData(['session'], user)
  cleanups.push(() => client.clear())
  return client
}

test('late pre-logout reads cannot populate the next login cache', async () => {
  const client = setup()
  client.setQueryData(['websites'], ['old'])
  let respond!: (data: string[]) => void
  // Intentionally ignore AbortSignal, matching transports not yet cancellable.
  const oldRead = client
    .fetchQuery({
      queryKey: ['websites'],
      staleTime: 0,
      queryFn: () =>
        new Promise<string[]>((resolve) => {
          respond = resolve
        }),
    })
    .catch(() => undefined)
  await replaceSession(client)
  expect(client.getQueryData(['websites'])).toBeUndefined()
  await replaceSession(client, user)
  respond(['late old response'])
  await oldRead
  expect(client.getQueryData(['websites'])).toBeUndefined()
  const observer = new QueryObserver(client, {
    queryKey: ['websites'],
    queryFn: async () => ['new session'],
  })
  expect(observer.getCurrentResult().data).toBeUndefined()
  cleanups.push(observer.subscribe(() => undefined))
  await observer.refetch()
  expect(client.getQueryData(['websites'])).toEqual(['new session'])
})

test('session replacement aborts cancellable reads', async () => {
  const client = setup()
  let signal!: AbortSignal
  const capture = (value: AbortSignal) => {
    signal = value
  }
  const read = client
    .fetchQuery({
      queryKey: ['nodes'],
      queryFn: (context) => {
        capture(context.signal)
        return new Promise(() => undefined)
      },
    })
    .catch(() => undefined)
  await replaceSession(client)
  await read
  expect(signal.aborted).toBe(true)
})

test('live views close before cache removal and late frames do nothing', async () => {
  const client = setup()
  class Source extends EventTarget {
    closed = false
    close() {
      this.closed = true
    }
  }
  const source = new Source()
  // The ES2022 lib has no Promise.withResolvers.
  let open!: () => void
  const opened = new Promise<void>((resolve) => {
    open = resolve
  })
  setLiveQueryQueryClient(client, undefined, () => {
    open()
    return source
  })
  cleanups.push(() => setLiveQueryQueryClient(client))
  const observer = new QueryObserver(client, {
    queryKey: ['nodes'],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery('/api/nodes/stream', queryKey, signal),
  })
  cleanups.push(observer.subscribe(() => undefined))
  await opened
  await replaceSession(client)
  expect(source.closed).toBe(true)
  await replaceSession(client, user)
  source.dispatchEvent(
    new MessageEvent('snapshot', {
      data: JSON.stringify({ code: 0, message: 'ok', data: ['late'] }),
    })
  )
  expect(client.getQueryData(['nodes'])).toBeUndefined()
})

test('rejected auth operations preserve the existing session', async () => {
  const client = setup()
  client.setQueryData(['websites'], ['existing'])
  api.defaults.adapter = async () => {
    throw new Error('rejected')
  }
  await expect(login(client, 'admin', 'bad')).rejects.toThrow('rejected')
  await expect(logout(client)).rejects.toThrow('rejected')
  expect(client.getQueryData(['websites'])).toEqual(['existing'])
  expect(client.getQueryData(['session'])).toEqual(user)
})

test('accepted login and logout clear resource caches through the actual auth functions', async () => {
  const client = setup()
  client.setQueryData(['websites'], ['old'])
  api.defaults.adapter = async (config) => ({
    config,
    status: 200,
    statusText: 'OK',
    headers: {},
    data: {
      code: 0,
      message: '',
      ...(config.url === '/auth/login' ? { data: { user } } : {}),
    },
  })
  await login(client, user.username, 'password')
  expect(client.getQueryData(['websites'])).toBeUndefined()
  expect(client.getQueryData(['session'])).toEqual(user)
  client.setQueryData(['tasks'], ['new tasks'])
  await logout(client)
  expect(client.getQueryCache().getAll()).toHaveLength(0)
})

test('malformed successful login responses cannot replace the current session', async () => {
  const client = setup()
  client.setQueryData(['websites'], ['existing'])
  for (const data of [
    null,
    {},
    { user: null },
    { user: {} },
    { user: { ...user, username: 123 } },
    { user: { ...user, nickname: null } },
    { user: { ...user, id: '' } },
  ]) {
    api.defaults.adapter = async (config) => ({
      config,
      status: 200,
      statusText: 'OK',
      headers: {},
      data: { code: 0, message: '', data },
    })
    await expect(login(client, 'admin', 'password')).rejects.toThrow(
      '服务响应格式不正确'
    )
    expect(client.getQueryData(['session'])).toEqual(user)
    expect(client.getQueryData(['websites'])).toEqual(['existing'])
  }
})

test('malformed current-user data never authenticates or enters the session cache', async () => {
  const client = setup()
  await replaceSession(client)
  api.defaults.adapter = async (config) => ({
    config,
    status: 200,
    statusText: 'OK',
    headers: {},
    data: { code: 0, message: '', data: { ...user, status: false } },
  })
  await expect(ensureAuthenticatedSession(client)).rejects.toThrow(
    '服务响应格式不正确'
  )
  expect(client.getQueryData(['session'])).toBeUndefined()
})
