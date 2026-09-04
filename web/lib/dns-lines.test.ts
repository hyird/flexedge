import { describe, expect, it } from 'vitest'
import { buildDnsLineTree, dnsLinePath, flattenDnsLineTree } from './dns-lines'
import type { DnsLine } from './types'

const lines: DnsLine[] = [
  { code: 'default', name: 'default', display_name: '默认', status: 'enabled' },
  {
    code: 'cn_mobile',
    name: 'mobile',
    display_name: '中国移动',
    status: 'enabled',
  },
  {
    code: 'cn_mobile_beijing',
    name: 'mobile_beijing',
    display_name: '中国移动_北京',
    status: 'enabled',
  },
]

describe('DNS line tree', () => {
  it('groups provider line names into their display hierarchy', () => {
    const tree = buildDnsLineTree(lines)
    expect(tree.map((node) => node.label)).toEqual(['默认', '中国移动'])
    expect(tree[1]?.line?.code).toBe('cn_mobile')
    expect(tree[1]?.children[0]?.label).toBe('北京')
  })

  it('keeps every selectable line and exposes a readable path', () => {
    expect(flattenDnsLineTree(buildDnsLineTree(lines))).toHaveLength(3)
    expect(dnsLinePath(lines, 'cn_mobile_beijing')).toBe('中国移动 / 北京')
    expect(dnsLinePath(lines, 'unknown')).toBe('unknown')
  })
})
