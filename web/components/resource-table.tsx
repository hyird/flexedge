import { useState } from 'react'
import {
  flexRender,
  getCoreRowModel,
  getSortedRowModel,
  useReactTable,
  type ColumnDef,
  type PaginationState,
  type SortingState,
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
  loading?: boolean
  error?: boolean
  onRetry?: () => void
  page: number
  pageSize: number
  totalPages: number
  onPaginationChange: (page: number, pageSize: number) => void
  emptyTitle?: string
  emptyDescription?: string
  minWidth?: string
}

export function ResourceTable<T>({
  columns,
  data,
  loading = false,
  error = false,
  onRetry,
  page,
  pageSize,
  totalPages,
  onPaginationChange,
  emptyTitle = '暂无数据',
  emptyDescription = '调整筛选条件或创建第一条记录。',
  minWidth = '760px',
}: Props<T>) {
  const [sorting, setSorting] = useState<SortingState>([])
  const pagination: PaginationState = {
    pageIndex: Math.max(0, page - 1),
    pageSize,
  }
  const table = useReactTable({
    data,
    columns,
    pageCount: Math.max(totalPages, 1),
    state: { sorting, pagination },
    manualPagination: true,
    onSortingChange: setSorting,
    onPaginationChange: (updater) => {
      const next = typeof updater === 'function' ? updater(pagination) : updater
      onPaginationChange(next.pageIndex + 1, next.pageSize)
    },
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
  })

  return (
    <div className='min-w-0 space-y-3'>
      <div className='overflow-hidden rounded-md border'>
        <div className='overflow-x-auto'>
          <Table style={{ minWidth }}>
            <TableHeader>
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
                  <TableCell
                    colSpan={columns.length}
                    className='h-48 text-center'
                  >
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
                  <TableCell
                    colSpan={columns.length}
                    className='h-48 text-center'
                  >
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
      </div>
      {!error && <DataTablePagination table={table} />}
    </div>
  )
}
