import { RE2JS } from 're2js'

export type RouteCondition = {
  source: 'header' | 'query'
  name: string
  op: 'equals' | 'not_equals' | 'exists' | 'absent'
  value: string
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
