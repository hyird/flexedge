import { useState } from 'react'
import { ArrowLeft, Code, Eye } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
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
import { Page } from '@/components/page'
import { Section } from '@/components/section'
import {
  cacheConditionExpression,
  cacheExpressionFields,
  cacheExpressionOperators,
  cacheRuleExpression,
  readCacheCondition,
  type CacheExpressionCondition,
} from './cache_expression'
import { cacheSettingDefinitions, newCacheSettings } from './cache_settings'
import { CacheSettingsEditor } from './cache_settings_editor'
import { cacheRuleSchema, type CacheRule } from './types'

export function CacheRuleEditor({
  rule,
  onClose,
  onSave,
}: {
  rule: CacheRule
  onClose: () => void
  onSave: (rule: CacheRule) => Promise<void>
}) {
  const [value, setValue] = useState(() => {
    const result = structuredClone(rule)
    if (!result.settings) {
      const settings = newCacheSettings()
      if (result.browser_ttl_mode !== 'default')
        settings.enabled.push('browser')
      if (result.ignore_query) {
        settings.enabled.push('key')
        settings.key.query_mode = 'ignore'
      }
      result.settings = settings
    }
    return result
  })
  const initialExpression = cacheRuleExpression(rule)
  const [condition, setCondition] = useState<CacheExpressionCondition>(
    () =>
      readCacheCondition(initialExpression) ?? {
        field: 'http.request.uri.path.extension',
        operator: 'in',
        value: '',
        lower: false,
      }
  )
  const [expressionMode, setExpressionMode] = useState(
    () => !readCacheCondition(initialExpression)
  )
  const [customExpression, setCustomExpression] = useState(initialExpression)
  const [matchChanged, setMatchChanged] = useState(false)
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [saving, setSaving] = useState(false)
  const [saveError, setSaveError] = useState<string>()
  const [preview, setPreview] = useState(false)
  const expression = expressionMode
    ? customExpression
    : cacheConditionExpression(condition)
  const change = (patch: Partial<CacheRule>) =>
    setValue((current) => ({ ...current, ...patch }))
  const conditionChange = (patch: Partial<CacheExpressionCondition>) => {
    setCondition((current) => ({ ...current, ...patch }))
    setMatchChanged(true)
  }
  const payload = () => ({
    ...value,
    skip_no_cache: value.ttl_mode !== 'fixed',
    ...(matchChanged
      ? { match_type: 'expression' as const, patterns: [expression.trim()] }
      : {}),
  })
  const formId = 'cache-rule-editor'
  const save = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    if (saving) return
    const result = cacheRuleSchema.safeParse(payload())
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      setSaveError(result.error.issues[0]?.message ?? '请检查缓存配置')
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
  }
  const nav = [
    { id: 'rule-basic', name: '规则信息' },
    { id: 'rule-match', name: '匹配条件' },
    { id: 'rule-action', name: '缓存资格' },
    ...(value.action === 'cache'
      ? cacheSettingDefinitions
          .filter((entry) => value.settings?.enabled.includes(entry.id))
          .map((entry) => ({ id: `setting-${entry.id}`, name: entry.name }))
      : []),
  ]
  return (
    <Page
      title={rule.name ? `编辑规则 · ${rule.name}` : '新建缓存规则'}
      description='规则按列表顺序应用；相同设置以后匹配的规则为准。'
      actions={
        <>
          <Button
            size='sm'
            variant='tertiary'
            isDisabled={saving}
            onPress={onClose}
          >
            <ArrowLeft />
            返回策略
          </Button>
          <Button
            size='sm'
            variant='secondary'
            isDisabled={saving}
            onPress={() => setPreview(true)}
          >
            <Eye />
            预览配置
          </Button>
          <Button
            size='sm'
            variant='primary'
            type='submit'
            form={formId}
            isPending={saving}
          >
            保存规则
          </Button>
        </>
      }
    >
      <div className='grid items-start gap-6 xl:grid-cols-[190px_minmax(0,1fr)]'>
        <aside className='sticky top-5 hidden xl:block'>
          <p className='mb-3 px-3 text-xs text-muted'>规则配置</p>
          <nav aria-label='规则配置章节' className='grid gap-1'>
            {nav.map((entry) => (
              <a
                key={entry.id}
                href={`#${entry.id}`}
                onClick={(event) => {
                  event.preventDefault()
                  document
                    .getElementById(entry.id)
                    ?.scrollIntoView({ behavior: 'smooth', block: 'start' })
                }}
                className='rounded-lg px-3 py-2 text-sm text-muted transition-colors hover:bg-default hover:text-foreground'
              >
                {entry.name}
              </a>
            ))}
          </nav>
          <p className='mx-3 mt-5 border-t border-border pt-4 text-xs leading-6 text-muted'>
            只覆盖添加的配置项。
            <br />
            其他项目沿用默认行为或前面的匹配规则。
          </p>
        </aside>
        <form
          id={formId}
          noValidate
          onSubmit={(event) => {
            void save(event)
          }}
          className='grid min-w-0 gap-5'
        >
          {saveError && <Notice>{saveError}</Notice>}
          <div inert={saving} className='grid gap-5'>
            <div id='rule-basic' className='scroll-mt-5'>
              <Section
                title='规则信息'
                description='为规则起一个便于识别的名称。'
              >
                <div className='grid gap-4'>
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
                </div>
              </Section>
            </div>
            <div id='rule-match' className='scroll-mt-5'>
              <Section
                title='当传入请求匹配时…'
                description='使用请求字段构建条件，或编辑组合表达式。'
                actions={
                  <Button
                    size='sm'
                    variant='tertiary'
                    onPress={() => {
                      if (expressionMode) {
                        const parsed = readCacheCondition(customExpression)
                        if (!parsed) {
                          setSaveError(
                            '当前组合表达式无法用单个条件表示，请继续使用表达式编辑。'
                          )
                          return
                        }
                        setCondition(parsed)
                      } else {
                        setCustomExpression(expression)
                      }
                      setExpressionMode(!expressionMode)
                      setSaveError(undefined)
                    }}
                  >
                    <Code />
                    {expressionMode ? '使用条件构建器' : '编辑表达式'}
                  </Button>
                }
              >
                <div className='grid gap-4'>
                  {expressionMode ? (
                    <TextAreaField
                      label='匹配表达式'
                      value={customExpression}
                      onChange={(text) => {
                        setCustomExpression(text)
                        setMatchChanged(true)
                      }}
                      rows={5}
                      error={errors.patterns}
                      hint='支持下方字段、请求头[0]、lower、and/or/xor/not 和括号组合；保存时由控制面校验。'
                    />
                  ) : (
                    <>
                      <div className='grid gap-4 sm:grid-cols-2'>
                        <Choice
                          label='匹配字段'
                          value={condition.field}
                          onChange={(field) => conditionChange({ field })}
                          items={[...cacheExpressionFields]}
                        />
                        <Choice
                          label='运算符'
                          value={condition.operator}
                          onChange={(operator) => conditionChange({ operator })}
                          items={[...cacheExpressionOperators]}
                        />
                      </div>
                      <Field
                        label='匹配值'
                        value={condition.value}
                        onChange={(text) => conditionChange({ value: text })}
                        error={errors.patterns}
                        hint={
                          condition.operator === 'in'
                            ? '多个值用逗号分隔；文件扩展名不带点，例如 js, css。'
                            : '填写此条件需要匹配的值。'
                        }
                      />
                      <Toggle
                        label='将字段转为小写后比较'
                        selected={condition.lower}
                        onChange={(lower) => conditionChange({ lower })}
                      />
                    </>
                  )}
                  <div>
                    <p className='mb-2 text-xs text-muted'>表达式预览</p>
                    <pre className='overflow-x-auto rounded-xl border border-border bg-default/40 p-4 text-xs leading-6 break-all whitespace-pre-wrap'>
                      <code>{expression}</code>
                    </pre>
                  </div>
                </div>
              </Section>
            </div>
            <div id='rule-action' className='scroll-mt-5'>
              <Section
                title='然后执行…'
                description='允许缓存仍需满足响应缓存条件。'
              >
                <Choice
                  label='缓存资格'
                  value={value.action}
                  onChange={(action) =>
                    change({ action: action as CacheRule['action'] })
                  }
                  items={[
                    { id: 'cache', label: '允许缓存（Eligible for cache）' },
                    { id: 'bypass', label: '绕过缓存（Bypass cache）' },
                  ]}
                />
              </Section>
            </div>
            {value.action === 'cache' && value.settings ? (
              <CacheSettingsEditor
                value={value}
                settings={value.settings}
                onChange={change}
                disabled={saving}
              />
            ) : (
              <Section title='匹配的请求将绕过节点缓存'>
                <p className='text-sm leading-6 text-muted'>
                  切回“允许缓存”后，保留的配置项会重新显示。
                </p>
              </Section>
            )}
            {!!value.status_codes.length && (
              <p className='text-xs leading-6 text-muted'>
                本规则保留已有状态码限制：{value.status_codes.join(', ')}。
              </p>
            )}
          </div>
          <div className='flex justify-end gap-2 border-t border-border pt-5'>
            <Button
              variant='secondary'
              isDisabled={saving}
              onPress={() => setPreview(true)}
            >
              <Eye />
              预览配置
            </Button>
            <Button type='submit' variant='primary' isPending={saving}>
              保存规则
            </Button>
          </div>
        </form>
      </div>
      {preview && (
        <FormSheet
          isOpen
          onClose={() => setPreview(false)}
          title='配置预览'
          description='此处展示即将保存的实际规则。'
          size='lg'
        >
          <pre className='overflow-x-auto rounded-xl bg-default/40 p-4 text-xs leading-6 break-all whitespace-pre-wrap'>
            {JSON.stringify(payload(), null, 2)}
          </pre>
        </FormSheet>
      )}
    </Page>
  )
}
