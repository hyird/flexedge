import { useState } from 'react'
import type { ColumnDef } from '@tanstack/react-table'
import { RefreshCw } from 'lucide-react'
import { formatDate } from '@/lib/format'
import { Button } from '@/components/ui/button'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import {
  taskKey,
  taskStatuses,
  taskTitle,
  taskTypes,
  useTasks,
  type Task,
} from './data'
import { TaskDetail } from './task-detail'
import { TaskStatus } from './task-status'

export function Tasks() {
  const [page, setPage] = useState(1)
  const [size, setSize] = useState(10)
  const [keyword, setKeyword] = useState('')
  const [search, setSearch] = useState('')
  const [type, setType] = useState('all')
  const [status, setStatus] = useState('all')
  const [days, setDays] = useState('0')
  const [selected, setSelected] = useState<Task | null>(null)
  const tasks = useTasks({
    page,
    page_size: size,
    type: type === 'all' ? undefined : type,
    status: status === 'all' ? undefined : status,
    keyword: search || undefined,
    days: Number(days),
  })
  const columns: ColumnDef<Task>[] = [
    {
      id: 'task',
      header: '任务',
      cell: ({ row }) => (
        <div className='min-w-40'>
          <p className='font-medium'>{taskTitle(row.original)}</p>
          <p className='max-w-72 truncate text-xs text-muted-foreground'>
            {row.original.name}
          </p>
        </div>
      ),
    },
    {
      accessorKey: 'status',
      header: '状态',
      cell: ({ row }) => <TaskStatus status={row.original.status} />,
    },
    { accessorKey: 'version', header: '版本' },
    {
      accessorKey: 'updated_at',
      header: '最近更新',
      cell: ({ row }) => (
        <span className='whitespace-nowrap'>
          {formatDate(row.original.updated_at)}
        </span>
      ),
    },
    {
      id: 'details',
      header: '操作',
      cell: ({ row }) => (
        <Button
          variant='ghost'
          size='sm'
          onClick={() => setSelected(row.original)}
          aria-label={`查看${taskTitle(row.original)} ${row.original.name}详情`}
        >
          查看详情
        </Button>
      ),
    },
  ]
  return (
    <FeatureShell
      title='任务中心'
      description='查看后台同步与节点发布结果。同步执行历史保留 7 天，自动重试会持续更新状态。'
      actions={
        <Button
          variant='outline'
          disabled={tasks.isFetching}
          onClick={() => void tasks.refetch()}
        >
          <RefreshCw
            className={tasks.isFetching ? 'motion-safe:animate-spin' : ''}
          />
          刷新
        </Button>
      }
    >
      <ResourceToolbar
        value={keyword}
        onChange={setKeyword}
        placeholder='搜索资源名称或 ID…'
        refreshing={tasks.isFetching}
        onSearch={() => {
          setPage(1)
          setSearch(keyword.trim())
          if (page === 1 && search === keyword.trim()) void tasks.refetch()
        }}
        onReset={() => {
          setPage(1)
          setKeyword('')
          setSearch('')
          setType('all')
          setStatus('all')
          setDays('0')
        }}
        filters={
          <>
            <Select
              value={days}
              onValueChange={(value) => {
                setDays(value)
                setPage(1)
              }}
            >
              <SelectTrigger className='w-36' aria-label='任务时间'>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value='0'>全部保留记录</SelectItem>
                <SelectItem value='1'>最近 24 小时</SelectItem>
                <SelectItem value='7'>最近 7 天</SelectItem>
              </SelectContent>
            </Select>
            <Select
              value={type}
              onValueChange={(value) => {
                setType(value)
                setPage(1)
              }}
            >
              <SelectTrigger className='w-36' aria-label='任务类型'>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value='all'>全部类型</SelectItem>
                {Object.entries(taskTypes).map(([value, label]) => (
                  <SelectItem key={value} value={value}>
                    {label}
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
              <SelectTrigger className='w-32' aria-label='任务状态'>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value='all'>全部状态</SelectItem>
                {Object.entries(taskStatuses).map(([value, label]) => (
                  <SelectItem key={value} value={value}>
                    {label}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </>
        }
      />
      <ResourceTable
        columns={columns}
        data={tasks.data?.list ?? []}
        loading={tasks.isPending}
        error={tasks.isError}
        onRetry={() => void tasks.refetch()}
        page={page}
        pageSize={size}
        totalPages={tasks.data?.total_pages ?? 0}
        onPaginationChange={(nextPage, nextSize) => {
          setPage(nextPage)
          setSize(nextSize)
        }}
        emptyTitle='暂无任务'
        emptyDescription='调整筛选条件，或在资源页面提交操作后查看执行状态。'
      />
      <TaskDetail
        task={
          selected
            ? (tasks.data?.list.find(
                (task) => taskKey(task) === taskKey(selected)
              ) ?? selected)
            : null
        }
        onClose={() => setSelected(null)}
      />
    </FeatureShell>
  )
}
