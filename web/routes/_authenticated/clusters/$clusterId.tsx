import { createFileRoute } from '@tanstack/react-router'
import { ClusterDetailLayout } from '@/features/clusters/cluster-detail-layout'
import { clusterDetailSearchSchema } from '@/features/clusters/cluster-search'

export const Route = createFileRoute('/_authenticated/clusters/$clusterId')({
  validateSearch: clusterDetailSearchSchema,
  staticData: { breadcrumb: '集群详情' },
  component: ClusterDetailLayout,
})
