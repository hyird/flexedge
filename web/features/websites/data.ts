import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
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
  if (patch.id !== current.id || !patch.node_id || patch.node_revision === undefined ||
      !patch.reported_at || !Array.isArray(patch.origin_states))
    return current
  const source = current.runtime.origin_sources?.find((item) => item.node_id === patch.node_id)
  if (!source || source.node_revision !== patch.node_revision ||
      Date.parse(patch.reported_at) <= Date.parse(source.reported_at)) return current
  const existing = current.runtime.origin_states.filter((state) => state.node_id === patch.node_id)
  const incomingLatest = Math.max(0, ...patch.origin_states.map((state) => state.checked_at_unix_millis))
  const existingLatest = Math.max(0, ...existing.map((state) => state.checked_at_unix_millis))
  if (incomingLatest > 0 && incomingLatest < existingLatest) return current
  return {
    ...current,
    runtime: {
      ...current.runtime,
      origin_sources: (current.runtime.origin_sources ?? []).map((item) =>
        item.node_id === patch.node_id ? { ...item, reported_at: patch.reported_at! } : item
      ),
      origin_states: [
        ...current.runtime.origin_states.filter((state) => state.node_id !== patch.node_id),
        ...patch.origin_states,
      ],
    },
  }
}

export function saveWebsite(
  values: { cluster_id: string; status: string; config: WebsiteConfig },
  website?: Pick<Website, 'id' | 'revision'>
) {
  const body = { status: values.status, config: values.config }
  const params = new URLSearchParams({ cluster_id: values.cluster_id })
  return website
    ? sendData(
        'put',
        `/websites/${website.id}?${params}`,
        body,
        website.revision
      )
    : sendData('post', `/websites?${params}`, body)
}

export function websitesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  cluster_id?: string
  status?: string
}) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.websites, 'list', params],
    queryFn: ({ queryKey, signal }) => readLiveQuery<PageData<Website>>(streamPath('/websites', params), queryKey, signal),
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
    queryKey: [...queryKeys.websites, id, 'detail'],
    enabled: !!id,
    queryFn: ({ queryKey, signal }) => {
      if (!id) throw new Error('网站不存在')
      return readLiveQuery<Website>(
        streamPath('/websites/' + id), queryKey, signal, undefined,
        patchWebsiteOriginRuntime
      )
    },
  })
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
    queryKey: [...queryKeys.websites, id, 'access-logs', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(streamPath('/websites/' + id + '/access-logs/history', params), queryKey, signal,
        (page) => accessLogPageSchema.parse(page)),
  })
}
