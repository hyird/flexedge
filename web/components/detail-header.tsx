import { Fragment, type ReactNode } from 'react'
import { Skeleton } from '@heroui/react'
import { PageHeader } from './page'

/** Placeholder with the DetailHeader layout while the first snapshot loads. */
export function DetailHeaderSkeleton() {
  return (
    <div role='status' aria-label='正在加载' className='flex flex-col gap-2'>
      <Skeleton className='h-8 w-48 max-w-full rounded-md' />
      <Skeleton className='h-6 w-72 max-w-[60vw] rounded-md' />
    </div>
  )
}

/** Resource detail header: visible title, status chip,
 * a meta line of short facts or cross-resource links, and actions (`返回`
 * followed by the resource deletion action). Followed by
 * `PageTabs` when the detail has tabs.
 *
 * The layout never depends on the current state: title and status share one
 * line (the title truncates), the meta line never wraps (it scrolls
 * sideways when it does not fit) and actions share the page title row,
 * wrapping below the header on narrow screens.
 * Give state chips a `reserve` and keep
 * the action set fixed so switching states moves nothing. */
export function DetailHeader({
  title,
  status,
  meta = [],
  actions,
}: {
  title: string
  status?: ReactNode
  /** Rendered in order, separated by a dot; falsy entries are skipped. */
  meta?: ReactNode[]
  actions?: ReactNode
}) {
  const facts = meta.filter(Boolean)
  return (
    <PageHeader
      title={title}
      status={status}
      actions={actions}
      meta={
        facts.length > 0 && (
          <div className='flex min-w-0 [scrollbar-width:none] items-center gap-x-2 overflow-x-auto whitespace-nowrap'>
            {facts.map((fact, index) => (
              <Fragment key={index}>
                {index > 0 && <span aria-hidden>·</span>}
                <span className='flex shrink-0 items-center gap-1.5'>
                  {fact}
                </span>
              </Fragment>
            ))}
          </div>
        )
      }
    />
  )
}
