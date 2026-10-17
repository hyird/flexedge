import { z } from 'zod'

export const fields = [
  { id: 'path', label: 'URI 路径', wire: 'http.request.uri.path' },
  { id: 'ip', label: '客户端 IP', wire: 'ip.src' },
  { id: 'asn', label: 'ASN', wire: 'ip.src.asnum' },
  { id: 'country', label: '国家 / 地区', wire: 'ip.src.country' },
  { id: 'host', label: '主机名', wire: 'http.host' },
  { id: 'method', label: '请求方法', wire: 'http.request.method' },
  { id: 'user_agent', label: 'User-Agent', wire: 'http.user_agent' },
] as const
export const operators = [
  { id: 'eq', label: '等于' },
  { id: 'ne', label: '不等于' },
  { id: 'contains', label: '包含' },
  { id: 'starts_with', label: '开头是' },
  { id: 'ends_with', label: '结尾是' },
  { id: 'in', label: '属于集合' },
  { id: 'in_list', label: '属于清单' },
] as const
export const conditionSchema = z.object({
  id: z.string(),
  field: z.enum([
    'path',
    'ip',
    'asn',
    'country',
    'host',
    'method',
    'user_agent',
  ]),
  operator: z.enum([
    'eq',
    'ne',
    'contains',
    'starts_with',
    'ends_with',
    'in',
    'in_list',
  ]),
  value: z.string().max(2000),
})
export type Condition = z.infer<typeof conditionSchema>
export function operatorOptions(field: Condition['field']) {
  const supported =
    field === 'ip' || field === 'asn' || field === 'country'
      ? ['eq', 'ne', 'in', 'in_list']
      : field === 'method'
        ? ['eq', 'ne', 'in']
        : [
            'eq',
            'ne',
            'contains',
            'starts_with',
            'ends_with',
            'in',
            ...(field === 'user_agent' ? ['in_list'] : []),
          ]
  return operators.filter((item) => supported.includes(item.id))
}
export const isIp = (value: string) =>
  z.union([z.ipv4(), z.ipv6()]).safeParse(value).success
export const isNetwork = (value: string) =>
  isIp(value) || z.union([z.cidrv4(), z.cidrv6()]).safeParse(value).success
const setValues = (value: string) => value.split(/[,，\s]+/).filter(Boolean)
export function conditionExpression(condition: Condition) {
  const field = fields.find((item) => item.id === condition.field)!.wire
  const quote = (value: string) =>
    ['ip', 'asn'].includes(condition.field) ? value : JSON.stringify(value)
  if (condition.operator === 'in_list') return `${field} in $${condition.value}`
  if (condition.operator === 'in')
    return `${field} in {${setValues(condition.value).map(quote).join(' ')}}`
  if (['starts_with', 'ends_with'].includes(condition.operator))
    return `${condition.operator}(${field}, ${JSON.stringify(condition.value)})`
  return `${field} ${condition.operator} ${quote(condition.value)}`
}
export function builderExpression(
  conditions: Condition[],
  logic: 'and' | 'or'
) {
  return conditions
    .map((item) => `(${conditionExpression(item)})`)
    .join(` ${logic} `)
}
export type ExpressionNode =
  | {
      kind: 'binary'
      operator: 'and' | 'or'
      left: ExpressionNode
      right: ExpressionNode
    }
  | { kind: 'not'; child: ExpressionNode }
  | {
      kind: 'match'
      field: Condition['field']
      operator: Exclude<Condition['operator'], 'in_list'>
      values: string[]
      list?: string
    }
  | { kind: 'literal'; value: boolean }

export function parseExpression(input: string): ExpressionNode {
  if (input.length > 8000) throw new Error('表达式最多 8000 个字符')
  const tokens: string[] = []
  const scanner =
    /\s*("(?:[^"\\]|\\.)*"|[(){},]|\$[a-z][a-z0-9_]*|[^\s(){},"]+)/gy
  let offset = 0
  while (offset < input.trimEnd().length) {
    scanner.lastIndex = offset
    const found = scanner.exec(input)
    if (!found) throw new Error(`表达式第 ${offset + 1} 个字符无效`)
    tokens.push(found[1])
    offset = scanner.lastIndex
  }
  if (!tokens.length) throw new Error('请填写表达式')
  let position = 0
  const take = () => {
    const value = tokens[position++]
    if (value === undefined) throw new Error('表达式不完整')
    return value
  }
  const expect = (token: string) => {
    if (take() !== token) throw new Error(`表达式缺少 ${token}`)
  }
  const readValue = () => {
    const token = take()
    if (token.startsWith('"')) {
      try {
        return JSON.parse(token) as string
      } catch {
        throw new Error('字符串转义无效')
      }
    }
    if (/[(){}$,]/.test(token)) throw new Error('匹配值无效')
    return token
  }
  const validateMatch = (node: Extract<ExpressionNode, { kind: 'match' }>) => {
    if (
      !operatorOptions(node.field).some((item) =>
        node.list ? item.id === 'in_list' : item.id === node.operator
      )
    )
      throw new Error('该字段不支持此运算符')
    if (
      !node.list &&
      (!node.values.length || node.values.some((value) => !value))
    )
      throw new Error('请填写匹配值')
    if (
      node.field === 'ip' &&
      !node.list &&
      node.values.some((value) =>
        node.operator === 'in' ? !isNetwork(value) : !isIp(value)
      )
    )
      throw new Error('请输入有效的 IP 或 CIDR')
    if (
      node.field === 'asn' &&
      node.values.some(
        (value) =>
          !/^\d+$/.test(value) ||
          Number(value) < 1 ||
          Number(value) > 4294967295
      )
    )
      throw new Error('ASN 应填写 1–4294967295 的数字')
    return node
  }
  const atom = (): ExpressionNode => {
    if (tokens[position] === 'not') {
      take()
      return { kind: 'not', child: atom() }
    }
    if (tokens[position] === '(') {
      take()
      const child = parseOr()
      expect(')')
      return child
    }
    if (['true', 'false'].includes(tokens[position]))
      return { kind: 'literal', value: take() === 'true' }
    const first = take(),
      functionCall = ['starts_with', 'ends_with'].includes(first)
    if (functionCall) expect('(')
    const wire = functionCall ? take() : first
    const field = fields.find((item) => item.wire === wire)
    if (!field) throw new Error(`不支持的字段：${wire}`)
    if (functionCall) expect(',')
    const operator = (functionCall ? first : take()) as Exclude<
      Condition['operator'],
      'in_list'
    >
    if (
      !operators.some((item) => item.id !== 'in_list' && item.id === operator)
    )
      throw new Error(`不支持的运算符：${operator}`)
    if (operator === 'in' && tokens[position]?.startsWith('$'))
      return validateMatch({
        kind: 'match',
        field: field.id,
        operator,
        values: [],
        list: take().slice(1),
      })
    const values: string[] = []
    if (operator === 'in') {
      expect('{')
      while (tokens[position] !== '}') {
        values.push(readValue())
        if (tokens[position] === ',') take()
      }
      expect('}')
    } else values.push(readValue())
    if (functionCall) expect(')')
    return validateMatch({ kind: 'match', field: field.id, operator, values })
  }
  const parseAnd = (): ExpressionNode => {
    let node = atom()
    while (tokens[position] === 'and') {
      take()
      node = { kind: 'binary', operator: 'and', left: node, right: atom() }
    }
    return node
  }
  const parseOr = (): ExpressionNode => {
    let node = parseAnd()
    while (tokens[position] === 'or') {
      take()
      node = { kind: 'binary', operator: 'or', left: node, right: parseAnd() }
    }
    return node
  }
  const result = parseOr()
  if (position !== tokens.length)
    throw new Error(`无法解析：${tokens[position]}`)
  return result
}
export function referencedLists(node: ExpressionNode): string[] {
  if (node.kind === 'binary')
    return [
      ...new Set([
        ...referencedLists(node.left),
        ...referencedLists(node.right),
      ]),
    ]
  if (node.kind === 'not') return referencedLists(node.child)
  return node.kind === 'match' && node.list ? [node.list] : []
}
export function listReferences(
  node: ExpressionNode
): { field: Condition['field']; name: string }[] {
  if (node.kind === 'binary')
    return [...listReferences(node.left), ...listReferences(node.right)]
  if (node.kind === 'not') return listReferences(node.child)
  return node.kind === 'match' && node.list
    ? [{ field: node.field, name: node.list }]
    : []
}
export function expressionToBuilder(node: ExpressionNode): {
  conditions: Condition[]
  logic: 'and' | 'or'
} {
  const logic = node.kind === 'binary' ? node.operator : 'and'
  const conditions: Condition[] = []
  const collect = (current: ExpressionNode) => {
    if (current.kind === 'binary' && current.operator === logic) {
      collect(current.left)
      collect(current.right)
      return
    }
    if (
      current.kind !== 'match' ||
      current.values.some(
        (value) => current.operator === 'in' && /\s/.test(value)
      )
    )
      throw new Error('此表达式含分组或 NOT，请继续使用表达式编辑器')
    conditions.push({
      id: crypto.randomUUID(),
      field: current.field,
      operator: current.list ? 'in_list' : current.operator,
      value: current.list ?? current.values.join(', '),
    })
  }
  collect(node)
  if (conditions.length > 8)
    throw new Error('生成器最多支持 8 个条件，请继续使用表达式编辑器')
  return { conditions, logic }
}
