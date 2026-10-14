import { type ReactNode } from 'react'
import { Card } from '@heroui/react'
import { cn } from '@/lib/utils'

/** Titled content block on a surface card (14px/600 heading, 16–20px padding).
 * Use for every grouped area of a detail or overview page. */
export function Section({
  title,
  description,
  actions,
  children,
  className,
  contentClassName,
}: {
  title?: ReactNode
  description?: ReactNode
  actions?: ReactNode
  children: ReactNode
  className?: string
  /** E.g. `p-0` when the content is a full-bleed table. */
  contentClassName?: string
}) {
  return (
    <Card className={cn('min-w-0 gap-4 p-4 sm:p-5', className)}>
      {(title || actions) && (
        <Card.Header className='flex flex-row flex-wrap items-start justify-between gap-x-4 gap-y-2'>
          <div className='flex min-w-0 flex-col gap-0.5'>
            {title && (
              <Card.Title className='text-sm font-semibold text-foreground'>
                {title}
              </Card.Title>
            )}
            {description && (
              <Card.Description className='text-[13px] text-muted'>
                {description}
              </Card.Description>
            )}
          </div>
          {actions && (
            <div className='flex flex-wrap items-center gap-2'>{actions}</div>
          )}
        </Card.Header>
      )}
      <Card.Content className={cn('min-w-0', contentClassName)}>
        {children}
      </Card.Content>
    </Card>
  )
}

type DescriptionItem = {
  label: ReactNode
  value: ReactNode
  /** Span the whole row, e.g. for long fingerprints or error text. */
  wide?: boolean
}

/** Key/value grid. Missing values should be passed as `—`, not omitted, so
 * rows stay aligned across resources. */
export function DescriptionList({
  items,
  columns = 2,
}: {
  items: DescriptionItem[]
  columns?: 1 | 2 | 3
}) {
  return (
    <dl
      className={cn(
        'grid grid-cols-1 gap-x-8 gap-y-4',
        columns >= 2 && 'sm:grid-cols-2',
        columns === 3 && 'xl:grid-cols-3'
      )}
    >
      {items.map((item, index) => (
        <div
          key={index}
          className={cn(
            'flex min-w-0 flex-col gap-1',
            item.wide && 'sm:col-span-full'
          )}
        >
          <dt className='text-xs text-muted'>{item.label}</dt>
          <dd className='min-w-0 text-sm break-words text-foreground'>
            {item.value}
          </dd>
        </div>
      ))}
    </dl>
  )
}
