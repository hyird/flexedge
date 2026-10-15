import type { DnsZoneOption } from '@/features/dns-zones/types'

/** The parts of a hosted zone the domain picker needs. */
export type HostedZone = Pick<DnsZoneOption, 'id' | 'domain'>

const hostnamePattern =
  /^(?:\*\.)?([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$/
const prefixLabelPattern = /^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$/

/** Backend `is_hostname`: optional leading `*.`, at least two labels, an
 * alphabetic last label, at most 253 characters. */
export function isWebsiteHostname(value: string) {
  return value.length <= 253 && hostnamePattern.test(value)
}

/** Lowercase, trimmed, without trailing dots (backend `normalize_domain_name`). */
function normalizeDomain(value: string) {
  return value.trim().toLowerCase().replace(/\.+$/, '')
}

/** The hosted zone that owns `hostname`: the longest zone whose domain equals
 * the hostname (leading `*.` ignored) or is a dot-suffix of it — the same zone
 * the backend writes the website CNAME to. */
export function findHostedZone<Zone extends HostedZone>(
  hostname: string,
  zones: readonly Zone[]
): Zone | undefined {
  let host = normalizeDomain(hostname)
  if (host.startsWith('*.')) host = host.slice(2)
  if (!host) return undefined
  let owner: Zone | undefined
  let ownerLength = 0
  for (const zone of zones) {
    const domain = normalizeDomain(zone.domain)
    if (!domain || domain.length <= ownerLength) continue
    if (host === domain || host.endsWith('.' + domain)) {
      owner = zone
      ownerLength = domain.length
    }
  }
  return owner
}

/** Splits a hostname into its owning zone and the host prefix in front of it:
 * `''` for the zone apex, `'*'` / `'*.img'` for wildcards. */
export function splitHostedDomain<Zone extends HostedZone>(
  hostname: string,
  zones: readonly Zone[]
): { zone: Zone; prefix: string } | undefined {
  const zone = findHostedZone(hostname, zones)
  if (!zone) return undefined
  const host = normalizeDomain(hostname)
  const domain = normalizeDomain(zone.domain)
  return {
    zone,
    prefix: host === domain ? '' : host.slice(0, -(domain.length + 1)),
  }
}

/** Full hostname for a prefix under a zone; the bare zone for an empty prefix. */
export function composeHostedDomain(prefix: string, zoneDomain: string) {
  const host = prefix.trim()
  if (!zoneDomain) return host
  return host ? `${host}.${zoneDomain}` : zoneDomain
}

/** Why a host prefix cannot be used, or undefined. An empty prefix binds the
 * zone apex. */
export function hostedPrefixError(prefix: string) {
  const value = prefix.trim()
  if (!value) return undefined
  if (value.startsWith('.') || value.endsWith('.'))
    return '主机前缀不能以点开头或结尾'
  if (value.includes('..')) return '主机前缀不能包含连续的点'
  const labels = value.split('.')
  for (const [index, label] of labels.entries()) {
    if (label === '*' && index === 0) continue
    if (label.includes('*')) return '通配符 * 只能单独作为主机前缀的第一段'
    if (label.length > 63) return '主机前缀每段不能超过 63 个字符'
    if (!prefixLabelPattern.test(label))
      return '主机前缀只能包含字母、数字和连字符，且每段不能以连字符开头或结尾'
  }
  return undefined
}
