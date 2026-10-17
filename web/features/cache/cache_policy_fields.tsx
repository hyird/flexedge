import { Choice, Field, TextAreaField } from '@/components/forms'
import { Section } from '@/components/section'
import type { CacheConfig, CachePolicyInput } from './types'

export function CacheNumberField({
  label,
  value,
  onChange,
  hint,
  error,
  unit = 1,
}: {
  label: string
  value: number
  onChange: (value: number) => void
  hint?: string
  error?: string
  unit?: number
}) {
  return (
    <Field
      label={label}
      type='number'
      min={0}
      step={1}
      value={String(value / unit)}
      onChange={(text) => onChange(Number(text) * unit)}
      hint={hint}
      error={error}
    />
  )
}
export function CachePolicyFields({
  value,
  onChange,
  errors = {},
}: {
  value: CachePolicyInput
  onChange: (value: CachePolicyInput) => void
  errors?: Record<string, string>
}) {
  const config = value.config
  const change = (patch: Partial<typeof config>) =>
    onChange({ ...value, config: { ...config, ...patch } })
  return (
    <div className='flex flex-col gap-6'>
      <Section
        title='基本信息'
        description='网站直接应用策略，策略修改会分发到所有引用网站。'
      >
        <div className='grid gap-4 sm:grid-cols-2'>
          <Field
            label='策略名称'
            required
            maxLength={100}
            value={value.name}
            onChange={(name) => onChange({ ...value, name })}
            error={errors.name}
          />
          <Choice
            label='状态'
            value={value.status}
            onChange={(status) =>
              onChange({
                ...value,
                status: status as CachePolicyInput['status'],
              })
            }
            items={[
              { id: 'enabled', label: '启用' },
              { id: 'disabled', label: '停用' },
            ]}
            hint='停用后，引用网站将绕过缓存。'
          />
          <div className='sm:col-span-2'>
            <TextAreaField
              label='描述'
              value={value.description}
              onChange={(description) => onChange({ ...value, description })}
              error={errors.description}
            />
          </div>
        </div>
      </Section>
      <Section
        title='缓存存储'
        description='容量按节点计算，同一节点上的所有网站与 Worker 共享策略容量。'
      >
        <div className='grid gap-4 sm:grid-cols-2'>
          <Choice
            label='存储方式'
            value={config.storage}
            onChange={(storage) =>
              change({ storage: storage as typeof config.storage })
            }
            items={[
              { id: 'tiered', label: '两级缓存（内存 + 磁盘）' },
              { id: 'memory', label: '内存缓存' },
              { id: 'disk', label: '磁盘缓存' },
            ]}
          />
          <CacheNumberField
            label='单对象大小上限（MiB）'
            unit={1024 ** 2}
            value={config.max_object_bytes}
            onChange={(max_object_bytes) => change({ max_object_bytes })}
            hint='最多64 MiB，超出上限的响应正常回源。'
            error={errors['config.max_object_bytes']}
          />
          {config.storage !== 'disk' && (
            <CacheCapacityFields
              kind='memory'
              config={config}
              change={change}
              errors={errors}
            />
          )}
          {config.storage !== 'memory' && (
            <>
              <CacheCapacityFields
                kind='disk'
                config={config}
                change={change}
                errors={errors}
              />
              <Field
                label='缓存目录'
                value={config.directory}
                onChange={(directory) => change({ directory })}
                hint='节点数据目录内的相对路径，例如 cache。'
                error={errors['config.directory']}
              />
            </>
          )}
        </div>
      </Section>
    </div>
  )
}

function CacheCapacityFields({
  kind,
  config,
  change,
  errors,
}: {
  kind: 'memory' | 'disk'
  config: CacheConfig
  change: (patch: Partial<CacheConfig>) => void
  errors: Record<string, string>
}) {
  const percent = `${kind}_percent` as const,
    bytes = `${kind}_bytes` as const,
    reserve = `${kind}_reserved_bytes` as const
  const label =
    kind === 'memory'
      ? config.storage === 'tiered'
        ? 'L1 内存'
        : '内存'
      : config.storage === 'tiered'
        ? 'L2 磁盘'
        : '磁盘'
  return (
    <div className='flex flex-col gap-4 rounded-xl border border-border p-4'>
      <Choice
        label={`${label}容量方式`}
        value={config[percent] ? 'percent' : 'fixed'}
        onChange={(value) =>
          change({
            [percent]: value === 'percent' ? (kind === 'memory' ? 10 : 50) : 0,
          })
        }
        items={[
          { id: 'percent', label: '按总容量百分比' },
          { id: 'fixed', label: '固定容量' },
        ]}
      />
      {config[percent] ? (
        <CacheNumberField
          label={`${label}占总容量（%）`}
          value={config[percent]}
          onChange={(value) => change({ [percent]: value })}
          error={errors[`config.${percent}`]}
          hint={
            kind === 'memory'
              ? '按每个节点的物理内存总量计算。'
              : '按缓存目录所在磁盘的总容量计算。'
          }
        />
      ) : (
        <CacheNumberField
          label={`${label}容量（MiB）`}
          unit={1024 ** 2}
          value={config[bytes]}
          onChange={(value) => change({ [bytes]: value })}
          error={errors[`config.${bytes}`]}
        />
      )}
      <CacheNumberField
        label={`${label}最低剩余空间（MiB）`}
        unit={1024 ** 2}
        value={config[reserve]}
        onChange={(value) => change({ [reserve]: value })}
        error={errors[`config.${reserve}`]}
        hint='达到预留线时停止新增并按 LRU 回收；其他进程的占用会影响实际可缓存容量。'
      />
    </div>
  )
}
