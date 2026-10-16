import type { WebsiteRouteRuleForm } from './website-form'

type ConflictRule = Pick<
  WebsiteRouteRuleForm,
  | 'status'
  | 'action'
  | 'match_type'
  | 'path'
  | 'methods'
  | 'hostnames'
  | 'conditions'
>

function normalizeRouteHostname(value: string) {
  return value.trim().split(':')[0].replace(/\.$/, '').toLowerCase()
}

function hosts(rule: ConflictRule) {
  return (rule.hostnames ?? []).map(normalizeRouteHostname).filter(Boolean)
}

export function conflictingRouteIndexes(rules: ConflictRule[], index: number) {
  const rule = rules[index]
  if (!rule || rule.status !== 'enabled' || rule.action !== 'redirect')
    return []
  return rules.flatMap((previous, candidate) => {
    if (
      candidate >= index ||
      previous.action !== 'redirect' ||
      previous.status !== 'enabled' ||
      previous.match_type !== rule.match_type ||
      previous.path !== rule.path
    )
      return []
    const overlaps =
      !previous.methods.length ||
      !rule.methods.length ||
      rule.methods.some((method) => previous.methods.includes(method))
    const previousHosts = hosts(previous)
    const currentHosts = hosts(rule)
    const hostOverlap =
      !previousHosts.length ||
      !currentHosts.length ||
      currentHosts.some((host) => previousHosts.includes(host))
    // Only warn when the earlier conditions are a subset of the current ones.
    // Other condition combinations may overlap, but are not proven shadowed.
    const conditionOverlap = (previous.conditions ?? []).every((condition) =>
      (rule.conditions ?? []).some(
        (current) => JSON.stringify(current) === JSON.stringify(condition)
      )
    )
    return overlaps && hostOverlap && conditionOverlap ? [candidate] : []
  })
}
