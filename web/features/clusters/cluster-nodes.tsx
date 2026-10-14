import { getRouteApi } from '@tanstack/react-router'
import { useListState } from '@/lib/list-search'
import type { NodeListSearch } from '@/features/nodes/node-search'
import { NodesTable } from '@/features/nodes/nodes-table'

const route = getRouteApi('/_authenticated/clusters/$clusterId')

/** Cluster detail default tab: the node list fixed to this cluster. Search,
 * filters and page are component state; the add-node sheet is the layout's
 * (its header button opens it too). */
export function ClusterNodes({
  isCreating,
  onCreatingChange,
}: {
  isCreating: boolean
  onCreatingChange: (creating: boolean) => void
}) {
  const { clusterId } = route.useParams()
  const list = useListState<NodeListSearch>()
  return (
    <NodesTable
      list={list}
      clusterId={clusterId}
      isCreating={isCreating}
      onCreatingChange={onCreatingChange}
    />
  )
}
