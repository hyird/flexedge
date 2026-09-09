import { afterEach, describe, expect, it } from 'vitest'
import {
  api,
  apiErrorMessage,
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
  function respond(data: unknown) {
    api.defaults.adapter = async (config) => ({
      config,
      status: 200,
      statusText: 'OK',
      headers: {},
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
