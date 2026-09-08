import { useQuery } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { getData, apiErrorMessage } from '@/lib/api'
import { formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import { Button } from '@/components/ui/button'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { Skeleton } from '@/components/ui/skeleton'
import { resourceLinks, taskTitle, type Task, type TaskHistory } from './data'
import { TaskStatus } from './task-status'

export function TaskDetail({
  task: selected,
  onClose,
}: {
  task: Task | null
  onClose: () => void
}) {
  const latest = useQuery({
    queryKey: [
      ...queryKeys.tasks,
      'detail',
      selected?.id,
      selected?.resource_id,
      selected?.version,
    ],
    enabled: Boolean(selected),
    queryFn: () =>
      getData<Task>(`/tasks/${selected!.id}`, {
        resource_id: selected!.resource_id,
        version: selected!.version,
      }),
  })
  const task = latest.data ?? selected
  const history = useQuery({
    queryKey: [...queryKeys.tasks, 'history', task?.id, task?.version],
    enabled: Boolean(task && task.resource_type !== 'node'),
    queryFn: () =>
      getData<TaskHistory>(`/tasks/${task!.id}/history`, {
        version: task!.version,
      }),
  })
  return (
    <Sheet
      open={Boolean(task)}
      onOpenChange={(open) => {
        if (!open) onClose()
      }}
    >
      <SheetContent className='w-full overflow-y-auto sm:max-w-lg'>
        <SheetHeader>
          <SheetTitle>任务详情</SheetTitle>
          <SheetDescription>
            {task ? `${taskTitle(task)} · ${task.name}` : '任务执行记录'}
          </SheetDescription>
        </SheetHeader>
        {task && (
          <div className='space-y-6 px-4 pb-6'>
            {latest.isError && (
              <div role='alert' className='text-sm'>
                <p>最新状态读取失败：{apiErrorMessage(latest.error)}</p>
                <Button
                  variant='outline'
                  size='sm'
                  className='mt-2'
                  onClick={() => void latest.refetch()}
                >
                  刷新详情
                </Button>
              </div>
            )}
            <div className='flex items-center justify-between gap-3'>
              <TaskStatus status={task.status} />
              <span className='text-sm text-muted-foreground'>
                版本 {task.version}
              </span>
            </div>
            <dl className='space-y-3 text-sm'>
              <div>
                <dt className='text-muted-foreground'>关联对象</dt>
                <dd className='mt-1 break-all'>{task.name}</dd>
              </div>
              <div>
                <dt className='text-muted-foreground'>资源 ID</dt>
                <dd className='mt-1 font-mono text-xs break-all'>
                  {task.resource_id}
                </dd>
              </div>
              <div>
                <dt className='text-muted-foreground'>最近更新</dt>
                <dd className='mt-1'>{formatDate(task.updated_at)}</dd>
              </div>
              {task.next_attempt_at && (
                <div>
                  <dt className='text-muted-foreground'>下次重试时间</dt>
                  <dd className='mt-1'>{formatDate(task.next_attempt_at)}</dd>
                </div>
              )}
            </dl>
            {task.status === 'retrying' && (
              <p className='rounded-md border bg-muted/40 p-3 text-sm'>
                系统正在自动重试，无需重复提交。
                {task.resource_type === 'node' &&
                  '节点恢复连接后会重新核对目标配置。'}
              </p>
            )}
            {task.status === 'recovered' && (
              <p className='rounded-md border bg-muted/40 p-3 text-sm'>
                此前的失败已恢复，当前版本已完成。
              </p>
            )}
            {task.status === 'superseded' && (
              <p className='text-sm text-muted-foreground'>
                此任务已被后续版本替代、移除或排除，不再表示当前资源状态。
              </p>
            )}
            {task.error && (
              <div className='rounded-md border p-3'>
                <p className='mb-2 text-sm font-medium'>最近错误</p>
                <p className='text-sm break-words whitespace-pre-wrap text-muted-foreground'>
                  {task.error}
                </p>
              </div>
            )}
            <section className='space-y-3' aria-label='执行记录'>
              <h3 className='text-sm font-semibold'>执行记录</h3>
              {task.resource_type === 'node' ? (
                <p className='text-sm text-muted-foreground'>
                  展示该节点对此发布版本的最新应用结果；尚未确认的发布保持排队状态。
                </p>
              ) : history.isPending ? (
                <Skeleton className='h-20 w-full' />
              ) : history.isError ? (
                <div role='alert' className='space-y-2 text-sm'>
                  <p>{apiErrorMessage(history.error)}</p>
                  <Button
                    variant='outline'
                    size='sm'
                    onClick={() => void history.refetch()}
                  >
                    重新加载记录
                  </Button>
                </div>
              ) : (
                <>
                  {!history.data?.list.length && (
                    <p className='text-sm text-muted-foreground'>
                      暂无保留的执行结果，任务可能尚未完成首次尝试。
                    </p>
                  )}
                  <ol className='space-y-3'>
                    {history.data?.list.map((attempt, index) => (
                      <li
                        key={`${attempt.emitted_at}-${index}`}
                        className='rounded-md border p-3 text-sm'
                      >
                        <div className='flex flex-wrap items-center justify-between gap-2'>
                          <span className='font-medium'>
                            {attempt.outcome === 'completed'
                              ? '执行成功'
                              : '本次尝试失败'}
                          </span>
                          <time className='text-xs text-muted-foreground'>
                            {formatDate(attempt.emitted_at)}
                          </time>
                        </div>
                        {attempt.outcome === 'failed' && (
                          <p className='mt-2 break-words whitespace-pre-wrap text-muted-foreground'>
                            {attempt.error || '此历史记录未保存错误详情。'}
                          </p>
                        )}
                      </li>
                    ))}
                  </ol>
                  {history.data?.truncated && (
                    <p className='text-xs text-muted-foreground'>
                      仅展示最近 100 次尝试。
                    </p>
                  )}
                  <p className='text-xs text-muted-foreground'>
                    同步执行记录保留 7 天，按最近时间排序。
                  </p>
                </>
              )}
            </section>
            <Button asChild variant='outline' className='w-full'>
              <Link to={resourceLinks[task.resource_type]} onClick={onClose}>
                前往关联资源
              </Link>
            </Button>
          </div>
        )}
      </SheetContent>
    </Sheet>
  )
}
