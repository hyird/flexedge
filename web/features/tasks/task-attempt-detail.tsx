import type { ReactNode } from 'react'
import { Chip, Link, Table } from '@heroui/react'
import { formatDate } from '@/lib/format'
import {
  describeStatus,
  statusReserve,
  type StatusDictionary,
} from '@/lib/status'
import { CopyButton } from '@/components/copy-button'
import { Mono } from '@/components/mono'
import { StatusChip } from '@/components/status-chip'
import { providerLabel } from '@/features/providers/provider-display'
import { RedactedText } from './task-attempt-trace'
import { redactedMarker } from './task-trace'
import { known, type TaskAttemptDetail as Detail } from './types'

type DetailOf<K extends Detail['kind']> = Extract<Detail, { kind: K }>

/** Per-kind execution detail of one attempt. `none` without a generation
 * failure (old records or attempts without detail) renders nothing.
 * `outcome` is the attempt's outcome: a failed provider verification never
 * listed zones, so its zero count means "not obtained". */
export function TaskAttemptDetail({
  detail,
  outcome,
}: {
  detail: Detail
  outcome: string
}) {
  if (detail.kind === 'invalid')
    return <p className='m-0 text-xs text-muted'>明细无法识别</p>
  if (detail.kind === 'none')
    return detail.truncated ? (
      <p className='m-0 text-xs text-muted'>明细生成失败</p>
    ) : null
  return (
    <>
      {detail.kind === 'dns_sync' && <DnsSyncDetail detail={detail} />}
      {detail.kind === 'certificate' && <CertificateDetail detail={detail} />}
      {detail.kind === 'website_domains' && (
        <WebsiteDomainsDetail detail={detail} />
      )}
      {detail.kind === 'provider' && (
        <ProviderDetail detail={detail} listed={outcome === 'completed'} />
      )}
      {detail.kind === 'node_apply' && <NodeApplyDetail detail={detail} />}
    </>
  )
}

/** Raw wire value outside the known set (e.g. from a newer backend). */
function UnknownValue({ value }: { value: string }) {
  return (
    <span className='text-muted' title='未识别的值'>
      {value || '未知'}
    </span>
  )
}

function DetailBlock({
  title,
  aside,
  children,
}: {
  title: string
  aside?: ReactNode
  children: ReactNode
}) {
  return (
    <section className='flex min-w-0 flex-col gap-1.5'>
      <div className='flex min-w-0 flex-wrap items-baseline justify-between gap-x-3'>
        <h4 className='m-0 text-xs font-medium text-muted'>{title}</h4>
        {aside && <span className='text-xs text-muted'>{aside}</span>}
      </div>
      {children}
    </section>
  )
}

/** Compact label/value grid for scalar facts. */
function Facts({ items }: { items: [string, ReactNode][] }) {
  return (
    <dl className='m-0 grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1 text-xs'>
      {items.map(([label, value]) => (
        <div key={label} className='contents'>
          <dt className='text-muted'>{label}</dt>
          <dd className='m-0 min-w-0 break-words text-foreground'>{value}</dd>
        </div>
      ))}
    </dl>
  )
}

// Detail tables sit inside the history list in a sheet: tighter cells than
// lists, 13px text. A cell's second line holds short facts (type, line, TTL)
// at the same size and never truncates; long values truncate with a title.
const compactTable =
  '[&_td]:px-2.5 [&_td]:py-2 [&_td]:text-[13px] [&_th]:px-2.5 [&_th]:py-2'

function TwoLines({ first, second }: { first: ReactNode; second: ReactNode }) {
  return (
    <span className='flex min-w-0 flex-col'>
      {first}
      <span className='whitespace-nowrap text-muted'>{second}</span>
    </span>
  )
}

const dnsChanges = {
  created: { label: '新增', tone: 'success' },
  updated: { label: '修改', tone: 'accent' },
  deleted: { label: '删除', tone: 'danger' },
} as const satisfies StatusDictionary
const dnsChangeReserve = statusReserve(dnsChanges)

type DnsRecord = DetailOf<'dns_sync'>['created'][number]

function DnsRecordValue({ record }: { record?: DnsRecord }) {
  if (!record) return <span className='text-muted'>—</span>
  return (
    <TwoLines
      first={
        <Mono truncate className='max-w-48'>
          {record.content}
        </Mono>
      }
      second={[
        `TTL ${record.ttl}`,
        record.priority !== null && `优先级 ${record.priority}`,
        record.proxied && '已代理',
      ]
        .filter(Boolean)
        .join(' · ')}
    />
  )
}

function DnsSyncDetail({ detail }: { detail: DetailOf<'dns_sync'> }) {
  const rows = [
    ...detail.created.map((after) => ({
      change: 'created' as const,
      record: after,
      before: undefined,
      after,
    })),
    ...detail.updated.map(({ before, after }) => ({
      change: 'updated' as const,
      record: after,
      before,
      after,
    })),
    ...detail.deleted.map((before) => ({
      change: 'deleted' as const,
      record: before,
      before,
      after: undefined,
    })),
  ]
  return (
    <>
      <DetailBlock
        title='记录变更'
        aside={`新增 ${detail.created_count} · 修改 ${detail.updated_count} · 删除 ${detail.deleted_count} · 未变化 ${detail.unchanged_count}`}
      >
        {rows.length > 0 && (
          <Table variant='secondary' className={compactTable}>
            <Table.ScrollContainer className='max-h-80 overflow-y-auto'>
              <Table.Content
                aria-label='DNS 记录变更'
                className='min-w-[400px]'
              >
                <Table.Header>
                  <Table.Column isRowHeader>记录</Table.Column>
                  <Table.Column>变更前</Table.Column>
                  <Table.Column>变更后</Table.Column>
                </Table.Header>
                <Table.Body>
                  {rows.map((row, index) => (
                    <Table.Row key={index}>
                      <Table.Cell>
                        <TwoLines
                          first={
                            <span className='flex min-w-0 items-center gap-1.5'>
                              <StatusChip
                                {...dnsChanges[row.change]}
                                reserve={dnsChangeReserve}
                              />
                              <Mono truncate className='max-w-36'>
                                {row.record.name}
                              </Mono>
                            </span>
                          }
                          second={`${row.record.type} · ${row.record.line_code}`}
                        />
                      </Table.Cell>
                      <Table.Cell>
                        <DnsRecordValue record={row.before} />
                      </Table.Cell>
                      <Table.Cell>
                        <DnsRecordValue record={row.after} />
                      </Table.Cell>
                    </Table.Row>
                  ))}
                </Table.Body>
              </Table.Content>
            </Table.ScrollContainer>
          </Table>
        )}
      </DetailBlock>
      {detail.conflict_count > 0 && (
        <DetailBlock title={`冲突 ${detail.conflict_count} 条`}>
          <Table variant='secondary' className={compactTable}>
            <Table.ScrollContainer>
              <Table.Content
                aria-label='DNS 记录冲突'
                className='min-w-[400px]'
              >
                <Table.Header>
                  <Table.Column isRowHeader>记录</Table.Column>
                  <Table.Column>本地</Table.Column>
                  <Table.Column>远端</Table.Column>
                </Table.Header>
                <Table.Body>
                  {detail.conflicts.map((conflict, index) => (
                    <Table.Row key={index}>
                      <Table.Cell>
                        <TwoLines
                          first={
                            <Mono truncate className='max-w-36'>
                              {conflict.name}
                            </Mono>
                          }
                          second={`${conflict.type} · ${conflict.line_code}`}
                        />
                      </Table.Cell>
                      <Table.Cell>
                        <Mono truncate className='max-w-48'>
                          {conflict.local_content}
                        </Mono>
                      </Table.Cell>
                      <Table.Cell>
                        <Mono truncate className='max-w-48'>
                          {conflict.remote_content}
                        </Mono>
                      </Table.Cell>
                    </Table.Row>
                  ))}
                </Table.Body>
              </Table.Content>
            </Table.ScrollContainer>
          </Table>
        </DetailBlock>
      )}
    </>
  )
}

const certificateOperations = { issue: '签发', renew: '续期' } as const

function CertificateDetail({ detail }: { detail: DetailOf<'certificate'> }) {
  const operation = known(certificateOperations, detail.operation)
  const completed = detail.stage === 'completed'
  return (
    <DetailBlock title='证书'>
      <Facts
        items={[
          ...(operation
            ? []
            : ([
                [
                  '操作',
                  <UnknownValue key='operation' value={detail.operation} />,
                ],
              ] satisfies [string, ReactNode][])),
          [
            '证书颁发机构',
            detail.provider ? providerLabel(detail.provider) : '—',
          ],
          [
            '域名',
            <span key='domains' className='flex flex-wrap gap-x-2'>
              {detail.domains.map((domain) => (
                <Mono key={domain} className='text-xs'>
                  {domain}
                </Mono>
              ))}
            </span>,
          ],
          ...(completed
            ? ([
                ['签发者', detail.issuer || '—'],
                [
                  '有效期',
                  `${formatDate(detail.not_before)} 至 ${formatDate(detail.not_after)}`,
                ],
                [
                  '序列号',
                  <Mono key='serial' className='text-xs break-all'>
                    {detail.serial_number || '—'}
                  </Mono>,
                ],
                [
                  'SHA-256 指纹',
                  detail.fingerprint_sha256 ? (
                    <span
                      key='fingerprint'
                      className='flex min-w-0 items-center gap-1'
                    >
                      <Mono className='min-w-0 text-xs break-all'>
                        {detail.fingerprint_sha256}
                      </Mono>
                      <CopyButton
                        value={detail.fingerprint_sha256}
                        label='复制指纹'
                      />
                    </span>
                  ) : (
                    '—'
                  ),
                ],
              ] satisfies [string, ReactNode][])
            : []),
        ]}
      />
    </DetailBlock>
  )
}

const domainStatuses = {
  verified: { label: '解析正确', tone: 'success' },
  invalid: { label: '解析不匹配', tone: 'danger' },
  unverified: { label: '无法检查', tone: 'warning' },
} as const satisfies StatusDictionary
const domainStatusReserve = statusReserve(domainStatuses)

function WebsiteDomainsDetail({
  detail,
}: {
  detail: DetailOf<'website_domains'>
}) {
  return (
    <DetailBlock
      title='域名解析检查'
      aside={`已检查 ${detail.checked_count} / ${detail.domain_count} 个域名`}
    >
      <Table variant='secondary' className={compactTable}>
        <Table.ScrollContainer>
          <Table.Content
            aria-label='域名解析检查结果'
            className='min-w-[440px]'
          >
            <Table.Header>
              <Table.Column isRowHeader>域名</Table.Column>
              <Table.Column>期望</Table.Column>
              <Table.Column>实际</Table.Column>
              <Table.Column>状态</Table.Column>
            </Table.Header>
            <Table.Body
              renderEmptyState={() => (
                <p className='m-0 py-2 text-center text-xs text-muted'>无</p>
              )}
            >
              {detail.domains.map((domain) => (
                <Table.Row key={domain.hostname}>
                  <Table.Cell>
                    <Mono truncate className='max-w-40'>
                      {domain.hostname}
                    </Mono>
                    <Mono truncate className='max-w-40 text-muted'>
                      {domain.query_name !== domain.hostname
                        ? `查询 ${domain.query_name}`
                        : '—'}
                    </Mono>
                  </Table.Cell>
                  <Table.Cell>
                    <Mono truncate className='max-w-40'>
                      {domain.expected || detail.expected_target || '—'}
                    </Mono>
                  </Table.Cell>
                  <Table.Cell>
                    <Mono truncate className='max-w-40'>
                      {domain.resolved.join(', ') || '—'}
                    </Mono>
                    <span
                      className='block max-w-40 truncate text-muted'
                      title={
                        domain.error.replaceAll(redactedMarker, '（已隐藏）') ||
                        undefined
                      }
                    >
                      {domain.error ? (
                        <RedactedText value={domain.error} />
                      ) : (
                        '—'
                      )}
                    </span>
                  </Table.Cell>
                  <Table.Cell>
                    <StatusChip
                      {...describeStatus(domainStatuses, domain.status)}
                      reserve={domainStatusReserve}
                    />
                  </Table.Cell>
                </Table.Row>
              ))}
            </Table.Body>
          </Table.Content>
        </Table.ScrollContainer>
      </Table>
    </DetailBlock>
  )
}

const externalAccounts = {
  '': '—',
  reused: '复用已保存的 EAB 凭据',
  fetched: '本次新获取 EAB 凭据',
  not_required: '不需要',
} as const

/** `listed`: the verification completed, so the zone list was read and a zero
 * count is real. Failed (or unknown-outcome) attempts stop before listing and
 * report `zone_count: 0` (contract v1.1), shown as not obtained. */
function ProviderDetail({
  detail,
  listed,
}: {
  detail: DetailOf<'provider'>
  listed: boolean
}) {
  const kind = detail.provider_kind
  return (
    <DetailBlock title='验证结果'>
      <Facts
        items={[
          [
            kind === 'dns'
              ? 'DNS 服务商'
              : kind === 'certificate'
                ? '证书颁发机构'
                : '服务商',
            providerLabel(detail.provider) || '—',
          ],
          ...(kind === 'dns'
            ? ([
                [
                  '可见托管域',
                  listed ? (
                    `${detail.zone_count} 个`
                  ) : (
                    <span key='zones' className='text-muted'>
                      未取得
                    </span>
                  ),
                ],
              ] satisfies [string, ReactNode][])
            : kind !== 'certificate'
              ? ([
                  ['类型', <UnknownValue key='kind' value={kind} />],
                ] satisfies [string, ReactNode][])
              : ([
                  [
                    'ACME 目录',
                    detail.directory_url ? (
                      <Mono key='directory' className='text-xs break-all'>
                        {detail.directory_url}
                      </Mono>
                    ) : (
                      '—'
                    ),
                  ],
                  [
                    '外部账户绑定',
                    known(externalAccounts, detail.external_account) ?? (
                      <UnknownValue
                        key='account'
                        value={detail.external_account}
                      />
                    ),
                  ],
                ] satisfies [string, ReactNode][])),
        ]}
      />
      {kind === 'dns' && detail.zones.length > 0 && (
        <ul className='m-0 flex list-none flex-wrap gap-1.5 p-0'>
          {detail.zones.map((zone) => (
            <li key={zone.name}>
              <Chip size='sm' variant='secondary' className='gap-1.5'>
                <Mono className='text-xs'>{zone.name}</Mono>
                <span className='text-muted'>{zone.status}</span>
              </Chip>
            </li>
          ))}
        </ul>
      )}
    </DetailBlock>
  )
}

const nodeResults = {
  applied: { label: '已应用', tone: 'success' },
  failed: { label: '应用失败', tone: 'danger' },
} as const satisfies StatusDictionary
const nodeResultReserve = statusReserve(nodeResults)
const nodePhases = {
  '': '—',
  stage: '暂存配置',
  validate: '校验配置',
  activate: '激活配置',
} as const

function NodeApplyDetail({ detail }: { detail: DetailOf<'node_apply'> }) {
  const failed = detail.result === 'failed'
  return (
    <DetailBlock title='节点应用记录'>
      <Facts
        items={[
          [
            '节点',
            <Link
              key='node'
              href={`/nodes/${detail.node_id}`}
              className='text-xs'
            >
              {detail.node_name || detail.node_id}
            </Link>,
          ],
          [
            '结果',
            <StatusChip
              key='result'
              {...describeStatus(nodeResults, detail.result)}
              reserve={nodeResultReserve}
            />,
          ],
          [
            '来源',
            detail.source === 'heartbeat' ? (
              '心跳确认已在运行'
            ) : detail.source === 'apply_result' ? (
              '节点上报应用结果'
            ) : (
              <UnknownValue key='source' value={detail.source} />
            ),
          ],
          ['Agent 版本', detail.agent_version || '—'],
          ...(failed
            ? ([
                [
                  '失败阶段',
                  known(nodePhases, detail.phase) ?? (
                    <UnknownValue key='phase' value={detail.phase} />
                  ),
                ],
                [
                  '错误代码',
                  detail.error_code ? (
                    <Mono key='code' className='text-xs'>
                      {detail.error_code}
                    </Mono>
                  ) : (
                    '—'
                  ),
                ],
                ['节点判断', detail.retryable ? '可重试' : '不可重试'],
              ] satisfies [string, ReactNode][])
            : []),
        ]}
      />
    </DetailBlock>
  )
}
