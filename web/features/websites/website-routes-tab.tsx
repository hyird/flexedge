import type { UseFieldArrayReturn, UseFormReturn } from 'react-hook-form'
import { Plus, X } from 'lucide-react'
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
import { TabsContent } from '@/components/ui/tabs'
import { Textarea } from '@/components/ui/textarea'
import { Switch } from '@/components/ui/switch'
import { originGroupLabel } from './website-display'
import { routeMethods, type WebsiteFormValues } from './website-form'

export function WebsiteRoutesTab({
  form,
  routeRules,
  selectableOriginGroups,
}: {
  form: UseFormReturn<WebsiteFormValues>
  routeRules: UseFieldArrayReturn<WebsiteFormValues, 'route_rules', 'formKey'>
  selectableOriginGroups: Array<{ name: string }>
}) {
  return (
    <TabsContent value='routes' className='space-y-3 py-4'>
                  <div className='flex items-center justify-between gap-4'>
                    <div>
                      <h3 className='font-medium'>路由规则</h3>
                      <p className='text-sm text-muted-foreground'>
                        按路径和请求方法覆盖默认回源，规则会按配置顺序匹配。
                      </p>
                    </div>
                    <Button
                      type='button'
                      variant='outline'
                      size='sm'
                      onClick={() =>
                        routeRules.append({
                          id: crypto.randomUUID(),
                          status: 'enabled',
                          match_type: 'prefix',
                          path: '/',
                          methods: [],
                          action: 'proxy',
                          rewrite_path: '',
                          redirect_url: '',
                          redirect_status: 0,
                          origin_group: form.getValues('default_origin_group'),
                          request_headers_text: '',
                          response_headers_text: '',
                        })
                      }
                    >
                      <Plus /> 添加规则
                    </Button>
                  </div>
                  {routeRules.fields.length === 0 ? (
                    <div className='rounded-lg border border-dashed p-8 text-center text-sm text-muted-foreground'>
                      暂无路由规则，将使用默认源站组处理全部请求。
                    </div>
                  ) : (
                    routeRules.fields.map((rule, index) => {
                      const action = form.watch(`route_rules.${index}.action`)
                      return (
                        <section
                          key={rule.formKey}
                          className='rounded-lg border'
                        >
                          <div className='flex items-center justify-between gap-4 border-b bg-muted/30 p-3'>
                            <div className='flex items-center gap-2'>
                              <span className='text-sm font-medium'>
                                规则 {index + 1}
                              </span>
                              <FormField
                                control={form.control}
                                name={`route_rules.${index}.status`}
                                render={({ field }) => (
                                  <FormItem className='flex items-center gap-2 space-y-0'>
                                    <FormLabel className='text-xs'>
                                      启用
                                    </FormLabel>
                                    <FormControl>
                                      <Switch
                                        checked={field.value === 'enabled'}
                                        onCheckedChange={(next) =>
                                          field.onChange(
                                            next ? 'enabled' : 'disabled'
                                          )
                                        }
                                      />
                                    </FormControl>
                                  </FormItem>
                                )}
                              />
                            </div>
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
                          <div className='space-y-4 p-3'>
                            <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-4'>
                              <FormField
                                control={form.control}
                                name={`route_rules.${index}.path`}
                                render={({ field }) => (
                                  <FormItem className='lg:col-span-2'>
                                    <FormLabel>匹配路径</FormLabel>
                                    <FormControl>
                                      <Input placeholder='/api/' {...field} />
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
                                      onValueChange={field.onChange}
                                    >
                                      <FormControl>
                                        <SelectTrigger>
                                          <SelectValue />
                                        </SelectTrigger>
                                      </FormControl>
                                      <SelectContent>
                                        <SelectItem value='prefix'>
                                          前缀匹配
                                        </SelectItem>
                                        <SelectItem value='exact'>
                                          精确匹配
                                        </SelectItem>
                                      </SelectContent>
                                    </Select>
                                  </FormItem>
                                )}
                              />
                              <FormField
                                control={form.control}
                                name={`route_rules.${index}.action`}
                                render={({ field }) => (
                                  <FormItem>
                                    <FormLabel>处理动作</FormLabel>
                                    <Select
                                      value={field.value}
                                      onValueChange={(next) => {
                                        field.onChange(next)
                                        if (next === 'proxy') {
                                          form.setValue(
                                            `route_rules.${index}.redirect_url`,
                                            ''
                                          )
                                          form.setValue(
                                            `route_rules.${index}.redirect_status`,
                                            0
                                          )
                                        } else {
                                          form.setValue(
                                            `route_rules.${index}.origin_group`,
                                            ''
                                          )
                                          form.setValue(
                                            `route_rules.${index}.rewrite_path`,
                                            ''
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
                                        <SelectItem value='proxy'>
                                          代理回源
                                        </SelectItem>
                                        <SelectItem value='redirect'>
                                          跳转
                                        </SelectItem>
                                      </SelectContent>
                                    </Select>
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
                                      const checked =
                                        field.value.includes(method)
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
                            {action === 'proxy' ? (
                              <div className='grid gap-3 sm:grid-cols-2'>
                                <FormField
                                  control={form.control}
                                  name={`route_rules.${index}.origin_group`}
                                  render={({ field }) => (
                                    <FormItem>
                                      <FormLabel>目标源站组</FormLabel>
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
                                <FormField
                                  control={form.control}
                                  name={`route_rules.${index}.rewrite_path`}
                                  render={({ field }) => (
                                    <FormItem>
                                      <FormLabel>重写路径（可选）</FormLabel>
                                      <FormControl>
                                        <Input placeholder='/v2/' {...field} />
                                      </FormControl>
                                      <FormMessage />
                                    </FormItem>
                                  )}
                                />
                              </div>
                            ) : (
                              <div className='grid gap-3 sm:grid-cols-[1fr_160px]'>
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
                                          <SelectItem value='301'>
                                            301 永久
                                          </SelectItem>
                                          <SelectItem value='302'>
                                            302 临时
                                          </SelectItem>
                                        </SelectContent>
                                      </Select>
                                      <FormMessage />
                                    </FormItem>
                                  )}
                                />
                              </div>
                            )}
                            <Collapsible>
                              <CollapsibleTrigger asChild>
                                <Button type='button' variant='ghost' size='sm'>
                                  高级请求/响应头
                                </Button>
                              </CollapsibleTrigger>
                              <CollapsibleContent className='grid gap-3 pt-3 sm:grid-cols-2'>
                                <FormField
                                  control={form.control}
                                  name={`route_rules.${index}.request_headers_text`}
                                  render={({ field }) => (
                                    <FormItem>
                                      <FormLabel>回源请求头</FormLabel>
                                      <FormDescription>
                                        每行一个，例如 X-Region: cn。
                                      </FormDescription>
                                      <FormControl>
                                        <Textarea
                                          className='min-h-28 font-mono text-xs'
                                          placeholder='X-Region: cn'
                                          {...field}
                                        />
                                      </FormControl>
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
                                      <FormDescription>
                                        每行一个，例如 X-Cache: HIT。
                                      </FormDescription>
                                      <FormControl>
                                        <Textarea
                                          className='min-h-28 font-mono text-xs'
                                          placeholder='X-Cache: HIT'
                                          {...field}
                                        />
                                      </FormControl>
                                      <FormMessage />
                                    </FormItem>
                                  )}
                                />
                              </CollapsibleContent>
                            </Collapsible>
                          </div>
                        </section>
                      )
                    })
                  )}
                </TabsContent>
  )
}
