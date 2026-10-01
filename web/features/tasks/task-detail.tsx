import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { Button, Tabs } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { Drawer } from '@/components/drawer'
import { Notice } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { taskDetailQuery, taskHistoryQuery } from './data'
import { resourceLinks, taskStatuses, taskTitle } from './task-display'
import type { Task } from './types'

export function TaskDetail({
  task,
  onClose,
}: {
  task: Task
  onClose: () => void
}) {
  const [tab, setTab] = useState('detail')
  const query = useQuery({
    ...taskDetailQuery(task),
    enabled: tab === 'detail',
  })
  const history = useQuery({
    ...taskHistoryQuery(task),
    enabled: tab === 'history' && task.resource_type !== 'node',
  })
  const navigate = useNavigate()
  const current = query.data ?? task
  return (
    <Drawer
      title={`${taskTitle(current)} · ${current.name}`}
      onClose={onClose}
      size='md'
    >
      <Tabs selectedKey={tab} onSelectionChange={(key) => setTab(String(key))}>
        <Tabs.ListContainer>
          <Tabs.List aria-label='任务详情'>
            <Tabs.Tab id='detail'>
              状态
              <Tabs.Indicator />
            </Tabs.Tab>
            {task.resource_type !== 'node' && (
              <Tabs.Tab id='history'>
                执行记录
                <Tabs.Indicator />
              </Tabs.Tab>
            )}
          </Tabs.List>
        </Tabs.ListContainer>
        <Tabs.Panel id='detail' className='grid gap-4 pt-4'>
          <QueryNotice query={query} onRetry={() => void query.refetch()} />
          <StatusChip
            status={current.status}
            label={taskStatuses[current.status]}
          />
          <dl className='grid gap-3 text-sm sm:grid-cols-2'>
            {[
              ['关联资源', current.name],
              ['资源 ID', current.resource_id],
              ['目标版本', String(current.version)],
              ['失败次数', String(current.failures)],
              ['最近更新', formatDate(current.updated_at)],
              ['下次尝试', formatDate(current.next_attempt_at)],
            ].map(([label, value]) => (
              <div key={label}>
                <dt className='text-xs text-muted'>{label}</dt>
                <dd className='mt-1 break-all'>{value}</dd>
              </div>
            ))}
          </dl>
          {current.error && <Notice>{current.error}</Notice>}
          {(current.status === 'retrying' || current.status === 'failed') && (
            <p className='text-sm text-muted'>
              服务端按任务规则自动重试；可前往关联资源检查配置并重新提交操作。
            </p>
          )}
          <div className='flex justify-end'>
            <Button
              size='sm'
              variant='secondary'
              onPress={() => {
                onClose()
                void navigate({ to: resourceLinks[current.resource_type] })
              }}
            >
              前往关联资源
            </Button>
          </div>
        </Tabs.Panel>
        {task.resource_type !== 'node' && (
          <Tabs.Panel id='history' className='grid gap-3 pt-4'>
            <QueryNotice
              query={history}
              onRetry={() => void history.refetch()}
            />
            {history.isPending && (
              <p role='status' className='text-sm text-muted'>
                正在加载执行记录…
              </p>
            )}
            {history.isSuccess && !history.data.list.length && (
              <p className='text-sm text-muted'>暂无保留的执行记录。</p>
            )}
            <ol className='grid gap-3'>
              {history.data?.list.map((attempt, index) => (
                <li
                  key={`${attempt.emitted_at}-${index}`}
                  className='grid gap-2 rounded-xl bg-surface-secondary p-3 text-sm'
                >
                  <div className='flex items-center justify-between gap-2'>
                    <StatusChip
                      status={attempt.outcome}
                      label={
                        attempt.outcome === 'completed'
                          ? '执行成功'
                          : '尝试失败'
                      }
                    />
                    <time className='text-xs text-muted'>
                      {formatDate(attempt.emitted_at)}
                    </time>
                  </div>
                  {attempt.error && (
                    <p className='break-words whitespace-pre-wrap text-muted'>
                      {attempt.error}
                    </p>
                  )}
                </li>
              ))}
            </ol>
            {history.data?.truncated && (
              <p className='text-xs text-muted'>仅展示最近 100 次尝试。</p>
            )}
            <div className='flex justify-end'>
              <Button
                size='sm'
                variant='ghost'
                isDisabled={history.isFetching}
                onPress={() => void history.refetch()}
              >
                刷新记录
              </Button>
            </div>
          </Tabs.Panel>
        )}
      </Tabs>
    </Drawer>
  )
}
