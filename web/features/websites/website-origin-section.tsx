import { useState } from 'react'
import { Pencil, Plus, TrashBin } from '@gravity-ui/icons'
import { Button, Chip } from '@heroui/react'
import { Choice, Field, Notice, Toggle } from '@/components/forms'
import { Mono } from '@/components/mono'
import { Section } from '@/components/section'
import {
  collectOriginGroups,
  nextOriginGroupName,
  originRemovalError,
  renameOriginGroup,
} from './origin-groups'
import { FieldNotice, IconAction, NumberField } from './settings-controls'
import type { WebsiteOrigin } from './types'
import { originGroupName } from './website-display'
import type { WebsiteSectionProps } from './website-sections'

function OriginGroupList({
  values,
  replace,
  onProblem,
}: Pick<WebsiteSectionProps, 'values' | 'replace'> & {
  onProblem: (message: string) => void
}) {
  const [renaming, setRenaming] = useState<string | null>(null)
  const [draft, setDraft] = useState('')
  const [renameError, setRenameError] = useState('')
  const groups = collectOriginGroups(values.origins)
  return (
    <ul className='divide-y divide-separator'>
      {groups.map((group) => (
        <li key={group.name} className='flex flex-col gap-2 py-3'>
          {renaming === group.name ? (
            <div className='flex flex-wrap items-start gap-2'>
              <div className='min-w-48 flex-1'>
                <Field
                  label='源站组新名称'
                  value={draft}
                  onChange={setDraft}
                  error={renameError}
                />
              </div>
              <div className='flex gap-2 sm:pt-6'>
                <Button
                  size='sm'
                  variant='tertiary'
                  onPress={() => {
                    const result = renameOriginGroup(
                      values,
                      group.name,
                      draft === originGroupName(group.name) ? group.name : draft
                    )
                    if (!result.ok) {
                      setRenameError(result.message)
                      return
                    }
                    replace((current) => ({ ...current, ...result.value }))
                    setRenaming(null)
                    onProblem('')
                  }}
                >
                  应用
                </Button>
                <Button
                  size='sm'
                  variant='tertiary'
                  onPress={() => setRenaming(null)}
                >
                  取消
                </Button>
              </div>
            </div>
          ) : (
            <div className='flex flex-wrap items-center gap-x-3 gap-y-1'>
              <span className='text-sm font-medium'>
                {originGroupName(group.name)}
              </span>
              {values.default_origin_group.trim() === group.name && (
                <Chip size='sm' variant='soft' color='accent'>
                  默认
                </Chip>
              )}
              <span className='text-xs text-muted tabular-nums'>
                {group.indexes.length} 个源站 · {group.enabledPrimaryCount} 主源
                / {group.enabledCount} 启用
              </span>
              <span className='ml-auto'>
                <IconAction
                  label={`重命名源站组 ${originGroupName(group.name)}`}
                  icon={<Pencil className='size-4' />}
                  onPress={() => {
                    setRenaming(group.name)
                    setDraft(originGroupName(group.name))
                    setRenameError('')
                  }}
                />
              </span>
            </div>
          )}
        </li>
      ))}
      {groups.length === 0 && (
        <li className='py-3 text-sm text-muted'>
          还没有源站组，添加源站时填写源站组名称。
        </li>
      )}
    </ul>
  )
}

function newOrigin(group: string): WebsiteOrigin {
  return {
    id: crypto.randomUUID(),
    group,
    protocol: 'http',
    host: '',
    port: 80,
    role: 'primary',
    weight: 100,
    status: 'enabled',
  }
}

export function OriginsSection({
  values,
  errors,
  update,
  replace,
}: WebsiteSectionProps) {
  const [problem, setProblem] = useState('')
  const groups = collectOriginGroups(values.origins)
  function patchOrigin(id: string, patch: Partial<WebsiteOrigin>) {
    update(
      'origins',
      values.origins.map((origin) =>
        origin.id === id ? { ...origin, ...patch } : origin
      )
    )
  }
  return (
    <>
      <Section
        title='源站组'
        description='请求默认发往默认源站组；路由规则可把请求转发到其他源站组。'
      >
        <div className='flex flex-col gap-4'>
          <div className='max-w-sm'>
            <Choice
              label='默认源站组'
              value={values.default_origin_group}
              onChange={(group) => update('default_origin_group', group)}
              items={groups.map((group) => ({
                id: group.name,
                label: originGroupName(group.name),
              }))}
              error={errors.default_origin_group}
            />
          </div>
          <OriginGroupList
            values={values}
            replace={replace}
            onProblem={setProblem}
          />
        </div>
      </Section>
      <Section
        title='源站'
        description='同一源站组中的主源按权重分发，主源都不可用时使用备用源站。'
        actions={
          <>
            <Button
              size='sm'
              variant='tertiary'
              isDisabled={values.origins.length >= 100}
              onPress={() =>
                update('origins', [
                  ...values.origins,
                  newOrigin(nextOriginGroupName(groups)),
                ])
              }
            >
              <Plus className='size-4' />
              添加源站组
            </Button>
            <Button
              size='sm'
              variant='tertiary'
              isDisabled={values.origins.length >= 100}
              onPress={() =>
                update('origins', [
                  ...values.origins,
                  newOrigin(values.default_origin_group || 'default'),
                ])
              }
            >
              <Plus className='size-4' />
              添加源站
            </Button>
          </>
        }
      >
        <div className='flex flex-col gap-3'>
          <Notice>{problem}</Notice>
          {values.origins.map((origin, index) => (
            <div
              key={origin.id}
              role='group'
              aria-label={`源站 ${index + 1}`}
              className='flex flex-col gap-3 rounded-xl border border-border p-3 sm:p-4'
            >
              <div className='flex items-center justify-between gap-3'>
                <h3 className='subsection-title min-w-0 truncate'>
                  源站 {index + 1}
                  {origin.host && (
                    <Mono className='ml-2 text-muted'>
                      {origin.protocol}://{origin.host}:{origin.port}
                    </Mono>
                  )}
                </h3>
                <div className='flex shrink-0 items-center gap-3 whitespace-nowrap'>
                  <Toggle
                    label='启用'
                    selected={origin.status === 'enabled'}
                    onChange={(enabled) =>
                      patchOrigin(origin.id, {
                        status: enabled ? 'enabled' : 'disabled',
                      })
                    }
                  />
                  <IconAction
                    label={`移除源站 ${index + 1}`}
                    icon={<TrashBin className='size-4' />}
                    onPress={() => {
                      const removal = originRemovalError(values, index)
                      if (removal) {
                        setProblem(removal)
                        return
                      }
                      setProblem('')
                      update(
                        'origins',
                        values.origins.filter((item) => item.id !== origin.id)
                      )
                    }}
                  />
                </div>
              </div>
              <div className='grid gap-3 sm:grid-cols-2 xl:grid-cols-[minmax(0,1fr)_minmax(0,1.5fr)_7rem_7rem_8rem_6rem]'>
                <Field
                  label='源站组'
                  value={originGroupName(origin.group)}
                  onChange={(group) => {
                    if (group !== originGroupName(origin.group))
                      patchOrigin(origin.id, { group })
                  }}
                  error={errors[`origins.${index}.group`]}
                />
                <Field
                  label='主机 / IP'
                  value={origin.host}
                  onChange={(host) => patchOrigin(origin.id, { host })}
                  error={errors[`origins.${index}.host`]}
                  placeholder='origin.example.com'
                  className='[&_input]:font-mono'
                />
                <Choice
                  label='协议'
                  value={origin.protocol}
                  onChange={(protocol) =>
                    patchOrigin(origin.id, {
                      protocol: protocol as WebsiteOrigin['protocol'],
                    })
                  }
                  items={[
                    { id: 'http', label: 'HTTP' },
                    { id: 'https', label: 'HTTPS' },
                  ]}
                />
                <NumberField
                  label='端口'
                  value={origin.port}
                  onChange={(port) => patchOrigin(origin.id, { port })}
                  error={errors[`origins.${index}.port`]}
                  min={1}
                  max={65535}
                />
                <Choice
                  label='角色'
                  value={origin.role}
                  onChange={(role) =>
                    patchOrigin(origin.id, {
                      role: role as WebsiteOrigin['role'],
                    })
                  }
                  items={[
                    { id: 'primary', label: '主源站' },
                    { id: 'backup', label: '备用源站' },
                  ]}
                />
                <NumberField
                  label='权重'
                  value={origin.weight}
                  onChange={(weight) => patchOrigin(origin.id, { weight })}
                  error={errors[`origins.${index}.weight`]}
                  min={1}
                  max={100}
                />
              </div>
            </div>
          ))}
          {values.origins.length === 0 && (
            <p className='py-2 text-sm text-muted'>
              还没有源站，至少添加一个源站才能保存。
            </p>
          )}
          <FieldNotice>{errors.origins}</FieldNotice>
        </div>
      </Section>
    </>
  )
}

export function OriginSettingsSection({
  values,
  errors,
  update,
}: WebsiteSectionProps) {
  return (
    <Section title='回源' description='边缘节点向源站发起请求时使用的参数。'>
      <div className='flex flex-col gap-4'>
        <div className='grid gap-4 sm:grid-cols-2'>
          <Field
            label='回源Host'
            value={values.origin_host_header}
            onChange={(host) => update('origin_host_header', host)}
            error={errors.origin_host_header}
            hint='$host保留客户端请求域名，也可指定源站域名。'
            className='[&_input]:font-mono'
          />
          <Choice
            label='回源HTTP版本'
            value={values.origin_http_version}
            onChange={(value) =>
              update('origin_http_version', value as 'auto' | 'http1' | 'http2')
            }
            items={[
              { id: 'auto', label: '自动协商' },
              { id: 'http1', label: 'HTTP/1.1' },
              { id: 'http2', label: 'HTTP/2' },
            ]}
            hint='自动模式通过TLS协商；明文HTTP/2请选择HTTP/2。'
            error={errors.origin_http_version}
          />
          <NumberField
            label='连接超时（秒）'
            value={values.origin_connect_timeout_seconds}
            onChange={(value) =>
              update('origin_connect_timeout_seconds', value)
            }
            error={errors.origin_connect_timeout_seconds}
            hint='1–300 秒。'
            min={1}
            max={300}
          />
          <NumberField
            label='读取超时（秒）'
            value={values.origin_read_timeout_seconds}
            onChange={(value) => update('origin_read_timeout_seconds', value)}
            error={errors.origin_read_timeout_seconds}
            hint='1–600 秒。'
            min={1}
            max={600}
          />
        </div>
        <Toggle
          label='传递客户端IP'
          hint='通过转发头把客户端地址传给源站。'
          selected={values.pass_client_ip}
          onChange={(selected) => update('pass_client_ip', selected)}
        />
      </div>
    </Section>
  )
}

export function HealthSection({ values, errors, update }: WebsiteSectionProps) {
  return (
    <Section
      title='健康检查'
      description='边缘节点定期探测源站，连续失败的源站会暂停接收请求。'
    >
      <div className='flex flex-col gap-4'>
        <Toggle
          label='主动健康检查'
          selected={values.health_check_enabled}
          onChange={(selected) => update('health_check_enabled', selected)}
        />
        {values.health_check_enabled && (
          <div className='grid gap-4 sm:grid-cols-2 xl:grid-cols-3'>
            <Field
              label='检查路径'
              value={values.health_check_path}
              onChange={(path) => update('health_check_path', path)}
              error={errors.health_check_path}
              className='[&_input]:font-mono'
            />
            <NumberField
              label='预期状态码'
              value={values.health_check_expected_status}
              onChange={(value) =>
                update('health_check_expected_status', value)
              }
              error={errors.health_check_expected_status}
              min={100}
              max={599}
            />
            <NumberField
              label='检查间隔（秒）'
              value={values.health_check_interval_seconds}
              onChange={(value) =>
                update('health_check_interval_seconds', value)
              }
              error={errors.health_check_interval_seconds}
              min={1}
              max={3600}
            />
            <NumberField
              label='检查超时（秒）'
              value={values.health_check_timeout_seconds}
              onChange={(value) =>
                update('health_check_timeout_seconds', value)
              }
              error={errors.health_check_timeout_seconds}
              min={1}
              max={300}
            />
            <NumberField
              label='恢复健康阈值'
              value={values.healthy_threshold}
              onChange={(value) => update('healthy_threshold', value)}
              error={errors.healthy_threshold}
              hint='连续成功次数，1–10。'
              min={1}
              max={10}
            />
            <NumberField
              label='判定失败阈值'
              value={values.unhealthy_threshold}
              onChange={(value) => update('unhealthy_threshold', value)}
              error={errors.unhealthy_threshold}
              hint='连续失败次数，1–10。'
              min={1}
              max={10}
            />
          </div>
        )}
      </div>
    </Section>
  )
}
