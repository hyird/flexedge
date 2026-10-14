import { createFileRoute } from '@tanstack/react-router'
import { NodeOverview } from '@/features/nodes/node-overview'

export const Route = createFileRoute('/_authenticated/nodes/$nodeId/')({
  component: NodeOverview,
})
