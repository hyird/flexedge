import { useState } from 'react'
import { Check, ChevronRight, ChevronsUpDown } from 'lucide-react'
import {
  buildDnsLineTree,
  dnsLinePath,
  type DnsLineTreeNode,
} from '@/lib/dns-lines'
import type { DnsLine } from '@/lib/types'
import { cn } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import {
  Collapsible,
  CollapsibleContent,
  CollapsibleTrigger,
} from '@/components/ui/collapsible'
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from '@/components/ui/popover'
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
  const [open, setOpen] = useState(false)
  const tree = buildDnsLineTree(lines)
  const knownValue = lines.some((line) => line.code === value)
  const selectLine = (lineCode: string) => {
    onValueChange(lineCode)
    setOpen(false)
  }

  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger asChild>
        <Button
          type='button'
          variant='outline'
          role='combobox'
          aria-label='DNS 线路'
          aria-expanded={open}
          className='w-full justify-between font-normal'
        >
          <span className='truncate'>
            {value ? dnsLinePath(lines, value) : '选择 DNS 线路'}
          </span>
          <ChevronsUpDown className='shrink-0 text-muted-foreground' />
        </Button>
      </PopoverTrigger>
      <PopoverContent
        align='start'
        className='w-[min(32rem,calc(100vw-2rem))] p-2'
      >
        <div className='mb-2 px-2 text-xs font-medium text-muted-foreground'>
          按线路分组逐级展开选择
        </div>
        <div role='tree' className='max-h-80 overflow-y-auto'>
          {!knownValue && value && (
            <button
              type='button'
              role='treeitem'
              aria-selected='true'
              className='flex min-h-8 w-full items-center gap-2 rounded-md px-2 text-start text-sm hover:bg-muted'
              onClick={() => selectLine(value)}
            >
              <Check className='size-4' />
              <span>{value}（当前配置）</span>
            </button>
          )}
          {tree.map((node) => (
            <DnsLineSelectNode
              key={node.key}
              node={node}
              depth={0}
              selectedCode={value}
              onSelect={selectLine}
            />
          ))}
        </div>
      </PopoverContent>
    </Popover>
  )
}

function DnsLineSelectNode({
  node,
  depth,
  selectedCode,
  onSelect,
}: {
  node: DnsLineTreeNode
  depth: number
  selectedCode: string
  onSelect: (value: string) => void
}) {
  const selected = node.line?.code === selectedCode
  const containsSelected =
    selected || node.children.some((child) => nodeHasLine(child, selectedCode))

  if (!node.children.length) {
    return (
      <button
        type='button'
        role='treeitem'
        aria-selected={selected}
        className={cn(
          'flex min-h-8 w-full items-center gap-2 rounded-md pe-2 text-start text-sm hover:bg-muted',
          selected && 'bg-muted'
        )}
        style={{ paddingInlineStart: `${12 + depth * 16}px` }}
        onClick={() => node.line && onSelect(node.line.code)}
      >
        <span className='min-w-0 flex-1 truncate'>{node.label}</span>
        {node.line && (
          <code className='text-xs text-muted-foreground'>
            {node.line.code}
          </code>
        )}
        <Check className={cn('size-4 shrink-0', !selected && 'opacity-0')} />
      </button>
    )
  }

  return (
    <Collapsible defaultOpen={containsSelected}>
      <div
        className={cn(
          'flex min-h-8 items-center gap-1 rounded-md pe-2 hover:bg-muted',
          selected && 'bg-muted'
        )}
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
        {node.line ? (
          <button
            type='button'
            role='treeitem'
            aria-selected={selected}
            className='flex min-w-0 flex-1 items-center gap-2 text-start text-sm font-medium'
            onClick={() => onSelect(node.line!.code)}
          >
            <span className='min-w-0 flex-1 truncate'>{node.label}</span>
            <code className='text-xs font-normal text-muted-foreground'>
              {node.line.code}
            </code>
            <Check
              className={cn('size-4 shrink-0', !selected && 'opacity-0')}
            />
          </button>
        ) : (
          <span className='min-w-0 flex-1 truncate text-sm font-medium'>
            {node.label}
          </span>
        )}
      </div>
      <CollapsibleContent className='ms-4 border-s ps-1'>
        {node.children.map((child) => (
          <DnsLineSelectNode
            key={child.key}
            node={child}
            depth={depth + 1}
            selectedCode={selectedCode}
            onSelect={onSelect}
          />
        ))}
      </CollapsibleContent>
    </Collapsible>
  )
}

function nodeHasLine(node: DnsLineTreeNode, code: string): boolean {
  return (
    node.line?.code === code ||
    node.children.some((child) => nodeHasLine(child, code))
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
