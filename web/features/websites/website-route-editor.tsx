import { useState } from 'react'
import { Button, Chip } from '@heroui/react'
import {
  Choice,
  Field,
  Notice,
  TextAreaField,
  Toggle,
} from '@/components/forms'
import { EmptyState } from '@/components/page'
import { ArrayLines } from './array-lines'
import { previewRoute } from './route-preview'
import { routeTabs } from './route-tabs'
import {
  routeMethods,
  routeRuleToForm,
  type WebsiteRouteRuleForm,
} from './website-form'

type Rule = WebsiteRouteRuleForm

export function WebsiteRouteEditor({
  rules,
  onChange,
  groups,
  errors,
}: {
  rules: Rule[]
  onChange: (rules: Rule[]) => void
  groups: string[]
  errors: Record<string, string>
}) {
  const [phase, setPhase] = useState<Rule['action']>('proxy')
  const [selected, setSelected] = useState<string | null>(null)
  const [testHost, setTestHost] = useState('')
  const [testTarget, setTestTarget] = useState('/')
  const [testMethod, setTestMethod] = useState('GET')
  const [testHeaders, setTestHeaders] = useState('')
  const phaseRules = rules.filter((rule) => rule.action === phase)
  const rule = phaseRules.find((item) => item.id === selected) ?? phaseRules[0]
  const index = rule ? rules.findIndex((item) => item.id === rule.id) : -1
  function update(patch: Partial<Rule>) {
    if (rule)
      onChange(
        rules.map((item) =>
          item.id === rule.id ? { ...item, ...patch } : item
        )
      )
  }
  function move(direction: -1 | 1) {
    if (!rule) return
    const other =
      phaseRules[
        phaseRules.findIndex((item) => item.id === rule.id) + direction
      ]
    if (!other) return
    const next = [...rules]
    const otherIndex = next.findIndex((item) => item.id === other.id)
    ;[next[index], next[otherIndex]] = [next[otherIndex], next[index]]
    onChange(next)
  }
  const error = (field: string) => errors[`route_rules.${index}.${field}`]
  const preview = previewRoute(rules, testMethod, testTarget, testHost, [
    ['host', testHost],
    ...testHeaders
      .split('\n')
      .filter((line) => line.includes(':'))
      .map((line): [string, string] => {
        const at = line.indexOf(':')
        return [line.slice(0, at).trim(), line.slice(at + 1).trim()]
      }),
  ])
  return (
    <div className='space-y-5'>
      <div className='flex flex-wrap items-end justify-between gap-3'>
        <Choice
          compact
          label='规则阶段'
          value={phase}
          onChange={(value) => {
            setPhase(value as Rule['action'])
            setSelected(null)
          }}
          items={Object.entries(routeTabs).map(([id, item]) => ({
            id,
            label: item.title,
          }))}
        />
        <Button
          size='sm'
          variant='secondary'
          isDisabled={rules.length >= 100}
          onPress={() => {
            const added = routeRuleToForm({
              id: crypto.randomUUID(),
              action: phase,
              origin_group: phase === 'proxy' ? (groups[0] ?? 'default') : '',
              redirect_status: phase === 'redirect' ? 302 : 0,
            })
            onChange([...rules, added])
            setSelected(added.id)
          }}
        >
          添加规则
        </Button>
      </div>
      <p className='text-sm text-muted'>{routeTabs[phase].description}</p>
      <Notice>{errors.route_rules}</Notice>
      {Object.entries(errors)
        .filter(([path]) => /^route_rules\.\d+\./.test(path))
        .map(([path, message]) => {
          const routeIndex = Number(path.split('.')[1])
          const invalid = rules[routeIndex]
          return (
            <Button
              key={path}
              size='sm'
              variant='danger-soft'
              className='h-auto text-start whitespace-normal'
              onPress={() => {
                if (invalid) {
                  setPhase(invalid.action)
                  setSelected(invalid.id)
                }
              }}
            >
              规则 {routeIndex + 1}：{message}
            </Button>
          )
        })}
      {!rule ? (
        <EmptyState
          title={routeTabs[phase].empty}
          description='添加规则可按路径、域名、请求方法和条件控制请求。'
        />
      ) : (
        <>
          <div className='flex flex-wrap items-end gap-2'>
            <div className='min-w-52 flex-1'>
              <Choice
                label='当前规则'
                value={rule.id}
                onChange={setSelected}
                items={phaseRules.map((item, position) => ({
                  id: item.id,
                  label: `${position + 1}. ${item.name || item.path}${item.status === 'disabled' ? '（停用）' : ''}`,
                }))}
              />
            </div>
            <Button
              size='sm'
              variant='tertiary'
              onPress={() => move(-1)}
              isDisabled={phaseRules[0]?.id === rule.id}
            >
              上移
            </Button>
            <Button
              size='sm'
              variant='tertiary'
              onPress={() => move(1)}
              isDisabled={phaseRules.at(-1)?.id === rule.id}
            >
              下移
            </Button>
            <Button
              size='sm'
              variant='secondary'
              onPress={() => {
                const copied = {
                  ...structuredClone(rule),
                  id: crypto.randomUUID(),
                  name: rule.name ? `${rule.name} 副本` : '',
                }
                onChange([...rules, copied])
                setSelected(copied.id)
              }}
              isDisabled={rules.length >= 100}
            >
              复制
            </Button>
            <Button
              size='sm'
              variant='danger-soft'
              onPress={() => {
                onChange(rules.filter((item) => item.id !== rule.id))
                setSelected(null)
              }}
            >
              移除
            </Button>
          </div>
          <section
            key={rule.id}
            className='space-y-4 rounded-2xl bg-surface-secondary p-4'
          >
            <div className='grid gap-3 sm:grid-cols-2'>
              <Field
                label='规则名称'
                value={rule.name}
                onChange={(name) => update({ name })}
                error={error('name')}
              />
              <Choice
                label='路径匹配'
                value={rule.match_type}
                onChange={(value) =>
                  update({ match_type: value as Rule['match_type'] })
                }
                items={[
                  { id: 'prefix', label: '前缀' },
                  { id: 'exact', label: '精确' },
                  { id: 'suffix', label: '后缀' },
                  { id: 'regex', label: 'RE2 正则' },
                ]}
              />
              <Field
                label='匹配路径'
                value={rule.path}
                onChange={(path) => update({ path })}
                error={error('path')}
                hint={
                  rule.match_type === 'regex'
                    ? '使用 RE2 语法，最多 9 个捕获组。'
                    : undefined
                }
              />
              <TextAreaField
                label='备注'
                value={rule.description}
                onChange={(description) => update({ description })}
                error={error('description')}
                rows={2}
              />
            </div>
            <Toggle
              label='启用规则'
              selected={rule.status === 'enabled'}
              onChange={(selected) =>
                update({ status: selected ? 'enabled' : 'disabled' })
              }
            />
            <ArrayLines
              label='匹配域名'
              value={rule.hostnames}
              onChange={(hostnames) => update({ hostnames })}
              hint='每行一个精确域名，留空匹配所有域名。'
              error={error('hostnames')}
            />
            <div>
              <h4 className='text-sm font-medium'>请求方法</h4>
              <p className='mt-1 text-xs text-muted'>
                全部关闭表示匹配所有方法。
              </p>
              <div className='grid gap-x-4 sm:grid-cols-3'>
                {routeMethods.map((method) => (
                  <Toggle
                    key={method}
                    label={method}
                    selected={rule.methods.includes(method)}
                    onChange={(selected) =>
                      update({
                        methods: selected
                          ? [...rule.methods, method]
                          : rule.methods.filter((item) => item !== method),
                      })
                    }
                  />
                ))}
              </div>
            </div>
            <div className='space-y-3'>
              <div className='flex items-center justify-between gap-3'>
                <h4 className='text-sm font-medium'>附加条件</h4>
                <Button
                  size='sm'
                  variant='tertiary'
                  isDisabled={rule.conditions.length >= 20}
                  onPress={() =>
                    update({
                      conditions: [
                        ...rule.conditions,
                        { source: 'header', name: '', op: 'equals', value: '' },
                      ],
                    })
                  }
                >
                  添加条件
                </Button>
              </div>
              {rule.conditions.map((condition, conditionIndex) => {
                const change = (patch: Partial<typeof condition>) =>
                  update({
                    conditions: rule.conditions.map((item, i) =>
                      i === conditionIndex ? { ...item, ...patch } : item
                    ),
                  })
                return (
                  <div
                    key={conditionIndex}
                    className='grid items-end gap-2 sm:grid-cols-2 lg:grid-cols-[8rem_1fr_8rem_1fr_auto]'
                  >
                    <Choice
                      label='来源'
                      value={condition.source}
                      onChange={(value) =>
                        change({ source: value as 'header' | 'query' })
                      }
                      items={[
                        { id: 'header', label: '请求头' },
                        { id: 'query', label: '查询参数' },
                      ]}
                    />
                    <Field
                      label='名称'
                      value={condition.name}
                      onChange={(name) => change({ name })}
                      error={error(`conditions.${conditionIndex}.name`)}
                    />
                    <Choice
                      label='判断'
                      value={condition.op}
                      onChange={(value) =>
                        change({
                          op: value as typeof condition.op,
                          value:
                            value === 'exists' || value === 'absent'
                              ? ''
                              : condition.value,
                        })
                      }
                      items={[
                        { id: 'equals', label: '等于' },
                        { id: 'not_equals', label: '不等于' },
                        { id: 'exists', label: '存在' },
                        { id: 'absent', label: '不存在' },
                      ]}
                    />
                    <Field
                      label='值'
                      value={condition.value}
                      onChange={(value) => change({ value })}
                      disabled={
                        condition.op === 'exists' || condition.op === 'absent'
                      }
                    />
                    <Button
                      size='sm'
                      variant='danger-soft'
                      onPress={() =>
                        update({
                          conditions: rule.conditions.filter(
                            (_, i) => i !== conditionIndex
                          ),
                        })
                      }
                    >
                      移除
                    </Button>
                  </div>
                )
              })}
            </div>
            <div className='grid gap-3 sm:grid-cols-2'>
              {phase === 'redirect' ? (
                <>
                  <Field
                    label='跳转地址'
                    value={rule.redirect_url}
                    onChange={(redirect_url) => update({ redirect_url })}
                    error={error('redirect_url')}
                    hint='以 /、http:// 或 https:// 开头。'
                  />
                  <Choice
                    label='跳转状态码'
                    value={String(rule.redirect_status)}
                    onChange={(value) =>
                      update({ redirect_status: Number(value) })
                    }
                    items={[301, 302, 307, 308].map((status) => ({
                      id: String(status),
                      label: String(status),
                    }))}
                  />
                </>
              ) : (
                <>
                  {phase === 'proxy' && (
                    <Choice
                      label='源站组'
                      value={rule.origin_group}
                      onChange={(origin_group) => update({ origin_group })}
                      items={groups.map((group) => ({
                        id: group,
                        label: group,
                      }))}
                    />
                  )}
                  <Choice
                    label='路径重写'
                    value={rule.rewrite_mode}
                    onChange={(value) =>
                      update({
                        rewrite_mode: value as Rule['rewrite_mode'],
                        rewrite_path:
                          value === 'none' || value === 'strip_prefix'
                            ? ''
                            : rule.rewrite_path,
                      })
                    }
                    items={[
                      { id: 'none', label: '保留路径' },
                      { id: 'replace_path', label: '替换整个路径' },
                      { id: 'strip_prefix', label: '移除匹配前缀' },
                      { id: 'replace_prefix', label: '替换匹配前缀' },
                    ]}
                  />
                  {(rule.rewrite_mode === 'replace_path' ||
                    rule.rewrite_mode === 'replace_prefix') && (
                    <Field
                      label='替换路径'
                      value={rule.rewrite_path}
                      onChange={(rewrite_path) => update({ rewrite_path })}
                      error={error('rewrite_path')}
                      hint='正则捕获组可使用 ${1} 到 ${9}。'
                    />
                  )}
                </>
              )}
              <Choice
                label='查询参数'
                value={rule.query_mode}
                onChange={(value) =>
                  update({
                    query_mode: value as Rule['query_mode'],
                    query_string: value === 'replace' ? rule.query_string : '',
                  })
                }
                items={[
                  { id: 'preserve', label: '保留原参数' },
                  { id: 'drop', label: '移除参数' },
                  { id: 'replace', label: '替换参数' },
                ]}
              />
              {rule.query_mode === 'replace' && (
                <Field
                  label='替换查询参数'
                  value={rule.query_string}
                  onChange={(query_string) => update({ query_string })}
                  error={error('query_string')}
                  hint='例如 key=value，不含开头问号。'
                />
              )}
              {phase === 'proxy' && (
                <>
                  <TextAreaField
                    label='请求头'
                    value={rule.request_headers_text}
                    onChange={(request_headers_text) =>
                      update({ request_headers_text })
                    }
                    hint='每行 Header: value。'
                    error={error('request_headers_text')}
                    rows={3}
                  />
                  <TextAreaField
                    label='响应头'
                    value={rule.response_headers_text}
                    onChange={(response_headers_text) =>
                      update({ response_headers_text })
                    }
                    hint='每行 Header: value。'
                    error={error('response_headers_text')}
                    rows={3}
                  />
                </>
              )}
            </div>
            <Notice>
              {error('action') ||
                error('origin_group') ||
                error('rewrite_mode')}
            </Notice>
          </section>
        </>
      )}
      <section className='space-y-3'>
        <h3 className='text-sm font-medium'>规则预览</h3>
        <p className='text-xs text-muted'>
          使用当前编辑内容模拟匹配，不向源站发送请求。
        </p>
        <div className='grid gap-3 sm:grid-cols-[8rem_1fr_1fr]'>
          <Choice
            label='方法'
            value={testMethod}
            onChange={setTestMethod}
            items={routeMethods.map((method) => ({
              id: method,
              label: method,
            }))}
          />
          <Field label='域名' value={testHost} onChange={setTestHost} />
          <Field label='请求路径' value={testTarget} onChange={setTestTarget} />
        </div>
        <TextAreaField
          label='模拟请求头'
          value={testHeaders}
          onChange={setTestHeaders}
          hint='每行 Header: value，Host 会自动加入。'
          rows={2}
        />
        {preview.error ? (
          <Notice>{preview.error}</Notice>
        ) : (
          <div className='flex flex-wrap items-center gap-2 rounded-2xl bg-surface-secondary p-3 text-sm'>
            <Chip size='sm' variant='soft' color='accent'>
              {preview.index < 0
                ? '默认源站'
                : rules[preview.index]?.name || `规则 ${preview.index + 1}`}
            </Chip>
            <code className='break-all'>{preview.target}</code>
            {!!preview.rewrites?.length && (
              <span className='text-xs text-muted'>
                重写 {preview.rewrites.length} 项
              </span>
            )}
          </div>
        )}
      </section>
    </div>
  )
}
