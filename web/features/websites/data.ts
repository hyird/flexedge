import { z } from 'zod'
import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { ApiBusinessError, ApiProtocolError } from '@/lib/api-response'
import { readLiveQuery, snapshotQueryOptions } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import { accessLogPageSchema } from './access-log-schema'
import type { Website, WebsiteConfig } from './types'

export function patchWebsiteOriginRuntime(
  current: Website | undefined,
  value: unknown
): Website | undefined {
  if (!current || !value || typeof value !== 'object') return current
  const patch = value as {
    id?: string
    node_id?: string
    node_revision?: number
    reported_at?: string
    origin_states?: Website['runtime']['origin_states']
  }
  if (
    patch.id !== current.id ||
    !patch.node_id ||
    patch.node_revision === undefined ||
    !patch.reported_at ||
    !Array.isArray(patch.origin_states)
  )
    return current
  const source = current.runtime.origin_sources?.find(
    (item) => item.node_id === patch.node_id
  )
  if (
    !source ||
    source.node_revision !== patch.node_revision ||
    Date.parse(patch.reported_at) <= Date.parse(source.reported_at)
  )
    return current
  const existing = current.runtime.origin_states.filter(
    (state) => state.node_id === patch.node_id
  )
  const incomingLatest = Math.max(
    0,
    ...patch.origin_states.map((state) => state.checked_at_unix_millis)
  )
  const existingLatest = Math.max(
    0,
    ...existing.map((state) => state.checked_at_unix_millis)
  )
  if (incomingLatest > 0 && incomingLatest < existingLatest) return current
  return {
    ...current,
    runtime: {
      ...current.runtime,
      origin_sources: (current.runtime.origin_sources ?? []).map((item) =>
        item.node_id === patch.node_id
          ? { ...item, reported_at: patch.reported_at! }
          : item
      ),
      origin_states: [
        ...current.runtime.origin_states.filter(
          (state) => state.node_id !== patch.node_id
        ),
        ...patch.origin_states,
      ],
    },
  }
}

type WebsiteInput = {
  cluster_id: string
  status: string
  config: WebsiteConfig
}

const createdWebsiteSchema = z.object({
  id: z.string().uuid(),
  revision: z.number().int().positive(),
})

/** `POST /websites?cluster_id=` answers `{ id, revision }` of the new website. */
export async function createWebsite(values: WebsiteInput) {
  const params = new URLSearchParams({ cluster_id: values.cluster_id })
  const response = await sendData<unknown>('post', `/websites?${params}`, {
    status: values.status,
    config: values.config,
  })
  const result = createdWebsiteSchema.safeParse(response.data)
  if (!result.success) throw new ApiProtocolError()
  return result.data
}

export function updateWebsite(
  values: WebsiteInput,
  website: Pick<Website, 'id' | 'revision'>
) {
  const params = new URLSearchParams({ cluster_id: values.cluster_id })
  return sendData(
    'put',
    `/websites/${website.id}?${params}`,
    { status: values.status, config: values.config },
    website.revision
  )
}

export type WebsitesListParams = {
  page: number
  page_size: number
  keyword?: string
  cluster_id?: string
  certificate_id?: string
  status?: string
}

export function websitesQuery(params: WebsitesListParams) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.websites, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<PageData<Website>>(
        streamPath('/websites', params),
        queryKey,
        signal
      ),
  })
}

/** One-shot page of `/websites/stream` for pages whose live stream belongs to
 * another resource (e.g. websites referencing a certificate). Re-read with
 * `refetch()` or by invalidating `websitesSnapshotKey`. */
export const websitesSnapshotKey = [...queryKeys.websites, 'list-snapshot']
export function websitesSnapshotQuery(params: WebsitesListParams) {
  return queryOptions({
    ...snapshotQueryOptions<PageData<Website>>(
      streamPath('/websites', params),
      [...websitesSnapshotKey, params]
    ),
    meta: { inlineError: true },
  })
}
export function removeWebsite(target: Pick<Website, 'id' | 'revision'>) {
  return sendData(
    'delete',
    '/websites/' + target.id,
    undefined,
    target.revision
  )
}
export function websiteDetailQuery(id: string | undefined) {
  return queryOptions({
    retry: false,
    // The detail layout renders "网站不存在" and stream errors itself.
    meta: { inlineError: true },
    queryKey: [...queryKeys.websites, id, 'detail'],
    enabled: !!id,
    queryFn: ({ queryKey, signal }) => {
      if (!id) throw new Error('网站不存在')
      return readLiveQuery<Website>(
        streamPath('/websites/' + id),
        queryKey,
        signal,
        undefined,
        patchWebsiteOriginRuntime
      )
    },
  })
}
/** One-shot read of the website detail stream for the detail header on tabs
 * whose own live stream is something else (traffic, logs). */
export function websiteSnapshotQuery(id: string) {
  return queryOptions({
    ...snapshotQueryOptions<Website>(streamPath('/websites/' + id), [
      ...queryKeys.websites,
      id,
      'header',
    ]),
    meta: { inlineError: true },
  })
}
/** `WebsiteError::NOT_FOUND` (service/domains/website/website_error.h), sent as
 * the detail stream's `resource-error` for unknown or deleted websites. */
const websiteNotFoundCode = 16601
export function isWebsiteNotFound(error: unknown) {
  return error instanceof ApiBusinessError && error.code === websiteNotFoundCode
}
export function probeWebsiteDns(id: string | undefined) {
  if (!id) throw new Error('网站不存在')
  return sendData('post', '/websites/' + id + '/dns-probe')
}
export function websiteAccessLogHistoryQuery(
  id: string,
  params: {
    page: number
    page_size: number
    keyword?: string
    method?: string
    status_class?: string
  }
) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.websites, id, 'access-logs', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/websites/' + id + '/access-logs/history', params),
        queryKey,
        signal,
        (page) => accessLogPageSchema.parse(page)
      ),
  })
}
