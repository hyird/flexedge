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
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table'
import { ScrollArea } from '@/components/ui/scroll-area'
import { StatusBadge } from '@/components/status-badge'

function lineLabel(line: DnsLine) {
  return (line.display_name || line.name || line.code)
    .split('_')
    .filter(Boolean)
    .join(' / ')
}

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
          aria-label='DNS线路选择器'
          aria-expanded={open}
          className='w-full justify-between font-normal'
        >
          <span className='truncate'>
            {value ? dnsLinePath(lines, value) : '选择DNS线路'}
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
        <div
          className='max-h-[min(20rem,60svh)] w-full overflow-y-auto overscroll-contain pe-1'
          onWheel={(event) => event.stopPropagation()}
        >
          <div role='tree'>
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
            {!lines.length && !value && (
              <p className='px-2 py-3 text-sm text-muted-foreground'>
                暂无可用线路
              </p>
            )}
          </div>
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

export function DnsLineTree({
  lines,
  className,
}: {
  lines: DnsLine[]
  className?: string
}) {
  const tree = buildDnsLineTree(lines)

  return (
    <ScrollArea
      type='auto'
      className={cn('h-full w-full rounded-md border', className)}
    >
      <div role='tree' className='space-y-1 p-2'>
        {tree.map((node) => (
          <DnsLineTreeViewNode key={node.key} node={node} depth={0} />
        ))}
        {!tree.length && (
          <p className='px-2 py-3 text-sm text-muted-foreground'>
            暂无线数据
          </p>
        )}
      </div>
    </ScrollArea>
  )
}

function DnsLineTreeViewNode({
  node,
  depth,
}: {
  node: DnsLineTreeNode
  depth: number
}) {
  const hasChildren = node.children.length > 0

  return (
    <Collapsible defaultOpen={false}>
      <div
        role='treeitem'
        className='flex min-h-9 items-center gap-1 rounded-md pe-2 hover:bg-muted'
        style={{ paddingInlineStart: `${depth * 16}px` }}
      >
        {hasChildren ? (
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
        ) : (
          <span className='size-8 shrink-0' aria-hidden='true' />
        )}
        <span className={cn('min-w-0 flex-1 truncate text-sm', hasChildren && 'font-medium')}>
          {node.label}
        </span>
        {node.line && (
          <code className='shrink-0 text-xs text-muted-foreground'>
            {node.line.code}
          </code>
        )}
      </div>
      {hasChildren && (
        <CollapsibleContent className='ms-4 border-s ps-1'>
          {node.children.map((child) => (
            <DnsLineTreeViewNode
              key={child.key}
              node={child}
              depth={depth + 1}
            />
          ))}
        </CollapsibleContent>
      )}
    </Collapsible>
  )
}

export function DnsLineTable({
  lines,
  fillHeight = false,
}: {
  lines: DnsLine[]
  fillHeight?: boolean
}) {
  return (
    <div
      className={cn(
        'min-h-0 min-w-0 overflow-hidden rounded-md border',
        fillHeight && 'flex flex-1 flex-col'
      )}
    >
      <Table
        containerClassName={cn(
          'overflow-auto overscroll-contain',
          fillHeight ? 'min-h-0 flex-1' : 'max-h-[min(45svh,24rem)]'
        )}
        containerLabel='DNS线路表格'
      >
        <TableHeader className='sticky top-0 z-10 bg-background'>
          <TableRow>
            <TableHead>线路名称</TableHead>
            <TableHead>线路代码</TableHead>
            <TableHead>状态</TableHead>
          </TableRow>
        </TableHeader>
        <TableBody>
          {lines.map((line) => (
            <TableRow key={line.code}>
              <TableCell>{lineLabel(line)}</TableCell>
              <TableCell>
                <code className='text-xs'>{line.code}</code>
              </TableCell>
              <TableCell>
                <StatusBadge status={line.status} />
              </TableCell>
            </TableRow>
          ))}
          {!lines.length && (
            <TableRow>
              <TableCell
                colSpan={3}
                className='h-24 text-center text-muted-foreground'
              >
                暂无可用线路
              </TableCell>
            </TableRow>
          )}
        </TableBody>
      </Table>
    </div>
  )
}
