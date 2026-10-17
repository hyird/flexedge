import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { getData, pageDataSchema, sendData } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveQuery } from '@/lib/live-query'
import {
  cachePolicySchema,
  cacheOperationSchema,
  type CachePolicyInput,
  type CachePolicy,
  type CacheOperationInput,
  type CacheOperationKind,
} from './types'

const cacheWebsiteSchema = z.object({
  id: z.string(),
  name: z.string(),
  status: z.string(),
  cache_enabled: z.boolean(),
  cache_policy_id: z.string(),
  domains: z.array(z.string()),
})
export function cacheWebsitesQuery(policyId?: string) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: ['cache', 'websites', policyId ?? ''],
    queryFn: async ({ signal }) =>
      parseResponseData(
        z.array(cacheWebsiteSchema),
        await getData<unknown>(
          '/cache/websites/options',
          policyId ? { policy_id: policyId } : undefined,
          signal
        )
      ),
  })
}
const root = ['cache'] as const
export const cachePolicyOptionsQuery = queryOptions({
  retry: false,
  meta: { inlineError: true },
  queryKey: [...root, 'options'],
  queryFn: async ({ signal }) =>
    parseResponseData(
      z.array(cachePolicySchema),
      await getData<unknown>('/cache/policies/options', undefined, signal)
    ),
})
export function cachePoliciesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  status?: string
}) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...root, 'policies', params],
    queryFn: async ({ signal }) =>
      parseResponseData(
        pageDataSchema(cachePolicySchema),
        await getData<unknown>('/cache/policies', params, signal)
      ),
  })
}
export function cachePolicyQuery(id: string) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...root, 'policy', id],
    queryFn: async ({ signal }) =>
      parseResponseData(
        cachePolicySchema,
        await getData<unknown>('/cache/policies/' + id, undefined, signal)
      ),
  })
}
export function saveCachePolicy(
  input: CachePolicyInput,
  current?: Pick<CachePolicy, 'id' | 'revision'>
) {
  return current
    ? sendData<CachePolicy>(
        'put',
        '/cache/policies/' + current.id,
        input,
        current.revision
      )
    : sendData<CachePolicy>('post', '/cache/policies', input)
}
export function removeCachePolicy(
  current: Pick<CachePolicy, 'id' | 'revision'>
) {
  return sendData(
    'delete',
    '/cache/policies/' + current.id,
    undefined,
    current.revision
  )
}
export function afterCachePolicyWrite(client: QueryClient) {
  return Promise.all([
    client.invalidateQueries({ queryKey: root }),
    client.invalidateQueries({ queryKey: ['websites'] }),
  ])
}
export function cacheOperationsQuery(operation: CacheOperationKind) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...root, 'operations', operation],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        '/api/cache/operations/stream?operation=' + operation,
        queryKey,
        signal,
        (value) => parseResponseData(z.array(cacheOperationSchema), value)
      ),
  })
}
export function submitCacheOperation(input: CacheOperationInput) {
  return sendData<{ id: string; target_node_count: number }>(
    'post',
    '/cache/operations',
    input
  )
}
export function retryCacheOperation(id: string) {
  return sendData('post', '/cache/operations/' + id + '/retry')
}
