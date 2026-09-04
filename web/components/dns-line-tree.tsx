import { ChevronRight } from 'lucide-react'
import {
  buildDnsLineTree,
  dnsLinePath,
  type DnsLineTreeNode,
} from '@/lib/dns-lines'
import type { DnsLine } from '@/lib/types'
import { Button } from '@/components/ui/button'
import {
  Collapsible,
  CollapsibleContent,
  CollapsibleTrigger,
} from '@/components/ui/collapsible'
import {
  Select,
  SelectContent,
  SelectGroup,
  SelectItem,
  SelectLabel,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { StatusBadge } from '@/components/status-badge'

export function DnsLineSelect({
  lines,
  value,
  onValueChange,
}: {
  lines: DnsLine[]
  value: string
  onValueChange: (value: string) => void
}) {
  const tree = buildDnsLineTree(lines)
  const knownValue = lines.some((line) => line.code === value)

  return (
    <Select value={value} onValueChange={onValueChange}>
      <SelectTrigger className='w-full' aria-label='DNS 线路'>
        <SelectValue placeholder='选择 DNS 线路'>
          {value ? dnsLinePath(lines, value) : undefined}
        </SelectValue>
      </SelectTrigger>
      <SelectContent className='max-h-80'>
        <SelectGroup>
          {!knownValue && value && (
            <SelectItem value={value}>{value}（当前配置）</SelectItem>
          )}
          {tree.map((node) => (
            <DnsLineSelectNode key={node.key} node={node} depth={0} />
          ))}
        </SelectGroup>
      </SelectContent>
    </Select>
  )
}

function DnsLineSelectNode({
  node,
  depth,
}: {
  node: DnsLineTreeNode
  depth: number
}) {
  return (
    <>
      {node.line ? (
        <SelectItem value={node.line.code}>
          <span
            className='flex min-w-0 items-center gap-2'
            style={{ paddingInlineStart: `${depth * 12}px` }}
          >
            {depth > 0 && (
              <span className='text-muted-foreground' aria-hidden='true'>
                └
              </span>
            )}
            <span className='truncate'>{node.label}</span>
            <code className='text-xs text-muted-foreground'>
              {node.line.code}
            </code>
          </span>
        </SelectItem>
      ) : (
        <SelectLabel style={{ paddingInlineStart: `${8 + depth * 12}px` }}>
          {node.label}
        </SelectLabel>
      )}
      {node.children.map((child) => (
        <DnsLineSelectNode key={child.key} node={child} depth={depth + 1} />
      ))}
    </>
  )
}

export function DnsLineTree({ lines }: { lines: DnsLine[] }) {
  if (!lines.length) {
    return <p className='text-sm text-muted-foreground'>暂无可用线路。</p>
  }

  return (
    <div className='space-y-1 rounded-md border p-2'>
      {buildDnsLineTree(lines).map((node) => (
        <DnsLineTreeItem key={node.key} node={node} depth={0} />
      ))}
    </div>
  )
}

function DnsLineTreeItem({
  node,
  depth,
}: {
  node: DnsLineTreeNode
  depth: number
}) {
  if (!node.children.length) {
    return (
      <div
        className='flex min-h-8 items-center gap-2 rounded-md px-2 text-sm hover:bg-muted/50'
        style={{ paddingInlineStart: `${8 + depth * 16}px` }}
      >
        <span className='min-w-0 flex-1 truncate'>{node.label}</span>
        {node.line && (
          <>
            <code className='text-xs text-muted-foreground'>
              {node.line.code}
            </code>
            <StatusBadge status={node.line.status} />
          </>
        )}
      </div>
    )
  }

  return (
    <Collapsible>
      <div
        className='flex min-h-8 items-center gap-1 rounded-md pe-2 hover:bg-muted/50'
        style={{ paddingInlineStart: `${depth * 16}px` }}
      >
        <CollapsibleTrigger asChild>
          <Button
            type='button'
            variant='ghost'
            size='icon'
            className='group size-8 shrink-0'
            aria-label={`展开或收起 ${node.label}`}
          >
            <ChevronRight className='transition-transform group-data-[state=open]:rotate-90' />
          </Button>
        </CollapsibleTrigger>
        <span className='min-w-0 flex-1 truncate text-sm font-medium'>
          {node.label}
        </span>
        {node.line && (
          <>
            <code className='text-xs text-muted-foreground'>
              {node.line.code}
            </code>
            <StatusBadge status={node.line.status} />
          </>
        )}
      </div>
      <CollapsibleContent className='ms-4 border-s ps-1'>
        {node.children.map((child) => (
          <DnsLineTreeItem key={child.key} node={child} depth={depth + 1} />
        ))}
      </CollapsibleContent>
    </Collapsible>
  )
}
