import type { CacheJob } from './cache-job-types'

export function canCancelCacheJob(job: Pick<CacheJob, 'operation' | 'status'>) {
  return (
    job.operation === 'preheat' && ['queued', 'running'].includes(job.status)
  )
}

export function canRetryCacheJob(job: Pick<CacheJob, 'status' | 'nodes'>) {
  return (
    ['completed', 'failed', 'cancelled'].includes(job.status) &&
    job.nodes.some((node) => ['failed', 'cancelled'].includes(node.status))
  )
}
