import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { CheckCircle2, Clock3, LoaderCircle, RotateCcw } from 'lucide-react'
import { getData } from '@/lib/api'
import { formatDate } from '@/lib/format'
import type { Task, TaskPage } from '@/lib/types'
import { Button } from '@/components/ui/button'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { DataTableColumnHeader } from '@/components/data-table'
import { FeatureShell } from '@/components/feature-shell'
import { MetricCard } from '@/components/metric-card'
import { ResourceTable } from '@/components/resource-table'
import { StatusBadge } from '@/components/status-badge'

const columns: ColumnDef<Task>[] = [
  {
    accessorKey: 'resource_name',
    header: ({ column }) => (
      <DataTableColumnHeader column={column} title='资源' />
    ),
    cell: ({ row }) => (
      <div>
        <div className='max-w-60 truncate font-medium'>
          {row.original.resource_name}
        </div>
        <div className='text-xs text-muted-foreground'>
          {row.original.resource_type} · #{row.original.sequence}
        </div>
      </div>
    ),
  },
  {
    accessorKey: 'operation',
    header: '操作',
    cell: ({ row }) => (
      <span className='whitespace-nowrap'>{row.original.operation}</span>
    ),
  },
  {
    accessorKey: 'status',
    header: '状态',
    cell: ({ row }) => <StatusBadge status={row.original.status} />,
  },
  {
    accessorKey: 'count_fails',
    header: '失败次数',
    cell: ({ row }) => (
      <span className='tabular-nums'>{row.original.count_fails}</span>
    ),
  },
  {
    accessorKey: 'updated_at',
    header: ({ column }) => (
      <DataTableColumnHeader column={column} title='更新时间' />
    ),
    cell: ({ row }) => (
      <span className='whitespace-nowrap text-muted-foreground'>
        {formatDate(row.original.updated_at)}
      </span>
    ),
  },
  {
    accessorKey: 'error',
    header: '错误',
    cell: ({ row }) => (
      <span
        className='block max-w-72 truncate text-sm text-destructive'
        title={row.original.error}
      >
        {row.original.error || '—'}
      </span>
    ),
  },
]

export function Tasks() {
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const [status, setStatus] = useState('all')
  const query = useQuery({
    queryKey: ['tasks', page, pageSize, status],
    queryFn: () =>
      getData<TaskPage>('/tasks/', {
        page,
        page_size: pageSize,
        status: status === 'all' ? undefined : status,
      }),
  })
  const summary = query.data?.summary

  return (
    <FeatureShell
      title='后台任务'
      description='跟踪配置分发、同步、签发与验证任务。'
      fixed
    >
      <div className='grid gap-4 sm:grid-cols-2 lg:grid-cols-4'>
        <MetricCard
          label='等待中'
          value={summary?.pending ?? 0}
          hint='尚未开始处理'
          icon={Clock3}
        />
        <MetricCard
          label='执行中'
          value={summary?.running ?? 0}
          hint='工作节点处理中'
          icon={LoaderCircle}
        />
        <MetricCard
          label='重试中'
          value={summary?.retry ?? 0}
          hint='等待下一次重试'
          icon={RotateCcw}
        />
        <MetricCard
          label='已完成'
          value={summary?.completed ?? 0}
          hint='成功处理的任务'
          icon={CheckCircle2}
        />
      </div>
      <div className='flex items-center gap-2'>
        <Select
          value={status}
          onValueChange={(value) => {
            setStatus(value)
            setPage(1)
          }}
        >
          <SelectTrigger className='w-40'>
            <SelectValue placeholder='全部状态' />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value='all'>全部状态</SelectItem>
            <SelectItem value='pending'>等待中</SelectItem>
            <SelectItem value='running'>执行中</SelectItem>
            <SelectItem value='retry'>重试中</SelectItem>
            <SelectItem value='completed'>已完成</SelectItem>
          </SelectContent>
        </Select>
        <Button
          variant='outline'
          size='icon'
          aria-label='刷新任务'
          disabled={query.isFetching}
          onClick={() => query.refetch()}
        >
          <RotateCcw
            className={query.isFetching ? 'animate-spin' : undefined}
          />
        </Button>
      </div>
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
        emptyTitle='暂无后台任务'
        emptyDescription='资源变更提交后，任务会显示在这里。'
        minWidth='980px'
      />
    </FeatureShell>
  )
}
