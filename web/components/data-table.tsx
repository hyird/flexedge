import { type CSSProperties, type ReactNode } from 'react'
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
  '[&_thead_th]:rounded-none [&_thead_th]:border-b [&_thead_th]:border-separator [&_thead_th]:bg-surface'

function defaultRowId(item: object) {
  if (
    'id' in item &&
    (typeof item.id === 'string' || typeof item.id === 'number')
  )
    return item.id
  throw new Error('DataTable 行缺少 id，请提供 getRowId')
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

/** The grid lays columns out by content (auto table layout), so a column's
 * `width`/`minWidth` is enforced by a sized header block: the column never
 * narrows below it, whatever its cells currently show. */
function withEnforcedWidth<T>(column: DataGridColumn<T>): DataGridColumn<T> {
  const minWidth =
    typeof column.width === 'number' ? column.width : column.minWidth
  if (minWidth === undefined || typeof column.header === 'function')
    return column
  return {
    ...column,
    header: (
      <span
        className='block'
        style={{ minWidth: `calc(${minWidth}px - 2rem)` }}
      >
        {column.header}
      </span>
    ),
  }
}

/** Pro DataGrid with the console's list states: layout-shaped skeleton while
 * the first rows load (the grid then mounts with them, so its columns and
 * scrollbars never re-lay out from an empty first frame), an Alert + retry
 * on error (stale rows stay visible), an empty state inside the grid, a
 * sticky header and an internal scroll viewport that scrolls horizontally on
 * narrow screens. Rows keep one height across state changes: cells never
 * wrap, column `width`/`minWidth` are enforced, status cells use
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
  minWidth = 760,
  footer,
  variant = 'primary',
  fit = false,
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
  /** Minimum table width before horizontal scrolling starts (px). */
  minWidth?: number
  /** Usually a `TablePagination`. */
  footer?: ReactNode
  /** Use `secondary` inside an existing Section or Widget surface. */
  variant?: 'primary' | 'secondary'
  /** Fill the parent's remaining height, scrolling only the table body. */
  fit?: boolean
}) {
  // Tailwind cannot see runtime widths, so the class reads a CSS variable.
  const widthVariable = {
    '--data-table-min-width': `${minWidth}px`,
  } as CSSProperties
  return (
    <div
      className={cn('flex min-w-0 flex-col gap-3', fit && 'min-h-0 flex-1')}
      style={widthVariable}
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
                className='min-w-(--data-table-min-width)'
              >
                <Table.Header>
                  {columns.map((column, index) => (
                    <Table.Column
                      key={column.id}
                      isRowHeader={index === 0}
                      className={column.headerClassName}
                      width={column.width}
                      minWidth={column.minWidth}
                      maxWidth={column.maxWidth}
                      aria-label={
                        typeof column.header === 'string'
                          ? column.header
                          : column.id
                      }
                    >
                      <span
                        className='block'
                        style={{
                          minWidth:
                            typeof column.width === 'number' ||
                            column.minWidth !== undefined
                              ? `calc(${typeof column.width === 'number' ? column.width : column.minWidth}px - 2rem)`
                              : undefined,
                        }}
                      >
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
          columns={columns.map(withEnforcedWidth)}
          getRowId={getRowId}
          variant='secondary'
          className={cn(
            'min-w-0',
            fit && 'data-grid--fit',
            tableClassName,
            variant === 'primary' &&
              'overflow-hidden rounded-2xl bg-surface shadow-surface'
          )}
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
          footerClassName='shrink-0 flex-wrap justify-between gap-3 px-3 pt-3 pb-2'
        />
      ) : null}
    </div>
  )
}
