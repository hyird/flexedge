import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type { CacheJob } from './cache-job-types'

export function cacheJobsQuery(
  websiteId: string | undefined,
  params = { page: 1, page_size: 20 }
) {
  return queryOptions({
    enabled: !!websiteId,
    retry: false,
    queryKey: [...queryKeys.websites, websiteId, 'cache-jobs', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<PageData<CacheJob>>(
        streamPath(`/websites/${websiteId}/cache-jobs`, params),
        queryKey,
        signal
      ),
  })
}
export function createCacheJob(
  websiteId: string,
  revision: number,
  input: Pick<
    CacheJob,
    'operation' | 'mode' | 'targets' | 'concurrency' | 'rate_limit_bps'
  >
) {
  return sendData('post', `/websites/${websiteId}/cache-jobs`, input, revision)
}
export function cancelCacheJob(
  job: Pick<CacheJob, 'website_id' | 'id' | 'revision'>
) {
  return sendData(
    'post',
    `/websites/${job.website_id}/cache-jobs/${job.id}/cancel`,
    undefined,
    job.revision
  )
}
export function retryCacheJob(
  job: Pick<CacheJob, 'website_id' | 'id' | 'revision'>
) {
  return sendData(
    'post',
    `/websites/${job.website_id}/cache-jobs/${job.id}/retry`,
    undefined,
    job.revision
  )
}
