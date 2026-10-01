import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { Choice } from '@/components/forms'
import { Page } from '@/components/page'
import { ResourceList, ResourceSearch } from '@/components/resource-list'
import { StatusChip } from '@/components/status-chip'
import { tasksQuery } from './data'
import { TaskDetail } from './task-detail'
import { taskKey, taskStatuses, taskTitle, taskTypes } from './task-display'
import type { Task } from './types'

export function TasksPage() {
  const [page, setPage] = useState(1)
  const [size, setSize] = useState(20)
  const [draft, setDraft] = useState('')
  const [keyword, setKeyword] = useState('')
  const [type, setType] = useState('all')
  const [status, setStatus] = useState('all')
  const [days, setDays] = useState('0')
  const [selected, setSelected] = useState<Task | null>(null)
  const query = useQuery({
    ...tasksQuery({
      page,
      page_size: size,
      type: type === 'all' ? undefined : type,
      status: status === 'all' ? undefined : status,
      keyword: keyword || undefined,
      days: Number(days),
    }),
    enabled: !selected,
  })
  const columns: DataGridColumn<Task>[] = [
    {
      id: 'name',
      header: '任务',
      isRowHeader: true,
      minWidth: 260,
      cell: (item) => (
        <div>
          <strong className='block font-medium'>{taskTitle(item)}</strong>
          <span className='text-xs text-muted'>{item.name}</span>
        </div>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 120,
      cell: (item) => (
        <StatusChip status={item.status} label={taskStatuses[item.status]} />
      ),
    },
    { id: 'version', header: '版本', minWidth: 80, accessorKey: 'version' },
    {
      id: 'failures',
      header: '失败次数',
      minWidth: 90,
      accessorKey: 'failures',
    },
    {
      id: 'updated',
      header: '最近更新',
      minWidth: 170,
      cell: (item) => (
        <span className='text-xs text-muted'>
          {formatDate(item.updated_at)}
        </span>
      ),
    },
    {
      id: 'detail',
      header: '操作',
      width: 100,
      cell: (item) => (
        <Button size='sm' variant='ghost' onPress={() => setSelected(item)}>
          查看详情
        </Button>
      ),
    },
  ]
  return (
    <Page
      title='任务中心'
      description='查看同步与节点发布结果。失败任务按服务端规则自动重试，执行历史保留 7 天。'
    >
      {query.data && (
        <div className='flex flex-wrap items-center gap-5 py-1 text-sm text-muted'>
          <span>
            进行中{' '}
            <strong className='ml-1 font-medium text-foreground tabular-nums'>
              {query.data.active}
            </strong>
          </span>
          <span>
            失败{' '}
            <strong className='ml-1 font-medium text-danger tabular-nums'>
              {query.data.failed}
            </strong>
          </span>
        </div>
      )}
      <ResourceList
        label='后台任务'
        query={query}
        columns={columns}
        getRowId={taskKey}
        page={page}
        onPageChange={setPage}
        pageSize={size}
        onPageSizeChange={(value) => {
          setSize(value)
          setPage(1)
        }}
        onRefresh={() => void query.refetch()}
        filters={
          <form
            className='flex flex-wrap items-end gap-2'
            onSubmit={(event) => {
              event.preventDefault()
              setKeyword(draft.trim())
              setPage(1)
            }}
          >
            <ResourceSearch
              label='搜索任务'
              value={draft}
              onChange={setDraft}
            />
            <Choice
              compact
              label='任务类型'
              value={type}
              onChange={(value) => {
                setType(value)
                setPage(1)
              }}
              items={[
                { id: 'all', label: '全部类型' },
                ...Object.entries(taskTypes).map(([id, label]) => ({
                  id,
                  label,
                })),
              ]}
            />
            <Choice
              compact
              label='任务状态'
              value={status}
              onChange={(value) => {
                setStatus(value)
                setPage(1)
              }}
              items={[
                { id: 'all', label: '全部状态' },
                ...Object.entries(taskStatuses).map(([id, label]) => ({
                  id,
                  label,
                })),
              ]}
            />
            <Choice
              compact
              label='任务时间'
              value={days}
              onChange={(value) => {
                setDays(value)
                setPage(1)
              }}
              items={[
                { id: '0', label: '全部保留记录' },
                { id: '1', label: '最近 24 小时' },
                { id: '7', label: '最近 7 天' },
              ]}
            />
            <Button size='sm' type='submit' variant='secondary'>
              搜索
            </Button>
            <Button
              size='sm'
              variant='ghost'
              onPress={() => {
                setDraft('')
                setKeyword('')
                setType('all')
                setStatus('all')
                setDays('0')
                setPage(1)
              }}
            >
              重置
            </Button>
          </form>
        }
        emptyTitle='暂无任务'
        emptyDescription='调整筛选条件，或在资源页面提交操作。'
      />
      {selected && (
        <TaskDetail task={selected} onClose={() => setSelected(null)} />
      )}
    </Page>
  )
}
