import { describe, expect, it } from 'vitest'
import {
  collectOriginGroups,
  nextOriginGroupName,
  originRemovalError,
  renameOriginGroup,
} from './origin-groups'
import type { WebsiteOrigin } from './types'

function origin(
  group: string,
  role: WebsiteOrigin['role'] = 'primary',
  status: WebsiteOrigin['status'] = 'enabled'
): WebsiteOrigin {
  return {
    id: crypto.randomUUID(),
    group,
    role,
    status,
    protocol: 'http',
    host: 'origin.test',
    port: 80,
    weight: 100,
  }
}

describe('origin group operations', () => {
  it('groups normalized names while preserving row indexes and enabled roles', () => {
    expect(
      collectOriginGroups([
        origin(' east '),
        origin('east', 'backup'),
        origin('west', 'primary', 'disabled'),
        origin(''),
      ])
    ).toEqual([
      {
        name: 'east',
        indexes: [0, 1],
        enabledCount: 2,
        enabledPrimaryCount: 1,
      },
      { name: 'west', indexes: [2], enabledCount: 0, enabledPrimaryCount: 0 },
    ])
  })

  it('renames all references without mutating input or losing route metadata', () => {
    const state = {
      origins: [origin(' east '), origin('west')],
      default_origin_group: 'east',
      route_rules: [
        { action: 'proxy', origin_group: 'east', id: 'route', enabled: false },
      ],
    }
    const result = renameOriginGroup(state, 'east', ' north ')
    expect(result.ok).toBe(true)
    if (!result.ok) throw new Error(result.message)
    expect(result.value.origins[0].group).toBe('north')
    expect(result.value.origins[1]).toBe(state.origins[1])
    expect(result.value.default_origin_group).toBe('north')
    expect(result.value.route_rules[0]).toEqual({
      action: 'proxy',
      origin_group: 'north',
      id: 'route',
      enabled: false,
    })
    expect(state.origins[0].group).toBe(' east ')
    expect(state.route_rules[0].origin_group).toBe('east')
  })

  it('rejects colliding or invalid names and keeps an unchanged name a no-op', () => {
    const state = {
      origins: [origin('east'), origin('west')],
      default_origin_group: 'east',
      route_rules: [],
    }
    for (const name of [' west ', '', 'x'.repeat(101), 'bad\u0000name']) {
      expect(renameOriginGroup(state, 'east', name).ok).toBe(false)
    }
    expect(renameOriginGroup(state, 'east', ' east ')).toEqual({
      ok: true,
      value: state,
    })
  })

  it('blocks removing the last member referenced by the default or a disabled proxy rule', () => {
    const state = {
      origins: [origin('east'), origin('west')],
      default_origin_group: 'east',
      route_rules: [{ action: 'proxy', origin_group: 'west', enabled: false }],
    }
    expect(originRemovalError(state, 0)).toContain('默认源站组')
    expect(originRemovalError(state, 1)).toContain('代理路由')
    expect(
      originRemovalError(
        { ...state, origins: [...state.origins, origin('west', 'backup')] },
        1
      )
    ).toBeUndefined()
  })

  it('allows removing an unreferenced group and rejects a missing index', () => {
    const state = {
      origins: [origin('east'), origin('west')],
      default_origin_group: 'east',
      route_rules: [],
    }
    expect(originRemovalError(state, 1)).toBeUndefined()
    expect(originRemovalError(state, 2)).toContain('不存在')
  })

  it('chooses a new name without colliding with existing numbered groups', () => {
    expect(
      nextOriginGroupName(
        collectOriginGroups([origin('源站组 3'), origin('源站组 4')])
      )
    ).toBe('源站组 5')
  })
})
