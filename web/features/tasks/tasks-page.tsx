import type { ReactNode } from 'react'
import { keepPreviousData, useQueries, useQuery } from '@tanstack/react-query'
import { getRouteApi, Link } from '@tanstack/react-router'
import { CircleCheck, CircleXmark, ListCheck } from '@gravity-ui/icons'
import { KPIGroup, type DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Link as HeroLink, Skeleton, Tooltip } from '@heroui/react'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { ActionMenu } from '@/components/action-menu'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Page } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { recentTaskCountQuery, tasksQuery } from './data'
import { taskFilterKeys, type TasksSearch } from './search'
import { TaskDetailSheet } from './task-detail-sheet'
import {
  taskKey,
  taskResourceHref,
  taskStatusLabels,
  taskStatuses,
  taskStatusTones,
  taskTitle,
  taskTypes,
} from './task-display'
import type { Task } from './types'

const route = getRouteApi('/_authenticated/tasks/')

const sheetClosed = {
  task: undefined,
  resource_id: undefined,
  version: undefined,
  tab: undefined,
} satisfies Partial<TasksSearch>

function sheetSearch(task: Task) {
  return {
    task: task.id,
    resource_id: task.resource_id,
    version: task.version,
    tab: undefined,
  } satisfies Partial<TasksSearch>
}

export function TasksPage() {
  const list = useListSearch(route)
  const { search } = list
  const navigate = route.useNavigate()
  const selection =
    search.task && search.resource_id
      ? { id: search.task, resource_id: search.resource_id }
      : null
  // The open detail sheet owns the page's only live stream; the list pauses
  // and keeps its last snapshot underneath.
  const query = useQuery({
    ...tasksQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      type: search.type,
      status: search.status,
      days: search.days,
    }),
    enabled: !selection,
    placeholderData: keepPreviousData,
  })
  const completed = useQueries({
    queries: (['completed', 'recovered'] as const).map((status) => ({
      ...recentTaskCountQuery(status, query.dataUpdatedAt),
      enabled: !selection && query.isSuccess,
      placeholderData: keepPreviousData,
    })),
    combine: (results) =>
      results.every((result) => result.data !== undefined)
        ? results.reduce((sum, result) => sum + (result.data ?? 0), 0)
        : undefined,
  })
  const filtered = hasListFilters(search, taskFilterKeys)
  const updateSheet = (patch: Partial<TasksSearch>, replace: boolean) =>
    navigate({
      search: (previous) => ({ ...previous, ...patch }),
      replace,
    })
  const columns: DataGridColumn<Task>[] = [
    {
      id: 'task',
      header: '任务',
      isRowHeader: true,
      minWidth: 180,
      cell: (task) => (
        <Link
          from='/tasks/'
          to='.'
          search={(previous) => ({ ...previous, ...sheetSearch(task) })}
          className='link font-medium'
        >
          {taskTitle(task)}
        </Link>
      ),
    },
    {
      id: 'resource',
      header: '关联资源',
      minWidth: 200,
      cell: (task) => (
        <div className='flex min-w-0 flex-col'>
          <HeroLink
            href={taskResourceHref(task)}
            className='truncate text-sm no-underline hover:underline'
          >
            {task.name}
          </HeroLink>
          <span className='text-xs text-muted'>
            {taskTypes[task.resource_type]}
          </span>
        </div>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 110,
      cell: (task) => {
        const chip = (
          <StatusChip
            status={task.status}
            label={taskStatuses[task.status]}
            tone={taskStatusTones[task.status]}
            reserve={taskStatusLabels}
          />
        )
        return task.error ? (
          <Tooltip delay={300}>
            <Tooltip.Trigger
              aria-label={`${taskStatuses[task.status]}：${task.error}`}
            >
              {chip}
            </Tooltip.Trigger>
            <Tooltip.Content className='max-w-80 whitespace-normal'>
              {task.error}
            </Tooltip.Content>
          </Tooltip>
        ) : (
          chip
        )
      },
    },
    {
      id: 'next_attempt',
      header: '下次尝试',
      minWidth: 110,
      cell: (task) =>
        task.status === 'retrying' && task.next_attempt_at ? (
          <RelativeTime
            value={task.next_attempt_at}
            className='text-sm text-muted'
          />
        ) : (
          <span className='text-muted'>—</span>
        ),
    },
    {
      id: 'failures',
      header: '失败次数',
      minWidth: 90,
      align: 'end',
      cell: (task) => <span className='tabular-nums'>{task.failures}</span>,
    },
    {
      id: 'updated',
      header: '最近更新',
      minWidth: 120,
      cell: (task) => (
        <RelativeTime value={task.updated_at} className='text-sm text-muted' />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      width: 56,
      align: 'end',
      cell: (task) => (
        <ActionMenu
          label={`任务 ${taskTitle(task)} · ${task.name} 的操作`}
          items={[
            {
              id: 'detail',
              label: '查看详情',
              onAction: () => void updateSheet(sheetSearch(task), false),
            },
            {
              id: 'resource',
              label: '前往关联资源',
              href: taskResourceHref(task),
            },
          ]}
        />
      ),
    },
  ]
  return (
    <Page
      title='任务中心'
      description='查看 DNS 同步、证书签发、服务商检测与节点发布的执行结果。失败任务按服务端规则自动重试，执行记录保留 7 天。'
    >
      <KPIGroup className='max-sm:flex-col'>
        <TaskKpi
          title='进行中'
          icon={<ListCheck />}
          value={query.data?.active}
        />
        <KPIGroup.Separator />
        <TaskKpi
          title='失败'
          icon={<CircleXmark />}
          status='danger'
          value={query.data?.failed}
        />
        <KPIGroup.Separator />
        <TaskKpi
          title='24 小时内完成'
          icon={<CircleCheck />}
          status='success'
          value={completed}
        />
      </KPIGroup>
      <div className='flex flex-col gap-3'>
        <ListToolbar
          search={{
            value: list.keyword,
            onChange: (q) => void list.setSearch({ q }),
            placeholder: '搜索资源名称或 ID',
          }}
          filters={
            <>
              <FilterSelect
                label='类型'
                value={search.type ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    type: (value || undefined) as TasksSearch['type'],
                  })
                }
                options={Object.entries(taskTypes).map(([id, label]) => ({
                  id,
                  label,
                }))}
              />
              <FilterSelect
                label='状态'
                value={search.status ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    status: (value || undefined) as TasksSearch['status'],
                  })
                }
                options={Object.entries(taskStatuses).map(([id, label]) => ({
                  id,
                  label,
                }))}
              />
              <FilterSelect
                label='时间'
                value={search.days ? String(search.days) : ''}
                allLabel='全部保留记录'
                onChange={(value) =>
                  void list.setSearch({
                    days: value ? (Number(value) as 1 | 7) : undefined,
                  })
                }
                options={[
                  { id: '1', label: '最近 24 小时' },
                  { id: '7', label: '最近 7 天' },
                ]}
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
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={() => query.refetch()}
          getRowId={taskKey}
          minWidth={900}
          empty={
            filtered
              ? {
                  title: '没有匹配的任务',
                  description: '调整筛选条件后重试。',
                }
              : {
                  title: '暂无任务',
                  description:
                    '在网站、证书、DNS 区域或节点上提交操作后，执行结果会显示在这里。',
                }
          }
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
      {selection && (
        <TaskDetailSheet
          selection={selection}
          version={search.version}
          tab={search.tab ?? 'detail'}
          onTabChange={(tab) =>
            void updateSheet(
              { tab: tab === 'history' ? 'history' : undefined },
              true
            )
          }
          onVersionResolved={(version) => void updateSheet({ version }, true)}
          onClose={() => void updateSheet(sheetClosed, true)}
        />
      )}
    </Page>
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
  status?: 'success' | 'danger'
  value: number | undefined
}) {
  return (
    <KPI className='flex-1'>
      <KPI.Header>
        <KPI.Icon status={status}>{icon}</KPI.Icon>
        <KPI.Title>{title}</KPI.Title>
      </KPI.Header>
      <KPI.Content>
        {value === undefined ? (
          <Skeleton className='h-8 w-16 rounded-md' />
        ) : (
          <KPI.Value value={value} maximumFractionDigits={0} />
        )}
      </KPI.Content>
    </KPI>
  )
}
