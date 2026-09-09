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
