import { QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { reconnectingEventSource } from '@/lib/event-stream'
import { createAppQueryClient } from '@/lib/query-client'
import { ensureAuthenticatedSession, login, logout } from './data'
import { registerSessionCleanup, replaceSession } from './session-lifecycle'

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

test('authenticated route re-entry activates streams after an external login', async () => {
  const client = setup()
  await replaceSession(client)
  let retired = false
  registerSessionCleanup(client, () => {
    retired = true
  })
  expect(retired).toBe(true)
  api.defaults.adapter = async (config) => ({
    config,
    status: 200,
    statusText: 'OK',
    headers: {},
    data: { code: 0, message: '', data: user },
  })
  await ensureAuthenticatedSession(client)
  let stopped = false
  cleanups.push(
    registerSessionCleanup(client, () => {
      stopped = true
    })
  )
  expect(stopped).toBe(false)
  await replaceSession(client)
  expect(stopped).toBe(true)
})

test('an obsolete session check cannot reactivate streams after logout', async () => {
  const client = setup()
  const check = ensureAuthenticatedSession(client).catch(() => undefined)
  await replaceSession(client)
  await check
  let stopped = false
  registerSessionCleanup(client, () => {
    stopped = true
  })
  expect(stopped).toBe(true)
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

test('both resource streams close before cache removal and late events do nothing', async () => {
  const client = setup()
  class Source extends EventTarget {
    readyState = 1
    close() {
      this.readyState = 2
    }
  }
  const sources = [new Source(), new Source()]
  const streams = sources.map((source) =>
    reconnectingEventSource('/stream', {
      createSource: () => source as unknown as EventSource,
      idleTimeoutMs: null,
    })
  )
  let events = 0
  for (const stream of streams)
    stream.addEventListener('ready', () => {
      events++
    })
  const dispose = registerSessionCleanup(client, () => {
    streams.forEach((stream) => stream.close())
  })
  cleanups.push(dispose)
  await replaceSession(client)
  expect(sources.every((source) => source.readyState === 2)).toBe(true)
  await replaceSession(client, user)
  sources.forEach((source) => source.dispatchEvent(new Event('ready')))
  expect(events).toBe(0)
  expect(client.getQueryData(['nodes'])).toBeUndefined()
})

test('rejected auth operations preserve the existing session and streams', async () => {
  const client = setup()
  client.setQueryData(['websites'], ['existing'])
  let stopped = false
  cleanups.push(
    registerSessionCleanup(client, () => {
      stopped = true
    })
  )
  api.defaults.adapter = async () => {
    throw new Error('rejected')
  }
  await expect(login(client, 'admin', 'bad')).rejects.toThrow('rejected')
  await expect(logout(client)).rejects.toThrow('rejected')
  expect(client.getQueryData(['websites'])).toEqual(['existing'])
  expect(client.getQueryData(['session'])).toEqual(user)
  expect(stopped).toBe(false)
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
  let stopped = false
  cleanups.push(
    registerSessionCleanup(client, () => {
      stopped = true
    })
  )
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
      'FlexEdge 服务响应格式不正确'
    )
    expect(client.getQueryData(['session'])).toEqual(user)
    expect(client.getQueryData(['websites'])).toEqual(['existing'])
    expect(stopped).toBe(false)
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
    'FlexEdge 服务响应格式不正确'
  )
  expect(client.getQueryData(['session'])).toBeUndefined()
  let stopped = false
  registerSessionCleanup(client, () => {
    stopped = true
  })
  expect(stopped).toBe(true)
})
