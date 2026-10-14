import { useState } from 'react'
import { z } from 'zod'
import { useNavigate } from '@tanstack/react-router'
import type { PageData } from './api'

export const pageSizeOptions = [10, 20, 50, 100] as const
export const defaultPageSize = 20
/** Client-side page of an already complete snapshot, for filters the list
 * endpoint does not support (read it once with the endpoint's page size limit). */
export function pageLocally<T>(
  items: T[],
  page: number,
  pageSize: number
): PageData<T> {
  return {
    list: items.slice((page - 1) * pageSize, page * pageSize),
    total: items.length,
    page,
    page_size: pageSize,
    total_pages: Math.max(1, Math.ceil(items.length / pageSize)),
  }
}

/** Optional URL value that falls back to "absent" instead of failing the route. */
export function optionalSearchValue<T extends z.ZodType>(schema: T) {
  return schema.optional().catch(undefined)
}

/** Shared list search params. Defaults are never written to the URL:
 * `page` 1, `page_size` 20 and an empty `q` are represented by absence. */
export const listSearchSchema = z.object({
  // Hand-typed `?q=123` arrives as a number from the router's JSON parser.
  q: optionalSearchValue(z.coerce.string().trim().max(200)).transform(
    (value) => value || undefined
  ),
  page: optionalSearchValue(z.number().int().min(2)),
  page_size: optionalSearchValue(
    z
      .number()
      .int()
      .refine((value) => value !== defaultPageSize)
      .refine((value) => (pageSizeOptions as readonly number[]).includes(value))
  ),
})
type ListSearch = { q?: string; page?: number; page_size?: number }
type SearchRecord = Record<string, unknown>

const searchDefaults: SearchRecord = { page: 1, page_size: defaultPageSize }

/** Merges a patch into list search params: drops empty and default values and
 * returns to the first page whenever anything other than `page` changes. */
export function applyListSearchPatch<T extends SearchRecord>(
  previous: T,
  patch: Partial<T>
): T {
  const next: SearchRecord = { ...previous, ...patch }
  if (Object.keys(patch).some((key) => key !== 'page')) delete next.page
  for (const [key, value] of Object.entries(next))
    if (value === undefined || value === '' || searchDefaults[key] === value)
      delete next[key]
  return next as T
}

/** True when any listed filter (including `q`) narrows the list. */
export function hasListFilters<T extends SearchRecord>(
  search: T,
  filterKeys: readonly (keyof T & string)[]
) {
  return ['q', ...filterKeys].some(
    (key) => search[key] !== undefined && search[key] !== ''
  )
}

/** Search state and writers for one list route; embedded list components
 * (e.g. a websites table reused on a cluster detail page) take this as a prop. */
export type ListSearchState<TSearch extends ListSearch> = {
  search: TSearch
  keyword: string
  page: number
  pageSize: number
  setSearch: (
    patch: Partial<TSearch>,
    options?: { push?: boolean }
  ) => Promise<void>
  setPage: (page: number) => Promise<void>
  setPageSize: (pageSize: number) => Promise<void>
  /** Clears `q` and the given filter keys, keeping other params. */
  clearFilters: (
    filterKeys: readonly (keyof TSearch & string)[]
  ) => Promise<void>
}

/** Reads and writes a list route's search params. Pass the file route (`Route`)
 * whose `validateSearch` extends `listSearchSchema`. Writes replace the history
 * entry, keep unrelated params and reset `page` unless only `page` changes. */
export function useListSearch<TSearch extends ListSearch>(route: {
  useSearch: () => TSearch
}): ListSearchState<TSearch> {
  const search = route.useSearch()
  const navigate = useNavigate()
  const setSearch = (patch: Partial<TSearch>, options?: { push?: boolean }) =>
    navigate({
      to: '.',
      search: (previous: SearchRecord) =>
        applyListSearchPatch(previous, patch as SearchRecord),
      replace: !options?.push,
    })
  return {
    search,
    keyword: search.q ?? '',
    page: search.page ?? 1,
    pageSize: search.page_size ?? defaultPageSize,
    setSearch,
    setPage: (page: number) =>
      setSearch({ page } as Partial<TSearch>, { push: true }),
    setPageSize: (pageSize: number) =>
      setSearch({ page_size: pageSize } as Partial<TSearch>),
    clearFilters: (filterKeys: readonly (keyof TSearch & string)[]) =>
      setSearch(
        Object.fromEntries(
          ['q', ...filterKeys].map((key) => [key, undefined])
        ) as Partial<TSearch>
      ),
  }
}

/** List state kept in the component instead of the URL, for lists inside an
 * in-page tab (cluster detail tabs, `/providers`, DNS records, access logs):
 * switching tabs must not change the URL. Same shape and patch rules as
 * `useListSearch`; `initial` seeds it on mount (e.g. from an arrival `q`). */
export function useListState<TSearch extends ListSearch>(
  initial: TSearch = {} as TSearch
): ListSearchState<TSearch> {
  const [search, setSearchState] = useState<TSearch>(
    () => applyListSearchPatch(initial as SearchRecord, {}) as TSearch
  )
  const setSearch = (patch: Partial<TSearch>) => {
    setSearchState(
      (previous) =>
        applyListSearchPatch(
          previous as SearchRecord,
          patch as SearchRecord
        ) as TSearch
    )
    return Promise.resolve()
  }
  return {
    search,
    keyword: search.q ?? '',
    page: search.page ?? 1,
    pageSize: search.page_size ?? defaultPageSize,
    setSearch,
    setPage: (page: number) => setSearch({ page } as Partial<TSearch>),
    setPageSize: (pageSize: number) =>
      setSearch({ page_size: pageSize } as Partial<TSearch>),
    clearFilters: (filterKeys: readonly (keyof TSearch & string)[]) =>
      setSearch(
        Object.fromEntries(
          ['q', ...filterKeys].map((key) => [key, undefined])
        ) as Partial<TSearch>
      ),
  }
}
