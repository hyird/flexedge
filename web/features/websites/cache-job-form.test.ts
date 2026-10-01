import { describe, expect, it } from 'vitest'
import { parseCacheJobInput, type CacheJobFormValues } from './cache-job-form'
import { defaultWebsiteConfig } from './website-form'

const website = {
  config: {
    ...defaultWebsiteConfig(),
    https_enabled: true,
    domains: [
      { id: 'domain', hostname: 'example.com', dns_mode: 'external' as const },
    ],
  },
}
const preheat: CacheJobFormValues = {
  operation: 'preheat',
  mode: 'url',
  targets: 'https://example.com/a',
  concurrency: '2',
  rate_mib: '5',
}

describe('cache job form submission', () => {
  it('submits purge using defaults after invalid preheat scheduling input', () => {
    const invalid = { ...preheat, concurrency: '0', rate_mib: '' }
    expect(parseCacheJobInput(website, invalid).ok).toBe(false)
    expect(
      parseCacheJobInput(website, { ...invalid, operation: 'purge' })
    ).toEqual({
      ok: true,
      input: {
        operation: 'purge',
        mode: 'url',
        targets: ['https://example.com/a'],
        concurrency: 2,
        rate_limit_bps: 5242880,
      },
    })
    expect(
      parseCacheJobInput(website, {
        ...invalid,
        operation: 'purge',
        mode: 'all',
        targets: 'not a URL',
      })
    ).toEqual({
      ok: true,
      input: {
        operation: 'purge',
        mode: 'all',
        targets: [],
        concurrency: 2,
        rate_limit_bps: 5242880,
      },
    })
  })
  it('restores URL mode on preheat and converts valid scheduling limits to bytes', () => {
    expect(
      parseCacheJobInput(website, {
        ...preheat,
        mode: 'all',
        concurrency: '4',
        rate_mib: '100',
      })
    ).toEqual({
      ok: true,
      input: {
        operation: 'preheat',
        mode: 'url',
        targets: ['https://example.com/a'],
        concurrency: 4,
        rate_limit_bps: 104857600,
      },
    })
    for (const patch of [
      { concurrency: '0' },
      { concurrency: '5' },
      { concurrency: '1.5' },
      { rate_mib: '0' },
      { rate_mib: '101' },
      { rate_mib: '1.5' },
    ])
      expect(parseCacheJobInput(website, { ...preheat, ...patch }).ok).toBe(
        false
      )
  })
  it('validates the visible target scope and website protocol', () => {
    for (const targets of [
      '',
      'https://other.example.com/a',
      'https://example.com/a\nhttps://example.com/a',
      'https://example.com:8443/a',
      'https://user@example.com/a',
      'https://example.com/a#fragment',
    ])
      expect(parseCacheJobInput(website, { ...preheat, targets }).ok).toBe(
        false
      )
    expect(
      parseCacheJobInput(website, {
        ...preheat,
        operation: 'purge',
        mode: 'prefix',
        targets: 'https://example.com/a\nhttps://example.com/b',
      }).ok
    ).toBe(false)
    const httpOnly = { config: { ...website.config, https_enabled: false } }
    expect(parseCacheJobInput(httpOnly, preheat).ok).toBe(false)
    expect(
      parseCacheJobInput(httpOnly, {
        ...preheat,
        targets: 'http://example.com/a',
      }).ok
    ).toBe(true)
  })
})
