import { type CSSProperties, type ReactNode } from 'react'
import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { Skeleton } from '@heroui/react'
import { EmptyState, QueryNotice } from './page'

type DataTableEmpty = {
  title: string
  description?: string
  action?: ReactNode
}

function defaultRowId(item: object) {
  if (
    'id' in item &&
    (typeof item.id === 'string' || typeof item.id === 'number')
  )
    return item.id
  throw new Error('DataTable 行缺少 id，请提供 getRowId')
}

/** Pro DataGrid with the console's list states: layout-shaped skeleton while
 * the first snapshot loads, an Alert + retry on error (stale rows stay
 * visible), an empty state inside the grid, a sticky header and an internal
 * scroll viewport that scrolls horizontally on narrow screens. */
export function DataTable<T extends object>({
  label,
  columns,
  data,
  isLoading,
  error,
  onRetry,
  empty,
  getRowId = defaultRowId,
  minWidth = 760,
  footer,
}: {
  /** Accessible table name, also used in loading announcements. */
  label: string
  columns: DataGridColumn<T>[]
  data: T[] | undefined
  /** True until the first snapshot arrives (`query.isPending`). */
  isLoading: boolean
  /** Query error (`query.isError ? query.error : undefined`). */
  error?: unknown
  onRetry?: () => unknown
  /** Shown when `data` is empty. Use a filter-specific message when filtered. */
  empty: DataTableEmpty
  /** Defaults to the row's `id`. */
  getRowId?: (item: T) => string | number
  /** Minimum table width before horizontal scrolling starts (px). */
  minWidth?: number
  /** Usually a `TablePagination`. */
  footer?: ReactNode
}) {
  // Tailwind cannot see runtime widths, so the class reads a CSS variable.
  const widthVariable = {
    '--data-table-min-width': `${minWidth}px`,
  } as CSSProperties
  return (
    <div className='flex min-w-0 flex-col gap-3' style={widthVariable}>
      <QueryNotice
        query={{ isError: error !== undefined, error }}
        onRetry={onRetry}
      />
      {isLoading && !data ? (
        <div
          role='status'
          aria-label={`正在加载${label}`}
          className='overflow-hidden rounded-2xl bg-surface-secondary p-1'
        >
          <div className='flex gap-4 px-3 py-2.5'>
            {columns.map((column) => (
              <Skeleton key={column.id} className='h-3.5 flex-1 rounded-md' />
            ))}
          </div>
          <div className='flex flex-col rounded-xl bg-surface'>
            {Array.from({ length: 6 }, (_, row) => (
              <div
                key={row}
                className='flex h-11 items-center gap-4 border-b border-separator px-3 last:border-b-0'
              >
                {columns.map((column) => (
                  <Skeleton
                    key={column.id}
                    className='h-3.5 flex-1 rounded-md'
                  />
                ))}
              </div>
            ))}
          </div>
        </div>
      ) : data && (data.length > 0 || error === undefined) ? (
        <DataGrid
          aria-label={label}
          data={data}
          columns={columns}
          getRowId={getRowId}
          className='min-w-0'
          contentClassName='min-w-(--data-table-min-width)'
          renderEmptyState={() => (
            <EmptyState
              size='sm'
              title={empty.title}
              description={empty.description}
              action={empty.action}
            />
          )}
          footer={footer}
          footerClassName='flex-wrap justify-between gap-3 px-3 pt-3 pb-2'
        />
      ) : null}
    </div>
  )
}
