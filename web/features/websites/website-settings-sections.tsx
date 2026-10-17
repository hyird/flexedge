import { useQuery } from '@tanstack/react-query'
import {
  ArrowDown,
  ArrowRotateLeft,
  ArrowUp,
  Plus,
  TrashBin,
} from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { Choice, Field, Toggle } from '@/components/forms'
import { Mono } from '@/components/mono'
import { QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
import { uncoveredHostnames } from '@/features/certificates/certificate-coverage'
import { usableCertificateOptionsQuery } from '@/features/certificates/data'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { ArrayLines } from './array-lines'
import {
  CheckOption,
  FieldNotice,
  IconAction,
  NumberField,
} from './settings-controls'
import {
  setWebsiteHttpsEnabled,
  websiteCertificateOptions,
  websiteHostnames,
  type WebsiteFormValues,
} from './website-form'
import type { WebsiteSectionProps } from './website-sections'

const statusItems = [
  { id: 'enabled', label: '启用' },
  { id: 'disabled', label: '停用' },
]

function RefreshOptions({
  label,
  isPending,
  onRefresh,
}: {
  label: string
  isPending: boolean
  onRefresh: () => void
}) {
  return (
    <Button
      size='sm'
      variant='tertiary'
      isPending={isPending}
      onPress={onRefresh}
    >
      <ArrowRotateLeft className='size-3.5' />
      {label}
    </Button>
  )
}

export function BasicSection({ values, errors, update }: WebsiteSectionProps) {
  const clusters = useQuery(clusterOptionsQuery)
  const cluster = clusters.data?.find((item) => item.id === values.cluster_id)
  return (
    <Section title='基本信息' description='网站名称、所属集群与启停状态。'>
      <div className='flex flex-col gap-4'>
        <QueryNotice
          query={clusters}
          title='集群列表加载失败'
          onRetry={() => void clusters.refetch()}
        />
        <div className='grid gap-4 sm:grid-cols-2'>
          <Field
            label='网站名称'
            value={values.name}
            onChange={(name) => update('name', name)}
            error={errors.name}
            maxLength={100}
          />
          <Choice
            label='状态'
            value={values.status}
            onChange={(status) =>
              update('status', status as WebsiteFormValues['status'])
            }
            items={statusItems}
            hint='停用后边缘节点不再为该网站提供服务。'
          />
          <div className='flex flex-col gap-1.5'>
            <Choice
              label='所属集群'
              value={values.cluster_id}
              onChange={(clusterId) => update('cluster_id', clusterId)}
              items={(clusters.data ?? []).map((item) => ({
                id: item.id,
                label: `${item.name}${item.status === 'disabled' ? '（已停用）' : ''}`,
              }))}
              error={errors.cluster_id}
              hint={
                cluster
                  ? `接入域名 ${cluster.access_domain} · 节点 ${cluster.online_node_count}/${cluster.node_count} 在线`
                  : '配置会分发到所选集群的全部边缘节点。'
              }
            />
            <div className='flex flex-wrap items-center gap-x-4 gap-y-1'>
              <RefreshOptions
                label='刷新集群'
                isPending={clusters.isFetching}
                onRefresh={() => void clusters.refetch()}
              />
            </div>
          </div>
        </div>
      </div>
    </Section>
  )
}

export function HttpsSection({
  values,
  errors,
  update,
  replace,
  website,
}: WebsiteSectionProps) {
  const certificates = useQuery({
    ...usableCertificateOptionsQuery,
    enabled: values.https_enabled,
    // QueryNotice below shows failures; no duplicate toast.
    meta: { inlineError: true },
  })
  const hostnames = websiteHostnames(values)
  const options = websiteCertificateOptions(
    certificates.data ?? [],
    values.certificate_ids,
    website?.certificates ?? [],
    hostnames
  )
  const selectedUsable = options.filter(
    (certificate) =>
      certificate.usable && values.certificate_ids.includes(certificate.id)
  )
  const uncovered = selectedUsable.length
    ? uncoveredHostnames(selectedUsable, hostnames)
    : []
  const expiry = new Map(
    (certificates.data ?? []).map((item) => [item.id, item.expires_at])
  )
  return (
    <>
      <Section title='HTTPS' description='启用后客户端可通过TLS访问网站。'>
        <Toggle
          label='启用HTTPS'
          hint='关闭时会同时解除证书绑定、HTTPS跳转和HSTS。'
          selected={values.https_enabled}
          onChange={(enabled) =>
            replace((current) => setWebsiteHttpsEnabled(current, enabled))
          }
        />
      </Section>
      {values.https_enabled && (
        <>
          <Section
            title='绑定证书'
            description='可选择多张可用证书覆盖网站域名。'
            actions={
              <RefreshOptions
                label='刷新证书'
                isPending={certificates.isFetching}
                onRefresh={() => void certificates.refetch()}
              />
            }
          >
            <div className='flex flex-col gap-3'>
              <QueryNotice
                query={certificates}
                title='证书列表加载失败'
                onRetry={() => void certificates.refetch()}
              />
              {options.length > 0 && (
                <ul className='divide-y divide-separator'>
                  {options.map((certificate) => (
                    <li
                      key={certificate.id}
                      className='flex flex-wrap items-center gap-x-4 gap-y-1 py-3'
                    >
                      <CheckOption
                        selected={values.certificate_ids.includes(
                          certificate.id
                        )}
                        onChange={(selected) =>
                          update(
                            'certificate_ids',
                            selected
                              ? [...values.certificate_ids, certificate.id]
                              : values.certificate_ids.filter(
                                  (id) => id !== certificate.id
                                )
                          )
                        }
                        label={
                          <Mono className='break-all'>
                            {certificate.domains.join('、') || certificate.id}
                          </Mono>
                        }
                      />
                      {certificate.usable && !certificate.matched ? (
                        <span className='text-xs text-danger'>
                          未匹配网站域名
                        </span>
                      ) : (
                        <span className='text-xs text-muted'>
                          {certificate.usable
                            ? expiry.get(certificate.id)
                              ? `到期 ${formatDate(expiry.get(certificate.id))}`
                              : '可用'
                            : certificates.isPending || certificates.isError
                              ? '正在读取可用证书，当前绑定仍可移除'
                              : '证书不可用，请取消绑定并选择可用证书'}
                        </span>
                      )}
                    </li>
                  ))}
                </ul>
              )}
              {!certificates.isPending && options.length === 0 && (
                <p className='py-2 text-sm text-muted'>
                  {hostnames.length
                    ? '暂无匹配网站域名的可用证书。'
                    : '请先绑定域名'}
                </p>
              )}
              {uncovered.length > 0 && (
                <p className='text-sm text-warning'>
                  证书未覆盖：{uncovered.join('、')}
                </p>
              )}
              <FieldNotice>{errors.certificate_ids}</FieldNotice>
            </div>
          </Section>
          <Section title='TLS与跳转'>
            <div className='flex flex-col gap-2'>
              <div className='max-w-xs'>
                <Choice
                  label='最低TLS版本'
                  value={values.minimum_tls_version}
                  onChange={(version) =>
                    update('minimum_tls_version', version as '1.2' | '1.3')
                  }
                  items={[
                    { id: '1.2', label: 'TLS 1.2' },
                    { id: '1.3', label: 'TLS 1.3' },
                  ]}
                />
              </div>
              <Toggle
                label='HTTP自动跳转HTTPS'
                selected={values.force_https}
                onChange={(selected) => update('force_https', selected)}
              />
              <Toggle
                label='启用HSTS'
                hint='通知浏览器后续仅通过HTTPS访问。'
                selected={values.hsts_enabled}
                onChange={(selected) => update('hsts_enabled', selected)}
              />
            </div>
          </Section>
        </>
      )}
    </>
  )
}

const compressionAlgorithms = ['br', 'zstd', 'gzip'] as const

export function CompressionSection({
  values,
  errors,
  update,
}: WebsiteSectionProps) {
  const algorithms = values.response_compression_algorithms
  function moveAlgorithm(index: number, direction: -1 | 1) {
    const next = [...algorithms]
    ;[next[index], next[index + direction]] = [
      next[index + direction],
      next[index],
    ]
    update('response_compression_algorithms', next)
  }
  return (
    <Section title='响应压缩' description='按客户端支持的算法压缩文本类响应。'>
      <div className='flex flex-col gap-4'>
        <Toggle
          label='启用响应压缩'
          selected={values.response_compression_enabled}
          onChange={(selected) =>
            update('response_compression_enabled', selected)
          }
        />
        {values.response_compression_enabled && (
          <>
            <div className='grid gap-4 sm:grid-cols-2'>
              <NumberField
                label='最小响应大小（字节）'
                value={values.response_compression_min_bytes}
                onChange={(value) =>
                  update('response_compression_min_bytes', value)
                }
                error={errors.response_compression_min_bytes}
                hint='256–1048576 字节。'
                min={256}
                max={1048576}
              />
              <NumberField
                label='最大响应大小（字节）'
                value={values.response_compression_max_bytes}
                onChange={(value) =>
                  update('response_compression_max_bytes', value)
                }
                error={errors.response_compression_max_bytes}
                hint='0 表示不限制。'
                min={0}
                max={67108864}
              />
            </div>
            <div className='flex flex-col gap-2'>
              <div>
                <h3 className='subsection-title'>压缩算法优先级</h3>
                <p className='text-xs text-muted'>
                  按列表顺序选择客户端支持的第一个算法。
                </p>
              </div>
              <ol className='divide-y divide-border rounded-xl border border-border'>
                {algorithms.map((algorithm, index) => (
                  <li
                    key={algorithm}
                    className='flex items-center gap-2 px-3 py-1.5'
                  >
                    <span className='w-5 text-xs text-muted tabular-nums'>
                      {index + 1}
                    </span>
                    <Mono className='flex-1'>{algorithm}</Mono>
                    <IconAction
                      label={`上移 ${algorithm}`}
                      icon={<ArrowUp className='size-4' />}
                      isDisabled={index === 0}
                      onPress={() => moveAlgorithm(index, -1)}
                    />
                    <IconAction
                      label={`下移 ${algorithm}`}
                      icon={<ArrowDown className='size-4' />}
                      isDisabled={index === algorithms.length - 1}
                      onPress={() => moveAlgorithm(index, 1)}
                    />
                    <IconAction
                      label={`移除 ${algorithm}`}
                      icon={<TrashBin className='size-4' />}
                      onPress={() =>
                        update(
                          'response_compression_algorithms',
                          algorithms.filter((item) => item !== algorithm)
                        )
                      }
                    />
                  </li>
                ))}
                {algorithms.length === 0 && (
                  <li className='px-3 py-2 text-sm text-muted'>
                    尚未选择压缩算法。
                  </li>
                )}
              </ol>
              <div className='flex flex-wrap gap-2'>
                {compressionAlgorithms
                  .filter((algorithm) => !algorithms.includes(algorithm))
                  .map((algorithm) => (
                    <Button
                      key={algorithm}
                      size='sm'
                      variant='tertiary'
                      onPress={() =>
                        update('response_compression_algorithms', [
                          ...algorithms,
                          algorithm,
                        ])
                      }
                    >
                      <Plus className='size-4' />
                      添加 {algorithm}
                    </Button>
                  ))}
              </div>
              <FieldNotice>
                {errors.response_compression_algorithms}
              </FieldNotice>
            </div>
            <div className='grid gap-4 lg:grid-cols-3'>
              <ArrayLines
                label='MIME类型'
                rows={8}
                value={values.response_compression_mime_types}
                onChange={(items) =>
                  update('response_compression_mime_types', items)
                }
                hint='每行一项，可使用text/*。'
              />
              <ArrayLines
                label='扩展名'
                rows={8}
                value={values.response_compression_extensions}
                onChange={(items) =>
                  update('response_compression_extensions', items)
                }
                hint='每行一项，如 .js。'
              />
              <ArrayLines
                label='排除扩展名'
                rows={8}
                value={values.response_compression_excluded_extensions}
                onChange={(items) =>
                  update('response_compression_excluded_extensions', items)
                }
                hint='命中的扩展名不压缩。'
              />
            </div>
          </>
        )}
      </div>
    </Section>
  )
}

const logFields = [
  ['access_log_request_headers', '请求头'],
  ['access_log_response_headers', '响应头'],
  ['access_log_request_body', '请求体'],
  ['access_log_query_params', '查询参数'],
  ['access_log_cookies', 'Cookie'],
  ['access_log_referer', 'Referer'],
  ['access_log_user_agent', 'User-Agent'],
  ['access_log_client_abort', '客户端中断'],
] as const

const statusRanges = ['1xx', '2xx', '3xx', '4xx', '5xx'] as const

export function LogsSection({ values, errors, update }: WebsiteSectionProps) {
  return (
    <Section
      title='访问日志'
      description='记录的日志可在网站的访问日志中查看，并计入流量统计。'
    >
      <div className='flex flex-col gap-4'>
        <Toggle
          label='记录访问日志'
          selected={values.access_log_enabled}
          onChange={(selected) => update('access_log_enabled', selected)}
        />
        {values.access_log_enabled && (
          <>
            <div role='group' aria-labelledby='access-log-fields'>
              <h3 id='access-log-fields' className='subsection-title'>
                记录内容
              </h3>
              <div className='grid gap-x-6 sm:grid-cols-2 xl:grid-cols-4'>
                {logFields.map(([key, label]) => (
                  <CheckOption
                    key={key}
                    label={label}
                    selected={values[key]}
                    onChange={(selected) => update(key, selected)}
                  />
                ))}
              </div>
            </div>
            <div role='group' aria-labelledby='access-log-status'>
              <h3 id='access-log-status' className='subsection-title'>
                记录的状态码
              </h3>
              <div className='flex flex-wrap gap-x-6'>
                {statusRanges.map((range) => (
                  <CheckOption
                    key={range}
                    label={<Mono>{range}</Mono>}
                    selected={values.access_log_status_code_ranges.includes(
                      range
                    )}
                    onChange={(selected) =>
                      update(
                        'access_log_status_code_ranges',
                        selected
                          ? [...values.access_log_status_code_ranges, range]
                          : values.access_log_status_code_ranges.filter(
                              (item) => item !== range
                            )
                      )
                    }
                  />
                ))}
              </div>
              <FieldNotice>{errors.access_log_status_code_ranges}</FieldNotice>
            </div>
          </>
        )}
      </div>
    </Section>
  )
}
