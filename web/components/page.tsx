import { type ReactNode } from 'react'
import { FolderOpen } from '@gravity-ui/icons'
import { EmptyState as HeroEmptyState } from '@heroui-pro/react'
import { Alert, Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { cn } from '@/lib/utils'

type PageHeaderProps = {
  title: ReactNode
  description?: ReactNode
  /** Primary and secondary page actions, right-aligned (wraps below on narrow screens). */
  actions?: ReactNode
  /** Short facts under the title, e.g. counts or a live connection state. */
  meta?: ReactNode
}

/** Page title block: 20px/600 visible heading, muted description, actions. */
function PageHeader({ title, description, actions, meta }: PageHeaderProps) {
  return (
    <header className='flex flex-wrap items-start justify-between gap-x-6 gap-y-3'>
      <div className='flex max-w-3xl min-w-0 flex-col gap-1'>
        <h1 className='text-xl font-semibold tracking-tight text-foreground'>
          {title}
        </h1>
        {description && (
          <p className='text-sm leading-6 text-muted'>{description}</p>
        )}
        {meta && (
          <div className='mt-1 flex flex-wrap items-center gap-x-4 gap-y-1 text-xs text-muted'>
            {meta}
          </div>
        )}
      </div>
      {actions && (
        <div className='flex flex-wrap items-center gap-2'>{actions}</div>
      )}
    </header>
  )
}

/** Page container. List and settings pages pass `title` (and optional header
 * props) to render a `PageHeader`; detail pages omit `title` and render a
 * `DetailHeader` as the first child instead. */
export function Page({
  children,
  width = 'default',
  title,
  ...header
}: {
  children: ReactNode
  /** `default` caps content at 1400px; `full` uses the whole workspace width. */
  width?: 'default' | 'full'
  title?: ReactNode
} & Omit<PageHeaderProps, 'title'>) {
  return (
    <div
      className={cn(
        // Fills the workspace height so a trailing SaveBar (`mt-auto`) sits at
        // the viewport bottom even when the content is short.
        'mx-auto flex min-h-[calc(100dvh-var(--workspace-content-offset))] w-full min-w-0 flex-col gap-6 px-4 pt-6 pb-10 sm:px-6 lg:px-8',
        width === 'default' && 'max-w-[1400px]'
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
