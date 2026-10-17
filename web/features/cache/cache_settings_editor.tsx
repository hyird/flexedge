import { useState } from 'react'
import { Plus, TrashBin } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field, Toggle } from '@/components/forms'
import { Section } from '@/components/section'
import { CacheNumberField } from './cache_policy_fields'
import {
  cacheSettingDefinitions,
  removeCacheSetting,
  type CacheSettings,
  type CacheSettingId,
} from './cache_settings'
import type { CacheRule } from './types'

const choices = (items: [string, string][]) =>
  items.map(([id, label]) => ({ id, label }))
const list = (text: string) =>
  text
    .split(/[,，\s]+/)
    .map((value) => value.trim())
    .filter(Boolean)
const varyActions = choices([
  ['normalize', '标准化'],
  ['passthrough', '原样区分'],
  ['bypass', '绕过缓存'],
])

function ListField({
  label,
  values,
  onChange,
  hint,
}: {
  label: string
  values: readonly (string | number)[]
  onChange: (value: string) => void
  hint?: string
}) {
  const [draft, setDraft] = useState({ values, text: values.join(', ') })
  if (draft.values !== values) {
    const matches =
      list(draft.text)
        .map((value) => value.toLowerCase())
        .join('\0') ===
      values.map((value) => String(value).toLowerCase()).join('\0')
    setDraft({ values, text: matches ? draft.text : values.join(', ') })
  }
  return (
    <Field
      label={label}
      value={draft.text}
      hint={hint}
      onChange={(next) => {
        setDraft({ values, text: next })
        onChange(next)
      }}
    />
  )
}

export function CacheSettingsEditor({
  value,
  settings,
  onChange,
  disabled = false,
}: {
  value: CacheRule
  settings: CacheSettings
  onChange: (patch: Partial<CacheRule>) => void
  disabled?: boolean
}) {
  const [adding, setAdding] = useState(false)
  const [search, setSearch] = useState('')
  const change = (patch: Partial<CacheSettings>) =>
    onChange({ settings: { ...settings, ...patch } })
  const keyChange = (patch: Partial<CacheSettings['key']>) =>
    change({ key: { ...settings.key, ...patch } })
  const content = (id: CacheSettingId) => {
    switch (id) {
      case 'edge':
        return (
          <div className='grid gap-4'>
            <Choice
              label='源站缓存头处理'
              value={value.ttl_mode}
              onChange={(ttl_mode) =>
                onChange({ ttl_mode: ttl_mode as CacheRule['ttl_mode'] })
              }
              items={choices([
                ['origin', '遵循源站；未指定时使用默认缓存时间'],
                ['origin_strict', '遵循源站；未指定时绕过缓存'],
                ['fixed', '忽略源站缓存头，使用指定时间'],
              ])}
            />
            {value.ttl_mode === 'fixed' && (
              <>
                <CacheNumberField
                  label='节点缓存时间（秒）'
                  value={value.ttl_seconds}
                  onChange={(ttl_seconds) => onChange({ ttl_seconds })}
                />
                <p className='text-xs leading-6 text-muted'>
                  覆盖 private、no-store 和 no-cache；Set-Cookie
                  响应移除该字段后缓存。
                </p>
              </>
            )}
          </div>
        )
      case 'browser':
        return (
          <div className='grid gap-4'>
            <Choice
              label='浏览器缓存策略'
              value={value.browser_ttl_mode}
              onChange={(mode) =>
                onChange({
                  browser_ttl_mode: mode as CacheRule['browser_ttl_mode'],
                  browser_ttl_seconds:
                    mode === 'override_origin'
                      ? value.browser_ttl_seconds || 14400
                      : value.browser_ttl_seconds,
                })
              }
              items={choices([
                ['default', '沿用全局默认（至少4小时）'],
                ['respect_origin', '遵循源站'],
                ['override_origin', '覆盖源站，使用指定时间'],
                ['bypass', '绕过浏览器缓存'],
              ])}
            />
            {value.browser_ttl_mode === 'override_origin' && (
              <CacheNumberField
                label='浏览器缓存时间（秒）'
                value={value.browser_ttl_seconds}
                onChange={(browser_ttl_seconds) =>
                  onChange({ browser_ttl_seconds })
                }
              />
            )}
          </div>
        )
      case 'status':
        return (
          <div className='grid gap-4'>
            {settings.status_ttl.map((status, index) => {
              const update = (patch: Partial<typeof status>) =>
                change({
                  status_ttl: settings.status_ttl.map((entry, position) =>
                    position === index ? { ...entry, ...patch } : entry
                  ),
                })
              return (
                <div
                  key={index}
                  className='grid gap-3 rounded-xl border border-border p-4 sm:grid-cols-2'
                >
                  <CacheNumberField
                    label='起始状态码'
                    value={status.from}
                    onChange={(from) => update({ from })}
                  />
                  <CacheNumberField
                    label='结束状态码'
                    value={status.to}
                    onChange={(to) => update({ to })}
                  />
                  <Choice
                    label='缓存行为'
                    value={
                      status.seconds < 0
                        ? 'no_store'
                        : status.seconds === 0
                          ? 'revalidate'
                          : 'ttl'
                    }
                    onChange={(mode) =>
                      update({
                        seconds:
                          mode === 'no_store'
                            ? -1
                            : mode === 'revalidate'
                              ? 0
                              : 7200,
                      })
                    }
                    items={choices([
                      ['ttl', '指定缓存时间'],
                      ['revalidate', '每次重新验证（0）'],
                      ['no_store', '不存储（-1）'],
                    ])}
                  />
                  {status.seconds > 0 && (
                    <CacheNumberField
                      label='缓存时间（秒）'
                      value={status.seconds}
                      onChange={(seconds) => update({ seconds })}
                    />
                  )}
                  <Button
                    size='sm'
                    variant='tertiary'
                    className='justify-self-end sm:col-span-2'
                    onPress={() =>
                      change({
                        status_ttl: settings.status_ttl.filter(
                          (_, position) => position !== index
                        ),
                      })
                    }
                  >
                    删除此条件
                  </Button>
                </div>
              )
            })}
            <Button
              size='sm'
              variant='secondary'
              className='justify-self-start'
              onPress={() =>
                change({
                  status_ttl: [
                    ...settings.status_ttl,
                    { from: 200, to: 299, seconds: 7200 },
                  ],
                })
              }
            >
              <Plus />
              添加状态码条件
            </Button>
            <p className='text-xs text-muted'>
              相同起止值表示单个状态码；范围不可重叠。此处的时间覆盖源站。
            </p>
          </div>
        )
      case 'key':
        return (
          <div className='grid gap-4'>
            <Choice
              label='查询参数'
              value={settings.key.query_mode}
              onChange={(query_mode) =>
                keyChange({
                  query_mode: query_mode as CacheSettings['key']['query_mode'],
                })
              }
              items={choices([
                ['all', '包含全部查询参数'],
                ['exclude', '包含全部，排除指定参数'],
                ['include', '仅包含指定参数'],
                ['ignore', '忽略全部查询参数'],
              ])}
            />
            {['include', 'exclude'].includes(settings.key.query_mode) && (
              <ListField
                label='查询参数名称'
                values={settings.key.query_parameters}
                onChange={(text) => keyChange({ query_parameters: list(text) })}
                hint='用逗号分隔，最多100个。'
              />
            )}
            <div className='grid gap-4 sm:grid-cols-2'>
              <ListField
                label='包含请求头的值'
                values={settings.key.headers}
                onChange={(text) =>
                  keyChange({ headers: list(text.toLowerCase()) })
                }
              />
              <ListField
                label='仅检查请求头是否存在'
                values={settings.key.header_presence}
                onChange={(text) =>
                  keyChange({ header_presence: list(text.toLowerCase()) })
                }
              />
              <ListField
                label='包含Cookie的值'
                values={settings.key.cookies}
                onChange={(text) => keyChange({ cookies: list(text) })}
              />
              <ListField
                label='仅检查Cookie是否存在'
                values={settings.key.cookie_presence}
                onChange={(text) => keyChange({ cookie_presence: list(text) })}
              />
            </div>
            {settings.key.header_values.map((field, index) => (
              <div
                key={index}
                className='grid gap-3 rounded-xl border border-border p-4 sm:grid-cols-2'
              >
                <Field
                  label='请求头名称'
                  value={field.name}
                  onChange={(name) =>
                    keyChange({
                      header_values: settings.key.header_values.map(
                        (entry, position) =>
                          position === index
                            ? { ...entry, name: name.toLowerCase() }
                            : entry
                      ),
                    })
                  }
                />
                <ListField
                  label='参与匹配的值'
                  values={field.values}
                  onChange={(text) =>
                    keyChange({
                      header_values: settings.key.header_values.map(
                        (entry, position) =>
                          position === index
                            ? { ...entry, values: list(text) }
                            : entry
                      ),
                    })
                  }
                  hint='1至3个值，用逗号分隔。'
                />
                <Button
                  size='sm'
                  variant='tertiary'
                  className='justify-self-end sm:col-span-2'
                  onPress={() =>
                    keyChange({
                      header_values: settings.key.header_values.filter(
                        (_, position) => position !== index
                      ),
                    })
                  }
                >
                  删除请求头条件
                </Button>
              </div>
            ))}
            <Button
              size='sm'
              variant='secondary'
              className='justify-self-start'
              onPress={() =>
                keyChange({
                  header_values: [
                    ...settings.key.header_values,
                    { name: '', values: [] },
                  ],
                })
              }
            >
              <Plus />
              按请求头的指定值区分
            </Button>
            <Choice
              label='缓存键中的主机名'
              value={settings.key.host}
              onChange={(host) =>
                keyChange({ host: host as CacheSettings['key']['host'] })
              }
              items={choices([
                ['original', '使用访客请求的原始主机名'],
                ['resolved', '使用源站解析主机名'],
              ])}
            />
            <div className='divide-y divide-border'>
              <Toggle
                label='包含Origin请求头'
                selected={settings.key.include_origin}
                onChange={(include_origin) => keyChange({ include_origin })}
              />
              <Toggle
                label='按访客国家区分'
                selected={settings.key.country}
                onChange={(country) => keyChange({ country })}
              />
              <Toggle
                label='按访客语言区分'
                selected={settings.key.language}
                onChange={(language) => keyChange({ language })}
              />
            </div>
          </div>
        )
      case 'armor':
        return (
          <Toggle
            label='启用缓存欺骗防护'
            selected={settings.armor}
            onChange={(armor) => change({ armor })}
            hint='检查扩展名与Content-Type；显式缓存指令或强制TTL可能覆盖此保护。'
          />
        )
      case 'device':
        return (
          <Toggle
            label='按设备类型区分缓存'
            selected={settings.device}
            onChange={(device) => change({ device })}
            hint='桌面、平板和手机不共用缓存。'
          />
        )
      case 'sort':
        return (
          <Toggle
            label='在缓存键中排序查询参数'
            selected={settings.sort_query}
            onChange={(sort_query) => change({ sort_query })}
            hint='参数内容相同、顺序不同的请求共用缓存。'
          />
        )
      case 'stale':
        return (
          <Toggle
            label='允许更新期间使用过期缓存'
            selected={settings.serve_stale}
            onChange={(serve_stale) => change({ serve_stale })}
            hint='仍遵循源站stale-while-revalidate及禁止使用过期内容的指令。'
          />
        )
      case 'etag':
        return (
          <Toggle
            label='遵循强ETag'
            selected={settings.strong_etags}
            onChange={(strong_etags) => change({ strong_etags })}
            hint='保留字节级验证语义；关闭时允许转换为弱ETag。'
          />
        )
      case 'origin':
        return (
          <Toggle
            label='启用源站缓存控制'
            selected={settings.origin_control}
            onChange={(origin_control) => change({ origin_control })}
            hint='默认开启，遵循源站共享缓存限制。'
          />
        )
      case 'vary':
        return (
          <div className='grid gap-4'>
            <Choice
              label='未单独配置的Vary字段'
              value={settings.vary_default}
              onChange={(vary_default) =>
                change({
                  vary_default: vary_default as CacheSettings['vary_default'],
                })
              }
              items={varyActions}
            />
            {settings.vary_headers.map((field, index) => {
              const update = (patch: Partial<typeof field>) =>
                change({
                  vary_headers: settings.vary_headers.map((entry, position) =>
                    position === index ? { ...entry, ...patch } : entry
                  ),
                })
              return (
                <div
                  key={index}
                  className='grid gap-3 rounded-xl border border-border p-4 sm:grid-cols-2'
                >
                  <Field
                    label='Vary请求头'
                    value={field.name}
                    onChange={(name) => update({ name: name.toLowerCase() })}
                  />
                  <Choice
                    label='变体处理'
                    value={field.action}
                    onChange={(action) =>
                      update({ action: action as typeof field.action })
                    }
                    items={varyActions}
                  />
                  {field.action === 'normalize' && (
                    <ListField
                      label='标准化允许值（可选）'
                      values={field.values}
                      onChange={(text) => update({ values: list(text) })}
                      hint='语言如zh、en；Accept使用MIME类型。'
                    />
                  )}
                  <Button
                    size='sm'
                    variant='tertiary'
                    className='self-end justify-self-end'
                    onPress={() =>
                      change({
                        vary_headers: settings.vary_headers.filter(
                          (_, position) => position !== index
                        ),
                      })
                    }
                  >
                    删除此字段
                  </Button>
                </div>
              )
            })}
            <Button
              size='sm'
              variant='secondary'
              className='justify-self-start'
              onPress={() =>
                change({
                  vary_headers: [
                    ...settings.vary_headers,
                    { name: '', action: 'passthrough', values: [] },
                  ],
                })
              }
            >
              <Plus />
              添加Vary字段
            </Button>
          </div>
        )
      case 'range':
        return (
          <Choice
            label='源站范围请求模式'
            value={settings.origin_range}
            onChange={(origin_range) =>
              change({
                origin_range: origin_range as CacheSettings['origin_range'],
              })
            }
            items={choices([
              ['default', '使用默认行为'],
              ['on', '开启'],
              ['off', '关闭'],
            ])}
            hint='关闭后仍可从完整缓存文件提供范围响应。'
          />
        )
      case 'ports':
        return (
          <ListField
            label='额外缓存端口'
            values={settings.ports}
            onChange={(text) => change({ ports: list(text).map(Number) })}
            hint='例如8443, 8080；不会创建新的监听端口。'
          />
        )
      case 'timeout':
        return (
          <CacheNumberField
            label='源站读取超时（秒）'
            value={settings.read_timeout}
            onChange={(read_timeout) => change({ read_timeout })}
            hint='两次读取之间的最大等待时间，不是整个下载的总时长。'
          />
        )
      case 'errors':
        return (
          <Toggle
            label='透传源站错误页面'
            selected={settings.error_passthrough}
            onChange={(error_passthrough) => change({ error_passthrough })}
          />
        )
    }
  }
  return (
    <div className='grid gap-4'>
      <div className='flex items-start justify-between gap-4'>
        <div>
          <h3 className='text-sm font-semibold'>缓存设置</h3>
          <p className='mt-1 text-xs leading-6 text-muted'>
            只覆盖添加的配置项，其他项目沿用默认行为或前面的匹配规则。
          </p>
        </div>
        <Button
          size='sm'
          variant='secondary'
          isDisabled={
            disabled ||
            settings.enabled.length === cacheSettingDefinitions.length
          }
          onPress={() => setAdding(true)}
        >
          <Plus />
          添加配置项
        </Button>
      </div>
      {cacheSettingDefinitions
        .filter((entry) => settings.enabled.includes(entry.id))
        .map((entry) => (
          <div
            key={entry.id}
            id={`setting-${entry.id}`}
            className='scroll-mt-5'
          >
            <Section
              title={entry.name}
              description={entry.english}
              actions={
                <Button
                  size='sm'
                  variant='tertiary'
                  isIconOnly
                  aria-label={`移除${entry.name}`}
                  isDisabled={disabled}
                  onPress={() =>
                    onChange({
                      settings: removeCacheSetting(settings, entry.id),
                    })
                  }
                >
                  <TrashBin />
                </Button>
              }
            >
              {content(entry.id)}
            </Section>
          </div>
        ))}
      {adding && (
        <FormSheet
          isOpen
          onClose={() => setAdding(false)}
          title='添加缓存配置'
          description='选择需要覆盖的设置。'
          size='lg'
        >
          <Field
            label='搜索配置项'
            value={search}
            onChange={setSearch}
            placeholder='例如缓存键、ETag、Vary…'
          />
          <div className='grid gap-2'>
            {cacheSettingDefinitions
              .filter((entry) =>
                `${entry.name} ${entry.english}`
                  .toLowerCase()
                  .includes(search.toLowerCase())
              )
              .map((entry) => (
                <div
                  key={entry.id}
                  className='flex items-center justify-between gap-4 rounded-xl border border-border p-4'
                >
                  <div>
                    <h4 className='text-sm font-medium'>{entry.name}</h4>
                    <p className='mt-1 text-xs text-muted'>{entry.english}</p>
                  </div>
                  <Button
                    size='sm'
                    variant='secondary'
                    isDisabled={settings.enabled.includes(entry.id)}
                    onPress={() => {
                      change({ enabled: [...settings.enabled, entry.id] })
                      setAdding(false)
                    }}
                  >
                    {settings.enabled.includes(entry.id) ? '已添加' : '添加'}
                  </Button>
                </div>
              ))}
          </div>
        </FormSheet>
      )}
    </div>
  )
}
