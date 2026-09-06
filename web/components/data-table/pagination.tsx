import {
  ChevronLeftIcon,
  ChevronRightIcon,
  DoubleArrowLeftIcon,
  DoubleArrowRightIcon,
} from '@radix-ui/react-icons'
import { type Table } from '@tanstack/react-table'
import { cn, getPageNumbers } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'

type DataTablePaginationProps<TData> = {
  table: Table<TData>
  className?: string
  disabled?: boolean
}

export function DataTablePagination<TData>({
  table,
  className,
  disabled = false,
}: DataTablePaginationProps<TData>) {
  const currentPage = table.getState().pagination.pageIndex + 1
  const totalPages = table.getPageCount()
  const pageNumbers = getPageNumbers(currentPage, totalPages)

  return (
    <div
      className={cn(
        'flex items-center justify-between gap-4 px-2 pt-1',
        '@max-2xl/content:flex-col-reverse @max-2xl/content:gap-4',
        className
      )}
    >
      <div className='flex w-full items-center justify-between'>
        <div className='flex w-25 items-center justify-center text-sm font-medium @2xl/content:hidden'>
          第 {currentPage} / {totalPages} 页
        </div>
        <div className='flex items-center gap-2 @max-2xl/content:flex-row-reverse'>
          <Select
            disabled={disabled}
            value={`${table.getState().pagination.pageSize}`}
            onValueChange={(value) => {
              table.setPagination({ pageIndex: 0, pageSize: Number(value) })
            }}
          >
            <SelectTrigger className='h-8 w-17.5' aria-label='每页行数'>
              <SelectValue placeholder={table.getState().pagination.pageSize} />
            </SelectTrigger>
            <SelectContent side='top'>
              {[10, 20, 30, 40, 50].map((pageSize) => (
                <SelectItem key={pageSize} value={`${pageSize}`}>
                  {pageSize}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <p className='hidden text-sm font-medium sm:block'>每页行数</p>
        </div>
      </div>

      <div className='flex items-center sm:space-x-6 lg:space-x-8'>
        <div className='flex w-25 items-center justify-center text-sm font-medium @max-2xl/content:hidden'>
          第 {currentPage} / {totalPages} 页
        </div>
        <div className='flex items-center space-x-2'>
          <Button
            variant='outline'
            className='size-8 p-0 @max-md/content:hidden'
            onClick={() => table.setPageIndex(0)}
            disabled={disabled || !table.getCanPreviousPage()}
          >
            <span className='sr-only'>转到第一页</span>
            <DoubleArrowLeftIcon className='h-4 w-4' />
          </Button>
          <Button
            variant='outline'
            className='size-8 p-0'
            onClick={() => table.previousPage()}
            disabled={disabled || !table.getCanPreviousPage()}
          >
            <span className='sr-only'>转到上一页</span>
            <ChevronLeftIcon className='h-4 w-4' />
          </Button>

          {/* Page number buttons */}
          {pageNumbers.map((pageNumber, index) => (
            <div
              key={`${pageNumber}-${index}`}
              className='hidden items-center @md/content:flex'
            >
              {pageNumber === '...' ? (
                <span className='px-1 text-sm text-muted-foreground'>...</span>
              ) : (
                <Button
                  variant={currentPage === pageNumber ? 'default' : 'outline'}
                  className='h-8 min-w-8 px-2'
                  onClick={() => table.setPageIndex((pageNumber as number) - 1)}
                  disabled={disabled}
                  aria-current={currentPage === pageNumber ? 'page' : undefined}
                >
                  <span className='sr-only'>转到第 {pageNumber} 页</span>
                  {pageNumber}
                </Button>
              )}
            </div>
          ))}

          <Button
            variant='outline'
            className='size-8 p-0'
            onClick={() => table.nextPage()}
            disabled={disabled || !table.getCanNextPage()}
          >
            <span className='sr-only'>转到下一页</span>
            <ChevronRightIcon className='h-4 w-4' />
          </Button>
          <Button
            variant='outline'
            className='size-8 p-0 @max-md/content:hidden'
            onClick={() => table.setPageIndex(table.getPageCount() - 1)}
            disabled={disabled || !table.getCanNextPage()}
          >
            <span className='sr-only'>转到最后一页</span>
            <DoubleArrowRightIcon className='h-4 w-4' />
          </Button>
        </div>
      </div>
    </div>
  )
}
