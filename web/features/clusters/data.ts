import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type { Cluster } from './types'

export const clusterOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.clusters, 'options'],
  queryFn: ({ queryKey, signal }) =>
    readLiveQuery<Cluster[]>(streamPath('/clusters/options'), queryKey, signal),
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
    queryKey: [...queryKeys.clusters, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery<PageData<Cluster>>(
        streamPath('/clusters', params),
        queryKey,
        signal
      ),
  })
}

export type ClusterInput = Pick<
  Cluster,
  'name' | 'dns_zone_id' | 'hostname_prefix'
> & { status: 'enabled' | 'disabled' }
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
