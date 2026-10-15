import { useState } from 'react'
import { getRouteApi } from '@tanstack/react-router'
import { useListState } from '@/lib/list-search'
import type { NodeListSearch } from '@/features/nodes/node-search'
import { NodesTable } from '@/features/nodes/nodes-table'

const route = getRouteApi('/_authenticated/clusters/$clusterId')

/** Cluster detail default tab: the node list fixed to this cluster. Search,
 * filters, page and the add-node sheet are component state. */
export function ClusterNodes() {
  const { clusterId } = route.useParams()
  const list = useListState<NodeListSearch>()
  const [creating, setCreating] = useState(false)
  return (
    <NodesTable
      list={list}
      clusterId={clusterId}
      isCreating={creating}
      onCreatingChange={setCreating}
    />
  )
}
