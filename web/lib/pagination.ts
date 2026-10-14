import { z } from 'zod'
import { ApiProtocolError } from './api-response'

export const collectionPageSchema = z.object({
  list: z.array(z.unknown()),
  total: z.number().int().nonnegative(),
  page: z.number().int().positive(),
  page_size: z.number().int().positive(),
  total_pages: z.number().int().nonnegative(),
})

// Validate navigation metadata here; item contracts belong to each domain.
export function validateCollectionPage(value: unknown, requestedPage: number) {
  const result = collectionPageSchema.safeParse(value)
  if (!result.success) throw new ApiProtocolError()
  const page = result.data
  if (
    page.page !== requestedPage ||
    page.total_pages !== Math.ceil(page.total / page.page_size) ||
    page.list.length > page.page_size ||
    (page.page < page.total_pages && page.list.length === 0)
  ) {
    throw new ApiProtocolError()
  }
}

type PageWindowItem = number | 'start-ellipsis' | 'end-ellipsis'

/** Page links for a pager: first, last, `siblings` around the current page,
 * and an ellipsis wherever more than one page is skipped. */
export function pageWindow(
  page: number,
  totalPages: number,
  siblings = 1
): PageWindowItem[] {
  const last = Math.max(1, totalPages)
  const current = Math.min(Math.max(1, page), last)
  const start = Math.max(2, current - siblings)
  const end = Math.min(last - 1, current + siblings)
  const items: PageWindowItem[] = [1]
  if (start > 3) items.push('start-ellipsis')
  else for (let value = 2; value < start; value++) items.push(value)
  for (let value = start; value <= end; value++) items.push(value)
  if (end < last - 2) items.push('end-ellipsis')
  else for (let value = end + 1; value < last; value++) items.push(value)
  if (last > 1) items.push(last)
  return items
}
