import { createFileRoute } from '@tanstack/react-router'
import { ClusterNodes } from '@/features/clusters/cluster-nodes'
import { nodesSearchSchema } from '@/features/nodes/node-search'

export const Route = createFileRoute('/_authenticated/clusters/$clusterId/')({
  validateSearch: nodesSearchSchema,
  component: ClusterNodes,
})
