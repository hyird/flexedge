import { createFileRoute } from '@tanstack/react-router'
import { NodeConfig } from '@/features/nodes/node-config'

export const Route = createFileRoute('/_authenticated/nodes/$nodeId/config')({
  staticData: { breadcrumb: '配置' },
  component: NodeConfig,
})
