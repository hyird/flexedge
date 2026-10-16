import { type ReactNode } from 'react'
import { Card } from '@heroui/react'
import { cn } from '@/lib/utils'

/** Titled content block using HeroUI's native card proportions.
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
  /** Omit for a header-only card (title and actions). */
  children?: ReactNode
  className?: string
  /** E.g. `p-0` when the content is a full-bleed table. */
  contentClassName?: string
}) {
  return (
    <Card className={cn('min-w-0', className)}>
      {(title || actions) && (
        <Card.Header
          className={cn(
            'grid grid-cols-1 items-start gap-x-4 gap-y-2',
            actions && 'sm:grid-cols-[minmax(0,1fr)_auto]'
          )}
        >
          <div className='flex min-w-0 flex-col gap-0.5'>
            {title && (
              <div className='flex min-h-8 items-center'>
                <Card.Title>{title}</Card.Title>
              </div>
            )}
            {description && <Card.Description>{description}</Card.Description>}
          </div>
          {actions && (
            <div className='flex min-h-8 flex-wrap items-center gap-2'>
              {actions}
            </div>
          )}
        </Card.Header>
      )}
      {children != null && (
        <Card.Content className={cn('min-w-0', contentClassName)}>
          {children}
        </Card.Content>
      )}
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
        'grid grid-cols-1 gap-x-8 gap-y-3',
        columns >= 2 && 'sm:grid-cols-2',
        columns === 3 && 'xl:grid-cols-3'
      )}
    >
      {items.map((item, index) => (
        <div
          key={index}
          className={cn(
            'flex min-w-0 flex-col gap-0.5',
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
