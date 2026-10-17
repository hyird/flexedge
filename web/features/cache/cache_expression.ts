import type { CacheRule } from './types'

export const cacheExpressionFields = [
  { id: 'http.request.uri.path.extension', label: '文件扩展名' },
  { id: 'http.request.uri.path', label: 'URI 路径' },
  { id: 'http.host', label: '主机名' },
  { id: 'http.cookie', label: 'Cookie' },
  { id: 'http.user_agent', label: 'User Agent' },
  { id: 'http.request.full_uri', label: '完整 URL' },
  { id: 'http.request.uri.query', label: '查询字符串' },
  { id: 'http.referer', label: 'Referer' },
  { id: 'http.request.method', label: '请求方法' },
] as const
export const cacheExpressionOperators = [
  { id: 'in', label: '在列表中' },
  { id: 'eq', label: '等于' },
  { id: 'ne', label: '不等于' },
  { id: 'contains', label: '包含' },
  { id: 'starts_with', label: '开头为' },
  { id: 'ends_with', label: '结尾为' },
  { id: 'wildcard', label: '通配符匹配（忽略大小写）' },
  { id: 'strict wildcard', label: '通配符匹配（区分大小写）' },
  { id: 'matches', label: '正则匹配' },
] as const
export type CacheExpressionCondition = {
  field: string
  operator: string
  value: string
  lower: boolean
}
const quote = (text: string) => JSON.stringify(text)
export function cacheConditionExpression(condition: CacheExpressionCondition) {
  const field = condition.lower ? `lower(${condition.field})` : condition.field
  if (condition.operator === 'in') {
    return `${field} in {${condition.value
      .split(/[,，\r\n]+/)
      .map((value) => value.trim())
      .filter(Boolean)
      .map(quote)
      .join(' ')}}`
  }
  if (
    condition.operator === 'starts_with' ||
    condition.operator === 'ends_with'
  ) {
    return `${condition.operator}(${field}, ${quote(condition.value)})`
  }
  return `${field} ${condition.operator} ${quote(condition.value)}`
}
export function cacheRuleExpression(rule: CacheRule): string {
  if (rule.match_type === 'expression') return rule.patterns[0] ?? ''
  if (rule.match_type === 'all') return 'true'
  if (rule.match_type === 'extension') {
    return cacheConditionExpression({
      field: 'http.request.uri.path.extension',
      operator: 'in',
      value: rule.patterns
        .map((value) => value.slice(1).toLowerCase())
        .join(', '),
      lower: true,
    })
  }
  if (rule.match_type === 'exact')
    return cacheConditionExpression({
      field: 'http.request.uri.path',
      operator: 'in',
      value: rule.patterns.join(', '),
      lower: false,
    })
  return rule.patterns
    .map((pattern) => {
      if (rule.match_type === 'prefix')
        return `starts_with(http.request.uri.path, ${quote(pattern)})`
      if (rule.match_type === 'regex')
        return `http.request.uri.path matches ${quote(pattern)}`
      const regex =
        '^' +
        [...pattern]
          .map((byte) =>
            byte === '*'
              ? '.*'
              : byte === '?'
                ? '.'
                : /[\\.^$|()[\]{}+]/.test(byte)
                  ? '\\' + byte
                  : byte
          )
          .join('') +
        '$'
      return `http.request.uri.path matches ${quote(regex)}`
    })
    .map((value) => `(${value})`)
    .join(' or ')
}
export function readCacheCondition(
  expression: string
): CacheExpressionCondition | undefined {
  const token = '(?:http\\.[a-z_.]+|lower\\(http\\.[a-z_.]+\\))'
  const comparison = new RegExp(
    `^(${token}) (in|eq|ne|contains|strict wildcard|wildcard|matches) (.+)$`
  ).exec(expression)
  const fn = new RegExp(`^(starts_with|ends_with)\\((${token}), (.+)\\)$`).exec(
    expression
  )
  const field = comparison?.[1] ?? fn?.[2]
  const operator = comparison?.[2] ?? fn?.[1]
  const input = comparison?.[3] ?? fn?.[3]
  if (!field || !operator || !input) return undefined
  const lower = field.startsWith('lower(')
  const plain = lower ? field.slice(6, -1) : field
  if (!cacheExpressionFields.some((value) => value.id === plain))
    return undefined
  try {
    if (operator === 'in') {
      if (!input.startsWith('{') || !input.endsWith('}')) return undefined
      let rest = input.slice(1, -1).trim()
      const values: string[] = []
      while (rest) {
        const match = /^"(?:[^"\\]|\\["\\])*"/.exec(rest)
        if (!match) return undefined
        values.push(JSON.parse(match[0]) as string)
        rest = rest.slice(match[0].length).trim()
      }
      return { field: plain, operator, value: values.join(', '), lower }
    }
    const value: unknown = JSON.parse(input)
    return typeof value === 'string'
      ? { field: plain, operator, value, lower }
      : undefined
  } catch {
    return undefined
  }
}
