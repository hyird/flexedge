import { useMemo, useState } from 'react'
import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import type { ColumnDef } from '@tanstack/react-table'
import { Pencil, Plus, Server, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import type { Cluster, DnsZoneOption, PageData } from '@/lib/types'
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
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'

const schema = z.object({
  name: z.string().trim().min(1, '请输入集群名称').max(100),
  dns_zone_id: z.string().uuid('请选择托管域名'),
  hostname_prefix: z
    .string()
    .trim()
    .min(1, '请输入主机前缀')
    .max(63)
    .regex(
      /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/,
      '仅支持字母、数字和连字符'
    ),
  status: z.enum(['enabled', 'disabled']),
})

type Values = z.infer<typeof schema>

export function Clusters() {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const [draftKeyword, setDraftKeyword] = useState('')
  const [keyword, setKeyword] = useState('')
  const [status, setStatus] = useState('all')
  const [dialog, setDialog] = useState<Cluster | 'new' | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Cluster | null>(null)

  const query = useQuery({
    queryKey: ['clusters', page, pageSize, keyword, status],
    queryFn: () =>
      getData<PageData<Cluster>>('/clusters/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        status: status === 'all' ? undefined : status,
      }),
  })
  const remove = useMutation({
    mutationFn: (item: Cluster) =>
      sendData('delete', `/clusters/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: ['clusters'] })
    },
  })

  const columns = useMemo<ColumnDef<Cluster>[]>(
    () => [
      {
        accessorKey: 'name',
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='集群' />
        ),
        cell: ({ row }) => (
          <div>
            <div className='font-medium'>{row.original.name}</div>
            <div className='text-xs text-muted-foreground'>
              {row.original.access_domain}
            </div>
          </div>
        ),
      },
      {
        accessorKey: 'dns_zone_domain',
        header: '托管域名',
        cell: ({ row }) => (
          <div>
            <div>{row.original.dns_zone_domain}</div>
            <div className='text-xs text-muted-foreground'>
              {row.original.dns_provider_name}
            </div>
          </div>
        ),
      },
      {
        accessorKey: 'hostname_prefix',
        header: '主机前缀',
        cell: ({ row }) => <code>{row.original.hostname_prefix}</code>,
      },
      {
        accessorKey: 'node_count',
        header: '节点',
        cell: ({ row }) => (
          <span className='tabular-nums'>
            {row.original.online_node_count}/{row.original.node_count} 在线
          </span>
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
            <DropdownMenuItem asChild>
              <Link to='/nodes' search={{ cluster_id: row.original.id }}>
                <Server /> 查看节点
              </Link>
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setDialog(row.original)}>
              <Pencil /> 编辑
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
    []
  )

  return (
    <FeatureShell
      title='集群'
      description='按接入域名组织边缘节点和配置分发。'
      actions={
        <Button onClick={() => setDialog('new')}>
          <Plus /> 创建集群
        </Button>
      }
    >
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
        placeholder='搜索集群名称…'
        filters={
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
        emptyTitle='暂无集群'
        emptyDescription='创建集群后即可添加边缘节点。'
      />
      {dialog && (
        <ClusterDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          cluster={dialog === 'new' ? undefined : dialog}
          open
          onOpenChange={(open) => !open && setDialog(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除集群'
        desc={`确定删除“${removeTarget?.name ?? ''}”吗？有关联节点时服务端会拒绝删除。`}
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}

function ClusterDialog({
  cluster,
  open,
  onOpenChange,
}: {
  cluster?: Cluster
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const queryClient = useQueryClient()
  const optionsQuery = useQuery({
    queryKey: ['dns-zones', 'options'],
    queryFn: () =>
      getData<{ list: DnsZoneOption[] }>('/dns-zones/options').then(
        (data) => data.list
      ),
  })
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      name: cluster?.name ?? '',
      dns_zone_id: cluster?.dns_zone_id ?? '',
      hostname_prefix: cluster?.hostname_prefix ?? '',
      status: (cluster?.status as Values['status']) ?? 'enabled',
    },
  })
  const mutation = useMutation({
    mutationFn: (values: Values) =>
      cluster
        ? sendData('put', `/clusters/${cluster.id}`, values, cluster.revision)
        : sendData('post', '/clusters/', values),
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({ queryKey: ['clusters'] })
    },
  })

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{cluster ? '编辑集群' : '创建集群'}</DialogTitle>
          <DialogDescription>
            接入域名由主机前缀和托管域名组合生成。
          </DialogDescription>
        </DialogHeader>
        <Form {...form}>
          <form
            id='cluster-form'
            className='grid gap-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='name'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>集群名称</FormLabel>
                  <FormControl>
                    <Input placeholder='华东边缘集群' {...field} />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name='dns_zone_id'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>托管域名</FormLabel>
                  <Select value={field.value} onValueChange={field.onChange}>
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue placeholder='选择托管域名' />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      {optionsQuery.data?.map((option) => (
                        <SelectItem key={option.id} value={option.id}>
                          {option.domain}
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
              name='hostname_prefix'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>主机前缀</FormLabel>
                  <FormControl>
                    <Input placeholder='edge' {...field} />
                  </FormControl>
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
                  <FormMessage />
                </FormItem>
              )}
            />
          </form>
        </Form>
        <DialogFooter>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            form='cluster-form'
            type='submit'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在保存…' : '保存'}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
