import type { NavigationTarget } from '@/lib/history-state'
import type { StatusTone } from '@/lib/status'
import {
  known,
  type Operation,
  type operationStatusValues,
  type Subtask,
} from './types'

/** Action names (contract §5.1); the task title is `名称 · target_name`. */
export const operationActions = {
  'website.create': '创建网站',
  'website.update': '保存网站配置',
  'website.enable': '启用网站',
  'website.disable': '停用网站',
  'website.delete': '删除网站',
  'website.check_domains': '检查网站域名',
  'cluster.update': '保存集群',
  'cluster.delete': '删除集群',
  'node.create': '添加节点',
  'node.update': '保存节点',
  'node.delete': '删除节点',
  'node.reset_credentials': '重置节点凭据',
  'dns_zone.create': '添加域名',
  'dns_zone.update_records': '修改 DNS 记录',
  'dns_zone.sync': '立即同步',
  'dns_zone.sync_local': '以本地为准同步',
  'dns_zone.sync_remote': '以远端为准同步',
  'dns_zone.delete': '删除域名',
  'certificate.create': '申请证书',
  'certificate.renew': '续期证书',
  'certificate.delete': '删除证书',
  'provider.update': '保存服务商',
  'provider.verify': '检测服务商',
  'system.certificate_auto_renew': '自动续期',
  'system.certificate_expired': '证书过期',
  'system.node_register': '节点接入',
  'system.release_upgrade': '配置格式升级',
  'system.dns_cleanup': '清理过期 DNS 记录',
  'system.untracked': '后台任务',
} as const

/** Task and subtask statuses with their chip colour (contract §5.2). */
export const operationStatuses = {
  queued: { label: '排队中', tone: 'default' },
  running: { label: '执行中', tone: 'accent' },
  retrying: { label: '重试中', tone: 'warning' },
  failed: { label: '失败', tone: 'danger' },
  partial_failed: { label: '部分失败', tone: 'danger' },
  attention_required: { label: '需处理', tone: 'warning' },
  completed: { label: '成功', tone: 'success' },
  merged: { label: '已合并', tone: 'default' },
  cancelled: { label: '已取消', tone: 'default' },
} as const satisfies Record<
  (typeof operationStatusValues)[number],
  { label: string; tone: StatusTone }
>

/** Every status label, for `StatusChip`'s `reserve`. */
export const operationStatusLabels = Object.values(operationStatuses).map(
  (status) => status.label
)

/** Label and tone of a status; a value from a newer backend keeps its raw
 * text in a neutral chip. */
export function describeOperationStatus(status: string): {
  label: string
  tone: StatusTone
} {
  return (
    known(operationStatuses, status) ?? {
      label: status || '未知',
      tone: 'default',
    }
  )
}

/** Status groups of the list filter (contract §2.1). */
export const operationStatusGroups = {
  active: '进行中',
  attention: '需关注',
} as const

export const activeOperationStatuses: readonly string[] = [
  'queued',
  'running',
  'retrying',
]

export const operationTargetTypeLabels = {
  website: '网站',
  cluster: '集群',
  node: '节点',
  dns_zone: '域名',
  certificate: '证书',
  provider: '服务商',
} as const

export const operationActorLabels = { me: '我', system: '系统' } as const

/** Subtask kinds (contract §5.3); a subtask's name is `种类 · resource_name`. */
export const subtaskKinds = {
  dns_sync: 'DNS 同步',
  dns_challenge: 'DNS 验证记录',
  dns_challenge_cleanup: '清理验证记录',
  dns_delete: '删除远程记录',
  certificate_issue: '证书签发',
  certificate_renew: '证书续期',
  website_check: '网站域名检查',
  provider_verify: '服务商检测',
  node_publish: '节点发布',
  node_config: '节点配置下发',
} as const

/** Why a settled subtask still needs attention (`warning`). */
export const subtaskWarnings = {
  dns_conflict: '存在冲突记录',
  dns_unpushed: '有记录未推送',
  domain_unverified: '域名未生效',
} as const

export function actionLabel(action: string) {
  return known(operationActions, action) ?? action
}

/** `保存网站配置 · 品牌官网`. */
export function operationTitle(
  operation: Pick<Operation, 'action' | 'target_name'>
) {
  return `${actionLabel(operation.action)} · ${operation.target_name}`
}

/** `节点发布 · 北京`. */
export function subtaskTitle(subtask: Pick<Subtask, 'kind' | 'resource_name'>) {
  return `${known(subtaskKinds, subtask.kind) ?? subtask.kind} · ${subtask.resource_name}`
}

export function actorLabel(
  operation: Pick<Operation, 'actor_type' | 'actor_name'>
) {
  return operation.actor_type === 'system' ? '系统' : operation.actor_name
}

const sectionLabels: Record<string, Record<string, string>> = {
  website: {
    basic: '基本',
    domains: '域名',
    origins: '源站',
    health_check: '健康检查',
    access_log: '访问日志',
    https: 'HTTPS',
    compression: '压缩',
    rules: '规则',
    cluster: '所属集群',
    status: '状态',
  },
  node: {
    basic: '基本',
    cluster: '所属集群',
    endpoints: '线路与地址',
    status: '状态',
  },
  cluster: { basic: '基本', dns: '接入域名', status: '状态' },
}

/** One-line change summary (contract §5.4), e.g. `变更：域名、HTTPS` or
 * `新增 2 · 修改 1`; '' when there is nothing to say. */
export function formatOperationSummary(
  operation: Pick<Operation, 'target_type' | 'summary'>
) {
  const { sections, record_counts, credentials_changed } = operation.summary
  const parts: string[] = []
  if (sections?.length) {
    const labels = sectionLabels[operation.target_type] ?? {}
    parts.push(
      `变更：${sections.map((section) => labels[section] ?? section).join('、')}`
    )
  }
  if (record_counts) {
    const counts = [
      ['新增', record_counts.created],
      ['修改', record_counts.updated],
      ['删除', record_counts.deleted],
    ] as const
    for (const [label, value] of counts)
      if (value > 0) parts.push(`${label} ${value}`)
  }
  if (credentials_changed) parts.push('已更换凭据')
  return parts.join(' · ')
}

/** Deep link that opens a task's detail sheet. */
export function operationHref(operationId: string) {
  return `/tasks?${new URLSearchParams({ task: operationId })}`
}

/** Detail page of a task's target; `undefined` when the target was deleted
 * or its type has no page. Providers have no detail page, so the provider
 * list opens on the tab of `target_kind` filtered to the account; the tab
 * and keyword travel as history state, never in the URL. */
export function operationTarget(
  operation: Pick<
    Operation,
    | 'target_type'
    | 'target_id'
    | 'target_name'
    | 'target_deleted'
    | 'target_kind'
  >
): NavigationTarget | undefined {
  if (operation.target_deleted) return undefined
  const encoded = encodeURIComponent(operation.target_id)
  switch (operation.target_type) {
    case 'website':
      return { href: `/websites/${encoded}` }
    case 'cluster':
      return { href: `/clusters/${encoded}` }
    case 'node':
      return { href: `/nodes/${encoded}` }
    case 'dns_zone':
      return { href: `/dns-zones/${encoded}` }
    case 'certificate':
      return { href: `/certificates/${encoded}` }
    case 'provider':
      return {
        href: '/providers',
        state: {
          providers: {
            tab:
              operation.target_kind === 'certificate' ? 'certificate' : 'dns',
            q: operation.target_name,
          },
        },
      }
    default:
      return undefined
  }
}

/** What a subtask row shows in its fixed extra-info slot (contract §5.2), in
 * priority order; `null` when nothing applies. */
export type SubtaskNote =
  | { kind: 'merged'; operationId: string; subtaskId: string; label: string }
  // Settled by another task without a receiver: 「因 … 取消」/「因 … 合并」.
  | { kind: 'closed'; label: string; verb: '取消' | '合并' }
  | { kind: 'offline' }
  | { kind: 'retry'; at: string }
  | { kind: 'warning'; label: string }
  | { kind: 'recovered'; failures: number }

export function subtaskNote(subtask: Subtask): SubtaskNote | null {
  if (subtask.status === 'merged' && subtask.merged_into)
    return {
      kind: 'merged',
      operationId: subtask.merged_into.operation_id,
      subtaskId: subtask.merged_into.subtask_id,
      label: operationTitle(subtask.merged_into),
    }
  if (
    (subtask.status === 'cancelled' || subtask.status === 'merged') &&
    subtask.closed_by
  )
    return {
      kind: 'closed',
      label: operationTitle(subtask.closed_by),
      verb: subtask.status === 'merged' ? '合并' : '取消',
    }
  if (subtask.node_offline) return { kind: 'offline' }
  if (subtask.status === 'retrying' && subtask.next_attempt_at)
    return { kind: 'retry', at: subtask.next_attempt_at }
  if (subtask.status === 'attention_required' && subtask.warning)
    return {
      kind: 'warning',
      label: known(subtaskWarnings, subtask.warning) ?? subtask.warning,
    }
  if (subtask.recovered && subtask.failure_count > 0)
    return { kind: 'recovered', failures: subtask.failure_count }
  return null
}

/** One level of nesting: each top-level subtask followed by its children, in
 * creation order. Children whose parent is missing stay at the top level. */
export type SubtaskRow = { subtask: Subtask; depth: 0 | 1 }

export function subtaskRows(subtasks: readonly Subtask[]): SubtaskRow[] {
  const ids = new Set(subtasks.map((subtask) => subtask.id))
  const children = new Map<string, Subtask[]>()
  for (const subtask of subtasks)
    if (subtask.parent_id && ids.has(subtask.parent_id))
      children.set(subtask.parent_id, [
        ...(children.get(subtask.parent_id) ?? []),
        subtask,
      ])
  const rows: SubtaskRow[] = []
  const visit = (subtask: Subtask, depth: 0 | 1) => {
    rows.push({ subtask, depth })
    for (const child of children.get(subtask.id) ?? []) visit(child, 1)
  }
  for (const subtask of subtasks)
    if (!subtask.parent_id || !ids.has(subtask.parent_id)) visit(subtask, 0)
  return rows
}
