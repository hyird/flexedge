import type { ReactNode } from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { CircleExclamation, ListCheck } from '@gravity-ui/icons'
import { KPIGroup, type DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Link as HeroLink, Skeleton } from '@heroui/react'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { cn } from '@/lib/utils'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Page } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { operationsQuery } from './data'
import { taskFilterKeys, type TasksSearch } from './search'
import { TaskDetailSheet } from './task-detail-sheet'
import {
  actionLabel,
  actorLabel,
  describeOperationStatus,
  operationActions,
  operationActorLabels,
  operationStatusGroups,
  operationStatuses,
  operationStatusLabels,
  operationTarget,
  operationTargetTypeLabels,
  operationTitle,
} from './task-display'
import type { Operation } from './types'

const route = getRouteApi('/_authenticated/tasks/')

const optionsOf = (labels: Record<string, string>) =>
  Object.entries(labels).map(([id, label]) => ({ id, label }))

const statusOptions = [
  ...optionsOf(operationStatusGroups),
  ...Object.entries(operationStatuses).map(([id, status]) => ({
    id,
    label: status.label,
  })),
]

export function TasksPage() {
  const list = useListSearch(route)
  const { search } = list
  const navigate = route.useNavigate()
  // The open detail sheet owns the page's only live stream; the list pauses
  // and keeps its last snapshot underneath.
  const query = useQuery({
    ...operationsQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      status: search.status,
      action: search.action,
      target_type: search.target_type,
      actor: search.actor,
      days: search.days,
    }),
    enabled: !search.task,
    placeholderData: keepPreviousData,
  })
  const filtered = hasListFilters(search, taskFilterKeys)
  const openSheet = (task: string | undefined) =>
    navigate({
      search: (previous) => ({ ...previous, task }),
      replace: task === undefined,
    })
  const columns: DataGridColumn<Operation>[] = [
    {
      id: 'operation',
      header: '操作',
      isRowHeader: true,
      minWidth: 260,
      cell: (operation) => <OperationCell operation={operation} />,
    },
    {
      id: 'actor',
      header: '发起人',
      minWidth: 110,
      cell: (operation) => (
        <span
          className={cn(
            'block truncate text-sm',
            operation.actor_type === 'system' && 'text-muted'
          )}
        >
          {actorLabel(operation) || '—'}
        </span>
      ),
    },
    {
      id: 'created',
      header: '时间',
      minWidth: 110,
      cell: (operation) => (
        <RelativeTime
          value={operation.created_at}
          className='text-sm text-muted'
        />
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 110,
      cell: (operation) => {
        const status = describeOperationStatus(operation.status)
        return (
          <StatusChip
            label={status.label}
            tone={status.tone}
            reserve={operationStatusLabels}
          />
        )
      },
    },
    {
      id: 'subtasks',
      header: '子任务',
      minWidth: 80,
      align: 'end',
      cell: (operation) => (
        <span className='text-sm tabular-nums'>
          <span
            className={cn(operation.failed_count > 0 && 'text-danger')}
            aria-label={
              operation.failed_count > 0
                ? `${operation.failed_count} 个失败或重试中`
                : undefined
            }
          >
            {operation.settled_count}
          </span>
          <span className='text-muted'>/{operation.subtask_count}</span>
        </span>
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>详情</span>,
      width: 72,
      cell: (operation) => (
        <Button
          size='sm'
          variant='tertiary'
          className='px-2'
          aria-label={`任务 ${operationTitle(operation)} 的详情`}
          onPress={() => void openSheet(operation.id)}
        >
          详情
        </Button>
      ),
    },
  ]
  return (
    <Page title='任务中心'>
      <KPIGroup className='max-sm:flex-col'>
        <TaskKpi
          title='进行中'
          icon={<ListCheck />}
          value={query.data?.active}
        />
        <KPIGroup.Separator />
        <TaskKpi
          title='需关注'
          icon={<CircleExclamation />}
          status='warning'
          value={query.data?.attention}
        />
      </KPIGroup>
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
                label='操作'
                value={search.action ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    action: (value || undefined) as TasksSearch['action'],
                  })
                }
                options={optionsOf(operationActions)}
              />
              <FilterSelect
                label='对象'
                value={search.target_type ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    target_type: (value ||
                      undefined) as TasksSearch['target_type'],
                  })
                }
                options={optionsOf(operationTargetTypeLabels)}
              />
              <FilterSelect
                label='发起人'
                value={search.actor ?? ''}
                onChange={(value) =>
                  void list.setSearch({
                    actor: (value || undefined) as TasksSearch['actor'],
                  })
                }
                options={optionsOf(operationActorLabels)}
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
      {search.task && (
        <TaskDetailSheet
          key={search.task}
          operationId={search.task}
          onClose={() => void openSheet(undefined)}
        />
      )}
    </Page>
  )
}

/** `动作 · 对象` on one line: the action is plain text (the 「详情」 button
 * opens the sheet), the object links to its resource unless deleted. */
function OperationCell({ operation }: { operation: Operation }) {
  const target = operationTarget(operation)
  return (
    <span className='flex min-w-0 items-center gap-1.5 text-sm whitespace-nowrap'>
      <span className='shrink-0 font-medium text-foreground'>
        {actionLabel(operation.action)}
      </span>
      <span aria-hidden className='text-muted'>
        ·
      </span>
      {target ? (
        <HeroLink
          href={target.href}
          routerOptions={target.state && { state: target.state }}
          className='min-w-0 truncate text-sm no-underline hover:underline'
        >
          {operation.target_name}
        </HeroLink>
      ) : (
        <span className='min-w-0 truncate text-muted'>
          {operation.target_name}
        </span>
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
  status?: 'warning'
  value: number | undefined
}): ReactNode {
  return (
    <KPI className='flex-1'>
      <KPI.Header>
        <KPI.Icon status={status}>{icon}</KPI.Icon>
        <KPI.Title>{title}</KPI.Title>
      </KPI.Header>
      <KPI.Content>
        {value === undefined ? (
          <Skeleton className='h-7 w-16 rounded-md' />
        ) : (
          <KPI.Value value={value} maximumFractionDigits={0} />
        )}
      </KPI.Content>
    </KPI>
  )
}
