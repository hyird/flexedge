import { describe, expect, it } from 'vitest'
import {
  composeHostedDomain,
  findHostedZone,
  hostedPrefixError,
  isWebsiteHostname,
  splitHostedDomain,
} from './hosted-domain'
import { defaultWebsiteConfig, validateWebsiteForm } from './website-form'
import { collectWebsiteFormErrors } from './website-sections'

const zones = [
  { id: 'root', domain: 'example.com' },
  { id: 'cdn', domain: 'cdn.example.com' },
  { id: 'other', domain: 'example.net' },
]

describe('hosted domains', () => {
  it('finds the longest owning zone, ignoring case, wildcards and trailing dots', () => {
    expect(findHostedZone('example.com', zones)?.id).toBe('root')
    expect(findHostedZone('WWW.Example.COM.', zones)?.id).toBe('root')
    expect(findHostedZone('x.cdn.example.com', zones)?.id).toBe('cdn')
    expect(findHostedZone('cdn.example.com', zones)?.id).toBe('cdn')
    expect(findHostedZone('*.cdn.example.com', zones)?.id).toBe('cdn')
    expect(findHostedZone('*.example.com', zones)?.id).toBe('root')
    expect(findHostedZone('badexample.com', zones)).toBeUndefined()
    expect(findHostedZone('example.org', zones)).toBeUndefined()
    expect(findHostedZone('', zones)).toBeUndefined()
  })

  it('splits hostnames into zone and prefix', () => {
    expect(splitHostedDomain('example.com', zones)).toEqual({
      zone: zones[0],
      prefix: '',
    })
    expect(splitHostedDomain('www.example.com', zones)?.prefix).toBe('www')
    expect(splitHostedDomain('img.x.cdn.example.com', zones)).toEqual({
      zone: zones[1],
      prefix: 'img.x',
    })
    expect(splitHostedDomain('*.example.net', zones)?.prefix).toBe('*')
    expect(splitHostedDomain('*.img.example.com', zones)?.prefix).toBe('*.img')
    expect(splitHostedDomain('www', zones)).toBeUndefined()
  })

  it('composes the apex for an empty prefix', () => {
    expect(composeHostedDomain('', 'example.com')).toBe('example.com')
    expect(composeHostedDomain('  www ', 'example.com')).toBe('www.example.com')
    expect(composeHostedDomain('*.img', 'example.com')).toBe(
      '*.img.example.com'
    )
    expect(composeHostedDomain('www', '')).toBe('www')
  })

  it('re-splits a pasted full hostname by its longest zone', () => {
    expect(splitHostedDomain(' Shop.CDN.Example.com. ', zones)).toEqual({
      zone: zones[1],
      prefix: 'shop',
    })
    expect(splitHostedDomain('example.net', zones)).toEqual({
      zone: zones[2],
      prefix: '',
    })
    expect(splitHostedDomain('img.cdn', zones)).toBeUndefined()
  })

  it('validates host prefixes', () => {
    for (const prefix of ['', 'www', '*', '*.img', 'a-b.c1', 'x'.repeat(63)])
      expect(hostedPrefixError(prefix)).toBeUndefined()
    expect(hostedPrefixError('.www')).toBe('主机前缀不能以点开头或结尾')
    expect(hostedPrefixError('www.')).toBe('主机前缀不能以点开头或结尾')
    expect(hostedPrefixError('a..b')).toBe('主机前缀不能包含连续的点')
    expect(hostedPrefixError('a.*')).toMatch(/通配符/)
    expect(hostedPrefixError('w*w')).toMatch(/通配符/)
    expect(hostedPrefixError('x'.repeat(64))).toMatch(/63/)
    expect(hostedPrefixError('-www')).toMatch(/连字符/)
    expect(hostedPrefixError('w_w')).toMatch(/字母、数字和连字符/)
  })

  it('mirrors the backend hostname rule', () => {
    for (const hostname of ['example.com', '*.example.com', 'a.b-c.io'])
      expect(isWebsiteHostname(hostname)).toBe(true)
    for (const hostname of [
      'localhost',
      'example.c0m',
      'a..com',
      '-a.com',
      'a.*.com',
      'example.com.',
      `${'a.'.repeat(126)}com`,
    ])
      expect(isWebsiteHostname(hostname)).toBe(false)
  })
})

type DomainInput = { hostname: string; dns_mode: 'managed' | 'external' }

function websiteWith(domains: DomainInput[]) {
  return {
    ...defaultWebsiteConfig(),
    name: 'test',
    status: 'enabled',
    cluster_id: '12345678-1234-4234-8234-123456789012',
    domains: domains.map((domain, index) => ({
      id: `12345678-1234-4234-8234-12345678901${index}`,
      ...domain,
    })),
    origins: [
      {
        id: '12345678-1234-4234-8234-123456789020',
        group: 'default',
        protocol: 'http',
        host: 'origin.example.org',
        port: 80,
        role: 'primary',
        weight: 1,
        status: 'enabled',
      },
    ],
  }
}

function domainErrors(domains: DomainInput[], hostedZones?: typeof zones) {
  const result = validateWebsiteForm(websiteWith(domains), hostedZones)
  return result.success ? {} : collectWebsiteFormErrors(result.error.issues)
}

describe('website domain validation', () => {
  it('accepts managed apex and subdomains inside hosted zones', () => {
    expect(
      domainErrors(
        [
          { hostname: 'example.com', dns_mode: 'managed' },
          { hostname: 'www.example.com', dns_mode: 'managed' },
          { hostname: '*.cdn.example.com', dns_mode: 'managed' },
        ],
        zones
      )
    ).toEqual({})
  })

  it('asks for a hosted zone when a managed hostname is outside every zone', () => {
    expect(
      domainErrors(
        [
          { hostname: '', dns_mode: 'managed' },
          { hostname: 'www.example.org', dns_mode: 'managed' },
          { hostname: 'www.example.org', dns_mode: 'external' },
        ],
        zones
      )
    ).toEqual({
      'domains.0.zone': '请选择托管域名',
      'domains.1.zone': '请选择托管域名',
    })
  })

  it('reports an invalid prefix in place of the hostname format error', () => {
    expect(
      domainErrors(
        [{ hostname: 'a..b.example.com', dns_mode: 'managed' }],
        zones
      )
    ).toEqual({ 'domains.0.hostname': '主机前缀不能包含连续的点' })
  })

  it('checks only the hostname format without the zone list', () => {
    expect(
      domainErrors([
        { hostname: 'www.example.org', dns_mode: 'managed' },
        { hostname: 'bad_host.example.com', dns_mode: 'external' },
      ])
    ).toEqual({ 'domains.1.hostname': '域名格式不正确' })
  })
})
