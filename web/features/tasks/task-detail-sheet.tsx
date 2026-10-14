import { useEffect, useEffectEvent } from 'react'
import { useQuery } from '@tanstack/react-query'
import { ArrowRotateLeft } from '@gravity-ui/icons'
import { Timeline } from '@heroui-pro/react'
import { Alert, Button, Link, Skeleton, Tabs } from '@heroui/react'
import { ApiBusinessError } from '@/lib/api-response'
import { formatDate } from '@/lib/format'
import { CopyButton } from '@/components/copy-button'
import { FormSheet } from '@/components/form-sheet'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import {
  latestTaskVersionQuery,
  taskDetailQuery,
  taskHistoryQuery,
} from './data'
import {
  taskResourceHref,
  taskStatuses,
  taskStatusTones,
  taskTitle,
  taskTypes,
} from './task-display'
import type { Task, TaskHistory } from './types'

type TaskSheetTab = 'detail' | 'history'

/** Task detail opened from `/tasks?task=&resource_id=&version=`. Subscribes to
 * the task's detail stream while open; execution history is a one-shot
 * snapshot read when its tab is shown. */
export function TaskDetailSheet({
  selection,
  version,
  tab,
  onTabChange,
  onVersionResolved,
  onClose,
}: {
  selection: Pick<Task, 'id' | 'resource_id'>
  version: number | undefined
  tab: TaskSheetTab
  onTabChange: (tab: TaskSheetTab) => void
  onVersionResolved: (version: number) => void
  onClose: () => void
}) {
  // Errors render inside the sheet (not-found state or QueryNotice).
  const latest = useQuery({
    ...latestTaskVersionQuery(version ? null : selection),
    meta: { inlineError: true },
  })
  const resolveVersion = useEffectEvent(onVersionResolved)
  useEffect(() => {
    if (latest.data) resolveVersion(latest.data)
  }, [latest.data])
  const detail = useQuery({
    ...taskDetailQuery(version ? { ...selection, version } : null),
    meta: { inlineError: true },
  })
  const task = detail.data
  const showsHistory = !!task && task.resource_type !== 'node'
  const history = useQuery({
    ...taskHistoryQuery(task ?? null),
    enabled: showsHistory && tab === 'history',
    meta: { inlineError: true },
  })
  const missing =
    latest.data === null || detail.error instanceof ApiBusinessError
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      size='md'
      title={task ? `${taskTitle(task)} · ${task.name}` : '任务详情'}
      description={
        task && (
          <span className='flex flex-wrap items-center gap-2'>
            <StatusChip
              status={task.status}
              label={taskStatuses[task.status]}
              tone={taskStatusTones[task.status]}
            />
            <Mono className='text-muted'>v{task.version}</Mono>
          </span>
        )
      }
      footer={
        <>
          <Button size='sm' variant='tertiary' onPress={onClose}>
            关闭
          </Button>
          {task && (
            <LinkButton href={taskResourceHref(task)}>前往关联资源</LinkButton>
          )}
        </>
      }
    >
      {missing ? (
        <EmptyState
          title='任务不存在或记录已过期'
          description={
            detail.error instanceof ApiBusinessError
              ? detail.error.message
              : '执行记录只保留 7 天，请在列表中查看最近的任务。'
          }
        />
      ) : (
        <>
          <QueryNotice query={latest} onRetry={() => latest.refetch()} />
          <QueryNotice query={detail} onRetry={() => detail.refetch()} />
          {!task ? (
            !detail.isError &&
            !latest.isError && (
              <div
                role='status'
                aria-label='正在加载任务详情'
                className='flex flex-col gap-3'
              >
                <Skeleton className='h-9 w-48 rounded-lg' />
                {Array.from({ length: 4 }, (_, index) => (
                  <Skeleton key={index} className='h-10 w-full rounded-lg' />
                ))}
              </div>
            )
          ) : (
            <Tabs
              selectedKey={showsHistory ? tab : 'detail'}
              onSelectionChange={(key) => onTabChange(key as TaskSheetTab)}
            >
              <Tabs.ListContainer>
                <Tabs.List aria-label='任务详情'>
                  <Tabs.Tab id='detail'>
                    状态
                    <Tabs.Indicator />
                  </Tabs.Tab>
                  {showsHistory && (
                    <Tabs.Tab id='history'>
                      执行记录
                      <Tabs.Indicator />
                    </Tabs.Tab>
                  )}
                </Tabs.List>
              </Tabs.ListContainer>
              <Tabs.Panel id='detail' className='flex flex-col gap-4 pt-4'>
                <TaskStatus task={task} />
              </Tabs.Panel>
              {showsHistory && (
                <Tabs.Panel id='history' className='flex flex-col gap-3 pt-4'>
                  <QueryNotice
                    query={history}
                    onRetry={() => history.refetch()}
                  />
                  <TaskAttempts history={history.data} />
                  <div className='flex justify-end'>
                    <Button
                      size='sm'
                      variant='tertiary'
                      isPending={history.isFetching}
                      onPress={() => void history.refetch()}
                    >
                      <ArrowRotateLeft className='size-4' />
                      刷新记录
                    </Button>
                  </div>
                </Tabs.Panel>
              )}
            </Tabs>
          )}
        </>
      )}
    </FormSheet>
  )
}

function TaskStatus({ task }: { task: Task }) {
  return (
    <>
      {task.error && (
        <Alert status={task.status === 'failed' ? 'danger' : 'warning'}>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>最近一次错误</Alert.Title>
            <Alert.Description className='break-words whitespace-pre-wrap'>
              {task.error}
            </Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      <DescriptionList
        items={[
          {
            label: '关联资源',
            value: <Link href={taskResourceHref(task)}>{task.name}</Link>,
          },
          { label: '任务类型', value: taskTypes[task.resource_type] },
          {
            label: '资源 ID',
            wide: true,
            value: (
              <span className='flex items-center gap-1'>
                <Mono truncate>{task.resource_id}</Mono>
                <CopyButton value={task.resource_id} label='复制资源 ID' />
              </span>
            ),
          },
          { label: '目标版本', value: <Mono>v{task.version}</Mono> },
          {
            label: '失败次数',
            value: <span className='tabular-nums'>{task.failures}</span>,
          },
          {
            label: '最近更新',
            value: (
              <span className='flex flex-col'>
                {formatDate(task.updated_at)}
                <RelativeTime
                  value={task.updated_at}
                  className='text-xs text-muted'
                />
              </span>
            ),
          },
          {
            label: '下次尝试',
            value: task.next_attempt_at ? (
              <span className='flex flex-col'>
                {formatDate(task.next_attempt_at)}
                <RelativeTime
                  value={task.next_attempt_at}
                  className='text-xs text-muted'
                />
              </span>
            ) : (
              '—'
            ),
          },
        ]}
      />
      {(task.status === 'retrying' || task.status === 'failed') && (
        <p className='text-[13px] text-muted'>
          服务端按任务规则自动重试；可前往关联资源检查配置并重新提交操作。
        </p>
      )}
    </>
  )
}

function TaskAttempts({ history }: { history: TaskHistory | undefined }) {
  if (!history)
    return (
      <div
        role='status'
        aria-label='正在加载执行记录'
        className='flex flex-col gap-3'
      >
        {Array.from({ length: 3 }, (_, index) => (
          <Skeleton key={index} className='h-12 w-full rounded-lg' />
        ))}
      </div>
    )
  if (!history.list.length)
    return (
      <EmptyState
        size='sm'
        title='暂无执行记录'
        description='任务开始执行后，每次尝试的结果会记录在这里。'
      />
    )
  return (
    <>
      <Timeline density='compact' size='sm'>
        {history.list.map((attempt, index) => (
          <Timeline.Item
            key={`${attempt.emitted_at}-${index}`}
            status={attempt.outcome === 'completed' ? 'success' : 'danger'}
          >
            <Timeline.Marker />
            <Timeline.Content className='gap-1'>
              <div className='flex min-w-0 flex-wrap items-center justify-between gap-x-3 gap-y-1'>
                <h3 className='m-0 text-sm font-medium text-foreground'>
                  {attempt.outcome === 'completed' ? '执行成功' : '尝试失败'}
                </h3>
                <time
                  dateTime={attempt.emitted_at}
                  className='text-xs text-muted'
                >
                  {formatDate(attempt.emitted_at)}
                </time>
              </div>
              {attempt.error && (
                <p className='m-0 text-xs leading-5 break-words whitespace-pre-wrap text-muted'>
                  {attempt.error}
                </p>
              )}
            </Timeline.Content>
          </Timeline.Item>
        ))}
      </Timeline>
      {history.truncated && (
        <p className='text-xs text-muted'>仅展示最近 100 次尝试。</p>
      )}
    </>
  )
}
