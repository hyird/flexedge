import { ListBox, Pagination, Select } from '@heroui/react'
import { formatNumber } from '@/lib/format'
import { defaultPageSize, pageSizeOptions } from '@/lib/list-search'
import { pageWindow } from '@/lib/pagination'

/** "共 N 条" summary, page-size select and numbered pages with ellipses.
 * Wire to `useListSearch` (`page`, `pageSize`, `setPage`, `setPageSize`). */
export function TablePagination({
  page,
  pageSize = defaultPageSize,
  total,
  onPageChange,
  onPageSizeChange,
}: {
  page: number
  pageSize?: number
  total: number
  onPageChange: (page: number) => void
  /** Omit to hide the page-size select (fixed-size embedded lists). */
  onPageSizeChange?: (pageSize: number) => void
}) {
  const totalPages = Math.max(1, Math.ceil(total / pageSize))
  return (
    <Pagination
      size='sm'
      aria-label='分页'
      className='w-full flex-wrap justify-end gap-3'
    >
      <Pagination.Summary className='flex items-center gap-3 text-xs text-muted'>
        <span className='tabular-nums'>共 {formatNumber(total)} 条</span>
        {onPageSizeChange && (
          <Select
            aria-label='每页条数'
            value={String(pageSize)}
            onChange={(key) => {
              if (typeof key === 'string') onPageSizeChange(Number(key))
            }}
            variant='secondary'
            className='w-32 shrink-0'
          >
            <Select.Trigger className='min-h-8 whitespace-nowrap'>
              <Select.Value />
              <Select.Indicator />
            </Select.Trigger>
            <Select.Popover>
              <ListBox>
                {pageSizeOptions.map((size) => (
                  <ListBox.Item
                    key={size}
                    id={String(size)}
                    textValue={`${size} 条/页`}
                  >
                    {size} 条/页
                    <ListBox.ItemIndicator />
                  </ListBox.Item>
                ))}
              </ListBox>
            </Select.Popover>
          </Select>
        )}
      </Pagination.Summary>
      {totalPages > 1 && (
        <Pagination.Content>
          <Pagination.Item>
            <Pagination.Previous
              aria-label='上一页'
              isDisabled={page <= 1}
              onPress={() => onPageChange(page - 1)}
            >
              <Pagination.PreviousIcon />
            </Pagination.Previous>
          </Pagination.Item>
          {pageWindow(page, totalPages).map((item) =>
            typeof item === 'number' ? (
              <Pagination.Item key={item}>
                <Pagination.Link
                  aria-label={`第 ${item} 页`}
                  isActive={item === page}
                  onPress={() => onPageChange(item)}
                >
                  {item}
                </Pagination.Link>
              </Pagination.Item>
            ) : (
              <Pagination.Item key={item}>
                <Pagination.Ellipsis />
              </Pagination.Item>
            )
          )}
          <Pagination.Item>
            <Pagination.Next
              aria-label='下一页'
              isDisabled={page >= totalPages}
              onPress={() => onPageChange(page + 1)}
            >
              <Pagination.NextIcon />
            </Pagination.Next>
          </Pagination.Item>
        </Pagination.Content>
      )}
    </Pagination>
  )
}
