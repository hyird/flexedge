import { createFileRoute } from '@tanstack/react-router'
import { NodeDetailLayout } from '@/features/nodes/node-detail-layout'

export const Route = createFileRoute('/_authenticated/nodes/$nodeId')({
  staticData: { breadcrumb: '节点详情' },
  component: NodeDetailLayout,
})
