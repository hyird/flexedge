import { describe, expect, it } from 'vitest'
import {
  conflictingRouteIndexes,
  previewRoute,
  type PreviewRule,
} from './route-preview'

const rule = (patch: Partial<PreviewRule> = {}): PreviewRule => ({
  status: 'enabled',
  match_type: 'prefix',
  path: '/',
  methods: [],
  action: 'proxy',
  rewrite_path: '',
  redirect_url: '',
  origin_group: 'default',
  ...patch,
})

describe('route preview matches node semantics', () => {
  it('filters by hostname without changing path precedence', () => {
    const rules = [
      rule({ path: '/api', hostnames: ['API.Example.com'] }),
      rule(),
    ]
    expect(
      previewRoute(rules, 'GET', '/api/users', 'api.example.com:443').index
    ).toBe(0)
    expect(
      previewRoute(rules, 'GET', '/api/users', 'www.example.com').index
    ).toBe(1)
    expect(previewRoute(rules, 'GET', '/apix', 'api.example.com').index).toBe(1)
    expect(
      conflictingRouteIndexes(
        [
          rule({ hostnames: ['a.example.com'] }),
          rule({ hostnames: ['b.example.com'] }),
        ],
        1
      )
    ).toEqual([])
  })
  it('strips and replaces prefixes without dropping suffixes', () => {
    const r = rule({ path: '/api', rewrite_mode: 'strip_prefix' })
    expect(previewRoute([r], 'GET', '/api/users?a=1').target).toBe('/users?a=1')
    expect(previewRoute([r], 'GET', '/api').target).toBe('/')
    expect(
      previewRoute([{ ...r, path: '/api/' }], 'GET', '/api/users').target
    ).toBe('/users')
    expect(
      previewRoute(
        [{ ...r, rewrite_mode: 'replace_prefix', rewrite_path: '/v2/' }],
        'GET',
        '/api/users?a=1'
      ).target
    ).toBe('/v2/users?a=1')
  })
  it('applies explicit query policies and keeps redirect fragments last', () => {
    const r = rule({
      path: '/api',
      rewrite_mode: 'replace_prefix',
      rewrite_path: '/v2',
      query_mode: 'drop',
    })
    expect(previewRoute([r], 'GET', '/api/users?a=1').target).toBe('/v2/users')
    expect(
      previewRoute(
        [{ ...r, query_mode: 'replace', query_string: 'b=2' }],
        'GET',
        '/api/users?a=1'
      ).target
    ).toBe('/v2/users?b=2')
    const redirect = rule({
      action: 'redirect',
      redirect_url: '/new?target=1#part',
      query_mode: 'replace',
      query_string: 'v=2',
    })
    expect(previewRoute([redirect], 'GET', '/old?a=1').target).toBe(
      '/new?v=2#part'
    )
    expect(
      previewRoute([{ ...redirect, query_mode: 'preserve' }], 'GET', '/old?a=1')
        .target
    ).toBe('/new?target=1#part')
    expect(
      previewRoute([{ ...redirect, query_mode: 'drop' }], 'GET', '/old?a=1')
        .target
    ).toBe('/new#part')
  })
  it('warns only about earlier enabled equal-path method overlaps', () => {
    const rules = [
      rule({ methods: ['GET'] }),
      rule({ methods: ['POST'] }),
      rule(),
    ]
    expect(conflictingRouteIndexes(rules, 1)).toEqual([])
    expect(conflictingRouteIndexes(rules, 2)).toEqual([0, 1])
    expect(
      conflictingRouteIndexes([rule({ status: 'disabled' }), rule()], 1)
    ).toEqual([])
    expect(
      conflictingRouteIndexes([rule(), rule({ match_type: 'exact' })], 1)
    ).toEqual([])
  })
  it('prioritizes exact and longest matches, preserving first ties', () => {
    const rules = [
      rule(),
      rule({ path: '/api' }),
      rule({ path: '/api', match_type: 'exact' }),
      rule({ path: '/api', match_type: 'exact' }),
    ]
    expect(previewRoute(rules, 'GET', '/api?q=1').index).toBe(2)
    expect(previewRoute(rules, 'GET', '/api/users').index).toBe(1)
    expect(previewRoute(rules, 'GET', '/apix').index).toBe(0)
  })
  it('skips disabled and method mismatches', () => {
    expect(
      previewRoute(
        [rule({ status: 'disabled' }), rule({ methods: ['POST'] })],
        'GET',
        '/'
      ).index
    ).toBe(-1)
  })
  it('replaces the whole path and preserves query strings', () => {
    expect(
      previewRoute([rule({ rewrite_path: '/v2/' })], 'GET', '/api/users?a=1')
        .target
    ).toBe('/v2/?a=1')
    expect(
      previewRoute(
        [rule({ action: 'redirect', redirect_url: '/new' })],
        'GET',
        '/old?a=1'
      ).target
    ).toBe('/new?a=1')
    expect(
      previewRoute(
        [rule({ action: 'redirect', redirect_url: '/new?b=2' })],
        'GET',
        '/old?a=1'
      ).target
    ).toBe('/new?b=2')
  })
})
