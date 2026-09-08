import { RE2JS } from 're2js'

export type RouteCondition = {
  source: 'header' | 'query'
  name: string
  op: 'equals' | 'not_equals' | 'exists' | 'absent'
  value: string
}

export const routeMatchLabels = {
  exact: '精确',
  prefix: '前缀',
  suffix: '后缀',
  regex: '正则',
}
export const routeMatchPriority: Record<string, number> = {
  exact: 4,
  prefix: 3,
  suffix: 2,
  regex: 1,
}
const patterns = new Map<string, RE2JS>()
const byteLength = (value: string) => new TextEncoder().encode(value).length
const hasControl = (value: string) =>
  Array.from(value).some(
    (char) => char.charCodeAt(0) < 32 || char.charCodeAt(0) === 127
  )

// RE2JS is a linear-time RE2 port; never evaluate user patterns with native RegExp.
export function compileRoutePattern(pattern: string) {
  if (
    !pattern ||
    byteLength(pattern) > 512 ||
    hasControl(pattern) ||
    pattern.includes('\\C')
  )
    throw new Error('正则最多512字节，不支持控制字符或 \\C')
  const cached = patterns.get(pattern)
  if (cached) return cached
  const compiled = RE2JS.compile(pattern)
  if (compiled.groupCount() > 9) throw new Error('最多9个捕获组')
  if (patterns.size >= 128) patterns.delete(patterns.keys().next().value!)
  patterns.set(pattern, compiled)
  return compiled
}

export function validCaptureTemplate(value: string, count: number) {
  for (let i = 0; i < value.length; i++) {
    if (value[i] !== '$' || i + 1 === value.length) continue
    if (value[i + 1] === '$') {
      i++
      continue
    }
    if (value[i + 1] !== '{') continue
    if (
      !/^[0-9]$/.test(value[i + 2] ?? '') ||
      value[i + 3] !== '}' ||
      Number(value[i + 2]) > count
    )
      return false
    i += 3
  }
  return true
}

export function expandRouteCaptures(
  value: string,
  path: string,
  pattern: RE2JS
) {
  if (!validCaptureTemplate(value, pattern.groupCount()))
    throw new Error('捕获组引用不存在')
  const matcher = pattern.matcher(path)
  if (!matcher.find()) throw new Error('正则未匹配请求路径')
  const result = value.replace(
    /\$\$|\$\{([0-9])\}/g,
    (token: string, group: string) =>
      token === '$$' ? '$' : (matcher.group(Number(group)) ?? '')
  )
  if (byteLength(result) > 16384 || hasControl(result) || result.includes(' '))
    throw new Error('替换结果过长或包含非法字符')
  return result
}

export function validRouteCondition(condition: RouteCondition) {
  return (
    ['header', 'query'].includes(condition.source) &&
    condition.name.length > 0 &&
    byteLength(condition.name) <= 256 &&
    !hasControl(condition.name) &&
    byteLength(condition.value) <= 2048 &&
    !hasControl(condition.value) &&
    ['equals', 'not_equals', 'exists', 'absent'].includes(condition.op) &&
    (!['exists', 'absent'].includes(condition.op) || condition.value === '') &&
    (condition.source !== 'header' ||
      /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(condition.name))
  )
}

export function queryMatchValues(
  target: string
): Array<[string, string]> | undefined {
  if (!target.includes('?')) return []
  try {
    return target
      .slice(target.indexOf('?') + 1)
      .split('&')
      .filter(Boolean)
      .map((part) => {
        const equal = part.indexOf('=')
        const key = equal < 0 ? part : part.slice(0, equal)
        const value = equal < 0 ? '' : part.slice(equal + 1)
        return [
          decodeURIComponent(key.replace(/\+/g, ' ')),
          decodeURIComponent(value.replace(/\+/g, ' ')),
        ]
      })
  } catch {
    return undefined
  }
}

export function routeConditionMatches(
  condition: RouteCondition,
  headers: Array<[string, string]>,
  query: Array<[string, string]> | undefined
) {
  if (condition.source === 'query' && !query) return false
  const values =
    condition.source === 'header'
      ? headers
          .filter(
            ([name]) => name.toLowerCase() === condition.name.toLowerCase()
          )
          .map(([, value]) => value.replace(/^[ \t]+|[ \t]+$/g, ''))
      : (query ?? [])
          .filter(([name]) => name === condition.name)
          .map(([, value]) => value)
  if (condition.op === 'exists') return values.length > 0
  if (condition.op === 'absent') return values.length === 0
  if (condition.op === 'not_equals')
    return values.length > 0 && !values.includes(condition.value)
  return values.includes(condition.value)
}

export function previewHeaderValues(
  text: string,
  host: string
): Array<[string, string]> {
  const headers: Array<[string, string]> = [['host', host]]
  for (const line of text.split('\n').filter((line) => line.trim())) {
    const colon = line.indexOf(':')
    if (colon <= 0) throw new Error('预览请求头请按每行 Header: value 填写')
    const name = line.slice(0, colon).trim()
    const value = line.slice(colon + 1).trim()
    if (!validRouteCondition({ source: 'header', name, value, op: 'equals' }))
      throw new Error('预览请求头格式不正确')
    if (name.toLowerCase() !== 'host') headers.push([name, value])
  }
  return headers
}
