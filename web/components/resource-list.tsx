import { type ReactNode } from 'react'
import { ArrowRotateLeft } from '@gravity-ui/icons'
import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { Button, SearchField, Skeleton, Tooltip } from '@heroui/react'
import type { PageData } from '@/lib/api'
import { Choice } from './forms'
import { EmptyState, QueryNotice } from './page'

type ResourceQuery<T extends object> = {
  isPending: boolean
  isError: boolean
  error: unknown
  data?: PageData<T> | T[]
  isFetching?: boolean
  refetch?: () => unknown
}
export function ResourceList<T extends object>({
  label,
  query,
  data = Array.isArray(query.data) ? undefined : query.data,
  columns,
  filters,
  toolbar,
  onRefresh,
  emptyTitle = '还没有资源',
  emptyDescription = '添加资源后，将显示在这里。',
  page,
  onPageChange,
  pageSize = 20,
  onPageSizeChange,
  getRowId,
}: {
  label: string
  query: ResourceQuery<T>
  data?: PageData<T>
  columns: DataGridColumn<T>[]
  filters?: ReactNode
  toolbar?: ReactNode
  onRefresh?: () => unknown
  emptyTitle?: string
  emptyDescription?: string
  page: number
  onPageChange: (page: number) => void
  pageSize?: number
  onPageSizeChange?: (size: number) => void
  getRowId?: (item: T) => string | number
}) {
  const refresh = onRefresh ?? query.refetch
  return (
    <div className='flex min-h-0 min-w-0 flex-col gap-4'>
      <div className='flex shrink-0 flex-wrap items-center gap-3'>
        {filters}
        {toolbar}
        <div className='ml-auto flex items-center gap-2'>
          {refresh && (
            <Tooltip delay={0}>
              <Button
                size='sm'
                variant='tertiary'
                isIconOnly
                aria-label={`刷新${label}`}
                isPending={query.isFetching}
                onPress={() => {
                  refresh()
                }}
              >
                <ArrowRotateLeft className='size-4' />
              </Button>
              <Tooltip.Content>刷新{label}</Tooltip.Content>
            </Tooltip>
          )}
        </div>
      </div>
      <QueryNotice query={query} onRetry={refresh} />
      {query.isPending ? (
        <div
          role='status'
          aria-label={`正在加载${label}`}
          className='flex flex-col gap-3'
        >
          <Skeleton className='h-10 w-full rounded-xl' />
          {Array.from({ length: 5 }, (_, index) => (
            <Skeleton key={index} className='h-12 w-full rounded-xl' />
          ))}
        </div>
      ) : data?.list.length ? (
        <DataGrid
          aria-label={label}
          data={data.list}
          columns={columns}
          getRowId={getRowId ?? ((item) => (item as { id: string }).id)}
          className='min-h-0 min-w-0'
          contentClassName='min-w-[720px]'
        />
      ) : !query.isError ? (
        <EmptyState title={emptyTitle} description={emptyDescription} />
      ) : null}
      {data && (
        <div className='flex shrink-0 flex-wrap items-center justify-between gap-3 text-xs text-muted'>
          <span className='tabular-nums'>共 {data.total} 条</span>
          <div className='flex flex-wrap items-center gap-2'>
            {onPageSizeChange && (
              <Choice
                compact
                label='每页条数'
                value={String(pageSize)}
                onChange={(value) => onPageSizeChange(Number(value))}
                items={[10, 20, 50, 100].map((size) => ({
                  id: String(size),
                  label: `${size} 条 / 页`,
                }))}
              />
            )}
            <Button
              size='sm'
              variant='ghost'
              isDisabled={page <= 1 || query.isPending}
              onPress={() => onPageChange(page - 1)}
            >
              上一页
            </Button>
            <span className='tabular-nums'>
              {page} / {Math.max(1, data.total_pages)}
            </span>
            <Button
              size='sm'
              variant='ghost'
              isDisabled={
                page >= Math.max(1, data.total_pages) || query.isPending
              }
              onPress={() => onPageChange(page + 1)}
            >
              下一页
            </Button>
          </div>
        </div>
      )}
    </div>
  )
}

export function ResourceSearch({
  label = '搜索',
  value,
  onChange,
}: {
  label?: string
  value: string
  onChange: (value: string) => void
}) {
  return (
    <SearchField
      aria-label={label}
      value={value}
      onChange={onChange}
      className='w-full max-w-64'
    >
      <SearchField.Group>
        <SearchField.SearchIcon />
        <SearchField.Input placeholder={`${label}…`} />
        <SearchField.ClearButton />
      </SearchField.Group>
    </SearchField>
  )
}
