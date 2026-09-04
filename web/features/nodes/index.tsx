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
import { formatBytesPerSecond, formatDate, fromNow } from '@/lib/format'
import type { Cluster, Node, PageData } from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
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
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
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
}: {
  initialClusterId?: string
}) {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const [draftKeyword, setDraftKeyword] = useState('')
  const [keyword, setKeyword] = useState('')
  const [clusterId, setClusterId] = useState(initialClusterId ?? 'all')
  const [status, setStatus] = useState('all')
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
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='节点' />
        ),
        cell: ({ row }) => (
          <div>
            <div className='font-medium'>{row.original.name}</div>
            <div className='text-xs text-muted-foreground'>
              {row.original.cluster_name} ·{' '}
              {row.original.runtime.agent_version || '未注册'}
            </div>
          </div>
        ),
      },
      {
        id: 'connection',
        header: '连接状态',
        cell: ({ row }) => (
          <div className='space-y-1'>
            <StatusBadge status={row.original.runtime.connection_status} />
            <div className='text-xs text-muted-foreground'>
              {fromNow(row.original.runtime.last_heartbeat_at)}
            </div>
          </div>
        ),
      },
      {
        id: 'endpoints',
        header: 'Endpoint',
        cell: ({ row }) => (
          <div className='space-y-1'>
            {row.original.config.endpoints.slice(0, 2).map((endpoint) => (
              <div key={endpoint.id} className='text-xs'>
                <code>{endpoint.ip_address}</code>
                <span className='ms-2 text-muted-foreground'>
                  {endpoint.line_code}
                </span>
              </div>
            ))}
          </div>
        ),
      },
      {
        id: 'metrics',
        header: '实时负载',
        cell: ({ row }) => (
          <div className='text-xs leading-5 text-muted-foreground'>
            <div>
              CPU {row.original.runtime.cpu_usage?.toFixed(1) ?? '—'}% · 内存{' '}
              {row.original.runtime.memory_usage?.toFixed(1) ?? '—'}%
            </div>
            <div>
              {formatBytesPerSecond(row.original.runtime.traffic_out_bps)} ·{' '}
              {row.original.runtime.connection_count ?? '—'} 连接
            </div>
          </div>
        ),
      },
      {
        accessorKey: 'status',
        header: '状态',
        cell: ({ row }) => <StatusBadge status={row.original.status} />,
      },
      {
        id: 'actions',
        cell: ({ row }) => (
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
        ),
      },
    ],
    [loadNodeCredentials]
  )

  return (
    <>
      <ResourceToolbar
        value={draftKeyword}
        onChange={setDraftKeyword}
        onSearch={() => {
          setKeyword(draftKeyword.trim())
          setPage(1)
        }}
        onReset={() => {
          setDraftKeyword('')
          setKeyword('')
          setPage(1)
        }}
        onRefresh={() => query.refetch()}
        refreshing={query.isFetching}
        placeholder='搜索节点名称…'
        actions={
          <Button size='sm' onClick={() => setDialog('new')}>
            <Plus /> 添加节点
          </Button>
        }
        filters={
          <>
            <Select
              value={clusterId}
              onValueChange={(value) => {
                setClusterId(value)
                setPage(1)
              }}
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
            <Select
              value={status}
              onValueChange={(value) => {
                setStatus(value)
                setPage(1)
              }}
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
        minWidth='1040px'
      />
      {dialog && (
        <NodeDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          node={dialog === 'new' ? undefined : dialog}
          clusters={clustersQuery.data ?? []}
          initialClusterId={clusterId === 'all' ? undefined : clusterId}
          open
          onCredentials={setCredentials}
          onOpenChange={(open) => !open && setDialog(null)}
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
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className='max-h-[90svh] overflow-y-auto sm:max-w-2xl'>
        <DialogHeader>
          <DialogTitle>{node ? '编辑节点' : '添加节点'}</DialogTitle>
          <DialogDescription>
            每个节点可配置最多 8 个唯一 IP 与 DNS 线路。
          </DialogDescription>
        </DialogHeader>
        <Form {...form}>
          <form
            id='node-form'
            className='grid gap-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <div className='grid gap-4 sm:grid-cols-2'>
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
            </div>
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
                        <FormLabel className='sr-only'>线路代码</FormLabel>
                        <FormControl>
                          <Input placeholder='default' {...field} />
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
        <DialogFooter>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button type='submit' form='node-form' disabled={mutation.isPending}>
            {mutation.isPending ? '正在保存…' : '保存'}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
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
    <Dialog open={!!credentials} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>节点接入凭据</DialogTitle>
          <DialogDescription>
            请安全保存密钥，不要通过公开渠道传输。
          </DialogDescription>
        </DialogHeader>
        <div className='space-y-3'>
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
        <DialogFooter>
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
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
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
      `/api/nodes/${node.id}/logs/stream?limit=100`,
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
  }, [node])

  return (
    <Sheet open onOpenChange={onOpenChange}>
      <SheetContent className='flex w-full flex-col sm:max-w-2xl'>
        <SheetHeader className='text-start'>
          <div className='flex items-center gap-2'>
            <SheetTitle>{node?.name} · 实时日志</SheetTitle>
            <Badge variant='outline'>{connected ? '已连接' : '连接中'}</Badge>
          </div>
          <SheetDescription>最多保留最近 200 条节点日志。</SheetDescription>
        </SheetHeader>
        <ScrollArea className='min-h-0 flex-1 px-4'>
          <div className='space-y-2 pb-5 font-mono text-xs'>
            {logs.map((log) => (
              <div key={log.id} className='rounded-md border p-3'>
                <div className='mb-1 flex flex-wrap gap-2 text-muted-foreground'>
                  <span>{formatDate(log.occurred_at)}</span>
                  <Badge variant='secondary'>{log.level}</Badge>
                  <span>{log.category}</span>
                </div>
                <p className='break-words'>{log.message}</p>
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
