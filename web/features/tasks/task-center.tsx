import { useState } from 'react'
import { Link } from '@tanstack/react-router'
import { ListTodo, RefreshCw } from 'lucide-react'
import { apiErrorMessage } from '@/lib/api'
import { formatDate } from '@/lib/format'
import { cn } from '@/lib/utils'
import { useIsMobile } from '@/hooks/use-mobile'
import { Button } from '@/components/ui/button'
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from '@/components/ui/popover'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
  SheetTrigger,
} from '@/components/ui/sheet'
import { Skeleton } from '@/components/ui/skeleton'
import { taskKey, taskTitle, useTasks, type Task } from './data'
import { TaskDetail } from './task-detail'
import { TaskStatus } from './task-status'

export function TaskCenter() {
  const mobile = useIsMobile()
  const [open, setOpen] = useState(false)
  const [selected, setSelected] = useState<Task | null>(null)
  const tasks = useTasks({ page: 1, page_size: 6 })
  const active = tasks.data?.active ?? 0
  const failed = tasks.data?.failed ?? 0
  const trigger = (
    <Button
      variant='ghost'
      size='icon'
      className='relative shrink-0'
      aria-label={`任务中心${active ? `，${active} 个进行中` : ''}${failed ? `，${failed} 个失败` : ''}`}
    >
      <ListTodo className='size-5' />
      {active > 0 && (
        <span className='absolute -end-1 -top-1 flex h-4 min-w-4 items-center justify-center rounded-full bg-primary px-1 text-[10px] text-primary-foreground'>
          {active > 99 ? '99+' : active}
        </span>
      )}
      {failed > 0 && (
        <span className='absolute end-0 bottom-0 size-2 rounded-full bg-destructive' />
      )}
    </Button>
  )
  const contents = (
    <>
      <div className='flex shrink-0 items-center justify-between border-b px-4 py-3'>
        <div>
          <h2 className='text-sm font-semibold'>最近任务</h2>
          <p className='mt-0.5 text-xs text-muted-foreground'>
            {tasks.isError
              ? '任务数据暂不可用'
              : `${active} 个进行中 · ${failed} 个失败`}
          </p>
        </div>
        <Button
          variant='ghost'
          size='icon'
          aria-label='刷新任务'
          disabled={tasks.isFetching}
          onClick={() => void tasks.refetch()}
        >
          <RefreshCw
            className={tasks.isFetching ? 'motion-safe:animate-spin' : ''}
          />
        </Button>
      </div>
      <div
        className={cn(
          'overflow-y-auto',
          mobile ? 'min-h-0 flex-1' : 'max-h-[min(65vh,28rem)]'
        )}
      >
        {tasks.isPending ? (
          <div className='space-y-3 p-4'>
            {[0, 1, 2].map((i) => (
              <Skeleton key={i} className='h-14 w-full' />
            ))}
          </div>
        ) : tasks.isError ? (
          <div role='alert' className='p-6 text-center text-sm'>
            <p>{apiErrorMessage(tasks.error)}</p>
            <Button
              className='mt-3'
              variant='outline'
              size='sm'
              onClick={() => void tasks.refetch()}
            >
              重试
            </Button>
          </div>
        ) : !tasks.data?.list.length ? (
          <div className='px-4 py-10 text-center text-sm text-muted-foreground'>
            暂无任务，后台执行记录会显示在这里。
          </div>
        ) : (
          <ul className='divide-y'>
            {tasks.data.list.map((task) => (
              <li key={taskKey(task)}>
                <button
                  type='button'
                  className='w-full space-y-2 px-4 py-3 text-start outline-none hover:bg-accent focus-visible:bg-accent focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-inset'
                  onClick={() => {
                    setOpen(false)
                    setSelected(task)
                  }}
                >
                  <div className='flex items-center justify-between gap-3'>
                    <span className='text-sm font-medium'>
                      {taskTitle(task)}
                    </span>
                    <TaskStatus status={task.status} />
                  </div>
                  <p className='truncate text-sm text-muted-foreground'>
                    {task.name}
                  </p>
                  <p className='text-xs text-muted-foreground'>
                    版本 {task.version} · {formatDate(task.updated_at)}
                  </p>
                </button>
              </li>
            ))}
          </ul>
        )}
      </div>
      <div className='shrink-0 border-t p-2'>
        <Button asChild variant='ghost' className='w-full'>
          <Link to='/tasks' onClick={() => setOpen(false)}>
            查看全部任务
          </Link>
        </Button>
      </div>
    </>
  )
  return (
    <>
      {mobile ? (
        <Sheet open={open} onOpenChange={setOpen}>
          <SheetTrigger asChild>{trigger}</SheetTrigger>
          <SheetContent className='w-full gap-0 p-0'>
            <SheetHeader>
              <SheetTitle>任务中心</SheetTitle>
              <SheetDescription>
                查看后台任务的执行与恢复状态。
              </SheetDescription>
            </SheetHeader>
            {contents}
          </SheetContent>
        </Sheet>
      ) : (
        <Popover open={open} onOpenChange={setOpen}>
          <PopoverTrigger asChild>{trigger}</PopoverTrigger>
          <PopoverContent
            align='end'
            className='w-96 p-0'
            aria-label='任务中心'
          >
            {contents}
          </PopoverContent>
        </Popover>
      )}
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
    </>
  )
}
