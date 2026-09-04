import { describe, expect, it } from 'vitest'
import { normalizeApiPath } from './api'

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
