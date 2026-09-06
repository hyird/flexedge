import { useEffect, useMemo, useState } from 'react'
import { z } from 'zod'
import { useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import {
  Clipboard,
  FileTerminal,
  KeyRound,
  Pencil,
  Plus,
  Trash2,
  X,
} from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData, type ApiEnvelope } from '@/lib/api'
import { dnsLinePath } from '@/lib/dns-lines'
import { formatBytesPerSecond, formatDate } from '@/lib/format'
import type { Cluster, DnsLine, DnsZone, Node, PageData } from '@/lib/types'
import { useResourceFilters } from '@/hooks/use-resource-filters'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  DropdownMenuItem,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
import {
  Form,
  FormControl,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import { ScrollArea } from '@/components/ui/scroll-area'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetFooter,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { DataTableCellContent } from '@/components/data-table/cell-content'
import { DnsLineSelect } from '@/components/dns-line-tree'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'

const endpointSchema = z.object({
  id: z.string().uuid(),
  ip_address: z.string().trim().min(1, '请输入 IP 地址').max(45),
  line_code: z.string().trim().min(1, '请输入线路代码').max(64),
})

const schema = z.object({
  cluster_id: z.string().uuid('请选择所属集群'),
  name: z.string().trim().min(1, '请输入节点名称').max(100),
  status: z.enum(['enabled', 'disabled']),
  endpoints: z.array(endpointSchema).min(1, '至少配置一个 IP').max(8),
})

type Values = z.infer<typeof schema>
type Credentials = { node_id: string; secret: string; revision: number }
type NodeLog = {
  id: string
  occurred_at: string
  level: string
  category: string
  message: string
}

export function NodesPanel({
  initialClusterId,
  createOpen = false,
  onCreateOpenChange,
  showClusterFilter = true,
}: {
  initialClusterId?: string
  createOpen?: boolean
  onCreateOpenChange?: (open: boolean) => void
  showClusterFilter?: boolean
}) {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const filter = useResourceFilters({
    keyword: '',
    clusterId: initialClusterId ?? 'all',
    status: 'all',
  })
  const { keyword, clusterId, status } = filter.filters
  const [dialog, setDialog] = useState<Node | 'new' | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Node | null>(null)
  const [credentials, setCredentials] = useState<Credentials | null>(null)
  const [logNode, setLogNode] = useState<Node | null>(null)

  const clustersQuery = useQuery({
    queryKey: ['clusters', 'options'],
    queryFn: () =>
      getData<PageData<Cluster>>('/clusters/', {
        page: 1,
        page_size: 100,
      }).then((data) => data.list),
  })
  const clusters = useMemo(() => clustersQuery.data ?? [], [clustersQuery.data])
  const clusterIds = useMemo(
    () => clusters.map((cluster) => cluster.id),
    [clusters]
  )
  const linesQuery = useQuery<Record<string, DnsLine[]>>({
    queryKey: ['dns-zones', 'lines-by-cluster', clusterIds],
    queryFn: async () => {
      const entries = await Promise.all(
        clusters.map(async (cluster) => {
          const zone = await getData<DnsZone>(
            `/dns-zones/${cluster.dns_zone_id}`
          )
          return [cluster.id, zone.runtime.lines] as const
        })
      )
      return Object.fromEntries(entries)
    },
    enabled: clusterIds.length > 0,
  })
  const query = useQuery({
    queryKey: ['nodes', page, pageSize, keyword, clusterId, status],
    queryFn: () =>
      getData<PageData<Node>>('/nodes/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        cluster_id: clusterId === 'all' ? undefined : clusterId,
        status: status === 'all' ? undefined : status,
      }),
  })
  const remove = useMutation({
    mutationFn: (item: Node) =>
      sendData('delete', `/nodes/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: ['nodes'] })
    },
  })
  const { mutate: loadNodeCredentials } = useMutation({
    mutationFn: (item: Node) =>
      getData<Credentials>(`/nodes/${item.id}/credentials`),
    onSuccess: setCredentials,
  })

  const columns = useMemo<ColumnDef<Node>[]>(
    () => [
      {
        accessorKey: 'name',
        size: 250,
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='节点 / 集群' />
        ),
        cell: ({ row }) => (
          <DataTableCellContent>
            <span className='font-medium' title={row.original.name}>
              {row.original.name}
            </span>
            <span className='text-xs text-muted-foreground'>
              {row.original.cluster_name} ·{' '}
              {row.original.runtime.agent_version || '未注册'}
            </span>
          </DataTableCellContent>
        ),
      },
      {
        id: 'connection',
        size: 275,
        header: '连接 / 心跳',
        cell: ({ row }) => (
          <DataTableCellContent>
            <StatusBadge status={row.original.runtime.connection_status} />
            <time
              className='text-xs text-muted-foreground tabular-nums'
              dateTime={row.original.runtime.last_heartbeat_at}
              title={row.original.runtime.last_heartbeat_at}
            >
              {formatDate(row.original.runtime.last_heartbeat_at)}
            </time>
          </DataTableCellContent>
        ),
      },
      {
        id: 'ip',
        size: 175,
        header: '节点 IP',
        cell: ({ row }) => {
          const endpoints = row.original.config.endpoints
          const addresses = endpoints
            .slice(0, 2)
            .map((endpoint) => endpoint.ip_address)
            .join(' · ')
          return (
            <DataTableCellContent>
              <code title={addresses}>{addresses}</code>
              {endpoints.length > 2 && (
                <span className='text-xs text-muted-foreground'>
                  +{endpoints.length - 2}
                </span>
              )}
            </DataTableCellContent>
          )
        },
      },
      {
        id: 'line',
        size: 130,
        header: 'DNS 线路',
        cell: ({ row }) => {
          const endpoints = row.original.config.endpoints
          const lines = endpoints
            .slice(0, 2)
            .map((endpoint) =>
              dnsLinePath(
                linesQuery.data?.[row.original.cluster_id] ?? [],
                endpoint.line_code
              )
            )
            .join(' · ')
          return (
            <DataTableCellContent>
              <span title={lines}>{lines}</span>
              {endpoints.length > 2 && (
                <span className='text-xs text-muted-foreground'>
                  +{endpoints.length - 2}
                </span>
              )}
            </DataTableCellContent>
          )
        },
      },
      {
        id: 'metrics',
        size: 300,
        header: '实时负载',
        cell: ({ row }) => (
          <div className='text-xs whitespace-nowrap text-muted-foreground'>
            CPU {row.original.runtime.cpu_usage?.toFixed(1) ?? '—'}% · 内存{' '}
            {row.original.runtime.memory_usage?.toFixed(1) ?? '—'}% ·{' '}
            {formatBytesPerSecond(row.original.runtime.traffic_out_bps)} ·{' '}
            {row.original.runtime.connection_count ?? '—'} 连接
          </div>
        ),
      },
      {
        accessorKey: 'status',
        size: 120,
        header: '启用状态',
        cell: ({ row }) => <StatusBadge status={row.original.status} />,
      },
      {
        id: 'actions',
        size: 52,
        header: () => <span className='sr-only'>操作</span>,
        cell: ({ row }) => (
          <div className='flex justify-end'>
            <RowActions>
              <DropdownMenuItem onSelect={() => setDialog(row.original)}>
                <Pencil /> 编辑
              </DropdownMenuItem>
              <DropdownMenuItem
                onSelect={() => loadNodeCredentials(row.original)}
              >
                <KeyRound /> 接入凭据
              </DropdownMenuItem>
              <DropdownMenuItem onSelect={() => setLogNode(row.original)}>
                <FileTerminal /> 实时日志
              </DropdownMenuItem>
              <DropdownMenuSeparator />
              <DropdownMenuItem
                variant='destructive'
                onSelect={() => setRemoveTarget(row.original)}
              >
                <Trash2 /> 删除
              </DropdownMenuItem>
            </RowActions>
          </div>
        ),
      },
    ],
    [linesQuery.data, loadNodeCredentials]
  )

  return (
    <>
      <ResourceToolbar
        value={filter.draft.keyword}
        onChange={(value) => filter.setField('keyword', value)}
        onSearch={() => {
          const changed = filter.apply()
          setPage(1)
          if (!changed && page === 1) void query.refetch()
        }}
        onReset={() => {
          const changed = filter.reset()
          setPage(1)
          if (!changed && page === 1) void query.refetch()
        }}
        refreshing={query.isFetching}
        placeholder='搜索节点名称…'
        filters={
          <>
            {showClusterFilter && (
              <Select
                value={filter.draft.clusterId}
                onValueChange={(value) => filter.setField('clusterId', value)}
              >
                <SelectTrigger className='w-44'>
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value='all'>全部集群</SelectItem>
                  {clustersQuery.data?.map((cluster) => (
                    <SelectItem key={cluster.id} value={cluster.id}>
                      {cluster.name}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            )}
            <Select
              value={filter.draft.status}
              onValueChange={(value) => filter.setField('status', value)}
            >
              <SelectTrigger className='w-36'>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value='all'>全部状态</SelectItem>
                <SelectItem value='enabled'>已启用</SelectItem>
                <SelectItem value='disabled'>已停用</SelectItem>
              </SelectContent>
            </Select>
          </>
        }
      />
      <ResourceTable
        columns={columns}
        data={query.data?.list ?? []}
        fixedLayout
        tableClassName='min-w-[68.75rem]'
        loading={query.isLoading}
        error={query.isError}
        onRetry={() => void query.refetch()}
        page={page}
        pageSize={pageSize}
        totalPages={query.data?.total_pages ?? 1}
        onPaginationChange={(nextPage, nextSize) => {
          setPage(nextPage)
          setPageSize(nextSize)
        }}
        emptyTitle='暂无节点'
        emptyDescription='添加节点后会生成一次性接入凭据。'
      />
      {(dialog || createOpen) && (
        <NodeDialog
          key={!dialog || dialog === 'new' ? 'new' : dialog.id}
          node={!dialog || dialog === 'new' ? undefined : dialog}
          clusters={clustersQuery.data ?? []}
          initialClusterId={clusterId === 'all' ? undefined : clusterId}
          open
          onCredentials={setCredentials}
          onOpenChange={(open) => {
            if (!open) {
              setDialog(null)
              onCreateOpenChange?.(false)
            }
          }}
        />
      )}
      <CredentialsDialog
        credentials={credentials}
        onOpenChange={(open) => !open && setCredentials(null)}
      />
      {logNode && (
        <NodeLogSheet
          key={logNode.id}
          node={logNode}
          onOpenChange={(open) => !open && setLogNode(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除节点'
        desc={`确定删除“${removeTarget?.name ?? ''}”吗？该节点的接入凭据将立即失效。`}
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </>
  )
}

function NodeDialog({
  node,
  clusters,
  initialClusterId,
  open,
  onOpenChange,
  onCredentials,
}: {
  node?: Node
  clusters: Cluster[]
  initialClusterId?: string
  open: boolean
  onOpenChange: (open: boolean) => void
  onCredentials: (credentials: Credentials) => void
}) {
  const queryClient = useQueryClient()
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      cluster_id: node?.cluster_id ?? initialClusterId ?? '',
      name: node?.name ?? '',
      status: (node?.status as Values['status']) ?? 'enabled',
      endpoints: node?.config.endpoints ?? [
        {
          id: crypto.randomUUID(),
          ip_address: '',
          line_code: 'default',
        },
      ],
    },
  })
  const clusterId = form.watch('cluster_id')
  const selectedCluster = clusters.find((cluster) => cluster.id === clusterId)
  const linesQuery = useQuery({
    queryKey: ['dns-zones', 'lines', selectedCluster?.dns_zone_id],
    queryFn: () =>
      getData<DnsZone>(`/dns-zones/${selectedCluster!.dns_zone_id}`).then(
        (zone) => zone.runtime.lines
      ),
    enabled: !!selectedCluster?.dns_zone_id,
  })
  const endpoints = useFieldArray({
    control: form.control,
    name: 'endpoints',
    keyName: 'formKey',
  })
  const mutation = useMutation({
    mutationFn: (values: Values) => {
      const body = {
        cluster_id: values.cluster_id,
        name: values.name,
        status: values.status,
        config: { endpoints: values.endpoints },
      }
      return node
        ? sendData('put', `/nodes/${node.id}`, body, node.revision)
        : sendData<Credentials>('post', '/nodes/', body)
    },
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      if (!node && response.data) onCredentials(response.data as Credentials)
      await queryClient.invalidateQueries({ queryKey: ['nodes'] })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto sm:max-w-2xl'>
        <SheetHeader>
          <SheetTitle>{node ? '编辑节点' : '添加节点'}</SheetTitle>
          <SheetDescription>
            每个节点可配置最多 8 个唯一 IP 与 DNS 线路。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='node-form'
            className='grid gap-4 px-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <div className='grid gap-4 sm:grid-cols-[minmax(0,1fr)_auto_auto]'>
              <FormField
                control={form.control}
                name='name'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>节点名称</FormLabel>
                    <FormControl>
                      <Input placeholder='edge-tpe-01' {...field} />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <FormField
                control={form.control}
                name='cluster_id'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>所属集群</FormLabel>
                    <Select value={field.value} onValueChange={field.onChange}>
                      <FormControl>
                        <SelectTrigger>
                          <SelectValue placeholder='选择集群' />
                        </SelectTrigger>
                      </FormControl>
                      <SelectContent>
                        {clusters.map((cluster) => (
                          <SelectItem key={cluster.id} value={cluster.id}>
                            {cluster.name}
                          </SelectItem>
                        ))}
                      </SelectContent>
                    </Select>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <FormField
                control={form.control}
                name='status'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>状态</FormLabel>
                    <Select value={field.value} onValueChange={field.onChange}>
                      <FormControl>
                        <SelectTrigger>
                          <SelectValue />
                        </SelectTrigger>
                      </FormControl>
                      <SelectContent>
                        <SelectItem value='enabled'>启用</SelectItem>
                        <SelectItem value='disabled'>停用</SelectItem>
                      </SelectContent>
                    </Select>
                  </FormItem>
                )}
              />
            </div>
            <div className='space-y-3'>
              <div className='flex items-center justify-between'>
                <div>
                  <FormLabel>Endpoint</FormLabel>
                  <p className='text-xs text-muted-foreground'>
                    配置节点对外提供服务的 IP。
                  </p>
                </div>
                <Button
                  type='button'
                  variant='outline'
                  size='sm'
                  disabled={endpoints.fields.length >= 8}
                  onClick={() =>
                    endpoints.append({
                      id: crypto.randomUUID(),
                      ip_address: '',
                      line_code: 'default',
                    })
                  }
                >
                  <Plus /> 添加 IP
                </Button>
              </div>
              {endpoints.fields.map((endpoint, index) => (
                <div
                  key={endpoint.formKey}
                  className='grid gap-2 rounded-md border p-3 sm:grid-cols-[1fr_160px_auto]'
                >
                  <FormField
                    control={form.control}
                    name={`endpoints.${index}.ip_address`}
                    render={({ field }) => (
                      <FormItem>
                        <FormLabel className='sr-only'>IP 地址</FormLabel>
                        <FormControl>
                          <Input placeholder='203.0.113.10' {...field} />
                        </FormControl>
                        <FormMessage />
                      </FormItem>
                    )}
                  />
                  <FormField
                    control={form.control}
                    name={`endpoints.${index}.line_code`}
                    render={({ field }) => (
                      <FormItem>
                        <FormLabel className='sr-only'>DNS线路</FormLabel>
                        <FormControl>
                          <DnsLineSelect
                            lines={linesQuery.data ?? []}
                            value={field.value}
                            onValueChange={field.onChange}
                          />
                        </FormControl>
                        <FormMessage />
                      </FormItem>
                    )}
                  />
                  <Button
                    type='button'
                    variant='ghost'
                    size='icon'
                    aria-label='移除 Endpoint'
                    disabled={endpoints.fields.length === 1}
                    onClick={() => endpoints.remove(index)}
                  >
                    <X />
                  </Button>
                </div>
              ))}
            </div>
          </form>
        </Form>
        <SheetFooter className='mt-0'>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button type='submit' form='node-form' disabled={mutation.isPending}>
            {mutation.isPending ? '正在保存…' : '保存'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}

function CredentialsDialog({
  credentials,
  onOpenChange,
}: {
  credentials: Credentials | null
  onOpenChange: (open: boolean) => void
}) {
  return (
    <Sheet open={!!credentials} onOpenChange={onOpenChange}>
      <SheetContent>
        <SheetHeader>
          <SheetTitle>节点接入凭据</SheetTitle>
          <SheetDescription>
            请安全保存密钥，不要通过公开渠道传输。
          </SheetDescription>
        </SheetHeader>
        <div className='space-y-3 px-4'>
          <div>
            <div className='mb-1 text-xs text-muted-foreground'>Node ID</div>
            <code className='block rounded-md bg-muted p-3 text-xs break-all'>
              {credentials?.node_id}
            </code>
          </div>
          <div>
            <div className='mb-1 text-xs text-muted-foreground'>Secret</div>
            <code className='block rounded-md bg-muted p-3 text-xs break-all'>
              {credentials?.secret}
            </code>
          </div>
        </div>
        <SheetFooter>
          <Button
            variant='outline'
            onClick={async () => {
              if (!credentials) return
              await navigator.clipboard.writeText(
                `NODE_ID=${credentials.node_id}\nNODE_SECRET=${credentials.secret}`
              )
              toast.success('凭据已复制')
            }}
          >
            <Clipboard /> 复制凭据
          </Button>
          <Button onClick={() => onOpenChange(false)}>完成</Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}

function nodeLogLevelClass(level: string) {
  switch (level.toLocaleLowerCase()) {
    case 'error':
    case 'fatal':
      return 'border-destructive/30 bg-destructive/10 text-destructive'
    case 'warn':
    case 'warning':
      return 'border-amber-500/30 bg-amber-500/10 text-amber-700 dark:text-amber-400'
    case 'debug':
      return 'border-slate-500/30 bg-slate-500/10 text-slate-700 dark:text-slate-300'
    default:
      return 'border-sky-500/30 bg-sky-500/10 text-sky-700 dark:text-sky-400'
  }
}

function NodeLogSheet({
  node,
  onOpenChange,
}: {
  node: Node
  onOpenChange: (open: boolean) => void
}) {
  const [logs, setLogs] = useState<NodeLog[]>([])
  const [connected, setConnected] = useState(false)

  useEffect(() => {
    const source = new EventSource(
      `/api/nodes/${node.id}/logs/stream?limit=1000`,
      { withCredentials: true }
    )
    source.addEventListener('ready', () => setConnected(true))
    source.addEventListener('logs', (event) => {
      const payload = JSON.parse(
        (event as MessageEvent<string>).data
      ) as ApiEnvelope<{
        list: NodeLog[]
      }>
      setLogs((current) => {
        const merged = [...payload.data.list, ...current]
        return Array.from(
          new Map(merged.map((item) => [item.id, item])).values()
        )
          .sort((a, b) => b.occurred_at.localeCompare(a.occurred_at))
          .slice(0, 200)
      })
    })
    source.onerror = () => setConnected(false)
    return () => source.close()
  }, [node.id])

  return (
    <Sheet open onOpenChange={onOpenChange}>
      <SheetContent className='flex w-full flex-col sm:max-w-2xl'>
        <SheetHeader className='text-start'>
          <div className='flex items-center gap-2'>
            <SheetTitle>{node.name} · 实时日志</SheetTitle>
            <Badge variant='outline' role='status'>
              {connected ? '已连接' : '正在重连'}
            </Badge>
          </div>
          <SheetDescription>实时显示最近 200 条节点日志，按接收时间倒序。</SheetDescription>
        </SheetHeader>
        <ScrollArea className='min-h-0 flex-1 px-4'>
          <div className='font-mono text-xs' role='region' aria-label='节点实时日志列表'>
            {logs.map((log) => (
              <div
                key={log.id}
                className='border-b px-3 py-2 last:border-b-0 hover:bg-muted/50'
              >
                <div className='flex min-w-0 items-center gap-2 text-muted-foreground'>
                  <time dateTime={log.occurred_at} className='shrink-0 tabular-nums'>
                    {formatDate(log.occurred_at)}
                  </time>
                  <Badge
                    variant='outline'
                    className={nodeLogLevelClass(log.level)}
                    aria-label={`日志等级 ${log.level}`}
                  >
                    {log.level}
                  </Badge>
                  <span className='min-w-0 truncate' title={log.category}>
                    {log.category}
                  </span>
                </div>
                <p className='mt-1 break-words text-foreground'>{log.message}</p>
              </div>
            ))}
            {!logs.length && (
              <p className='py-16 text-center text-muted-foreground'>
                等待日志事件…
              </p>
            )}
          </div>
        </ScrollArea>
      </SheetContent>
    </Sheet>
  )
}
