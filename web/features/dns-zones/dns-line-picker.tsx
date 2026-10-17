import { useId, useMemo, useState } from 'react'
import { ChevronDown } from '@gravity-ui/icons'
import { FileTree } from '@heroui-pro/react'
import { Button, Input, Label, Popover, TextField } from '@heroui/react'
import {
  dnsLineIndex,
  filterDnsLineTree,
  type DnsLineTreeNode,
} from './dns-lines'
import type { DnsLine } from './types'

function treeItems(nodes: DnsLineTreeNode[]) {
  return nodes.map((node) => (
    <FileTree.Item
      key={node.key}
      id={node.line?.code ?? `group:${node.key}`}
      textValue={node.path.join('/')}
      title={node.label}
    >
      {treeItems(node.children)}
    </FileTree.Item>
  ))
}

function branchKeys(nodes: DnsLineTreeNode[]): string[] {
  return nodes.flatMap((node) =>
    node.children.length
      ? [node.line?.code ?? `group:${node.key}`, ...branchKeys(node.children)]
      : []
  )
}

function disabledKeys(nodes: DnsLineTreeNode[], value: string): string[] {
  return nodes.flatMap((node) => [
    ...(!node.line ||
    (node.line.status !== 'enabled' && node.line.code !== value)
      ? [node.line?.code ?? `group:${node.key}`]
      : []),
    ...disabledKeys(node.children, value),
  ])
}

/** The shared picker for record and node forms. Groups only expand; provider
 * lines can be selected at any depth. Search keeps the matching hierarchy. */
export function DnsLinePicker({
  lines,
  value,
  onChange,
  error,
  disabled,
  label = '解析线路',
}: {
  lines: DnsLine[]
  value: string
  onChange: (code: string) => void
  error?: string
  disabled?: boolean
  label?: string
}) {
  const id = useId()
  const [open, setOpen] = useState(false)
  const [search, setSearch] = useState('')
  const [expanded, setExpanded] = useState<Set<string>>(new Set())
  const index = dnsLineIndex(lines)
  const nodes = useMemo(() => {
    if (!value || index.labels.has(value)) return index.tree
    return [
      {
        key: `unknown:${value}`,
        label: value === 'default' ? '默认' : value,
        path: [value],
        line: {
          code: value,
          name: value,
          display_name: value,
          status: 'enabled',
        },
        children: [],
      },
      ...index.tree,
    ]
  }, [index, value])
  const filtered = useMemo(
    () => filterDnsLineTree(nodes, search),
    [nodes, search]
  )
  const selectedLabel =
    index.labels.get(value) ?? (value === 'default' ? '默认' : value)
  return (
    <div className='flex min-w-0 flex-col gap-1'>
      <Label id={`${id}-label`}>{label}</Label>
      <Popover
        isOpen={open}
        onOpenChange={(next) => {
          setOpen(next)
          if (next) {
            setSearch('')
            const path =
              index.options.find(({ line }) => line.code === value)?.path ?? []
            setExpanded(
              new Set(
                branchKeys(nodes).filter((key) => {
                  const nodePath = index.options.find(
                    ({ line }) => line.code === key
                  )?.path
                  return nodePath
                    ? nodePath.length < path.length &&
                        nodePath.every((part, i) => part === path[i])
                    : key.startsWith('group:') &&
                        path.join('/').startsWith(key.slice(6) + '/')
                })
              )
            )
          }
        }}
      >
        <Button
          variant='tertiary'
          isDisabled={disabled}
          aria-labelledby={`${id}-label ${id}-value`}
          aria-invalid={!!error || undefined}
          aria-describedby={error ? `${id}-error` : undefined}
          className='h-10 w-full min-w-0 justify-between rounded-xl px-3 font-normal'
        >
          <span id={`${id}-value`} className='truncate'>
            {selectedLabel || '选择线路'}
          </span>
          <ChevronDown className='size-4 shrink-0 text-muted' />
        </Button>
        <Popover.Content
          placement='bottom start'
          className='w-(--trigger-width) max-w-[calc(100vw-2rem)] min-w-64'
        >
          <Popover.Dialog
            aria-label={label}
            className='flex flex-col gap-2 p-1'
          >
            <TextField
              aria-label='搜索线路'
              value={search}
              onChange={setSearch}
              variant='secondary'
            >
              <Input placeholder='搜索线路' />
            </TextField>
            {filtered.length ? (
              <FileTree
                key={search ? 'search' : 'browse'}
                aria-label={label}
                selectionMode='single'
                selectionBehavior='replace'
                selectedKeys={value ? [value] : []}
                disallowEmptySelection
                disabledBehavior='selection'
                disabledKeys={disabledKeys(nodes, value)}
                expandedKeys={search ? new Set(branchKeys(filtered)) : expanded}
                onExpandedChange={(keys) =>
                  setExpanded(new Set(Array.from(keys, String)))
                }
                onSelectionChange={(keys) => {
                  if (keys === 'all') return
                  const key = keys.values().next().value
                  if (typeof key !== 'string' || key.startsWith('group:'))
                    return
                  onChange(key)
                  setOpen(false)
                }}
                showGuideLines
                className='max-h-80 overflow-y-auto'
              >
                {treeItems(filtered)}
              </FileTree>
            ) : (
              <p className='px-3 py-2 text-sm text-muted'>没有匹配的线路</p>
            )}
          </Popover.Dialog>
        </Popover.Content>
      </Popover>
      {error && (
        <p id={`${id}-error`} className='text-xs text-danger'>
          {error}
        </p>
      )}
    </div>
  )
}
