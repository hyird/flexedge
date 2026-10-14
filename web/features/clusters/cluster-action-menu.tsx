import { useState } from 'react'
import { useNavigate } from '@tanstack/react-router'
import { Eye, Pencil, Plus, TrashBin } from '@gravity-ui/icons'
import { toast } from '@heroui/react'
import { ActionMenu, type ActionMenuItem } from '@/components/action-menu'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { removeCluster } from './data'
import type { Cluster } from './types'

/** Cluster overflow menu with the delete confirmation. Editing is delegated
 * to the page (`?edit=` on the list, a local sheet on the detail); the list's
 * “添加节点” opens the detail with the add-node sheet (history state). */
export function ClusterActionMenu({
  cluster,
  context,
  onEdit,
}: {
  cluster: Cluster
  context: 'list' | 'detail'
  onEdit: () => void
}) {
  const navigate = useNavigate()
  const [confirming, setConfirming] = useState(false)
  const detail = `/clusters/${cluster.id}`
  const items: ActionMenuItem[] = [
    ...(context === 'list'
      ? [{ id: 'view', label: '查看详情', icon: <Eye />, href: detail }]
      : []),
    { id: 'edit', label: '编辑集群', icon: <Pencil />, onAction: onEdit },
    ...(context === 'list'
      ? [
          {
            id: 'add-node',
            label: '添加节点',
            icon: <Plus />,
            href: detail,
            state: { cluster: { createNode: true } },
          },
        ]
      : []),
    {
      id: 'remove',
      label: '删除集群',
      icon: <TrashBin />,
      danger: true,
      separated: true,
      onAction: () => setConfirming(true),
    },
  ]
  return (
    <>
      <ActionMenu label={`集群 ${cluster.name} 的操作`} items={items} />
      <ConfirmDialog
        isOpen={confirming}
        onClose={() => setConfirming(false)}
        title={`删除集群 ${cluster.name}？`}
        description={
          cluster.node_count > 0
            ? `集群下还有 ${cluster.node_count} 个节点，需先删除或迁移这些节点和集群内的网站后才能删除集群。`
            : '集群的接入域名将不再解析，此操作不可撤销。集群内仍有网站时无法删除。'
        }
        confirmLabel='删除集群'
        onConfirm={async () => {
          const response = await removeCluster(cluster)
          toast.success(response.message)
          if (context === 'detail') void navigate({ to: '/clusters' })
        }}
      />
    </>
  )
}
