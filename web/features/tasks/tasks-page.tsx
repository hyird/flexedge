import type { ReactNode } from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { CircleExclamation, ListCheck } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Link as HeroLink, Skeleton } from '@heroui/react'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { describeStatus } from '@/lib/status'
import { cn } from '@/lib/utils'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Page } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { tasksQuery } from './data'
import { taskFilterKeys, type TasksSearch } from './search'
import { TaskDetailSheet } from './task-detail-sheet'
import {
  parseTaskKey,
  taskKey,
  taskStatuses,
  taskStatusGroups,
  taskStatusReserve,
  taskTarget,
  taskTitle,
  taskTypeLabel,
  taskTypeLabels,
} from './task-display'
import { TaskNoteSlot } from './task-note'
import { taskRefOf, type Task } from './types'

const route = getRouteApi('/_authenticated/tasks/')

const statusOptions = [
  ...Object.entries(taskStatusGroups).map(([id, label]) => ({ id, label })),
  ...Object.entries(taskStatuses).map(([id, status]) => ({
    id,
    label: status.label,
  })),
]
// Certificate tasks are named by their operation (签发 / 续期).
const typeOptions = Object.entries({
  ...taskTypeLabels,
  certificate: '证书签发 / 续期',
}).map(([id, label]) => ({ id, label }))

export function TasksPage() {
  const list = useListSearch(route)
  const { search } = list
  const navigate = route.useNavigate()
  const openTask = search.task ? parseTaskKey(search.task) : undefined
  // An open detail sheet takes over the page's only live stream; the list
  // keeps its last snapshot underneath and recovers when the sheet closes.
  const query = useQuery({
    ...tasksQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      status: search.status,
      type: search.type,
    }),
    placeholderData: keepPreviousData,
    enabled: !openTask,
  })
  const filtered = hasListFilters(search, taskFilterKeys)
  const openSheet = (task: string | undefined) =>
    navigate({
      search: (previous) => ({ ...previous, task }),
      replace: task === undefined,
    })
  const columns: DataGridColumn<Task>[] = [
    {
      id: 'task',
      header: '任务',
      isRowHeader: true,
      minWidth: 280,
      cell: (task) => <TaskCell task={task} />,
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 240,
      cell: (task) => (
        <span className='flex min-w-0 items-center gap-2 whitespace-nowrap'>
          <StatusChip
            {...describeStatus(taskStatuses, task.status)}
            reserve={taskStatusReserve}
          />
          <TaskNoteSlot
            task={task}
            className='w-32 shrink-0 truncate text-xs leading-5'
          />
        </span>
      ),
    },
    {
      id: 'updated',
      header: '最近执行',
      minWidth: 110,
      cell: (task) => (
        <RelativeTime value={task.updated_at} className='text-sm text-muted' />
      ),
    },
    {
      id: 'failures',
      header: '失败次数',
      minWidth: 90,
      align: 'end',
      cell: (task) => (
        <span
          className={cn(
            'text-sm tabular-nums',
            task.failure_count > 0 ? 'text-danger' : 'text-muted'
          )}
        >
          {task.failure_count}
        </span>
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>详情</span>,
      width: 72,
      cell: (task) => (
        <Button
          size='sm'
          variant='tertiary'
          className='px-2'
          aria-label={`任务 ${taskTitle(task)} 的详情`}
          onPress={() => void openSheet(taskKey(taskRefOf(task)))}
        >
          详情
        </Button>
      ),
    },
  ]
  return (
    <Page title='任务中心'>
      <div className='grid grid-cols-1 gap-3 sm:grid-cols-2'>
        <TaskKpi
          title='进行中'
          icon={<ListCheck />}
          value={query.data?.active}
        />
        <TaskKpi
          title='失败'
          icon={<CircleExclamation />}
          status='danger'
          value={query.data?.failed}
        />
      </div>
      <div className='flex flex-col gap-3'>
        <ListToolbar
          search={{
            value: list.keyword,
            onChange: (q) => void list.setSearch({ q }),
            placeholder: '搜索对象名称或 ID',
          }}
          filters={
            <>
              <FilterSelect
                label='状态'
                value={search.status ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    status: (value || undefined) as TasksSearch['status'],
                  })
                }
                options={statusOptions}
              />
              <FilterSelect
                label='类型'
                value={search.type ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    type: (value || undefined) as TasksSearch['type'],
                  })
                }
                options={typeOptions}
              />
            </>
          }
          hasFilters={filtered}
          onClearFilters={() => void list.clearFilters(taskFilterKeys)}
          onRefresh={() => query.refetch()}
          isRefreshing={query.isFetching && !query.isPending}
        />
        <DataTable
          label='任务'
          columns={columns}
          data={query.data?.list}
          getRowId={(task) => taskKey(taskRefOf(task))}
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={() => query.refetch()}
          minWidth={820}
          empty={filtered ? { title: '没有匹配的任务' } : { title: '暂无任务' }}
          footer={
            <TablePagination
              page={list.page}
              pageSize={list.pageSize}
              total={query.data?.total ?? 0}
              onPageChange={(page) => void list.setPage(page)}
              onPageSizeChange={(size) => void list.setPageSize(size)}
            />
          }
        />
      </div>
      {openTask && (
        <TaskDetailSheet
          key={search.task}
          task={openTask}
          onClose={() => void openSheet(undefined)}
        />
      )}
    </Page>
  )
}

/** `类型 · 对象` on one line: the type is plain text (the 「详情」 button
 * opens the sheet), the object links to its resource page. */
function TaskCell({ task }: { task: Task }) {
  const target = taskTarget(task)
  return (
    <span className='flex min-w-0 items-center gap-1.5 text-sm whitespace-nowrap'>
      <span className='shrink-0 font-medium text-foreground'>
        {taskTypeLabel(task)}
      </span>
      <span aria-hidden className='text-muted'>
        ·
      </span>
      {target ? (
        <HeroLink
          href={target}
          className='min-w-0 truncate text-sm no-underline hover:underline'
        >
          {task.name}
        </HeroLink>
      ) : (
        <span className='min-w-0 truncate text-muted'>{task.name}</span>
      )}
    </span>
  )
}

function TaskKpi({
  title,
  icon,
  status,
  value,
}: {
  title: string
  icon: ReactNode
  status?: 'danger'
  value: number | undefined
}): ReactNode {
  return (
    <KPI className='min-w-0 flex-row items-center justify-between gap-4 px-4 py-3'>
      <KPI.Header className='mb-0 min-w-0'>
        <KPI.Icon
          status={status}
          className={cn('size-7', !status && 'bg-default text-muted')}
        >
          {icon}
        </KPI.Icon>
        <KPI.Title className='whitespace-nowrap'>{title}</KPI.Title>
      </KPI.Header>
      <KPI.Content className='min-h-8 flex-none'>
        {value === undefined ? (
          <Skeleton className='h-7 w-16 rounded-md' />
        ) : (
          <KPI.Value
            className={cn(status && value > 0 && 'text-danger')}
            value={value}
            maximumFractionDigits={0}
          />
        )}
      </KPI.Content>
    </KPI>
  )
}
