import { z } from 'zod'
import { isNetwork } from './expression_model'

export type ListMember = {
  id: string
  value: string
  note: string
  source: string
  ruleId?: string
  addedAt: number
  expiresAt: number | null
}
export type ListKind = 'ip' | 'asn' | 'country' | 'ua'
export const listKinds = [
  { id: 'ip', label: 'IP / CIDR' },
  { id: 'asn', label: 'ASN' },
  { id: 'country', label: '国家 / 地区' },
  { id: 'ua', label: 'User-Agent' },
] as const
export const fieldListKind: Record<string, ListKind> = {
  ip: 'ip',
  asn: 'asn',
  country: 'country',
  user_agent: 'ua',
}
export type SecurityList = {
  revision?: number
  kind: ListKind
  id: string
  name: string
  description: string
  members: ListMember[]
}
export const listInputSchema = z.object({
  name: z
    .string()
    .trim()
    .regex(
      /^[a-z][a-z0-9_]{0,49}$/,
      '名称使用小写字母、数字、下划线，以字母开头'
    ),
  description: z.string().trim().max(200),
})
export function normalizeAddress(value: string) {
  const address = value.trim().toLowerCase()
  if (!isNetwork(address)) throw new Error(`无效的 IP 或 CIDR：${value}`)
  const [host, prefix] = address.split('/')
  const normalizedHost = host.includes(':')
    ? new URL(`http://[${host}]/`).hostname.slice(1, -1)
    : host
  if (!prefix) return normalizedHost
  const bits = host.includes(':') ? 128 : 32,
    mask = Number(prefix)
  if (mask === bits) return normalizedHost
  // Reject non-network CIDR values instead of silently expanding their scope.
  if (host.includes(':')) {
    const groups = normalizedHost.split('::'),
      left = groups[0] ? groups[0].split(':') : [],
      right = groups[1] ? groups[1].split(':') : []
    const words =
      groups.length === 1
        ? left
        : [
            ...left,
            ...Array<string>(8 - left.length - right.length).fill('0'),
            ...right,
          ]
    const number = words.reduce(
      (total, word) => (total << 16n) | BigInt(`0x${word}`),
      0n
    )
    if ((number & ((1n << BigInt(bits - mask)) - 1n)) !== 0n)
      throw new Error(`CIDR 请填写网络地址：${value}`)
  } else {
    const number = host
      .split('.')
      .reduce((total, byte) => (total << 8n) | BigInt(byte), 0n)
    if ((number & ((1n << BigInt(bits - mask)) - 1n)) !== 0n)
      throw new Error(`CIDR 请填写网络地址：${value}`)
  }
  return `${normalizedHost}/${mask}`
}
export function parseAddresses(input: string) {
  const entries = input.split(/[\n,，;\s]+/).filter(Boolean)
  if (!entries.length) throw new Error('请填写至少一个 IP 或 CIDR')
  if (entries.length > 500) throw new Error('每次最多添加 500 项')
  return [...new Set(entries.map(normalizeAddress))]
}
export function parseListValues(kind: ListKind, input: string) {
  if (kind === 'ip') return parseAddresses(input)
  const values = input
    .split(kind === 'ua' ? /\r?\n/ : /[,，;\s]+/)
    .map((item) => item.trim())
    .filter(Boolean)
  if (!values.length) throw new Error('请填写至少一项')
  if (values.length > 500) throw new Error('每次最多添加 500 项')
  return [
    ...new Set(
      values.map((value) => {
        if (kind === 'asn') {
          const normalized = value.replace(/^AS/i, '')
          if (
            !/^\d+$/.test(normalized) ||
            Number(normalized) < 1 ||
            Number(normalized) > 4294967295
          )
            throw new Error(`无效的 ASN：${value}`)
          return String(Number(normalized))
        }
        if (kind === 'country') {
          const code = value.toUpperCase()
          const name = /^[A-Z]{2}$/.test(code)
            ? new Intl.DisplayNames(['zh-CN'], { type: 'region' }).of(code)
            : undefined
          if (
            code === 'ZZ' ||
            (!['T1', 'XX'].includes(code) && (!name || name === code))
          )
            throw new Error(`无效的国家代码：${value}`)
          return code
        }
        if (value.length > 500) throw new Error('每条 UA 最多 500 个字符')
        return value
      })
    ),
  ]
}
export function activeMembers(list: SecurityList, now: number) {
  return list.members.filter(
    (member) => member.expiresAt === null || member.expiresAt > now
  )
}
