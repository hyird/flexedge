import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type { CachePolicy } from './types'
import { DEFAULT_CACHE_RULE_ID, isDefaultCacheRule } from './utils'

export function cachePoliciesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  status?: string
}) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.cachePolicies, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<PageData<CachePolicy>>(
        streamPath('/cache-policies', params),
        queryKey,
        signal
      ),
  })
}
export const cachePolicyOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.cachePolicies, 'options'],
  queryFn: ({ queryKey, signal }) =>
    readLiveQuery<Pick<CachePolicy, 'id' | 'name' | 'status' | 'revision'>[]>(
      streamPath('/cache-policies/options'),
      queryKey,
      signal
    ),
})
export const cachePolicyDetailQuery = (id: string | undefined) =>
  queryOptions({
    retry: false,
    enabled: !!id,
    queryKey: [...queryKeys.cachePolicies, id],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<CachePolicy>(
        streamPath(`/cache-policies/${id}`),
        queryKey,
        signal
      ),
  })
export function createCachePolicy(
  input: Pick<CachePolicy, 'name' | 'description' | 'status' | 'rules'>
) {
  return sendData('post', '/cache-policies', input)
}
export function updateCachePolicy(
  p: Pick<CachePolicy, 'id' | 'revision'>,
  input: Pick<CachePolicy, 'name' | 'description' | 'status' | 'rules'>
) {
  return sendData('put', `/cache-policies/${p.id}`, input, p.revision)
}
export function removeCachePolicy(p: Pick<CachePolicy, 'id' | 'revision'>) {
  return sendData('delete', `/cache-policies/${p.id}`, undefined, p.revision)
}
export function copyCachePolicy(p: CachePolicy, name: string) {
  return createCachePolicy({
    name,
    description: p.description,
    status: p.status,
    rules: p.rules.map((rule) => ({
      ...structuredClone(rule),
      id: isDefaultCacheRule(rule)
        ? DEFAULT_CACHE_RULE_ID
        : crypto.randomUUID(),
    })),
  })
}
