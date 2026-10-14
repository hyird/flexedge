import { useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import {
  ArrowRotateRight,
  Eye,
  FileText,
  Gear,
  Key,
  TrashBin,
} from '@gravity-ui/icons'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { ActionMenu, type ActionMenuItem } from '@/components/action-menu'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { clusterSnapshotKey } from '@/features/clusters/data'
import {
  getNodeCredentials,
  nodeSnapshotKey,
  removeNode,
  resetNodeCredentials,
  type NodeCredentials,
} from './data'
import { NodeCredentialsSheet } from './node-credentials-sheet'
import type { Node } from './types'

/** Node overflow menu with its credential sheet and confirmations. `list`
 * rows add shortcuts to the detail tabs; the detail header returns to the
 * list after deletion. */
export function NodeActionMenu({
  node,
  context,
}: {
  node: Node
  context: 'list' | 'detail'
}) {
  const client = useQueryClient()
  const navigate = useNavigate()
  const [credentials, setCredentials] = useState<NodeCredentials | null>(null)
  const [confirm, setConfirm] = useState<'reset' | 'remove' | null>(null)
  const reveal = useMutation({
    mutationFn: () => getNodeCredentials(node.id),
    onSuccess: setCredentials,
    onError: (error) => toast.danger(apiErrorMessage(error)),
  })
  const detail = `/nodes/${node.id}`
  const items: ActionMenuItem[] = [
    ...(context === 'list'
      ? ([
          { id: 'view', label: '查看详情', icon: <Eye />, href: detail },
          {
            id: 'logs',
            label: '实时日志',
            icon: <FileText />,
            href: detail,
            state: { node: { tab: 'logs' } },
          },
          {
            id: 'config',
            label: '编辑配置',
            icon: <Gear />,
            href: detail,
            state: { node: { tab: 'config' } },
          },
        ] satisfies ActionMenuItem[])
      : []),
    {
      id: 'credentials',
      label: '查看接入凭据',
      icon: <Key />,
      disabled: reveal.isPending,
      separated: context === 'list',
      onAction: () => reveal.mutate(),
    },
    {
      id: 'reset',
      label: '重置接入凭据',
      icon: <ArrowRotateRight />,
      onAction: () => setConfirm('reset'),
    },
    {
      id: 'remove',
      label: '删除节点',
      icon: <TrashBin />,
      danger: true,
      separated: true,
      onAction: () => setConfirm('remove'),
    },
  ]
  // Writes re-read the page's header snapshots (node detail on the logs tab,
  // cluster detail around its nodes tab); live tabs update from their stream.
  const refreshSnapshots = () => {
    void client.invalidateQueries({ queryKey: nodeSnapshotKey })
    void client.invalidateQueries({ queryKey: clusterSnapshotKey })
  }
  return (
    <>
      <ActionMenu label={`节点 ${node.name} 的操作`} items={items} />
      <ConfirmDialog
        isOpen={confirm === 'reset'}
        onClose={() => setConfirm(null)}
        tone='accent'
        title={`重置节点 ${node.name} 的接入凭据？`}
        description='旧凭据立即失效，节点会断开连接并回到待接入状态，需要用新凭据重新运行安装命令。'
        confirmLabel='重置凭据'
        onConfirm={async () => {
          const result = await resetNodeCredentials(node)
          // The credentials response carries the generic envelope message "ok".
          toast.success('接入凭据已重置')
          setCredentials(result.data)
          refreshSnapshots()
        }}
      />
      <ConfirmDialog
        isOpen={confirm === 'remove'}
        onClose={() => setConfirm(null)}
        title={`删除节点 ${node.name}？`}
        description='节点将无法继续接入控制面，已分发的配置会在节点上停止更新。此操作不可撤销。'
        confirmLabel='删除节点'
        onConfirm={async () => {
          const response = await removeNode(node)
          toast.success(response.message)
          if (context === 'detail') void navigate({ to: '/nodes' })
          else refreshSnapshots()
        }}
      />
      <NodeCredentialsSheet
        credentials={credentials}
        nodeName={node.name}
        onClose={() => setCredentials(null)}
      />
    </>
  )
}
