import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
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

function setup() {
  const client = createAppQueryClient(() => undefined)
  client.setQueryData(['session'], user)
  cleanups.push(() => client.clear())
  return client
}

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
