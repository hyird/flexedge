import { useLayoutEffect, useRef, useState } from 'react'
import {
  flexRender,
  getCoreRowModel,
  useReactTable,
  type ColumnDef,
  type PaginationState,
} from '@tanstack/react-table'
import { CircleAlert, Inbox, RotateCcw } from 'lucide-react'
import { cn } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import { Skeleton } from '@/components/ui/skeleton'
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table'
import { DataTablePagination } from '@/components/data-table'

type Props<T> = {
  columns: ColumnDef<T>[]
  data: T[]
  tableClassName?: string
  fixedLayout?: boolean
  loading?: boolean
  error?: boolean
  onRetry?: () => void
  page: number
  pageSize: number
  totalPages: number
  onPaginationChange: (page: number, pageSize: number) => void
  emptyTitle?: string
  emptyDescription?: string
}

export function ResourceTable<T>({
  columns,
  data,
  tableClassName,
  fixedLayout = false,
  loading = false,
  error = false,
  onRetry,
  page,
  pageSize,
  totalPages,
  onPaginationChange,
  emptyTitle = '暂无数据',
  emptyDescription = '调整筛选条件或创建第一条记录。',
}: Props<T>) {
  const tableContainerRef = useRef<HTMLDivElement>(null)
  const [useContentWidth, setUseContentWidth] = useState(false)
  const pagination: PaginationState = {
    pageIndex: Math.max(0, page - 1),
    pageSize,
  }
  const table = useReactTable({
    data,
    columns,
    pageCount: Math.max(totalPages, 1),
    state: { pagination },
    manualPagination: true,
    defaultColumn: { enableSorting: false },
    onPaginationChange: (updater) => {
      const next = typeof updater === 'function' ? updater(pagination) : updater
      onPaginationChange(next.pageIndex + 1, next.pageSize)
    },
    getCoreRowModel: getCoreRowModel(),
  })
  const visibleColumns = table.getVisibleLeafColumns()
  const fixedColumnsTotal = visibleColumns.reduce(
    (total, column) => total + column.getSize(),
    0
  )
  const fixedColumnWidth = (column: (typeof visibleColumns)[number]) =>
    fixedLayout && !useContentWidth && fixedColumnsTotal > 0
      ? { width: `${(column.getSize() / fixedColumnsTotal) * 100}%` }
      : undefined

  useLayoutEffect(() => {
    const container = tableContainerRef.current
    if (!container) return

    const updateLayout = () => {
      const overflowed = container.scrollWidth > container.clientWidth + 1
      setUseContentWidth((current) =>
        current === overflowed ? current : overflowed
      )
    }
    updateLayout()

    if (typeof ResizeObserver === 'undefined') return

    const observer = new ResizeObserver(updateLayout)
    observer.observe(container)
    return () => observer.disconnect()
  }, [columns, data, error, fixedLayout, loading])

  return (
    <div className='min-w-0 space-y-3' aria-busy={loading}>
      {loading && (
        <span role='status' className='sr-only'>
          正在加载数据…
        </span>
      )}
      <div className='overflow-hidden rounded-lg border bg-card shadow-sm'>
        <Table
          containerLabel='资源列表'
          containerRef={tableContainerRef}
          className={cn(
            fixedLayout &&
              (useContentWidth
                ? 'w-max min-w-max table-auto'
                : 'w-full min-w-0 table-fixed'),
            tableClassName
          )}
        >
          {fixedLayout && !useContentWidth && (
            <colgroup>
              {visibleColumns.map((column) => (
                <col key={column.id} style={fixedColumnWidth(column)} />
              ))}
            </colgroup>
          )}
          <TableHeader className='bg-muted/35'>
            {table.getHeaderGroups().map((group) => (
              <TableRow key={group.id}>
                {group.headers.map((header) => (
                  <TableHead key={header.id}>
                    {header.isPlaceholder
                      ? null
                      : flexRender(
                          header.column.columnDef.header,
                          header.getContext()
                        )}
                  </TableHead>
                ))}
              </TableRow>
            ))}
          </TableHeader>
          <TableBody>
            {error ? (
              <TableRow>
                <TableCell colSpan={columns.length} className='h-48 text-center'>
                  <div
                    role='alert'
                    className='mx-auto flex max-w-sm flex-col items-center'
                  >
                    <div className='mb-3 flex size-10 items-center justify-center rounded-full bg-destructive/10'>
                      <CircleAlert className='size-5 text-destructive' />
                    </div>
                    <p className='font-medium'>数据加载失败</p>
                    <p className='mt-1 text-sm text-muted-foreground'>
                      请检查网络连接后重试。
                    </p>
                    {onRetry && (
                      <Button
                        variant='outline'
                        size='sm'
                        className='mt-4'
                        onClick={onRetry}
                      >
                        <RotateCcw /> 重新加载
                      </Button>
                    )}
                  </div>
                </TableCell>
              </TableRow>
            ) : loading ? (
              Array.from({ length: Math.min(pageSize, 8) }).map(
                (_, rowIndex) => (
                  <TableRow key={rowIndex}>
                    {columns.map((_, cellIndex) => (
                      <TableCell key={cellIndex}>
                        <Skeleton
                          className={cn(
                            'h-5',
                            cellIndex === 0 ? 'w-36' : 'w-20'
                          )}
                        />
                      </TableCell>
                    ))}
                  </TableRow>
                )
              )
            ) : table.getRowModel().rows.length ? (
              table.getRowModel().rows.map((row) => (
                <TableRow key={row.id}>
                  {row.getVisibleCells().map((cell) => (
                    <TableCell key={cell.id}>
                      {flexRender(
                        cell.column.columnDef.cell,
                        cell.getContext()
                      )}
                    </TableCell>
                  ))}
                </TableRow>
              ))
            ) : (
              <TableRow>
                <TableCell colSpan={columns.length} className='h-48 text-center'>
                  <div className='mx-auto flex max-w-sm flex-col items-center'>
                    <div className='mb-3 flex size-10 items-center justify-center rounded-full bg-muted'>
                      <Inbox className='size-5 text-muted-foreground' />
                    </div>
                    <p className='font-medium'>{emptyTitle}</p>
                    <p className='mt-1 text-sm text-muted-foreground'>
                      {emptyDescription}
                    </p>
                  </div>
                </TableCell>
              </TableRow>
            )}
          </TableBody>
        </Table>
      </div>
      {!error && <DataTablePagination table={table} disabled={loading} />}
    </div>
  )
}
