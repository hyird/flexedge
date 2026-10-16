import { afterEach, describe, expect, it } from 'vitest'
import {
  api,
  apiErrorMessage,
  isRevisionConflict,
  getData,
  sendData,
  normalizeApiPath,
} from './api'
import { ApiBusinessError, ApiProtocolError } from './api-response'

const originalAdapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = originalAdapter
})

describe('HTTP response contracts', () => {
  function respond(data: unknown, headers: Record<string, string> = {}) {
    api.defaults.adapter = async (config) => ({
      config,
      status: 200,
      statusText: 'OK',
      headers,
      data,
    })
  }
  it('accepts operation acknowledgements but requires data for reads', async () => {
    respond({ code: 0, message: 'saved' })
    expect(await sendData('put', '/resource')).toEqual({
      code: 0,
      message: 'saved',
    })
    await expect(getData('/resource')).rejects.toBeInstanceOf(ApiProtocolError)
    respond({ code: 0, message: '', data: null })
    expect(await getData('/resource')).toBeNull()
  })
  it('returns the revision a write produced from its ETag', async () => {
    respond({ code: 0, message: 'saved' }, { etag: '"8"' })
    expect(await sendData('put', '/resource', {}, 7)).toEqual({
      code: 0,
      message: 'saved',
      revision: 8,
    })
  })
  it('rejects business failures even when HTTP status is successful', async () => {
    respond({ code: 10001, message: 'invalid resource' })
    await expect(getData('/resource')).rejects.toBeInstanceOf(ApiBusinessError)
    await expect(sendData('post', '/resource')).rejects.toThrow(
      'invalid resource'
    )
    expect(
      apiErrorMessage(new ApiBusinessError(10001, 'invalid resource'))
    ).toBe('invalid resource')
  })
  it('rejects malformed envelopes before they reach resource consumers', async () => {
    for (const value of [
      '<html>proxy</html>',
      null,
      [],
      { code: '0', message: '' },
      { code: 0, message: {} },
    ]) {
      respond(value)
      await expect(getData('/resource')).rejects.toBeInstanceOf(
        ApiProtocolError
      )
    }
  })
  it('leaves explicit binary downloads intact', async () => {
    const blob = new Blob(['zip data'])
    respond(blob)
    expect(
      (await api.get('/certificate/download', { responseType: 'blob' })).data
    ).toBe(blob)
  })
})

describe('normalizeApiPath', () => {
  it('removes trailing slashes from resource routes', () => {
    expect(normalizeApiPath('/overview/')).toBe('/overview')
    expect(normalizeApiPath('/dns-zones///')).toBe('/dns-zones')
  })

  it('preserves query strings, hashes, and the API root', () => {
    expect(normalizeApiPath('/tasks/?page=2')).toBe('/tasks?page=2')
    expect(normalizeApiPath('/nodes/#online')).toBe('/nodes#online')
    expect(normalizeApiPath('/')).toBe('/')
  })
})

describe('revision conflicts', () => {
  it('treats 409 and stale If-Match 412 as conflicts with a shared message', async () => {
    for (const status of [409, 412]) {
      api.defaults.adapter = async (config) =>
        Promise.reject(
          Object.assign(new Error('conflict'), {
            isAxiosError: true,
            config,
            response: { status, data: {}, headers: {}, config, statusText: '' },
          })
        )
      const error = await getData('/websites').catch((reason) => reason)
      expect(isRevisionConflict(error)).toBe(true)
      expect(apiErrorMessage(error)).toBe(
        '数据已被其他人更新，请重新加载后重试'
      )
    }
    expect(isRevisionConflict(new Error('网络错误'))).toBe(false)
  })
})
