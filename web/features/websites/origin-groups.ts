import type { WebsiteOrigin } from './types'

export type OriginGroup = {
  name: string
  indexes: number[]
  enabledCount: number
  enabledPrimaryCount: number
}

type RouteReference = { action: string; origin_group: string }
type OriginGroupState<T extends RouteReference> = {
  origins: WebsiteOrigin[]
  default_origin_group: string
  route_rules: T[]
}

export function collectOriginGroups(
  origins: readonly WebsiteOrigin[]
): OriginGroup[] {
  const groups = new Map<string, OriginGroup>()
  origins.forEach((origin, index) => {
    const name = origin.group.trim()
    if (!name) return
    const group = groups.get(name) ?? {
      name,
      indexes: [],
      enabledCount: 0,
      enabledPrimaryCount: 0,
    }
    group.indexes.push(index)
    if (origin.status === 'enabled') {
      group.enabledCount += 1
      if (origin.role === 'primary') group.enabledPrimaryCount += 1
    }
    groups.set(name, group)
  })
  return [...groups.values()]
}

export function nextOriginGroupName(groups: readonly OriginGroup[]) {
  let suffix = groups.length + 1
  while (groups.some((group) => group.name === `源站组 ${suffix}`)) suffix += 1
  return `源站组 ${suffix}`
}

export function renameOriginGroup<T extends RouteReference>(
  state: OriginGroupState<T>,
  currentName: string,
  draftName: string
): { ok: true; value: OriginGroupState<T> } | { ok: false; message: string } {
  const nextName = draftName.trim()
  if (nextName === currentName) return { ok: true, value: state }
  const hasControlCharacter = Array.from(nextName).some((character) => {
    const code = character.charCodeAt(0)
    return code < 32 || code === 127
  })
  if (!nextName || nextName.length > 100 || hasControlCharacter) {
    return { ok: false, message: '源站组名称需为 1–100 个非控制字符' }
  }
  if (state.origins.some((origin) => origin.group.trim() === nextName)) {
    return { ok: false, message: '源站组名称不能重复' }
  }
  return {
    ok: true,
    value: {
      origins: state.origins.map((origin) =>
        origin.group.trim() === currentName
          ? { ...origin, group: nextName }
          : origin
      ),
      default_origin_group:
        state.default_origin_group.trim() === currentName
          ? nextName
          : state.default_origin_group,
      route_rules: state.route_rules.map((rule) =>
        rule.origin_group === currentName
          ? { ...rule, origin_group: nextName }
          : rule
      ),
    },
  }
}

export function originRemovalError<T extends RouteReference>(
  state: OriginGroupState<T>,
  index: number
): string | undefined {
  const origin = state.origins[index]
  if (!origin) return '源站不存在，请刷新后重试'
  const groupName = origin.group.trim()
  const removesGroup = !state.origins.some(
    (item, itemIndex) => itemIndex !== index && item.group.trim() === groupName
  )
  if (!removesGroup) return
  if (state.default_origin_group.trim() === groupName)
    return '请先在“基础”页切换默认源站组，再删除该组'
  if (
    state.route_rules.some(
      (rule) => rule.action === 'proxy' && rule.origin_group === groupName
    )
  )
    return '该源站组仍被代理路由引用，请先修改路由规则'
}
