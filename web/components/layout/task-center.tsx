import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { ArrowRight, ArrowRotateLeft, ListCheck } from '@gravity-ui/icons'
import { Sheet, useSidebar } from '@heroui-pro/react'
import { Button, Link, Popover, Skeleton, Tooltip } from '@heroui/react'
import { streamPath } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import { formatRelative, formatUtcDate } from '@/lib/format'
import { snapshotQueryOptions } from '@/lib/live-query'
import { EmptyState, QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import {
  activeTaskStatuses,
  taskHref,
  taskStatusLabels,
  taskStatuses,
  taskStatusTones,
  taskTitle,
} from '@/features/tasks/task-display'
import { taskPageSchema } from '@/features/tasks/types'

const visibleTaskCount = 6

/** Navbar entry showing recent tasks. Opening the popover (or the bottom sheet
 * on narrow screens) reads one snapshot; it never holds a subscription, so the
 * page keeps at most its own live stream. “刷新” reads a new snapshot. */
const recentTasksQuery = snapshotQueryOptions(
  streamPath('/tasks', { page: 1, page_size: 20 }),
  ['task-center', 'recent'],
  (value) => {
    const result = taskPageSchema.safeParse(value)
    if (!result.success) throw new ApiProtocolError()
    return result.data
  }
)

export function TaskCenter() {
  const [isOpen, setOpen] = useState(false)
  const { isMobile } = useSidebar()
  const trigger = (
    <Tooltip delay={300}>
      <Button
        size='sm'
        variant='ghost'
        isIconOnly
        aria-label='任务中心'
        onPress={() => setOpen(true)}
      >
        <ListCheck className='size-4' />
      </Button>
      <Tooltip.Content>任务中心</Tooltip.Content>
    </Tooltip>
  )
  if (isMobile)
    return (
      <>
        {trigger}
        <Sheet isOpen={isOpen} onOpenChange={setOpen} placement='bottom'>
          <Sheet.Backdrop>
            <Sheet.Content>
              <Sheet.Dialog>
                <Sheet.Handle />
                <Sheet.Header>
                  <Sheet.Heading>最近任务</Sheet.Heading>
                </Sheet.Header>
                {/* Contains absolutely positioned descendants (see FormSheet). */}
                <Sheet.Body className='relative pb-6'>
                  <RecentTasks onNavigate={() => setOpen(false)} />
                </Sheet.Body>
              </Sheet.Dialog>
            </Sheet.Content>
          </Sheet.Backdrop>
        </Sheet>
      </>
    )
  return (
    <Popover isOpen={isOpen} onOpenChange={setOpen}>
      {trigger}
      <Popover.Content
        placement='bottom end'
        className='w-96 max-w-[calc(100vw-2rem)]'
      >
        <Popover.Dialog
          aria-label='最近任务'
          className='flex flex-col gap-3 p-1'
        >
          <Popover.Heading className='px-2 pt-2 text-sm font-semibold'>
            最近任务
          </Popover.Heading>
          <RecentTasks onNavigate={() => setOpen(false)} />
        </Popover.Dialog>
      </Popover.Content>
    </Popover>
  )
}

function RecentTasks({ onNavigate }: { onNavigate: () => void }) {
  const query = useQuery(recentTasksQuery)
  const tasks = [...(query.data?.list ?? [])]
    .sort(
      (left, right) =>
        Number(activeTaskStatuses.includes(right.status)) -
          Number(activeTaskStatuses.includes(left.status)) ||
        Date.parse(right.updated_at) - Date.parse(left.updated_at)
    )
    .slice(0, visibleTaskCount)
  return (
    <div className='flex flex-col gap-2'>
      <div className='flex items-center justify-between gap-2 px-2 text-xs text-muted'>
        <span>打开时的快照</span>
        <Tooltip delay={300}>
          <Button
            size='sm'
            variant='tertiary'
            isIconOnly
            aria-label='刷新任务'
            isPending={query.isFetching}
            onPress={() => void query.refetch()}
          >
            <ArrowRotateLeft className='size-4' />
          </Button>
          <Tooltip.Content>刷新任务</Tooltip.Content>
        </Tooltip>
      </div>
      <QueryNotice query={query} onRetry={() => void query.refetch()} />
      {query.isPending ? (
        <div
          role='status'
          aria-label='正在加载任务'
          className='flex flex-col gap-2 px-2'
        >
          {Array.from({ length: 4 }, (_, index) => (
            <Skeleton key={index} className='h-11 w-full rounded-lg' />
          ))}
        </div>
      ) : tasks.length === 0 && !query.isError ? (
        <EmptyState
          size='sm'
          title='暂无任务'
          description='DNS 同步、证书签发和节点发布任务会显示在这里。'
        />
      ) : (
        <ul className='flex flex-col'>
          {tasks.map((task) => (
            <li key={`${task.id}:${task.resource_id}:${task.version}`}>
              <Link
                href={taskHref(task)}
                onPress={onNavigate}
                className='flex w-full items-center gap-3 rounded-lg px-2 py-2 no-underline hover:bg-default'
              >
                <span className='min-w-0 flex-1 truncate text-sm font-medium text-foreground'>
                  {taskTitle(task)}
                </span>
                <span className='flex shrink-0 flex-col items-end gap-1'>
                  <StatusChip
                    status={task.status}
                    label={taskStatuses[task.status]}
                    tone={taskStatusTones[task.status]}
                    reserve={taskStatusLabels}
                  />
                  <time
                    dateTime={task.updated_at}
                    title={formatUtcDate(task.updated_at)}
                    className='text-[11px] text-muted'
                  >
                    {formatRelative(task.updated_at)}
                  </time>
                </span>
              </Link>
            </li>
          ))}
        </ul>
      )}
      <div className='border-t border-separator px-2 pt-2'>
        <Link
          href='/tasks'
          onPress={onNavigate}
          className='flex items-center justify-center gap-1 py-1 text-sm no-underline'
        >
          查看全部
          <ArrowRight className='size-3.5' />
        </Link>
      </div>
    </div>
  )
}
