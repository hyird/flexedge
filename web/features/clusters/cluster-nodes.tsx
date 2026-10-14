import { getRouteApi } from '@tanstack/react-router'
import { useListSearch } from '@/lib/list-search'
import { NodesTable } from '@/features/nodes/nodes-table'

const route = getRouteApi('/_authenticated/clusters/$clusterId/')

/** Cluster detail index tab: the node list fixed to this cluster. */
export function ClusterNodes() {
  const { clusterId } = route.useParams()
  const list = useListSearch(route)
  return <NodesTable list={list} clusterId={clusterId} />
}
