import { z } from 'zod'
import {
  keepPreviousData,
  queryOptions,
  type QueryKey,
  type UseQueryOptions,
} from '@tanstack/react-query'
import { getData, pageDataSchema } from './api'
import { parseResponseData } from './api-response'

/** Fields every revisioned resource DTO carries. */
export const revisionedSchema = z.object({
  id: z.string(),
  revision: z.number().int(),
  created_at: z.string(),
  updated_at: z.string(),
})
export type Revisioned = z.infer<typeof revisionedSchema>

/** One resource found by the command palette. */
export type ResourceSearchHit = { id: string; label: string }

export type ResourceSearchQuery = UseQueryOptions<
  ResourceSearchHit[],
  Error,
  ResourceSearchHit[],
  QueryKey
>

/** Command palette search of one resource list: the first five matches of
 * `keyword` from `GET path` (items parsed with the domain's `item` schema),
 * as hits. Nothing is read for an empty keyword; the result is dropped once
 * the palette closes. */
export function resourceListSearchQuery<T extends z.ZodType>(
  path: string,
  scope: QueryKey,
  keyword: string,
  item: T,
  toHit: (item: z.infer<T>) => ResourceSearchHit
): ResourceSearchQuery {
  const queryKey: QueryKey = [...scope, 'search', keyword]
  const page = pageDataSchema(item)
  // `path`, the item schema and `toHit` are fixed per `scope`: each domain
  // owns one search source, so `scope` + `keyword` identify the result.
  // eslint-disable-next-line @tanstack/query/exhaustive-deps
  return queryOptions({
    queryKey,
    enabled: keyword.length > 0,
    retry: false,
    gcTime: 0,
    placeholderData: keepPreviousData,
    queryFn: async ({ signal }) =>
      parseResponseData(
        page,
        await getData<unknown>(path, { keyword, page: 1, page_size: 5 }, signal)
      ).list.map(toHit),
  })
}
