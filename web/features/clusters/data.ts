import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { getData, pageDataSchema, sendData, streamPath } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import { resourceListSearchQuery } from '@/lib/resource'
import { clusterSchema, type Cluster } from './types'

const clusterOptionsSchema = z.array(clusterSchema)
const clusterPageSchema = pageDataSchema(clusterSchema)

export const clusterOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.clusters, 'options'],
  queryFn: async ({ signal }) =>
    parseResponseData(
      clusterOptionsSchema,
      await getData<unknown>('/clusters/options', undefined, signal)
    ),
})

export function clustersQuery(params: {
  page: number
  page_size: number
  keyword?: string
  status?: string
  dns_zone_id?: string
}) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.clusters, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/clusters', params),
        queryKey,
        signal,
        (value) => parseResponseData(clusterPageSchema, value)
      ),
  })
}

/** Command palette search of clusters. */
export function clusterSearchQuery(keyword: string) {
  return resourceListSearchQuery(
    '/clusters',
    queryKeys.clusters,
    keyword,
    clusterSchema.pick({ id: true, name: true }),
    (cluster) => ({ id: cluster.id, label: cluster.name })
  )
}

export const clusterSnapshotKey = [...queryKeys.clusters, 'snapshot'] as const

/** One read of `GET /api/clusters/:id` (same DTO as list items): the
 * cluster detail's live stream belongs to the active tab's list. Writes on
 * the page re-read it through `afterClusterWrite`. */
export function clusterSnapshotQuery(id: string) {
  return queryOptions({
    retry: false,
    gcTime: 0,
    meta: { inlineError: true },
    queryKey: [...clusterSnapshotKey, id],
    queryFn: async ({ signal }) =>
      parseResponseData(
        clusterSchema,
        await getData<unknown>('/clusters/' + id, undefined, signal)
      ),
  })
}

/** Re-reads one cluster's detail snapshot (e.g. after its nodes changed). */
export function refreshClusterSnapshot(client: QueryClient, id: string) {
  return client.invalidateQueries({ queryKey: [...clusterSnapshotKey, id] })
}

/** After a cluster create/update: the live list follows its own stream;
 * options (cluster pickers) and detail snapshots are re-read. */
export function afterClusterWrite(client: QueryClient) {
  return Promise.all([
    client.invalidateQueries({ queryKey: [...queryKeys.clusters, 'options'] }),
    client.invalidateQueries({ queryKey: clusterSnapshotKey }),
  ])
}

/** After a cluster delete: its snapshot is dropped, not re-read (it would
 * 404), and every other cluster read is refreshed. */
export function afterClusterRemove(client: QueryClient, id: string) {
  client.removeQueries({ queryKey: [...clusterSnapshotKey, id] })
  return client.invalidateQueries({ queryKey: queryKeys.clusters })
}

export type ClusterInput = Pick<
  Cluster,
  'name' | 'dns_zone_id' | 'hostname_prefix' | 'status'
>
export function saveCluster(
  input: ClusterInput,
  current?: Pick<Cluster, 'id' | 'revision'>
) {
  return current
    ? sendData('put', '/clusters/' + current.id, input, current.revision)
    : sendData('post', '/clusters', input)
}
export function removeCluster(current: Pick<Cluster, 'id' | 'revision'>) {
  return sendData(
    'delete',
    '/clusters/' + current.id,
    undefined,
    current.revision
  )
}
