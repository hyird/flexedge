import { useState } from 'react'
import { Code, Plus, TrashBin } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { Choice, Field, TextAreaField, Toggle } from '@/components/forms'
import { SaveBar } from '@/components/save-bar'
import {
  builderExpression,
  expressionToBuilder,
  fields,
  listReferences,
  operatorOptions,
  parseExpression,
  type Condition,
} from './expression_model'
import { fieldListKind, type SecurityList } from './lists_model'
import { actions, expression, ruleSchema, type Rule } from './model'

export function SecurityRuleEditor({
  rule,
  creating,
  rules,
  lists,
  onSave,
}: {
  rule: Rule
  creating: boolean
  rules: Rule[]
  lists: SecurityList[]
  onSave: (rule: Rule, position: string) => Promise<Rule>
}) {
  const [value, setValue] = useState(() => structuredClone(rule))
  const [position, setPosition] = useState(creating ? 'last' : 'keep')
  const [error, setError] = useState('')
  const [saving, setSaving] = useState(false)
  const change = (patch: Partial<Rule>) => {
    setValue((current) => ({ ...current, ...patch }))
    setError('')
  }
  const conditionChange = (id: string, patch: Partial<Condition>) =>
    change({
      conditions: value.conditions.map((item) =>
        item.id === id ? { ...item, ...patch } : item
      ),
    })
  const toggleExpression = () => {
    try {
      if (value.expression_mode)
        change({
          ...expressionToBuilder(parseExpression(value.raw_expression)),
          expression_mode: false,
        })
      else
        change({
          expression_mode: true,
          raw_expression: builderExpression(value.conditions, value.logic),
        })
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '表达式无效')
    }
  }
  const add = (logic: Rule['logic']) =>
    change({
      logic,
      conditions: [
        ...value.conditions,
        { id: crypto.randomUUID(), field: 'path', operator: 'eq', value: '' },
      ],
    })
  const target = lists.find((list) => list.id === value.enrollment.list_id)
  const save = async () => {
    if (saving) return
    const result = ruleSchema.safeParse(value)
    if (!result.success) {
      setError(result.error.issues[0]?.message ?? '请检查规则配置')
      return
    }
    if (value.stage !== 'managed')
      for (const reference of listReferences(
        parseExpression(expression(result.data))
      )) {
        const list = lists.find((item) => item.name === reference.name)
        if (!list || list.kind !== fieldListKind[reference.field]) {
          setError(`清单 $${reference.name} 不存在或类型不匹配`)
          return
        }
      }
    if (value.enrollment.enabled && !target) {
      setError('请选择目标清单')
      return
    }
    setSaving(true)
    try {
      const saved = await onSave(result.data, position)
      setValue(saved)
      setPosition('keep')
    } catch (reason) {
      setError(apiErrorMessage(reason))
    } finally {
      setSaving(false)
    }
  }
  return (
    <form
      noValidate
      className='flex min-h-0 flex-1 flex-col gap-5'
      onSubmit={(event) => {
        event.preventDefault()
        void save()
      }}
    >
      <Field
        label='规则名称'
        value={value.name}
        onChange={(name) => change({ name })}
        placeholder='为规则输入一个名称'
        maxLength={80}
        required
      />
      <>
        <div className='flex flex-col gap-4 rounded-xl border border-border p-4'>
          <div className='flex flex-wrap items-center justify-between gap-2'>
            <h3 className='text-sm font-semibold'>当传入请求匹配…</h3>
            <Button size='sm' variant='tertiary' onPress={toggleExpression}>
              <Code />
              {value.expression_mode ? '使用表达式生成器' : '编辑表达式'}
            </Button>
          </div>
          {value.expression_mode ? (
            <TextAreaField
              label='匹配表达式'
              value={value.raw_expression}
              onChange={(raw_expression) => change({ raw_expression })}
              rows={6}
              placeholder='(ip.src in $malicious_ips) or (ip.src.asnum eq 64500)'
              className='[&_textarea]:font-mono [&_textarea]:text-xs'
            />
          ) : (
            <>
              {value.conditions.length > 1 && (
                <Choice
                  label='条件关系'
                  compact
                  value={value.logic}
                  items={[
                    { id: 'and', label: '满足全部（AND）' },
                    { id: 'or', label: '满足任意（OR）' },
                  ]}
                  onChange={(logic) =>
                    change({ logic: logic as Rule['logic'] })
                  }
                />
              )}
              {value.conditions.map((condition, index) => (
                <div
                  key={condition.id}
                  className='grid grid-cols-[minmax(0,1fr)_auto] items-end gap-2'
                >
                  <div className='grid gap-3 sm:grid-cols-[1fr_1fr_1.5fr]'>
                    <Choice
                      label={`字段 ${index + 1}`}
                      value={condition.field}
                      items={fields.map(({ id, label }) => ({ id, label }))}
                      onChange={(field) =>
                        conditionChange(condition.id, {
                          field: field as Condition['field'],
                          operator: 'eq',
                          value: '',
                        })
                      }
                    />
                    <Choice
                      label={`运算符 ${index + 1}`}
                      value={condition.operator}
                      items={operatorOptions(condition.field)}
                      onChange={(operator) =>
                        conditionChange(condition.id, {
                          operator: operator as Condition['operator'],
                          value: '',
                        })
                      }
                    />
                    {condition.operator === 'in_list' ? (
                      <Choice
                        label={`清单 ${index + 1}`}
                        value={condition.value}
                        items={lists
                          .filter(
                            (list) =>
                              list.kind === fieldListKind[condition.field]
                          )
                          .map((list) => ({
                            id: list.name,
                            label: `$${list.name}`,
                          }))}
                        onChange={(next) =>
                          conditionChange(condition.id, { value: next })
                        }
                      />
                    ) : (
                      <Field
                        label={`值 ${index + 1}`}
                        value={condition.value}
                        onChange={(next) =>
                          conditionChange(condition.id, {
                            value:
                              condition.field === 'country'
                                ? next.toUpperCase()
                                : next,
                          })
                        }
                        placeholder={
                          condition.field === 'ip'
                            ? condition.operator === 'in'
                              ? '192.0.2.10, 203.0.113.0/24'
                              : '192.0.2.10'
                            : condition.field === 'country'
                              ? 'CN, SG, US'
                              : condition.field === 'asn'
                                ? '64500'
                                : condition.field === 'method'
                                  ? 'POST'
                                  : '/api/login'
                        }
                        required
                      />
                    )}
                  </div>
                  <Button
                    isIconOnly
                    size='sm'
                    variant='tertiary'
                    aria-label={`删除条件 ${index + 1}`}
                    isDisabled={value.conditions.length === 1}
                    onPress={() =>
                      change({
                        conditions: value.conditions.filter(
                          (item) => item.id !== condition.id
                        ),
                      })
                    }
                  >
                    <TrashBin />
                  </Button>
                </div>
              ))}
              <div className='flex gap-2'>
                <Button
                  size='sm'
                  variant='secondary'
                  isDisabled={value.conditions.length >= 8}
                  onPress={() => add('and')}
                >
                  <Plus />
                  AND
                </Button>
                <Button
                  size='sm'
                  variant='secondary'
                  isDisabled={value.conditions.length >= 8}
                  onPress={() => add('or')}
                >
                  <Plus />
                  OR
                </Button>
              </div>
              <div className='border-t border-border pt-3'>
                <p className='mb-2 text-xs text-muted'>表达式预览</p>
                <pre
                  aria-label='表达式预览'
                  className='text-xs leading-6 break-all whitespace-pre-wrap'
                >
                  {expression(value)}
                </pre>
              </div>
            </>
          )}
        </div>
        {value.stage === 'rate' && (
          <div className='flex flex-col gap-3 rounded-xl border border-border p-4'>
            <h3 className='text-sm font-semibold'>当请求速率超过…</h3>
            <p className='text-xs text-muted'>按客户端 IP 统计匹配请求</p>
            <div className='grid gap-3 sm:grid-cols-2'>
              <Field
                label='请求数'
                type='number'
                min={1}
                value={String(value.limit)}
                onChange={(limit) => change({ limit: Number(limit) })}
              />
              <Field
                label='时间窗口（秒）'
                type='number'
                min={1}
                value={String(value.window)}
                onChange={(window) => change({ window: Number(window) })}
              />
            </div>
          </div>
        )}
      </>
      <div className='flex flex-col gap-4 rounded-xl border border-border p-4'>
        <h3 className='text-sm font-semibold'>然后采取操作…</h3>
        <Choice
          label='选择动作'
          value={value.action}
          items={Object.entries(actions)
            .filter(([id]) => id !== 'skip' || value.stage === 'custom')
            .map(([id, action]) => ({ id, label: action.label }))}
          onChange={(action) =>
            change({
              action: action as Rule['action'],
              enrollment: {
                ...value.enrollment,
                enabled: action === 'skip' ? false : value.enrollment.enabled,
              },
            })
          }
        />
        {value.action === 'skip' ? (
          <div className='flex flex-col gap-1'>
            {[
              { id: 'custom', label: '剩余自定义规则' },
              { id: 'rate', label: '所有限速规则' },
              { id: 'managed', label: '所有托管规则' },
            ].map((stage) => (
              <Toggle
                key={stage.id}
                label={stage.label}
                selected={value.skip.includes(stage.id as Rule['stage'])}
                onChange={(selected) =>
                  change({
                    skip: selected
                      ? [...value.skip, stage.id as Rule['stage']]
                      : value.skip.filter((item) => item !== stage.id),
                  })
                }
              />
            ))}
          </div>
        ) : (
          value.stage === 'rate' && (
            <Field
              label='处置时长（秒）'
              type='number'
              min={1}
              value={String(value.duration)}
              onChange={(duration) => change({ duration: Number(duration) })}
            />
          )
        )}
        {value.action === 'block' && (
          <>
            <Toggle
              label='自定义响应'
              selected={value.response.enabled}
              onChange={(enabled) =>
                change({ response: { ...value.response, enabled } })
              }
            />
            {value.response.enabled && (
              <>
                <div className='grid gap-3 sm:grid-cols-2'>
                  <Choice
                    label='响应类型'
                    value={value.response.content_type}
                    items={[
                      { id: 'text/html', label: 'HTML' },
                      { id: 'text/plain', label: '纯文本' },
                      { id: 'application/json', label: 'JSON' },
                      { id: 'application/xml', label: 'XML' },
                    ]}
                    onChange={(content_type) =>
                      change({
                        response: {
                          ...value.response,
                          content_type:
                            content_type as Rule['response']['content_type'],
                        },
                      })
                    }
                  />
                  <Field
                    label='响应状态码'
                    type='number'
                    min={400}
                    max={499}
                    value={String(value.response.status)}
                    onChange={(status) =>
                      change({
                        response: { ...value.response, status: Number(status) },
                      })
                    }
                  />
                </div>
                <TextAreaField
                  label='响应正文'
                  value={value.response.body}
                  onChange={(body) =>
                    change({ response: { ...value.response, body } })
                  }
                  rows={4}
                  hint='最多 2 KiB'
                  className='[&_textarea]:font-mono [&_textarea]:text-xs'
                />
              </>
            )}
          </>
        )}
      </div>
      {value.action !== 'skip' && (
        <div className='flex flex-col gap-3 rounded-xl border border-border p-4'>
          <Toggle
            label='触发后加入清单'
            selected={value.enrollment.enabled}
            onChange={(enabled) =>
              change({ enrollment: { ...value.enrollment, enabled } })
            }
          />
          {value.enrollment.enabled && (
            <>
              <Choice
                label='目标清单'
                value={value.enrollment.list_id}
                items={lists.map((list) => ({
                  id: list.id,
                  label: `${list.description || list.name} · $${list.name}`,
                }))}
                onChange={(list_id) =>
                  change({ enrollment: { ...value.enrollment, list_id } })
                }
                required
              />
              {target && (
                <p className='text-xs text-muted'>
                  写入：
                  {
                    {
                      ip: '客户端 IP',
                      asn: '客户端 ASN',
                      country: '客户端国家代码',
                      ua: '完整 User-Agent',
                    }[target.kind]
                  }
                </p>
              )}
              <Field
                label='保留时间（秒）'
                type='number'
                min={60}
                max={2592000}
                value={String(value.enrollment.ttl_seconds)}
                onChange={(ttl) =>
                  change({
                    enrollment: {
                      ...value.enrollment,
                      ttl_seconds: Number(ttl),
                    },
                  })
                }
              />
              <Toggle
                label='重复触发时续期'
                selected={value.enrollment.renew}
                onChange={(renew) =>
                  change({ enrollment: { ...value.enrollment, renew } })
                }
              />
            </>
          )}
        </div>
      )}
      <Choice
        label='放置位置'
        value={position}
        items={[
          ...(!creating ? [{ id: 'keep', label: '保持当前位置' }] : []),
          { id: 'first', label: '第一条' },
          { id: 'last', label: '最后一条' },
          ...rules
            .filter(
              (item) => item.stage === value.stage && item.id !== value.id
            )
            .map((item) => ({
              id: item.id,
              label: `在「${item.name}」之后`,
            })),
        ]}
        onChange={setPosition}
      />
      <Toggle
        label='启用规则'
        selected={value.enabled}
        onChange={(enabled) => change({ enabled })}
      />
      <SaveBar
        isDirty={
          creating ||
          JSON.stringify(value) !== JSON.stringify(rule) ||
          position !== 'keep'
        }
        isSaving={saving}
        onSave={save}
        onDiscard={() => {
          setValue(structuredClone(rule))
          setPosition(creating ? 'last' : 'keep')
          setError('')
        }}
        saveLabel={
          value.enabled ? (creating ? '部署规则' : '保存规则') : '保存草稿'
        }
        error={error}
      />
    </form>
  )
}
