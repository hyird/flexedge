import { useState } from 'react'
import type { UseFieldArrayReturn, UseFormReturn } from 'react-hook-form'
import { ArrowUp, ArrowDown, Copy, Plus, X } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import {
  Collapsible,
  CollapsibleContent,
  CollapsibleTrigger,
} from '@/components/ui/collapsible'
import {
  FormControl,
  FormDescription,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { Switch } from '@/components/ui/switch'
import { TabsContent } from '@/components/ui/tabs'
import { Textarea } from '@/components/ui/textarea'
import { RouteConditionFields } from './route-condition-fields'
import { previewHeaderValues, routeMatchLabels } from './route-matching'
import { RouteMetadataFields } from './route-metadata-fields'
import { conflictingRouteIndexes, previewRoute } from './route-preview'
import { routeTabs } from './route-tabs'
import { originGroupLabel } from './website-display'
import { routeMethods, type WebsiteFormValues } from './website-form'

export function WebsiteRoutesTab({
  phase,
  form,
  routeRules,
  selectableOriginGroups,
  invalidRouteIndex,
  clearInvalidRoute,
}: {
  phase: keyof typeof routeTabs
  form: UseFormReturn<WebsiteFormValues>
  routeRules: UseFieldArrayReturn<WebsiteFormValues, 'route_rules', 'formKey'>
  selectableOriginGroups: Array<{ name: string }>
  invalidRouteIndex: number | null
  clearInvalidRoute: () => void
}) {
  const [previewTarget, setPreviewTarget] = useState('/api/users?a=1')
  const [previewMethod, setPreviewMethod] = useState('GET')
  const [previewHost, setPreviewHost] = useState('')
  const [previewHeaders, setPreviewHeaders] = useState('')
  const [editingId, setEditingId] = useState<string | null>(null)
  const rules = form.watch('route_rules')
  const category = routeTabs[phase]
  const visibleRules = routeRules.fields
    .map((rule, index) => ({ rule, index }))
    .filter(({ index }) => rules[index]?.action === phase)
  let headerError = ''
  let headers: Array<[string, string]> = []
  try {
    headers = previewHeaderValues(previewHeaders, previewHost)
  } catch (error) {
    headerError = error instanceof Error ? error.message : '请求头不正确'
  }
  const preview = previewRoute(
    rules,
    previewMethod,
    previewTarget,
    previewHost,
    headers
  )
  const matched = rules[preview.index]
  return (
    <TabsContent value={phase} className='space-y-3 py-4'>
      <div className='flex flex-col items-start justify-between gap-3 sm:flex-row sm:items-center'>
        <div>
          <h3 className='font-medium'>{category.title}</h3>
          <p className='text-sm text-muted-foreground'>
            {category.description}
          </p>
        </div>
        <Button
          type='button'
          variant='outline'
          size='sm'
          disabled={routeRules.fields.length >= 100}
          onClick={() =>
            routeRules.append({
              id: crypto.randomUUID(),
              name: '',
              description: '',
              hostnames: [],
              conditions: [],
              rewrite_mode: 'none',
              query_mode: 'preserve',
              query_string: '',
              status: 'enabled',
              match_type: 'prefix',
              path: '/',
              methods: [],
              action: phase,
              rewrite_path: '',
              redirect_url: '',
              redirect_status: phase === 'redirect' ? 302 : 0,
              origin_group:
                phase === 'proxy' ? form.getValues('default_origin_group') : '',
              request_headers_text: '',
              response_headers_text: '',
            })
          }
        >
          <Plus /> 添加规则
        </Button>
      </div>

      {visibleRules.length === 0 ? (
        <div className='rounded-lg border border-dashed p-8 text-center text-sm text-muted-foreground'>
          {category.empty}
        </div>
      ) : (
        visibleRules.map(({ rule, index }, position) => {
          const action = form.watch(`route_rules.${index}.action`)
          const rewriteMode = form.watch(`route_rules.${index}.rewrite_mode`)
          const queryMode = form.watch(`route_rules.${index}.query_mode`)
          const matchType = form.watch(`route_rules.${index}.match_type`)
          const conflicts = conflictingRouteIndexes(rules, index)
          return (
            <Collapsible
              key={rule.formKey}
              open={
                invalidRouteIndex !== null
                  ? invalidRouteIndex === index
                  : editingId === rule.formKey
              }
              onOpenChange={(open) => {
                clearInvalidRoute()
                setEditingId(open ? rule.formKey : null)
              }}
              className='rounded-lg border'
            >
              <div className='flex flex-wrap items-center justify-between gap-3 bg-muted/30 p-3'>
                <div className='flex items-center gap-2'>
                  <span className='text-sm font-medium'>
                    {rules[index]?.name || `规则 ${index + 1}`}
                  </span>
                  <FormField
                    control={form.control}
                    name={`route_rules.${index}.status`}
                    render={({ field }) => (
                      <FormItem className='flex items-center gap-2 space-y-0'>
                        <FormLabel className='text-xs'>启用</FormLabel>
                        <FormControl>
                          <Switch
                            checked={field.value === 'enabled'}
                            onCheckedChange={(next) =>
                              field.onChange(next ? 'enabled' : 'disabled')
                            }
                          />
                        </FormControl>
                      </FormItem>
                    )}
                  />
                </div>
                <div className='flex shrink-0 items-center gap-1'>
                  <CollapsibleTrigger asChild>
                    <Button type='button' variant='outline' size='sm'>
                      {editingId === rule.formKey ? '收起' : '编辑'}
                    </Button>
                  </CollapsibleTrigger>
                  <Button
                    type='button'
                    variant='ghost'
                    size='icon'
                    aria-label={`上移规则 ${index + 1}`}
                    disabled={position === 0}
                    onClick={() =>
                      routeRules.move(index, visibleRules[position - 1].index)
                    }
                  >
                    <ArrowUp />
                  </Button>
                  <Button
                    type='button'
                    variant='ghost'
                    size='icon'
                    aria-label={`下移规则 ${index + 1}`}
                    disabled={position === visibleRules.length - 1}
                    onClick={() =>
                      routeRules.move(index, visibleRules[position + 1].index)
                    }
                  >
                    <ArrowDown />
                  </Button>
                  <Button
                    type='button'
                    variant='ghost'
                    size='icon'
                    aria-label={`复制规则 ${index + 1}`}
                    disabled={rules.length >= 100}
                    onClick={() =>
                      routeRules.insert(index + 1, {
                        ...form.getValues(`route_rules.${index}`),
                        id: crypto.randomUUID(),
                        status: 'disabled',
                      })
                    }
                  >
                    <Copy />
                  </Button>
                  <Button
                    type='button'
                    variant='ghost'
                    size='icon'
                    aria-label={`移除规则 ${index + 1}`}
                    onClick={() => routeRules.remove(index)}
                  >
                    <X />
                  </Button>
                </div>
              </div>
              <div className='space-y-1 px-3 pb-3 text-sm text-muted-foreground'>
                <p className='break-all'>
                  {rules[index]?.hostnames
                    .filter((host) => host.trim())
                    .join('、') || '全部域名'}{' '}
                  · 参数
                  {queryMode === 'drop'
                    ? '丢弃'
                    : queryMode === 'replace'
                      ? '替换'
                      : '保留'}
                </p>
                <p className='break-all'>
                  {routeMatchLabels[matchType]} ·{' '}
                  {rules[index]?.path || '未填写路径'} ·{' '}
                  {rules[index]?.methods.join(', ') || '全部方法'} →{' '}
                  {action === 'redirect'
                    ? `跳转 ${rules[index]?.redirect_status} ${rules[index]?.redirect_url}`
                    : action === 'rewrite'
                      ? `URL 重写 ${rules[index]?.rewrite_path || '查询参数策略'}`
                      : `源站组 ${originGroupLabel(rules[index]?.origin_group ?? '')}`}
                </p>
                {(rules[index]?.conditions.length ?? 0) > 0 && (
                  <p>{rules[index].conditions.length} 个请求条件（全部满足）</p>
                )}
                {conflicts.length > 0 && (
                  <p className='text-destructive'>
                    与规则 {conflicts.map((item) => item + 1).join('、')}{' '}
                    存在匹配重叠
                  </p>
                )}
              </div>
              <CollapsibleContent className='border-t'>
                <div className='space-y-4 p-3'>
                  <RouteMetadataFields form={form} index={index} />
                  {conflicts.length > 0 && (
                    <p role='status' className='text-sm text-destructive'>
                      与靠前规则{' '}
                      {conflicts.map((candidate) => candidate + 1).join('、')}{' '}
                      存在匹配重叠；重叠请求会命中靠前规则。
                    </p>
                  )}
                  <div className='grid items-start gap-4 sm:grid-cols-3'>
                    <FormField
                      control={form.control}
                      name={`route_rules.${index}.path`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>匹配路径</FormLabel>
                          <FormControl>
                            <Input
                              placeholder={
                                matchType === 'regex'
                                  ? '^/old/(.*)$'
                                  : matchType === 'suffix'
                                    ? '.jpg'
                                    : '/api/'
                              }
                              {...field}
                            />
                          </FormControl>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                    <FormField
                      control={form.control}
                      name={`route_rules.${index}.match_type`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>匹配方式</FormLabel>
                          <Select
                            value={field.value}
                            onValueChange={(next) => {
                              field.onChange(next)
                              if (
                                next !== 'prefix' &&
                                (rewriteMode === 'strip_prefix' ||
                                  rewriteMode === 'replace_prefix')
                              ) {
                                form.setValue(
                                  `route_rules.${index}.rewrite_mode`,
                                  'none',
                                  { shouldDirty: true }
                                )
                                form.setValue(
                                  `route_rules.${index}.rewrite_path`,
                                  '',
                                  { shouldDirty: true }
                                )
                              }
                            }}
                          >
                            <FormControl>
                              <SelectTrigger>
                                <SelectValue />
                              </SelectTrigger>
                            </FormControl>
                            <SelectContent>
                              <SelectItem value='prefix'>前缀匹配</SelectItem>
                              <SelectItem value='exact'>精确匹配</SelectItem>
                              <SelectItem value='suffix'>后缀匹配</SelectItem>
                              <SelectItem value='regex'>
                                正则匹配（RE2）
                              </SelectItem>
                            </SelectContent>
                          </Select>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                  </div>
                  <FormField
                    control={form.control}
                    name={`route_rules.${index}.methods`}
                    render={({ field }) => (
                      <FormItem>
                        <FormLabel>请求方法</FormLabel>
                        <FormDescription>
                          不选择即匹配全部请求方法。
                        </FormDescription>
                        <div className='flex flex-wrap gap-2'>
                          {routeMethods.map((method) => {
                            const checked = field.value.includes(method)
                            return (
                              <label
                                key={method}
                                className='flex cursor-pointer items-center gap-2 rounded-md border px-3 py-2 text-sm'
                              >
                                <Checkbox
                                  checked={checked}
                                  onCheckedChange={(next) =>
                                    field.onChange(
                                      next
                                        ? [...field.value, method]
                                        : field.value.filter(
                                            (item) => item !== method
                                          )
                                    )
                                  }
                                />
                                {method}
                              </label>
                            )
                          })}
                        </div>
                      </FormItem>
                    )}
                  />
                  <RouteConditionFields
                    key={`${rule.formKey}-${index}`}
                    form={form}
                    index={index}
                  />
                  {matchType === 'regex' && (
                    <p className='text-sm text-muted-foreground'>
                      正则仅匹配原始编码路径，不含查询参数；不加 ^ / $
                      时允许部分匹配。最多9个捕获组，替换路径或跳转地址可使用{' '}
                      {'${1}'}…{'${9}'}，{'${0}'} 为整个匹配，$$
                      表示字面美元符。未匹配的可选组为空；不支持前后查找和回溯引用。
                    </p>
                  )}
                  {action !== 'redirect' ? (
                    <div className='grid items-start gap-4 sm:grid-cols-3'>
                      {action === 'proxy' && (
                        <FormField
                          control={form.control}
                          name={`route_rules.${index}.origin_group`}
                          render={({ field }) => (
                            <FormItem>
                              <FormLabel>
                                目标源站组（匹配重写后的 URL）
                              </FormLabel>
                              <Select
                                value={field.value}
                                onValueChange={field.onChange}
                              >
                                <FormControl>
                                  <SelectTrigger>
                                    <SelectValue placeholder='选择源站组' />
                                  </SelectTrigger>
                                </FormControl>
                                <SelectContent>
                                  {selectableOriginGroups.map((group) => (
                                    <SelectItem
                                      key={group.name}
                                      value={group.name}
                                    >
                                      {originGroupLabel(group.name)}
                                    </SelectItem>
                                  ))}
                                </SelectContent>
                              </Select>
                              <FormMessage />
                            </FormItem>
                          )}
                        />
                      )}
                      <FormField
                        control={form.control}
                        name={`route_rules.${index}.rewrite_mode`}
                        render={({ field }) => (
                          <FormItem>
                            <FormLabel>路径处理</FormLabel>
                            <Select
                              value={field.value}
                              onValueChange={(mode) => {
                                field.onChange(mode)
                                if (mode === 'none' || mode === 'strip_prefix')
                                  form.setValue(
                                    `route_rules.${index}.rewrite_path`,
                                    ''
                                  )
                                if (
                                  mode === 'strip_prefix' ||
                                  mode === 'replace_prefix'
                                )
                                  form.setValue(
                                    `route_rules.${index}.match_type`,
                                    'prefix'
                                  )
                              }}
                            >
                              <FormControl>
                                <SelectTrigger>
                                  <SelectValue />
                                </SelectTrigger>
                              </FormControl>
                              <SelectContent>
                                <SelectItem value='none'>保持原路径</SelectItem>
                                <SelectItem value='replace_path'>
                                  替换整个路径
                                </SelectItem>
                                <SelectItem
                                  value='strip_prefix'
                                  disabled={
                                    matchType === 'regex' ||
                                    matchType === 'suffix'
                                  }
                                >
                                  去除匹配前缀
                                </SelectItem>
                                <SelectItem
                                  value='replace_prefix'
                                  disabled={
                                    matchType === 'regex' ||
                                    matchType === 'suffix'
                                  }
                                >
                                  替换匹配前缀
                                </SelectItem>
                              </SelectContent>
                            </Select>
                            <FormMessage />
                          </FormItem>
                        )}
                      />
                      <FormField
                        control={form.control}
                        name={`route_rules.${index}.rewrite_path`}
                        render={({ field }) => (
                          <FormItem>
                            <FormLabel>替换路径 / 前缀</FormLabel>
                            <FormControl>
                              <Input
                                disabled={
                                  rewriteMode === 'none' ||
                                  rewriteMode === 'strip_prefix'
                                }
                                placeholder={
                                  rewriteMode === 'none' ||
                                  rewriteMode === 'strip_prefix'
                                    ? '无需填写'
                                    : '/v2/'
                                }
                                {...field}
                              />
                            </FormControl>
                            <FormDescription>
                              {rewriteMode === 'replace_path'
                                ? '替换整个路径，不保留后缀。'
                                : rewriteMode === 'replace_prefix' ||
                                    rewriteMode === 'strip_prefix'
                                  ? '使用上方匹配路径作为前缀，保留后续路径；参数按下方策略处理。'
                                  : '保留原始路径。'}
                            </FormDescription>
                            <FormMessage />
                          </FormItem>
                        )}
                      />
                    </div>
                  ) : (
                    <div className='grid items-start gap-4 sm:grid-cols-2'>
                      <FormField
                        control={form.control}
                        name={`route_rules.${index}.redirect_url`}
                        render={({ field }) => (
                          <FormItem>
                            <FormLabel>跳转地址</FormLabel>
                            <FormControl>
                              <Input
                                placeholder='https://www.example.com/new'
                                {...field}
                              />
                            </FormControl>
                            <FormMessage />
                          </FormItem>
                        )}
                      />
                      <FormField
                        control={form.control}
                        name={`route_rules.${index}.redirect_status`}
                        render={({ field }) => (
                          <FormItem>
                            <FormLabel>跳转状态</FormLabel>
                            <Select
                              value={String(field.value)}
                              onValueChange={(next) =>
                                field.onChange(Number(next))
                              }
                            >
                              <FormControl>
                                <SelectTrigger>
                                  <SelectValue />
                                </SelectTrigger>
                              </FormControl>
                              <SelectContent>
                                <SelectItem value='301'>301 永久</SelectItem>
                                <SelectItem value='302'>302 临时</SelectItem>
                                <SelectItem value='307'>
                                  307 临时（保留方法/请求体）
                                </SelectItem>
                                <SelectItem value='308'>
                                  308 永久（保留方法/请求体）
                                </SelectItem>
                              </SelectContent>
                            </Select>
                            <FormMessage />
                          </FormItem>
                        )}
                      />
                    </div>
                  )}
                  <div className='grid items-start gap-4 sm:grid-cols-2'>
                    <FormField
                      control={form.control}
                      name={`route_rules.${index}.query_mode`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>查询参数策略</FormLabel>
                          <Select
                            value={field.value}
                            onValueChange={(mode) => {
                              field.onChange(mode)
                              if (mode !== 'replace')
                                form.setValue(
                                  `route_rules.${index}.query_string`,
                                  ''
                                )
                            }}
                          >
                            <FormControl>
                              <SelectTrigger>
                                <SelectValue />
                              </SelectTrigger>
                            </FormControl>
                            <SelectContent>
                              <SelectItem value='preserve'>保留</SelectItem>
                              <SelectItem value='drop'>丢弃</SelectItem>
                              <SelectItem value='replace'>替换</SelectItem>
                            </SelectContent>
                          </Select>
                          <FormDescription>
                            {action === 'redirect'
                              ? '保留：目标已有参数时使用目标参数，否则携带原请求参数。丢弃：移除全部参数。'
                              : '保留：不覆盖前面规则的参数设置。丢弃或替换：覆盖前面匹配规则的参数设置。'}
                          </FormDescription>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                    <FormField
                      control={form.control}
                      name={`route_rules.${index}.query_string`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>替换参数</FormLabel>
                          <FormControl>
                            <Input
                              disabled={queryMode !== 'replace'}
                              placeholder='a=1&b=2'
                              {...field}
                            />
                          </FormControl>
                          <FormDescription>
                            不含开头 ?；留空等同清空，特殊字符需 URL 编码。
                          </FormDescription>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                  </div>
                  {action !== 'rewrite' && (
                    <Collapsible>
                      <CollapsibleTrigger asChild>
                        <Button type='button' variant='ghost' size='sm'>
                          高级请求/响应头
                        </Button>
                      </CollapsibleTrigger>
                      <CollapsibleContent className='grid items-start gap-4 pt-3 sm:grid-cols-2'>
                        <FormField
                          control={form.control}
                          name={`route_rules.${index}.request_headers_text`}
                          render={({ field }) => (
                            <FormItem>
                              <FormLabel>回源请求头</FormLabel>
                              <FormControl>
                                <Textarea
                                  className='[field-sizing:fixed] h-40 min-h-40 resize-none overflow-y-auto font-mono text-xs'
                                  placeholder='X-Region: cn'
                                  {...field}
                                />
                              </FormControl>
                              <FormDescription>
                                每行一个，例如 X-Region: cn。
                              </FormDescription>
                              <FormMessage />
                            </FormItem>
                          )}
                        />
                        <FormField
                          control={form.control}
                          name={`route_rules.${index}.response_headers_text`}
                          render={({ field }) => (
                            <FormItem>
                              <FormLabel>响应头</FormLabel>
                              <FormControl>
                                <Textarea
                                  className='[field-sizing:fixed] h-40 min-h-40 resize-none overflow-y-auto font-mono text-xs'
                                  placeholder='X-Cache: HIT'
                                  {...field}
                                />
                              </FormControl>
                              <FormDescription>
                                每行一个，例如 X-Cache: HIT。
                              </FormDescription>
                              <FormMessage />
                            </FormItem>
                          )}
                        />
                      </CollapsibleContent>
                    </Collapsible>
                  )}
                </div>
                <Collapsible
                  className='border-t p-3'
                  onOpenChange={(open) => {
                    if (open) {
                      setPreviewHost(
                        form
                          .getValues(`route_rules.${index}.hostnames`)
                          .find((host) => host.trim()) ??
                          form.getValues('domains')[0]?.hostname ??
                          ''
                      )
                      setPreviewTarget(
                        matchType === 'regex'
                          ? '/old/example'
                          : matchType === 'suffix'
                            ? `/example${form.getValues(`route_rules.${index}.path`)}`
                            : form.getValues(`route_rules.${index}.path`)
                      )
                      setPreviewMethod(
                        form.getValues(`route_rules.${index}.methods`)[0] ??
                          'GET'
                      )
                    }
                  }}
                >
                  <CollapsibleTrigger asChild>
                    <Button type='button' variant='ghost' size='sm'>
                      测试执行阶段
                    </Button>
                  </CollapsibleTrigger>
                  <CollapsibleContent className='space-y-3 pt-3'>
                    <p className='text-sm text-muted-foreground'>
                      以此规则的路径和方法测试全部规则，检查实际命中项。仅基于未保存表单计算，不发送请求，也不代表节点已应用配置。
                    </p>
                    <div className='grid items-start gap-3 sm:grid-cols-3'>
                      <div className='space-y-2'>
                        <label
                          htmlFor={`route-preview-host-${index}`}
                          className='text-sm font-medium'
                        >
                          请求域名
                        </label>
                        <Input
                          id={`route-preview-host-${index}`}
                          value={previewHost}
                          onChange={(event) =>
                            setPreviewHost(event.target.value)
                          }
                          placeholder='api.example.com'
                        />
                      </div>
                      <div className='space-y-2'>
                        <label
                          htmlFor={`route-preview-method-${index}`}
                          className='text-sm font-medium'
                        >
                          请求方法
                        </label>
                        <Select
                          value={previewMethod}
                          onValueChange={setPreviewMethod}
                        >
                          <SelectTrigger
                            id={`route-preview-method-${index}`}
                            className='w-full'
                          >
                            <SelectValue />
                          </SelectTrigger>
                          <SelectContent>
                            {routeMethods.map((method) => (
                              <SelectItem key={method} value={method}>
                                {method}
                              </SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                      </div>
                      <div className='space-y-2'>
                        <label
                          htmlFor={`route-preview-target-${index}`}
                          className='text-sm font-medium'
                        >
                          路径与查询参数
                        </label>
                        <Input
                          id={`route-preview-target-${index}`}
                          value={previewTarget}
                          onChange={(event) =>
                            setPreviewTarget(event.target.value)
                          }
                        />
                      </div>
                    </div>
                    <div className='space-y-2'>
                      <label
                        htmlFor={`route-preview-headers-${index}`}
                        className='text-sm font-medium'
                      >
                        预览请求头
                      </label>
                      <Textarea
                        id={`route-preview-headers-${index}`}
                        value={previewHeaders}
                        onChange={(event) =>
                          setPreviewHeaders(event.target.value)
                        }
                        placeholder='X-Channel: beta'
                        className='h-20 font-mono text-xs'
                      />
                      <p className='text-xs text-muted-foreground'>
                        每行 Header: value；Host 自动使用上方请求域名。
                      </p>
                    </div>
                    <p role='status' className='text-sm break-all'>
                      {headerError ||
                        preview.error ||
                        (!previewTarget.startsWith('/')
                          ? '请输入以 / 开头的请求路径。'
                          : matched
                            ? `命中规则 ${preview.index + 1} · ${matched.action === 'redirect' ? `跳转 ${matched.redirect_status}` : `源站组 ${originGroupLabel(matched.origin_group)}`} → ${preview.target}`
                            : `使用默认源站组 ${originGroupLabel(form.watch('default_origin_group'))} → ${preview.target}`)}
                    </p>
                    {!preview.error && !headerError && (
                      <p className='text-sm text-muted-foreground'>
                        重写阶段：
                        {preview.rewrites
                          ?.map((item) => item + 1)
                          .join(' → ') || '无'}
                        ； 选源阶段：
                        {preview.origins?.map((item) => item + 1).join(' → ') ||
                          '默认源站组'}
                        。
                        数字为规则编号，同项设置以后面的为准；预览不会发送真实请求。
                      </p>
                    )}
                  </CollapsibleContent>
                </Collapsible>
              </CollapsibleContent>
            </Collapsible>
          )
        })
      )}
    </TabsContent>
  )
}
