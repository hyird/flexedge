import { createFileRoute } from '@tanstack/react-router'
import { Clusters } from '@/features/clusters'

export const Route = createFileRoute('/_authenticated/clusters')({
  component: Clusters,
})
