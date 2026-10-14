import { getRouteApi } from '@tanstack/react-router'
import { useListState } from '@/lib/list-search'
import type { WebsitesSearch } from '@/features/websites/websites-search'
import { WebsitesTable } from '@/features/websites/websites-table'

const route = getRouteApi('/_authenticated/clusters/$clusterId')

/** Cluster detail “网站” tab: the website list fixed to this cluster, with
 * search, filters and page in component state. */
export function ClusterWebsites() {
  const { clusterId } = route.useParams()
  const list = useListState<WebsitesSearch>()
  return <WebsitesTable list={list} clusterId={clusterId} />
}
