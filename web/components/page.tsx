import { type ReactNode } from 'react'
import { FolderOpen } from '@gravity-ui/icons'
import { EmptyState as HeroEmptyState } from '@heroui-pro/react'
import { Alert, Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { cn } from '@/lib/utils'

type PageHeaderProps = {
  title: ReactNode
  status?: ReactNode
  description?: ReactNode
  /** Primary and secondary page actions, aligned with the title row. */
  actions?: ReactNode
  /** Short facts under the title, e.g. counts or a live connection state. */
  meta?: ReactNode
}

/** Page title block following the official dashboard's heading hierarchy. */
export function PageHeader({
  title,
  status,
  description,
  actions,
  meta,
}: PageHeaderProps) {
  return (
    <header
      className={cn(
        'grid grid-cols-1 items-start gap-x-6 gap-y-3',
        actions && 'sm:grid-cols-[minmax(0,1fr)_auto]'
      )}
    >
      <div className='flex min-w-0 flex-col gap-1'>
        <div className='flex min-h-8 min-w-0 items-center gap-3'>
          <h1
            title={typeof title === 'string' ? title : undefined}
            className='page-title min-w-0 truncate'
          >
            {title}
          </h1>
          {status && <span className='flex shrink-0'>{status}</span>}
        </div>
        {description && (
          <p className='max-w-3xl text-sm leading-6 text-muted'>
            {description}
          </p>
        )}
        {meta && (
          <div className='mt-1 min-w-0 text-sm leading-6 text-muted'>
            {meta}
          </div>
        )}
      </div>
      {actions && (
        <div className='flex min-h-8 flex-wrap items-center gap-2'>
          {actions}
        </div>
      )}
    </header>
  )
}

/** Page container. List and settings pages pass `title` (and optional header
 * props) to render a `PageHeader`; detail pages omit `title` and render a
 * `DetailHeader` as the first child instead. */
export function Page({
  children,
  fit = false,
  title,
  ...header
}: {
  children: ReactNode
  /** Keep the page inside the viewport; its content owns scrolling. */
  fit?: boolean
  title?: ReactNode
} & Omit<PageHeaderProps, 'title'>) {
  return (
    <div
      className={cn(
        // Fills the workspace height so a trailing SaveBar (`mt-auto`) sits at
        // the viewport bottom even when the content is short.
        'mx-auto flex w-full min-w-0 flex-col gap-6 px-4 pt-6 pb-10 sm:px-6',
        fit
          ? 'h-[calc(100dvh-var(--workspace-content-offset))] min-h-0 overflow-hidden'
          : 'min-h-[calc(100dvh-var(--workspace-content-offset))]'
      )}
    >
      {title !== undefined && <PageHeader title={title} {...header} />}
      {children}
    </div>
  )
}

/** Empty or not-found state: one sentence of explanation and a primary action.
 * `sm` sits inside cards and widgets, so it keeps a shorter minimum height. */
export function EmptyState({
  title,
  description,
  action,
  icon = <FolderOpen />,
  size = 'md',
}: {
  title: string
  description?: string
  action?: ReactNode
  icon?: ReactNode
  size?: 'sm' | 'md' | 'lg'
}) {
  return (
    <HeroEmptyState
      size={size}
      className={size === 'sm' ? 'min-h-28 py-4' : 'min-h-48'}
    >
      <HeroEmptyState.Header>
        <HeroEmptyState.Media variant='icon'>{icon}</HeroEmptyState.Media>
        <HeroEmptyState.Title>{title}</HeroEmptyState.Title>
        {description && (
          <HeroEmptyState.Description>{description}</HeroEmptyState.Description>
        )}
      </HeroEmptyState.Header>
      {action && <HeroEmptyState.Content>{action}</HeroEmptyState.Content>}
    </HeroEmptyState>
  )
}

/** Query error as a danger Alert with an inline retry action. Renders nothing
 * while the query is healthy, so it can sit above any content block. */
export function QueryNotice({
  query,
  onRetry,
  title = '数据加载失败',
}: {
  query: { isError: boolean; error: unknown }
  onRetry?: () => unknown
  title?: string
}) {
  if (!query.isError) return null
  return (
    <Alert status='danger'>
      <Alert.Indicator />
      <Alert.Content>
        <Alert.Title>{title}</Alert.Title>
        <Alert.Description>{apiErrorMessage(query.error)}</Alert.Description>
      </Alert.Content>
      {onRetry && (
        <Button
          size='sm'
          variant='tertiary'
          className='shrink-0 self-center'
          onPress={() => {
            onRetry()
          }}
        >
          重试
        </Button>
      )}
    </Alert>
  )
}
