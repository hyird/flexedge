import {
  compileRoutePattern,
  expandRouteCaptures,
  queryMatchValues,
  routeConditionMatches,
  type RouteCondition,
} from './route-matching'

export type PreviewRule = {
  name?: string
  hostnames?: string[]
  conditions?: RouteCondition[]
  rewrite_mode?: string
  query_mode?: string
  query_string?: string
  status: string
  match_type: string
  path: string
  methods: string[]
  action: string
  rewrite_path: string
  redirect_url: string
  origin_group: string
}

export function normalizeRouteHostname(value: string) {
  return value.trim().split(':')[0].replace(/\.$/, '').toLowerCase()
}

function hosts(rule: PreviewRule) {
  return (rule.hostnames ?? []).map(normalizeRouteHostname).filter(Boolean)
}

export function rewriteRoutePath(path: string, rule: PreviewRule) {
  const mode =
    rule.rewrite_mode || (rule.rewrite_path ? 'replace_path' : 'none')
  if (mode === 'none') return path
  if (mode === 'replace_path')
    return rule.match_type === 'regex'
      ? expandRouteCaptures(
          rule.rewrite_path,
          path,
          compileRoutePattern(rule.path)
        )
      : rule.rewrite_path
  if (!path.startsWith(rule.path)) return path
  let suffix = path.slice(rule.path.length)
  let prefix = mode === 'strip_prefix' ? '/' : rule.rewrite_path
  if (!suffix) return prefix || '/'
  if (prefix.endsWith('/') && suffix.startsWith('/')) suffix = suffix.slice(1)
  else if (!prefix.endsWith('/') && !suffix.startsWith('/')) prefix += '/'
  return prefix + suffix
}

export function applyRouteQuery(
  destination: string,
  target: string,
  rule: PreviewRule
) {
  const fragmentAt = destination.indexOf('#')
  const fragment = fragmentAt < 0 ? '' : destination.slice(fragmentAt)
  let base = fragmentAt < 0 ? destination : destination.slice(0, fragmentAt)
  if (rule.query_mode === 'drop' || rule.query_mode === 'replace') {
    base = base.split('?')[0]
    if (rule.query_mode === 'replace' && rule.query_string)
      base += '?' + rule.query_string
  } else if (!base.includes('?') && target.includes('?'))
    base += target.slice(target.indexOf('?'))
  return base + fragment
}

export function conflictingRouteIndexes(rules: PreviewRule[], index: number) {
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

// Match and evaluate one terminating rule against a phase snapshot.
function previewSingleRoute(
  rules: PreviewRule[],
  method: string,
  target: string,
  host = '',
  headers: Array<[string, string]> = [['host', host]]
): { index: number; target: string; error?: string } {
  const queryIndex = target.indexOf('?')
  const path = queryIndex < 0 ? target : target.slice(0, queryIndex)
  let index = -1
  let error: string | undefined
  const query = queryMatchValues(target)
  rules.forEach((rule, candidate) => {
    if (index !== -1 || error) return
    const hostnames = hosts(rule)
    if (hostnames.length && !hostnames.includes(normalizeRouteHostname(host)))
      return
    if (
      rule.status !== 'enabled' ||
      (rule.methods.length && !rule.methods.includes(method))
    )
      return
    if (
      !(rule.conditions ?? []).every((condition) =>
        routeConditionMatches(condition, headers, query)
      )
    )
      return
    if (rule.match_type === 'regex') {
      try {
        compileRoutePattern(rule.path)
      } catch {
        error = `规则 ${candidate + 1} 的正则不正确`
        return
      }
    }
    const matches =
      rule.match_type === 'exact'
        ? path === rule.path
        : rule.match_type === 'suffix'
          ? path.endsWith(rule.path)
          : rule.match_type === 'regex'
            ? compileRoutePattern(rule.path).matcher(path).find()
            : path.startsWith(rule.path) &&
              (rule.path === '/' ||
                rule.path.endsWith('/') ||
                path.length === rule.path.length ||
                path[rule.path.length] === '/')
    if (!matches) return
    index = candidate
  })
  const rule = rules[index]
  if (error) return { index: -1, target, error }
  if (!rule) return { index, target }
  try {
    const destination =
      rule.action === 'redirect'
        ? rule.match_type === 'regex'
          ? expandRouteCaptures(
              rule.redirect_url,
              path,
              compileRoutePattern(rule.path)
            )
          : rule.redirect_url
        : rewriteRoutePath(path, rule)
    return { index, target: applyRouteQuery(destination, target, rule) }
  } catch (failure) {
    return {
      index: -1,
      target,
      error: failure instanceof Error ? failure.message : '规则替换失败',
    }
  }
}

// Each phase uses immutable input; later settings override independently by field.
export function previewRoute(
  rules: PreviewRule[],
  method: string,
  target: string,
  host = '',
  headers: Array<[string, string]> = [['host', host]]
): {
  index: number
  target: string
  error?: string
  rewrites?: number[]
  origins?: number[]
} {
  const rewrites: number[] = []
  const origins: number[] = []
  for (const [index, rule] of rules.entries()) {
    if (rule.action !== 'redirect') continue
    const result = previewSingleRoute([rule], method, target, host, headers)
    if (result.error)
      return {
        ...result,
        index: -1,
        error: result.error.replace('规则 1 ', `规则 ${index + 1} `),
      }
    if (result.index === 0)
      return { index, target: result.target, rewrites, origins }
  }
  let rewritten = target
  for (const [index, rule] of rules.entries()) {
    if (rule.action === 'redirect') continue
    const result = previewSingleRoute([rule], method, target, host, headers)
    if (result.error)
      return {
        ...result,
        index: -1,
        error: result.error.replace('规则 1 ', `规则 ${index + 1} `),
      }
    if (result.index < 0) continue
    const mode =
      rule.rewrite_mode || (rule.rewrite_path ? 'replace_path' : 'none')
    if (mode !== 'none') {
      const queryAt = rewritten.indexOf('?')
      rewritten =
        result.target.split('?')[0] +
        (queryAt < 0 ? '' : rewritten.slice(queryAt))
    }
    if (rule.query_mode === 'drop' || rule.query_mode === 'replace')
      rewritten = applyRouteQuery(rewritten, target, rule)
    if (
      mode !== 'none' ||
      rule.query_mode === 'drop' ||
      rule.query_mode === 'replace'
    )
      rewrites.push(index)
  }
  for (const [index, rule] of rules.entries()) {
    if (rule.action !== 'proxy') continue
    const result = previewSingleRoute(
      [
        {
          ...rule,
          rewrite_mode: 'none',
          rewrite_path: '',
          query_mode: 'preserve',
          query_string: '',
        },
      ],
      method,
      rewritten,
      host,
      headers
    )
    if (result.error)
      return {
        ...result,
        index: -1,
        error: result.error.replace('规则 1 ', `规则 ${index + 1} `),
      }
    if (result.index === 0) origins.push(index)
  }
  return { index: origins.at(-1) ?? -1, target: rewritten, rewrites, origins }
}
