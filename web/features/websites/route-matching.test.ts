import { describe, expect, it } from 'vitest'
import {
  compileRoutePattern,
  expandRouteCaptures,
  previewHeaderValues,
  queryMatchValues,
  routeConditionMatches,
  validCaptureTemplate,
  type RouteCondition,
} from './route-matching'
import {
  conflictingRouteIndexes,
  previewRoute,
  type PreviewRule,
} from './route-preview'

const rule = (patch: Partial<PreviewRule> = {}): PreviewRule => ({
  status: 'enabled',
  match_type: 'regex',
  path: '^/old/(.*)$',
  methods: [],
  action: 'redirect',
  rewrite_path: '',
  redirect_url: '/new/${1}',
  origin_group: '',
  ...patch,
})
const condition = (patch: Partial<RouteCondition> = {}): RouteCondition => ({
  source: 'query',
  name: 'tag',
  op: 'equals',
  value: 'two',
  ...patch,
})

describe('second-batch route policies', () => {
  it('uses RE2 captures, optional groups and literal dollars', () => {
    const regex = compileRoutePattern('^/old/([^/]+)/(.*)$')
    expect(
      expandRouteCaptures('/new/${2}/${1}?cash=$$', '/old/books/a%2Fb', regex)
    ).toBe('/new/a%2Fb/books?cash=$')
    expect(
      expandRouteCaptures(
        '/b/${1}',
        '/a',
        compileRoutePattern('^/a(?:/(.*))?$')
      )
    ).toBe('/b/')
    expect(validCaptureTemplate('/${3}', 2)).toBe(false)
    expect(validCaptureTemplate('/${10}', 2)).toBe(false)
    expect(validCaptureTemplate('/${broken}', 2)).toBe(false)
    expect(validCaptureTemplate('/$${1}', 0)).toBe(true)
    for (const regex of [
      '(?<=a)b',
      '(a)\\1',
      '[',
      'a'.repeat(513),
      '(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)',
    ])
      expect(() => compileRoutePattern(regex)).toThrow()
    expect(
      compileRoutePattern('(a+)+$')
        .matcher('a'.repeat(100000) + '!')
        .find()
    ).toBe(false)
  })
  it('compares decoded repeated query parameters and original headers', () => {
    const query = queryMatchValues(
      '/?tag=one&tag=two&space=a+b&escaped=%2B&flag'
    )
    const headers: Array<[string, string]> = [
      ['X-Channel', ' beta '],
      ['x-channel', 'stable'],
    ]
    expect(
      routeConditionMatches(
        condition({ source: 'header', name: 'x-channel', value: 'beta' }),
        headers,
        query
      )
    ).toBe(true)
    expect(
      routeConditionMatches(
        condition({
          source: 'header',
          name: 'x-channel',
          value: 'beta',
          op: 'not_equals',
        }),
        headers,
        query
      )
    ).toBe(false)
    expect(routeConditionMatches(condition(), headers, query)).toBe(true)
    expect(
      routeConditionMatches(
        condition({ op: 'not_equals', value: 'one' }),
        headers,
        query
      )
    ).toBe(false)
    expect(
      routeConditionMatches(
        condition({ name: 'space', value: 'a b' }),
        headers,
        query
      )
    ).toBe(true)
    expect(
      routeConditionMatches(
        condition({ name: 'escaped', value: '+' }),
        headers,
        query
      )
    ).toBe(true)
    expect(
      routeConditionMatches(
        condition({ name: 'flag', value: '' }),
        headers,
        query
      )
    ).toBe(true)
    expect(
      routeConditionMatches(
        condition({ name: 'missing', op: 'absent', value: '' }),
        headers,
        query
      )
    ).toBe(true)
    expect(
      routeConditionMatches(
        condition({ name: 'missing', op: 'not_equals' }),
        headers,
        query
      )
    ).toBe(false)
    for (const target of ['/?tag=%FF', '/?tag=%G1', '/?tag=%']) {
      expect(queryMatchValues(target)).toBeUndefined()
      expect(
        routeConditionMatches(
          condition({ name: 'missing', op: 'absent', value: '' }),
          headers,
          queryMatchValues(target)
        )
      ).toBe(false)
    }
  })
  it('applies AND conditions before precedence and keeps the first regex', () => {
    const rules = [
      rule({
        conditions: [
          condition(),
          condition({ source: 'header', name: 'x-channel', value: 'beta' }),
        ],
      }),
      rule({ path: '^/old/([^/]+)(/.*)?$' }),
      rule({ match_type: 'suffix', path: '.jpg', redirect_url: '/image' }),
      rule({ match_type: 'prefix', path: '/old/api', redirect_url: '/api' }),
      rule({
        match_type: 'exact',
        path: '/old/api/a.jpg',
        redirect_url: '/exact',
      }),
    ]
    const headers: Array<[string, string]> = [['X-Channel', 'beta']]
    expect(
      previewRoute(rules, 'GET', '/old/a?tag=two', '', headers).index
    ).toBe(0)
    expect(
      previewRoute(rules, 'GET', '/old/a?tag=one', '', headers).index
    ).toBe(1)
    expect(previewRoute(rules, 'GET', '/old/a?tag=two').index).toBe(1)
    expect(previewRoute(rules, 'GET', '/old/a.jpg').index).toBe(2)
    expect(previewRoute(rules, 'GET', '/old/api/b.jpg').index).toBe(3)
    expect(previewRoute(rules, 'GET', '/old/api/a.jpg').index).toBe(4)
  })
  it('rewrites capture paths and redirect queries without losing fragments', () => {
    expect(
      previewRoute(
        [
          rule({
            redirect_url: '/new/${1}#section',
            query_mode: 'replace',
            query_string: 'v=2',
          }),
        ],
        'POST',
        '/old/a%2Fb?tag=two'
      ).target
    ).toBe('/new/a%2Fb?v=2#section')
    expect(
      previewRoute(
        [
          rule({
            action: 'proxy',
            rewrite_mode: 'replace_path',
            rewrite_path: '/internal/${1}',
          }),
        ],
        'GET',
        '/old/a?x=1'
      ).target
    ).toBe('/internal/a?x=1')
    expect(
      previewRoute([rule({ path: '[' })], 'GET', '/old/a').error
    ).toBeTruthy()
    expect(
      previewRoute([rule({ redirect_url: '/${2}' })], 'GET', '/old/a').error
    ).toBeTruthy()
  })
  it('does not falsely warn when earlier conditions differ', () => {
    expect(
      conflictingRouteIndexes(
        [
          rule({ conditions: [condition()] }),
          rule({ conditions: [condition({ value: 'one' })] }),
        ],
        1
      )
    ).toEqual([])
    expect(
      conflictingRouteIndexes([rule(), rule({ conditions: [condition()] })], 1)
    ).toEqual([0])
    expect(
      previewHeaderValues('X-Channel: beta\nHost: ignored', 'api.example.com')
    ).toEqual([
      ['host', 'api.example.com'],
      ['X-Channel', 'beta'],
    ])
    expect(() => previewHeaderValues('broken', '')).toThrow()
  })
})
