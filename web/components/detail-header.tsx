import { Fragment, type ReactNode } from 'react'
import { Skeleton } from '@heroui/react'

/** Placeholder with the DetailHeader layout while the first snapshot loads. */
export function DetailHeaderSkeleton() {
  return (
    <div role='status' aria-label='正在加载' className='flex items-start gap-3'>
      <Skeleton className='size-10 shrink-0 rounded-lg' />
      <div className='flex flex-col gap-2 pt-0.5'>
        <Skeleton className='h-6 w-48 rounded-md' />
        <Skeleton className='h-3.5 w-72 max-w-[60vw] rounded-md' />
      </div>
    </div>
  )
}

/** Resource detail header: icon tile, visible title (20px/600), status chip,
 * a meta line of short facts or cross-resource links, actions and an overflow
 * menu (`ActionMenu`). Followed by `PageTabs` when the detail has tabs.
 *
 * The layout never depends on the current state: title and status share one
 * line (the title truncates), the meta line never wraps (it scrolls
 * sideways when it does not fit) and actions sit in their own grid column,
 * below the title on narrow screens. Give state chips a `reserve` and keep
 * the action set fixed so switching states moves nothing. */
export function DetailHeader({
  icon,
  title,
  status,
  meta = [],
  actions,
  menu,
}: {
  icon: ReactNode
  title: string
  status?: ReactNode
  /** Rendered in order, separated by a dot; falsy entries are skipped. */
  meta?: ReactNode[]
  actions?: ReactNode
  menu?: ReactNode
}) {
  const facts = meta.filter(Boolean)
  return (
    <header className='grid gap-4 sm:grid-cols-[minmax(0,1fr)_auto] sm:items-start sm:gap-x-6'>
      <div className='flex min-w-0 items-start gap-3'>
        <span
          aria-hidden
          className='flex size-10 shrink-0 items-center justify-center rounded-lg border border-border bg-surface text-muted [&_svg]:size-5'
        >
          {icon}
        </span>
        <div className='flex min-w-0 flex-1 flex-col gap-1'>
          <div className='flex min-w-0 items-center gap-3'>
            <h1
              title={title}
              className='min-w-0 truncate text-xl font-semibold tracking-tight text-foreground'
            >
              {title}
            </h1>
            {status && <span className='flex shrink-0'>{status}</span>}
          </div>
          {facts.length > 0 && (
            <div className='flex min-w-0 [scrollbar-width:none] items-center gap-x-2 overflow-x-auto text-[13px] whitespace-nowrap text-muted'>
              {facts.map((fact, index) => (
                <Fragment key={index}>
                  {index > 0 && <span aria-hidden>·</span>}
                  <span className='flex shrink-0 items-center gap-1.5'>
                    {fact}
                  </span>
                </Fragment>
              ))}
            </div>
          )}
        </div>
      </div>
      {(actions || menu) && (
        <div className='flex flex-wrap items-center gap-2'>
          {actions}
          {menu}
        </div>
      )}
    </header>
  )
}
