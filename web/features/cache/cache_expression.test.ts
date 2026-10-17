import { describe, expect, it } from 'vitest'
import {
  cacheConditionExpression,
  cacheRuleExpression,
  readCacheCondition,
} from './cache_expression'
import { newCacheRule } from './types'

describe('cache rule expressions', () => {
  it('keeps quoted values and regex escapes while switching editor modes', () => {
    for (const condition of [
      {
        field: 'http.request.uri.path',
        operator: 'matches',
        value: '^/a\\.js$',
        lower: false,
      },
      {
        field: 'http.cookie',
        operator: 'contains',
        value: 'session="active"',
        lower: false,
      },
      {
        field: 'http.host',
        operator: 'in',
        value: 'a.example.com, b.example.com',
        lower: true,
      },
      {
        field: 'http.request.uri.path',
        operator: 'starts_with',
        value: '/assets/',
        lower: false,
      },
    ])
      expect(readCacheCondition(cacheConditionExpression(condition))).toEqual(
        condition
      )
  })
  it('keeps combined expressions in expression mode', () => {
    expect(
      readCacheCondition(
        'http.host eq "a.example.com" or http.host eq "b.example.com"'
      )
    ).toBeUndefined()
    expect(
      readCacheCondition('http.request.headers["x-preview"][0] eq "1"')
    ).toBeUndefined()
  })
  it('preserves legacy extension and exact-path case semantics in the preview', () => {
    const rule = {
      ...newCacheRule(),
      name: 'static',
      patterns: ['.JS', '.css'],
    }
    expect(cacheRuleExpression(rule)).toBe(
      'lower(http.request.uri.path.extension) in {"js" "css"}'
    )
    expect(
      cacheRuleExpression({
        ...rule,
        match_type: 'exact',
        patterns: ['/API.js'],
      })
    ).toBe('http.request.uri.path in {"/API.js"}')
  })
})
