import { createFileRoute } from '@tanstack/react-router'
import { clustersSearchSchema } from '@/features/clusters/cluster-search'
import { ClustersPage } from '@/features/clusters/clusters-page'

export const Route = createFileRoute('/_authenticated/clusters/')({
  validateSearch: clustersSearchSchema,
  staticData: { breadcrumb: '集群' },
  component: ClustersPage,
})
