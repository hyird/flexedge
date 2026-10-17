import { useState } from 'react'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { FormSheet } from '@/components/form-sheet'
import {
  Choice,
  Field,
  TextAreaField,
  Toggle,
  Notice,
} from '@/components/forms'
import { CacheNumberField } from './cache_policy_fields'
import { cacheRuleSchema, cacheMatchLabels, type CacheRule } from './types'

export function CacheRuleEditor({
  rule,
  onClose,
  onSave,
}: {
  rule: CacheRule
  onClose: () => void
  onSave: (rule: CacheRule) => Promise<void>
}) {
  const [value, setValue] = useState(() => structuredClone(rule))
  const [patterns, setPatterns] = useState(rule.patterns.join('\n'))
  const [codes, setCodes] = useState(rule.status_codes.join(', '))
  const [advanced, setAdvanced] = useState(false)
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [saving, setSaving] = useState(false)
  const [saveError, setSaveError] = useState<string>()
  const change = (patch: Partial<CacheRule>) =>
    setValue((current) => ({ ...current, ...patch }))
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      title='编辑缓存规则'
      description='规则按列表顺序匹配，第一条命中的规则生效。'
      isSubmitting={saving}
      error={saveError}
      onSubmit={async (event) => {
        event.preventDefault()
        if (saving) return
        const result = cacheRuleSchema.safeParse({
          ...value,
          patterns:
            value.match_type === 'all'
              ? []
              : patterns
                  .split(/\r?\n/)
                  .map((v) => v.trim())
                  .filter(Boolean),
          status_codes: codes
            .split(/[,，\s]+/)
            .filter(Boolean)
            .map(Number),
        })
        if (!result.success) {
          setErrors(fieldErrors(result.error.issues))
          return
        }
        setErrors({})
        setSaveError(undefined)
        setSaving(true)
        try {
          await onSave(result.data)
        } catch (error) {
          setSaveError(apiErrorMessage(error))
        } finally {
          setSaving(false)
        }
      }}
    >
      <div inert={saving} className='flex flex-col gap-4'>
        <Field
          label='规则名称'
          value={value.name}
          onChange={(name) => change({ name })}
          required
          maxLength={100}
          error={errors.name}
        />
        <Toggle
          label='启用规则'
          selected={value.enabled}
          onChange={(enabled) => change({ enabled })}
        />
        <Choice
          label='匹配方式'
          value={value.match_type}
          onChange={(match_type) =>
            change({ match_type: match_type as CacheRule['match_type'] })
          }
          items={Object.entries(cacheMatchLabels).map(([id, label]) => ({
            id,
            label,
          }))}
        />
        {value.match_type !== 'all' && (
          <TextAreaField
            label='匹配内容'
            value={patterns}
            onChange={setPatterns}
            hint={
              value.match_type === 'extension'
                ? '每行一个扩展名，例如 .js、.css。'
                : '每行一项，多个条件满足任意一项即可。'
            }
            error={errors.patterns}
          />
        )}
        <Choice
          label='规则行为'
          value={value.action}
          onChange={(action) =>
            change({ action: action as CacheRule['action'] })
          }
          items={[
            { id: 'cache', label: '缓存' },
            { id: 'bypass', label: '绕过缓存' },
          ]}
        />
        {value.action === 'cache' && (
          <>
            <Choice
              label='缓存时间'
              value={value.ttl_mode}
              onChange={(ttl_mode) =>
                change({ ttl_mode: ttl_mode as CacheRule['ttl_mode'] })
              }
              items={[
                { id: 'origin', label: '遵循源站缓存时间' },
                { id: 'fixed', label: '指定缓存时间' },
              ]}
            />
            {value.ttl_mode === 'fixed' && (
              <CacheNumberField
                label='缓存时间（秒）'
                value={value.ttl_seconds}
                onChange={(ttl_seconds) => change({ ttl_seconds })}
                hint='例如 3600 为1小时，86400 为1天。'
                error={errors.ttl_seconds}
              />
            )}
            <Toggle
              label='忽略查询参数'
              selected={value.ignore_query}
              onChange={(ignore_query) => change({ ignore_query })}
              hint='开启后，相同路径的不同查询参数共用缓存。'
            />
            <Toggle
              label='高级设置'
              selected={advanced}
              onChange={setAdvanced}
            />
            {advanced && (
              <div className='flex flex-col gap-4'>
                <Choice
                  label='请求方法'
                  value={value.methods.length === 2 ? 'both' : value.methods[0]}
                  onChange={(v) =>
                    change({
                      methods:
                        v === 'both' ? ['GET', 'HEAD'] : [v as 'GET' | 'HEAD'],
                    })
                  }
                  items={[
                    { id: 'both', label: 'GET和HEAD' },
                    { id: 'GET', label: '仅GET' },
                  ]}
                />
                <Field
                  label='缓存状态码'
                  value={codes}
                  onChange={setCodes}
                  hint='多个状态码以逗号分隔，默认 200。'
                  error={errors.status_codes}
                />
                <CacheNumberField
                  label='最小内容大小（字节）'
                  value={value.min_object_bytes}
                  onChange={(min_object_bytes) => change({ min_object_bytes })}
                  error={errors.min_object_bytes}
                />
                <CacheNumberField
                  label='最大内容大小（字节）'
                  value={value.max_object_bytes}
                  onChange={(max_object_bytes) => change({ max_object_bytes })}
                  hint='0 表示使用策略的单对象大小上限。'
                  error={errors.max_object_bytes}
                />
                <Toggle
                  label='跳过带Set-Cookie的响应'
                  selected={value.skip_set_cookie}
                  onChange={(skip_set_cookie) => change({ skip_set_cookie })}
                />
                <CacheNumberField
                  label='浏览器缓存时间（秒）'
                  value={value.browser_ttl_seconds}
                  onChange={(browser_ttl_seconds) =>
                    change({ browser_ttl_seconds })
                  }
                  hint='0 表示保留源站的浏览器缓存设置。'
                  error={errors.browser_ttl_seconds}
                />
                <Toggle
                  label='跳过源站no-cache'
                  selected={value.skip_no_cache}
                  onChange={(skip_no_cache) => change({ skip_no_cache })}
                />
                <Toggle
                  label='遵循浏览器刷新'
                  selected={value.respect_request_no_cache}
                  onChange={(respect_request_no_cache) =>
                    change({ respect_request_no_cache })
                  }
                />
                <Notice>
                  关闭“跳过源站no-cache”后，按本规则缓存。开启“遵循浏览器刷新”时，刷新请求直接回源。private和no-store始终不缓存。
                </Notice>
              </div>
            )}
          </>
        )}
      </div>
    </FormSheet>
  )
}
