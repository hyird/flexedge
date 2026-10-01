import { createFileRoute } from '@tanstack/react-router'
import { NodesPage } from '@/features/nodes'

export const Route = createFileRoute('/_authenticated/nodes')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: NodesPage,
})
