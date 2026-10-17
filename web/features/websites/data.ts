import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import {
  apiErrorCode,
  getData,
  pageDataSchema,
  sendData,
  streamPath,
} from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveEvent, readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import { resourceListSearchQuery } from '@/lib/resource'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { liveLogsQuery } from '@/features/logs/data'
import { accessLogPageSchema, parseAccessLogs } from './access-log-schema'
import { parseWebsiteDashboard } from './dashboard-schema'
import {
  websiteOriginRuntimePatchSchema,
  websiteSchema,
  type Website,
  type WebsiteConfig,
} from './types'
import { websiteName } from './website-display'

const websitePageSchema = pageDataSchema(websiteSchema)

/** Applies an `origin-runtime` event (one node's origin health for one
 * website) to the website detail frame; stale or foreign reports leave it
 * unchanged. */
export function patchWebsiteOriginRuntime(
  current: Website | undefined,
  value: unknown,
  event: string
): Website | undefined {
  if (!current || event !== 'origin-runtime') return current
  const patch = parseResponseData(websiteOriginRuntimePatchSchema, value)
  if (patch.id !== current.id) return current
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
          ? { ...item, reported_at: patch.reported_at }
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
  status: Website['status']
  config: WebsiteConfig
}

/** `website_created_dto` (service/domains/website/website_types.h). */
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
  return parseResponseData(createdWebsiteSchema, response.data)
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

export function removeWebsite(target: Pick<Website, 'id' | 'revision'>) {
  return sendData(
    'delete',
    '/websites/' + target.id,
    undefined,
    target.revision
  )
}

/** After a website create or update: snapshot lists, bound certificates,
 * managed DNS zones, cluster counts and the website's own snapshot re-read. */
export function afterWebsiteWrite(client: QueryClient, websiteId?: string) {
  return Promise.all([
    client.invalidateQueries({ queryKey: websitesSnapshotKey }),
    client.invalidateQueries({ queryKey: queryKeys.certificates }),
    client.invalidateQueries({ queryKey: queryKeys.dnsZones }),
    client.invalidateQueries({ queryKey: queryKeys.clusters }),
    ...(websiteId
      ? [
          client.invalidateQueries({
            queryKey: [...websiteSnapshotKey, websiteId],
          }),
        ]
      : []),
  ])
}

/** After a confirmed delete: every read of the website (detail, snapshot,
 * access logs, dashboard: `[...websites, kind, id, ...]`) is dropped so it
 * never reports 404, and every other website read is refreshed. */
export function afterWebsiteRemove(client: QueryClient, id: string) {
  client.removeQueries({
    queryKey: queryKeys.websites,
    predicate: (query) => query.queryKey[2] === id,
  })
  return client.invalidateQueries({ queryKey: queryKeys.websites })
}

export type WebsitesListParams = {
  page: number
  page_size: number
  keyword?: string
  cluster_id?: string
  certificate_id?: string
  status?: Website['status']
}

export function websitesQuery(params: WebsitesListParams) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.websites, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/websites', params),
        queryKey,
        signal,
        (value) => parseResponseData(websitePageSchema, value)
      ),
  })
}

/** Command palette search of websites. */
export function websiteSearchQuery(keyword: string) {
  return resourceListSearchQuery(
    '/websites',
    queryKeys.websites,
    keyword,
    websiteSchema.pick({ id: true, config: true, access_domain: true }),
    (website) => ({ id: website.id, label: websiteName(website) })
  )
}

/** One read of `GET /api/websites` for pages whose live stream belongs to
 * another resource (e.g. websites referencing a certificate). Re-read with
 * `refetch()` or by invalidating `websitesSnapshotKey`. */
export const websitesSnapshotKey = [...queryKeys.websites, 'list-snapshot']
export function websitesSnapshotQuery(params: WebsitesListParams) {
  return queryOptions({
    retry: false,
    gcTime: 0,
    meta: { inlineError: true },
    queryKey: [...websitesSnapshotKey, params],
    queryFn: async ({ signal }) =>
      parseResponseData(
        websitePageSchema,
        await getData<unknown>('/websites', params, signal)
      ),
  })
}

export function websiteDetailQuery(id: string | undefined) {
  return queryOptions({
    retry: false,
    // The detail layout renders "网站不存在" and stream errors itself.
    meta: { inlineError: true },
    queryKey: [...queryKeys.websites, 'detail', id],
    enabled: !!id,
    queryFn: ({ queryKey, signal }) => {
      if (!id) throw new Error('网站不存在')
      return readLiveQuery(
        streamPath('/websites/' + id),
        queryKey,
        signal,
        (value) => parseResponseData(websiteSchema, value),
        patchWebsiteOriginRuntime
      )
    },
  })
}

/** Hosted zones for managed domains. The domains group and the editor's
 * validation share this snapshot, read only while some domain is managed;
 * the domains group shows failures inline with a retry. */
export function hostedZoneOptionsQuery(enabled: boolean) {
  return queryOptions({
    ...dnsZoneOptionsQuery,
    enabled,
    meta: { inlineError: true },
  })
}

/** One read of `GET /api/websites/:id` for the detail header on tabs whose
 * own live stream is something else (traffic, logs). */
export const websiteSnapshotKey = [...queryKeys.websites, 'snapshot']
export function websiteSnapshotQuery(id: string) {
  return queryOptions({
    retry: false,
    gcTime: 0,
    meta: { inlineError: true },
    queryKey: [...websiteSnapshotKey, id],
    queryFn: async ({ signal }) =>
      parseResponseData(
        websiteSchema,
        await getData<unknown>('/websites/' + id, undefined, signal)
      ),
  })
}

/** `WebsiteError::NOT_FOUND` (service/domains/website/website_error.h), the
 * detail stream's `resource-error` or the detail read's error response for
 * unknown or deleted websites. */
const websiteNotFoundCode = 16601
export function isWebsiteNotFound(error: unknown) {
  return apiErrorCode(error) === websiteNotFoundCode
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
    queryKey: [...queryKeys.websites, 'access-logs', id, params],
    queryFn: async ({ signal }) =>
      parseResponseData(
        accessLogPageSchema,
        await getData<unknown>(
          '/websites/' + id + '/access-logs/history',
          params,
          signal
        )
      ),
  })
}

/** Live access log tail of the 访问日志 tab. */
export function websiteAccessLogsQuery(id: string) {
  return liveLogsQuery(
    streamPath('/websites/' + id + '/access-logs'),
    [...queryKeys.websites, 'access-logs', id, 'live'],
    parseAccessLogs
  )
}

/** Live statistics of the 流量分析 tab: `dashboard` frames of the website's
 * dashboard stream. */
export function websiteDashboardQuery(id: string) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.websites, 'dashboard', id],
    queryFn: ({ queryKey, signal }) =>
      readLiveEvent(
        streamPath('/websites/' + id + '/dashboard'),
        'dashboard',
        queryKey,
        signal,
        parseWebsiteDashboard
      ),
  })
}
