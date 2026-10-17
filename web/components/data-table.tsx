import { type ReactNode } from 'react'
import { DataGrid, type DataGridColumn } from '@heroui-pro/react'
import { Skeleton, Table } from '@heroui/react'
import { cn } from '@/lib/utils'
import { EmptyState, QueryNotice } from './page'

type DataTableEmpty = {
  title: string
  description?: string
  action?: ReactNode
}

// Native secondary tables provide a flat body. Standalone lists add one
// surface to the root; embedded lists share their Section or Widget surface.
const tableClassName =
  '[&_.table__column]:rounded-none [&_.table__column]:border-b [&_.table__column]:border-separator [&_.table__column]:bg-surface'

function defaultRowId(item: object) {
  if (
    'id' in item &&
    (typeof item.id === 'string' || typeof item.id === 'number')
  )
    return item.id
  throw new Error('DataTable行缺少id，请提供getRowId')
}

/** Free text in a cell (errors, notes): one line, truncated at `className`'s
 * max width (default 20rem), full text in the native tooltip. */
export function TruncatedText({
  text,
  className,
}: {
  text?: string
  className?: string
}) {
  return text ? (
    <span title={text} className={cn('block max-w-80 truncate', className)}>
      {text}
    </span>
  ) : (
    <span className='text-muted'>—</span>
  )
}

/** Pro DataGrid with the console's list states: layout-shaped skeleton while
 * the first rows load (the grid then mounts with them, so its columns and
 * scrollbars never re-lay out from an empty first frame), an Alert + retry
 * on error (stale rows stay visible), an empty state inside the grid, a
 * sticky header and an internal scroll viewport that scrolls horizontally on
 * narrow screens. Rows keep one height across state changes: cells never
 * wrap, columns size to their content and fill the table, status cells use
 * `StatusChip`'s `reserve`. */
export function DataTable<T extends object>({
  label,
  columns,
  data,
  isLoading,
  error,
  onRetry,
  empty,
  getRowId = defaultRowId,
  getChildren,
  footer,
  variant = 'primary',
  fit = false,
  virtualized = false,
  autoHeight = false,
}: {
  /** Accessible table name, also used in loading announcements. */
  label: string
  columns: DataGridColumn<T>[]
  data: T[] | undefined
  /** True until the first snapshot arrives (`query.isPending`), or while a
   * live source opens without rows yet; shows the skeleton instead of an
   * empty grid. */
  isLoading: boolean
  /** Query error (`query.isError ? query.error : undefined`). */
  error?: unknown
  onRetry?: () => unknown
  /** Shown when `data` is empty. Use a filter-specific message when filtered. */
  empty: DataTableEmpty
  /** Defaults to the row's `id`. */
  getRowId?: (item: T) => string | number
  /** Hierarchical rows use the first row-header column for expansion. */
  getChildren?: (item: T) => T[] | undefined
  /** Usually a `TablePagination`. */
  footer?: ReactNode
  /** Use `secondary` inside an existing Section or Widget surface. */
  variant?: 'primary' | 'secondary'
  /** Fill the parent's remaining height, scrolling only the table body. */
  fit?: boolean
  /** Render only visible rows, using the shared fixed row and header heights. */
  virtualized?: boolean
  /** Grow with all rows instead of creating an internal vertical viewport. */
  autoHeight?: boolean
}) {
  return (
    <div
      className={cn(
        'flex w-full min-w-0 flex-col gap-3',
        fit && 'min-h-0 flex-1'
      )}
    >
      <QueryNotice
        query={{ isError: error !== undefined, error }}
        onRetry={onRetry}
      />
      {isLoading && !data?.length ? (
        <div
          role='status'
          aria-label={`正在加载${label}`}
          className={fit ? 'flex min-h-0 flex-1 flex-col' : undefined}
        >
          <Table
            variant='secondary'
            className={cn(
              'data-grid',
              fit && 'data-grid--fit',
              tableClassName,
              variant === 'primary' &&
                'overflow-hidden rounded-2xl bg-surface shadow-surface'
            )}
          >
            <Table.ScrollContainer>
              <Table.Content
                aria-label={`${label}加载中`}
                className='w-full table-auto'
              >
                <Table.Header>
                  {columns.map((column, index) => (
                    <Table.Column
                      key={column.id}
                      isRowHeader={index === 0}
                      className={column.headerClassName}
                      aria-label={
                        typeof column.header === 'string'
                          ? column.header
                          : column.id
                      }
                    >
                      <span className='block'>
                        <Skeleton
                          className={cn(
                            'h-3.5 w-20 max-w-full rounded-md',
                            column.align === 'end' && 'ms-auto',
                            column.align === 'center' && 'mx-auto'
                          )}
                        />
                      </span>
                    </Table.Column>
                  ))}
                </Table.Header>
                <Table.Body>
                  {Array.from({ length: 6 }, (_, row) => (
                    <Table.Row key={row}>
                      {columns.map((column) => (
                        <Table.Cell
                          key={column.id}
                          className={column.cellClassName}
                        >
                          <Skeleton
                            className={cn(
                              'h-3.5 w-3/4 rounded-md',
                              column.align === 'end' && 'ms-auto',
                              column.align === 'center' && 'mx-auto'
                            )}
                          />
                        </Table.Cell>
                      ))}
                    </Table.Row>
                  ))}
                </Table.Body>
              </Table.Content>
            </Table.ScrollContainer>
          </Table>
        </div>
      ) : data && (data.length > 0 || error === undefined) ? (
        <DataGrid
          aria-label={label}
          data={data}
          columns={columns}
          getRowId={getRowId}
          getChildren={getChildren}
          virtualized={virtualized}
          rowHeight={44}
          headingHeight={40}
          scrollContainerClassName={
            virtualized && fit
              ? '[@media(max-height:700px)]:max-h-64'
              : undefined
          }
          variant='secondary'
          className={cn(
            'min-w-0',
            fit && 'data-grid--fit',
            autoHeight && 'data-grid--auto-height',
            tableClassName,
            variant === 'primary' &&
              'overflow-hidden rounded-2xl bg-surface shadow-surface'
          )}
          contentClassName='w-full table-auto'
          renderEmptyState={() => (
            <EmptyState
              size='sm'
              title={empty.title}
              description={empty.description}
              action={empty.action}
            />
          )}
          footer={footer}
          footerClassName='shrink-0 flex-wrap justify-between gap-3 px-3 pt-3 pb-2'
        />
      ) : null}
    </div>
  )
}
