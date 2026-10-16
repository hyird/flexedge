import { describe, expect, it } from 'vitest'
import { conflictingRouteIndexes } from './route-conflicts'
import { routeRuleToForm, type WebsiteRouteRuleForm } from './website-form'

const rule = (patch: Partial<WebsiteRouteRuleForm> = {}) =>
  routeRuleToForm({
    action: 'redirect',
    status: 'enabled',
    path: '/',
    ...patch,
  })

describe('route conflict warnings', () => {
  it('warns only for earlier enabled redirects with overlapping methods', () => {
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
      conflictingRouteIndexes([rule({ action: 'proxy' }), rule()], 1)
    ).toEqual([])
    expect(
      conflictingRouteIndexes([rule(), rule({ match_type: 'exact' })], 1)
    ).toEqual([])
  })

  it('compares normalized hostnames without warning for disjoint hosts', () => {
    expect(
      conflictingRouteIndexes(
        [
          rule({ hostnames: ['a.example.com'] }),
          rule({ hostnames: ['b.example.com'] }),
        ],
        1
      )
    ).toEqual([])
    expect(
      conflictingRouteIndexes(
        [
          rule({ hostnames: ['API.Example.com.'] }),
          rule({ hostnames: ['api.example.com'] }),
        ],
        1
      )
    ).toEqual([0])
  })

  it('does not warn when the earlier condition differs', () => {
    const condition = {
      source: 'query',
      name: 'tag',
      op: 'equals',
      value: 'two',
    } as const
    expect(
      conflictingRouteIndexes(
        [
          rule({ conditions: [condition] }),
          rule({ conditions: [{ ...condition, value: 'one' }] }),
        ],
        1
      )
    ).toEqual([])
    expect(
      conflictingRouteIndexes([rule(), rule({ conditions: [condition] })], 1)
    ).toEqual([0])
  })
})
