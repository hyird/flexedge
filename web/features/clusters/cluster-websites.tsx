import { getRouteApi } from '@tanstack/react-router'
import { useListSearch } from '@/lib/list-search'
import { WebsitesTable } from '@/features/websites/websites-table'

const route = getRouteApi('/_authenticated/clusters/$clusterId')

/** Cluster detail `?tab=websites`: the website list fixed to this cluster. */
export function ClusterWebsites() {
  const { clusterId } = route.useParams()
  const list = useListSearch(route)
  return <WebsitesTable list={list} clusterId={clusterId} />
}
