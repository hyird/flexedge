import { AxiosError, type InternalAxiosRequestConfig } from 'axios'
import { describe, expect, it } from 'vitest'
import { createApiClient } from './api-client'

function deferred() {
  let resolve!: () => void
  const promise = new Promise<void>((done) => {
    resolve = done
  })
  return { promise, resolve }
}

function response(config: InternalAxiosRequestConfig, status = 200) {
  return {
    config,
    status,
    statusText: '',
    headers: {},
    data: { code: 0, message: '' },
  }
}

function unauthorized(config: InternalAxiosRequestConfig) {
  return new AxiosError(
    'unauthorized',
    'ERR_BAD_REQUEST',
    config,
    undefined,
    response(config, 401)
  )
}

describe('session refresh ownership', () => {
  it('shares one refresh for concurrent failures and replays each request once', async () => {
    const client = createApiClient()
    const started = deferred()
    const release = deferred()
    const calls: string[] = []
    let refreshed = false
    client.defaults.adapter = async (config) => {
      calls.push(config.url!)
      if (config.url === '/auth/refresh') {
        started.resolve()
        await release.promise
        refreshed = true
      } else if (!refreshed) throw unauthorized(config)
      return response(config)
    }
    const requests = Promise.all([
      client.get('/nodes'),
      client.get('/clusters'),
    ])
    await started.promise
    release.resolve()
    await requests
    expect(calls.filter((url) => url === '/auth/refresh')).toHaveLength(1)
    expect(calls.filter((url) => url === '/nodes')).toHaveLength(2)
    expect(calls.filter((url) => url === '/clusters')).toHaveLength(2)
  })

  it('rejects every waiter on refresh failure and permits a later refresh', async () => {
    const client = createApiClient()
    const started = deferred()
    const release = deferred()
    let refreshes = 0
    let refreshed = false
    client.defaults.adapter = async (config) => {
      if (config.url === '/auth/refresh') {
        refreshes += 1
        if (refreshes === 1) {
          started.resolve()
          await release.promise
          throw unauthorized(config)
        }
        refreshed = true
      } else if (!refreshed) throw unauthorized(config)
      return response(config)
    }
    const requests = Promise.allSettled([
      client.get('/nodes'),
      client.get('/clusters'),
    ])
    await started.promise
    release.resolve()
    expect((await requests).map((result) => result.status)).toEqual([
      'rejected',
      'rejected',
    ])
    expect(refreshes).toBe(1)
    await client.get('/nodes')
    expect(refreshes).toBe(2)
  })

  it('does not loop when a replay is still unauthorized or login fails', async () => {
    const client = createApiClient()
    const calls: string[] = []
    client.defaults.adapter = async (config) => {
      calls.push(config.url!)
      if (config.url === '/auth/refresh') return response(config)
      throw unauthorized(config)
    }
    await expect(client.get('/nodes')).rejects.toBeInstanceOf(AxiosError)
    expect(calls).toEqual(['/nodes', '/auth/refresh', '/nodes'])
    await expect(client.post('/auth/login')).rejects.toBeInstanceOf(AxiosError)
    expect(calls.at(-1)).toBe('/auth/login')
    expect(calls).toHaveLength(4)
  })

  it('keeps refresh state independent between clients', async () => {
    const first = createApiClient()
    const second = createApiClient()
    const release = deferred()
    const started = deferred()
    let firstReady = false
    let secondReady = false
    first.defaults.adapter = async (config) => {
      if (config.url === '/auth/refresh') {
        started.resolve()
        await release.promise
        firstReady = true
      } else if (!firstReady) throw unauthorized(config)
      return response(config)
    }
    second.defaults.adapter = async (config) => {
      if (config.url === '/auth/refresh') secondReady = true
      else if (!secondReady) throw unauthorized(config)
      return response(config)
    }
    const pending = first.get('/nodes')
    await started.promise
    await second.get('/nodes')
    expect(secondReady).toBe(true)
    expect(firstReady).toBe(false)
    release.resolve()
    await pending
  })
})
