import { type ReactNode } from 'react'
import { FolderOpen } from '@gravity-ui/icons'
import { EmptyState as HeroEmptyState } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { Notice } from './forms'

export function Page({
  title,
  description,
  actions,
  children,
}: {
  title: string
  description?: string
  actions?: ReactNode
  children: ReactNode
}) {
  return (
    <section
      aria-label={title}
      className='mx-auto flex w-full max-w-7xl flex-col gap-6 px-4 pt-8 pb-8 sm:px-6 lg:px-8'
    >
      <header className='flex flex-wrap items-start justify-between gap-4'>
        <div className='max-w-2xl'>
          {description && (
            <p className='text-sm leading-6 text-muted'>{description}</p>
          )}
        </div>
        {actions && (
          <div className='flex flex-wrap items-center gap-2'>{actions}</div>
        )}
      </header>
      {children}
    </section>
  )
}

export function EmptyState({
  title,
  description,
  action,
}: {
  title: string
  description?: string
  action?: ReactNode
}) {
  return (
    <HeroEmptyState className='min-h-56'>
      <HeroEmptyState.Header>
        <HeroEmptyState.Media variant='icon'>
          <FolderOpen />
        </HeroEmptyState.Media>
        <HeroEmptyState.Title>{title}</HeroEmptyState.Title>
        {description && (
          <HeroEmptyState.Description>{description}</HeroEmptyState.Description>
        )}
      </HeroEmptyState.Header>
      {action && <HeroEmptyState.Content>{action}</HeroEmptyState.Content>}
    </HeroEmptyState>
  )
}

export function QueryNotice({
  query,
  onRetry,
}: {
  query: { isError: boolean; error: unknown }
  onRetry?: () => unknown
}) {
  return query.isError ? (
    <div className='flex flex-col gap-3'>
      <Notice>{apiErrorMessage(query.error)}</Notice>
      {onRetry && (
        <Button
          size='sm'
          variant='tertiary'
          className='self-start'
          onPress={() => {
            onRetry()
          }}
        >
          重新加载
        </Button>
      )}
    </div>
  ) : null
}
