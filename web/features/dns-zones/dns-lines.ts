import type { DnsLine } from '@/features/dns-zones/types'

type DnsLineTreeNode = {
  key: string
  label: string
  path: string[]
  line?: DnsLine
  children: DnsLineTreeNode[]
}

export type DnsLineTreeOption = {
  line: DnsLine
  path: string[]
}

type MutableDnsLineTreeNode = DnsLineTreeNode & {
  childMap: Map<string, MutableDnsLineTreeNode>
  children: MutableDnsLineTreeNode[]
}

function lineSegments(line: DnsLine) {
  const label = line.display_name || line.name || line.code
  return label
    .split('_')
    .map((segment) => segment.trim())
    .filter(Boolean)
}

export function buildDnsLineTree(lines: DnsLine[]): DnsLineTreeNode[] {
  const roots: MutableDnsLineTreeNode[] = []
  const rootMap = new Map<string, MutableDnsLineTreeNode>()

  for (const line of lines) {
    const segments = lineSegments(line)
    let siblings = roots
    let siblingMap = rootMap
    let path: string[] = []
    let current: MutableDnsLineTreeNode | undefined

    for (const segment of segments) {
      path = [...path, segment]
      current = siblingMap.get(segment)
      if (!current) {
        current = {
          key: path.join('/'),
          label: segment,
          path,
          children: [],
          childMap: new Map(),
        }
        siblingMap.set(segment, current)
        siblings.push(current)
      }
      siblings = current.children
      siblingMap = current.childMap
    }

    if (current) current.line = line
  }

  const finalize = (node: MutableDnsLineTreeNode): DnsLineTreeNode => ({
    key: node.key,
    label: node.label,
    path: node.path,
    line: node.line,
    children: node.children.map(finalize),
  })

  return roots.map(finalize)
}

export function flattenDnsLineTree(
  nodes: DnsLineTreeNode[]
): DnsLineTreeOption[] {
  return nodes.flatMap((node) => [
    ...(node.line ? [{ line: node.line, path: node.path }] : []),
    ...flattenDnsLineTree(node.children),
  ])
}

export type DnsLineIndex = {
  /** Selectable lines in hierarchy order. */
  options: DnsLineTreeOption[]
  /** Line code → readable path (`中国地区 / 电信 / 北京`). */
  labels: Map<string, string>
}

// One index per line snapshot: live zone patches keep `runtime.lines` by
// reference (structural sharing), so table cells, the line picker and the
// lines tab share one tree build instead of rebuilding it per cell or option.
const lineIndexes = new WeakMap<DnsLine[], DnsLineIndex>()

export function dnsLineIndex(lines: DnsLine[]): DnsLineIndex {
  let index = lineIndexes.get(lines)
  if (!index) {
    const options = flattenDnsLineTree(buildDnsLineTree(lines))
    index = {
      options,
      labels: new Map(
        options.map(({ line, path }) => [line.code, path.join(' / ')])
      ),
    }
    lineIndexes.set(lines, index)
  }
  return index
}

export function dnsLinePath(lines: DnsLine[], code: string) {
  return dnsLineIndex(lines).labels.get(code) ?? code
}
