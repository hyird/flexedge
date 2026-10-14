import { createFileRoute } from '@tanstack/react-router'
import { ClusterDetailLayout } from '@/features/clusters/cluster-detail-layout'

export const Route = createFileRoute('/_authenticated/clusters/$clusterId')({
  staticData: { breadcrumb: '集群详情' },
  component: ClusterDetailLayout,
})
