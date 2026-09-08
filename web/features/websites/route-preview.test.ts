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
  it('filters by hostname while preserving rule order', () => {
    const rules = [
      rule({ path: '/api', hostnames: ['API.Example.com'] }),
      rule(),
    ]
    expect(
      previewRoute(rules, 'GET', '/api/users', 'api.example.com:443').origins
    ).toEqual([0, 1])
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
      rule({ action: 'redirect', methods: ['GET'] }),
      rule({ action: 'redirect', methods: ['POST'] }),
      rule({ action: 'redirect' }),
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
  it('uses the last origin match regardless of type and respects reordering', () => {
    const rules = [
      rule(),
      rule({ path: '/api' }),
      rule({ path: '/api', match_type: 'exact' }),
      rule({ path: '/api', match_type: 'exact' }),
    ]
    expect(previewRoute(rules, 'GET', '/api?q=1').index).toBe(3)
    expect(previewRoute(rules, 'GET', '/api/users').index).toBe(1)
    expect(previewRoute(rules, 'GET', '/apix').index).toBe(0)
    expect(previewRoute([rules[2], rules[0]], 'GET', '/api').index).toBe(1)
    expect(previewRoute([rules[2], rules[0]], 'GET', '/api/users').index).toBe(
      1
    )
    expect(
      previewRoute(
        [rules[0], rule({ match_type: 'regex', path: '[' })],
        'GET',
        '/api'
      ).error
    ).toBeDefined()
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

describe('phase composition', () => {
  it('executes redirects before earlier proxy and rewrite entries', () => {
    const result = previewRoute(
      [
        rule(),
        rule({ action: 'rewrite', rewrite_path: '/api' }),
        rule({
          action: 'redirect',
          redirect_url: '/login',
          query_mode: 'drop',
        }),
        rule({ action: 'redirect', redirect_url: '/later' }),
      ],
      'GET',
      '/old?q=1'
    )
    expect(result.index).toBe(2)
    expect(result.target).toBe('/login')
  })
  it('matches immutable rewrite input and routes on the final path and query', () => {
    const rules = [
      rule({
        action: 'rewrite',
        path: '/old',
        query_mode: 'replace',
        query_string: 'v=2',
      }),
      rule({
        action: 'rewrite',
        path: '/old',
        rewrite_mode: 'replace_prefix',
        rewrite_path: '/api',
      }),
      rule({ action: 'rewrite', path: '/api', rewrite_path: '/wrong' }),
      rule({
        action: 'rewrite',
        path: '/old',
        rewrite_mode: 'replace_prefix',
        rewrite_path: '/final',
      }),
      rule({
        path: '/final',
        conditions: [{ source: 'query', name: 'v', op: 'equals', value: '2' }],
      }),
    ]
    const result = previewRoute(rules, 'GET', '/old/item?v=1')
    expect(result.target).toBe('/final/item?v=2')
    expect(result.rewrites).toEqual([0, 1, 3])
    expect(result.origins).toEqual([4])
    expect(previewRoute(rules.slice(0, 4), 'GET', '/old/item?v=1').index).toBe(
      -1
    )
  })
  it('does not run redirects again after rewriting and skips disabled rules', () => {
    const result = previewRoute(
      [
        rule({ action: 'rewrite', path: '/old', rewrite_path: '/api' }),
        rule({ action: 'redirect', path: '/api', redirect_url: '/wrong' }),
        rule({ action: 'rewrite', status: 'disabled', rewrite_path: '/wrong' }),
      ],
      'GET',
      '/old'
    )
    expect(result.target).toBe('/api')
    expect(result.index).toBe(-1)
  })
})
