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
 * menu (`ActionMenu`). Followed by `RouteTabs` when the detail has tabs. */
export function DetailHeader({
  icon,
  title,
  status,
  meta = [],
  actions,
  menu,
}: {
  icon: ReactNode
  title: ReactNode
  status?: ReactNode
  /** Rendered in order, separated by a dot; falsy entries are skipped. */
  meta?: ReactNode[]
  actions?: ReactNode
  menu?: ReactNode
}) {
  const facts = meta.filter(Boolean)
  return (
    <header className='flex flex-wrap items-start justify-between gap-x-6 gap-y-4'>
      <div className='flex min-w-0 items-start gap-3'>
        <span
          aria-hidden
          className='flex size-10 shrink-0 items-center justify-center rounded-lg border border-border bg-surface text-muted [&_svg]:size-5'
        >
          {icon}
        </span>
        <div className='flex min-w-0 flex-col gap-1'>
          <div className='flex min-w-0 flex-wrap items-center gap-x-3 gap-y-1'>
            <h1 className='truncate text-xl font-semibold tracking-tight text-foreground'>
              {title}
            </h1>
            {status}
          </div>
          {facts.length > 0 && (
            <div className='flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1 text-[13px] text-muted'>
              {facts.map((fact, index) => (
                <Fragment key={index}>
                  {index > 0 && <span aria-hidden>·</span>}
                  <span className='min-w-0'>{fact}</span>
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
