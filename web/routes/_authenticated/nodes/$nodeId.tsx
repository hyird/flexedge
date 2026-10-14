import { createFileRoute } from '@tanstack/react-router'
import { NodeDetailLayout } from '@/features/nodes/node-detail-layout'
import { nodeDetailSearchSchema } from '@/features/nodes/node-search'

export const Route = createFileRoute('/_authenticated/nodes/$nodeId')({
  validateSearch: nodeDetailSearchSchema,
  staticData: { breadcrumb: '节点详情' },
  component: NodeDetailLayout,
})
