import { useEffect, useEffectEvent, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
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
import { latestTaskVersionQuery, taskDetailQuery } from './data'
import { RedactedText } from './task-attempt-trace'
import {
  taskResourceTarget,
  taskStatusLabels,
  taskStatuses,
  taskStatusTones,
  taskTitle,
  taskTypes,
} from './task-display'
import { TaskHistoryPanel } from './task-history-panel'
import type { Task } from './types'

type TaskSheetTab = 'detail' | 'history'

/** Task detail opened from `/tasks?task=&resource_id=&version=`. Subscribes to
 * the task's detail stream while open; execution history is a one-shot
 * snapshot read when its tab is shown. The tab is component state: switching
 * it never changes the URL. */
export function TaskDetailSheet({
  selection,
  version,
  onVersionResolved,
  onClose,
}: {
  selection: Pick<Task, 'id' | 'resource_id'>
  version: number | undefined
  onVersionResolved: (version: number) => void
  onClose: () => void
}) {
  const [tab, setTab] = useState<TaskSheetTab>('detail')
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
  const missing =
    latest.data === null || detail.error instanceof ApiBusinessError
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      size='lg'
      title={task ? `${taskTitle(task)} · ${task.name}` : '任务详情'}
      description={
        task && (
          <StatusChip
            status={task.status}
            label={taskStatuses[task.status]}
            tone={taskStatusTones[task.status]}
            reserve={taskStatusLabels}
          />
        )
      }
      footer={
        <>
          <Button size='sm' variant='tertiary' onPress={onClose}>
            关闭
          </Button>
          {task && (
            <LinkButton {...taskResourceTarget(task)}>前往关联资源</LinkButton>
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
              selectedKey={tab}
              onSelectionChange={(key) => setTab(key as TaskSheetTab)}
            >
              <Tabs.ListContainer>
                <Tabs.List aria-label='任务详情'>
                  <Tabs.Tab id='detail'>
                    状态
                    <Tabs.Indicator />
                  </Tabs.Tab>
                  <Tabs.Tab id='history'>
                    执行记录
                    <Tabs.Indicator />
                  </Tabs.Tab>
                </Tabs.List>
              </Tabs.ListContainer>
              <Tabs.Panel id='detail' className='flex flex-col gap-4 pt-4'>
                <TaskStatus task={task} />
              </Tabs.Panel>
              <Tabs.Panel id='history' className='flex flex-col gap-4 pt-4'>
                <TaskHistoryPanel task={task} />
              </Tabs.Panel>
            </Tabs>
          )}
        </>
      )}
    </FormSheet>
  )
}

function TaskStatus({ task }: { task: Task }) {
  const target = taskResourceTarget(task)
  return (
    <>
      {task.error && (
        <Alert status={task.status === 'failed' ? 'danger' : 'warning'}>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>最近一次错误</Alert.Title>
            <Alert.Description className='break-words whitespace-pre-wrap'>
              <RedactedText value={task.error} />
            </Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      <DescriptionList
        items={[
          {
            label: '关联资源',
            value: (
              <Link
                href={target.href}
                routerOptions={target.state && { state: target.state }}
              >
                {task.name}
              </Link>
            ),
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
          {
            label: '失败次数',
            value: <span className='tabular-nums'>{task.failures}</span>,
          },
          {
            label: '最近更新',
            value: (
              <span className='flex flex-wrap gap-x-2'>
                {formatDate(task.updated_at)}
                <RelativeTime value={task.updated_at} className='text-muted' />
              </span>
            ),
          },
          {
            label: '下次尝试',
            value: task.next_attempt_at ? (
              <span className='flex flex-wrap gap-x-2'>
                {formatDate(task.next_attempt_at)}
                <RelativeTime
                  value={task.next_attempt_at}
                  className='text-muted'
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
