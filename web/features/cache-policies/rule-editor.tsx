import { useState } from 'react'
import { Button } from '@heroui/react'
import { Choice, Field, TextAreaField, Toggle } from '@/components/forms'
import {
  BYTE_UNITS,
  DURATION_UNITS,
  MAX_SAFE_INTEGER,
  cacheRuleSchema,
  defaultByteUnit,
  defaultDurationUnit,
  formatUnitValue,
  unitValueToBase,
} from './form'
import type { CacheRule } from './types'
import { cloneCacheRule, isDefaultCacheRule } from './utils'

type Unit = {
  value: string
  label: string
  factor: number
  integerBase?: boolean
}
function Quantity({
  label,
  value,
  onChange,
  units,
  initialUnit,
  max,
  hint,
  error,
}: {
  label: string
  value: number
  onChange: (value: number) => void
  units: readonly Unit[]
  initialUnit: string
  max: number
  hint?: string
  error?: string
}) {
  const [unit, setUnit] = useState(initialUnit)
  const [text, setText] = useState(() =>
    formatUnitValue(
      value,
      units.find((item) => item.value === initialUnit) ?? units[0]
    )
  )
  const definition = units.find((item) => item.value === unit) ?? units[0]
  return (
    <div className='grid gap-1'>
      <div className='grid grid-cols-[minmax(0,1fr)_7rem] items-end gap-2'>
        <Field
          label={label}
          type='number'
          min={0}
          step='any'
          value={text}
          onChange={(raw) => {
            setText(raw)
            onChange(unitValueToBase(raw, definition, max) ?? Number.NaN)
          }}
          error={error}
        />
        <Choice
          label={`${label}单位`}
          value={unit}
          onChange={(next) => {
            const nextDefinition =
              units.find((item) => item.value === next) ?? units[0]
            setUnit(next)
            setText(formatUnitValue(value, nextDefinition))
          }}
          items={units.map((item) => ({ id: item.value, label: item.label }))}
        />
      </div>
      {hint && <p className='text-xs text-muted'>{hint}</p>}
    </div>
  )
}

export function CacheRuleEditor({
  rule,
  onApply,
  onCancel,
}: {
  rule: CacheRule
  onApply: (value: CacheRule) => void
  onCancel: () => void
}) {
  const [values, setValues] = useState(() => cloneCacheRule(rule))
  const [patterns, setPatterns] = useState(rule.patterns.join('\n'))
  const [errors, setErrors] = useState<Record<string, string>>({})
  const fixed = isDefaultCacheRule(rule)
  const update = <
    K extends Exclude<
      keyof CacheRule,
      'id' | 'status_codes' | 'stale_if_error_seconds'
    >,
  >(
    name: K,
    value: CacheRule[K]
  ) => setValues((draft) => ({ ...draft, [name]: value }))
  return (
    <form
      className='grid gap-5'
      onSubmit={(event) => {
        event.preventDefault()
        const result = cacheRuleSchema.safeParse({
          ...values,
          patterns:
            values.match_type === 'all'
              ? []
              : patterns
                  .split(/\r?\n/)
                  .map((item) => item.trim())
                  .filter(Boolean),
        })
        if (!result.success) {
          setErrors(
            Object.fromEntries(
              result.error.issues.map((issue) => [
                String(issue.path[0]),
                issue.message,
              ])
            )
          )
          return
        }
        setErrors({})
        onApply(result.data)
      }}
    >
      {fixed && (
        <p className='text-sm text-muted'>
          默认规则始终启用、匹配全部请求并执行缓存，其匹配结构不能修改。
        </p>
      )}
      <div className='grid gap-3 sm:grid-cols-2'>
        <Field
          label='条件名称'
          value={values.name}
          onChange={(value) => update('name', value)}
          required
          disabled={fixed}
          error={errors.name}
        />
        <Choice
          label='状态'
          value={values.status}
          onChange={(value) => update('status', value as CacheRule['status'])}
          disabled={fixed}
          items={[
            { id: 'enabled', label: '启用' },
            { id: 'disabled', label: '停用' },
          ]}
        />
      </div>
      <div className='grid gap-3 sm:grid-cols-2'>
        <Choice
          label='匹配方式'
          value={values.match_type}
          onChange={(value) => {
            update('match_type', value as CacheRule['match_type'])
            if (value === 'all') setPatterns('')
          }}
          disabled={fixed}
          items={[
            { id: 'all', label: '全部请求' },
            { id: 'prefix', label: '路径前缀' },
            { id: 'exact', label: '精确路径' },
            { id: 'extension', label: '文件扩展名' },
          ]}
        />
        <Choice
          label='执行行为'
          value={values.action}
          onChange={(value) => update('action', value as CacheRule['action'])}
          disabled={fixed}
          items={[
            { id: 'cache', label: '缓存' },
            { id: 'bypass', label: '绕过缓存' },
          ]}
        />
      </div>
      {values.match_type !== 'all' && (
        <TextAreaField
          label='匹配项'
          value={patterns}
          onChange={setPatterns}
          rows={4}
          required
          error={errors.patterns}
          hint={
            values.match_type === 'extension'
              ? '每行一个扩展名，例如 .css 或 .png，最多 64 项。'
              : '每行一个以 / 开头的路径，不含查询参数或片段，最多 64 项。'
          }
        />
      )}
      {values.action === 'cache' && (
        <>
          <section className='grid gap-3'>
            <h3 className='text-sm font-medium'>缓存与过期</h3>
            <div className='max-w-sm'>
              <Quantity
                label='缓存 TTL'
                value={values.ttl_seconds}
                onChange={(value) => update('ttl_seconds', value)}
                units={DURATION_UNITS}
                initialUnit={defaultDurationUnit(rule.ttl_seconds)}
                max={31536000}
                hint={
                  values.ignore_origin_cache_control
                    ? '0 秒表示每次复用前验证；最长 365 天。'
                    : '优先使用源站缓存时长；无源站时长且为 0 时，复用前验证。'
                }
                error={errors.ttl_seconds}
              />
            </div>
            <Choice
              label='查询参数'
              value={values.query_mode}
              onChange={(value) =>
                update('query_mode', value as CacheRule['query_mode'])
              }
              items={[
                { id: 'include', label: '包含在缓存键中' },
                { id: 'ignore', label: '忽略查询参数' },
              ]}
            />
          </section>
          <section className='grid gap-3'>
            <h3 className='text-sm font-medium'>对象限制</h3>
            <div className='grid gap-3 sm:grid-cols-2'>
              <Quantity
                label='最小对象大小'
                value={values.min_object_bytes}
                onChange={(value) => update('min_object_bytes', value)}
                units={BYTE_UNITS}
                initialUnit={defaultByteUnit(rule.min_object_bytes)}
                max={MAX_SAFE_INTEGER}
                error={errors.min_object_bytes}
              />
              <Quantity
                label='最大对象大小'
                value={values.max_object_bytes}
                onChange={(value) => update('max_object_bytes', value)}
                units={BYTE_UNITS}
                initialUnit={defaultByteUnit(rule.max_object_bytes)}
                max={MAX_SAFE_INTEGER}
                hint='0 表示使用节点对象上限。'
                error={errors.max_object_bytes}
              />
            </div>
          </section>
          <section>
            <Toggle
              label='忽略源站缓存时长'
              hint='使用策略缓存时长，仍遵守源站禁止缓存及重新验证指令。'
              selected={values.ignore_origin_cache_control}
              onChange={(value) => update('ignore_origin_cache_control', value)}
            />
            <Toggle
              label='支持范围请求'
              hint='允许缓存对象响应 Range 请求。'
              selected={values.range_enabled}
              onChange={(value) => update('range_enabled', value)}
            />
          </section>
        </>
      )}
      <div className='flex justify-end gap-2'>
        <Button size='sm' variant='secondary' onPress={onCancel}>
          取消条件
        </Button>
        <Button size='sm' type='submit'>
          应用到草稿
        </Button>
      </div>
    </form>
  )
}
