import { describe, expect, it } from 'vitest'
import type { DnsLine } from '@/features/dns-zones/types'
import {
  buildDnsLineTree,
  dnsLineIndex,
  dnsLineName,
  dnsLinePath,
  flattenDnsLineTree,
} from './dns-lines'

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

  it('shows only the most specific line name without changing picker paths', () => {
    expect(dnsLineName(lines, 'cn_mobile_beijing')).toBe('北京')
    expect(dnsLineName(lines, 'cn_mobile')).toBe('中国移动')
    expect(dnsLineName(lines, 'default')).toBe('默认')
    expect(dnsLineName([], 'default')).toBe('默认线路')
    expect(dnsLineName(lines, 'unknown')).toBe('unknown')
    expect(dnsLinePath(lines, 'cn_mobile_beijing')).toBe('中国移动 / 北京')
  })

  it('builds one index per line snapshot, whatever the number of lookups', () => {
    // An Aliyun-sized list: label lookups for every row and option of the
    // records tab must not rebuild the tree (it was O(rows × lines²)).
    const many: DnsLine[] = Array.from({ length: 500 }, (_, index) => ({
      code: `line_${index}`,
      name: `name_${index}`,
      display_name: `中国地区_运营商${index % 8}_${index}`,
      status: 'enabled',
    }))
    const index = dnsLineIndex(many)
    expect(index.options).toHaveLength(500)
    expect(flattenDnsLineTree(index.tree)).toEqual(index.options)
    for (let lookup = 0; lookup < 300 * 500; lookup++)
      dnsLinePath(many, `line_${lookup % 500}`)
    expect(dnsLineIndex(many)).toBe(index)
    expect(dnsLinePath(many, 'line_9')).toBe('中国地区 / 运营商1 / 9')
    // A new snapshot (another line list) gets its own index.
    expect(dnsLineIndex([...many])).not.toBe(index)
  })
})
