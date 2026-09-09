import type { UseFieldArrayReturn, UseFormReturn } from 'react-hook-form'
import { Plus, X } from 'lucide-react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  FormControl,
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
import { originGroupLabel } from './website-display'
import type { WebsiteFormValues } from './website-form'

import type { OriginGroup } from './origin-groups'

export function WebsiteOriginsTab({
  form,
  origins,
  originGroups,
  createOriginGroup,
  appendOrigin,
  renameOriginGroup,
  removeOrigin,
}: {
  form: UseFormReturn<WebsiteFormValues>
  origins: UseFieldArrayReturn<WebsiteFormValues, 'origins', 'formKey'>
  originGroups: OriginGroup[]
  createOriginGroup: () => void
  appendOrigin: (
    group: string,
    role: WebsiteFormValues['origins'][number]['role']
  ) => void
  renameOriginGroup: (currentName: string, draftName: string) => boolean
  removeOrigin: (index: number) => void
}) {
  return (
    <TabsContent value='origins' className='space-y-3 py-4'>
                  <div className='flex items-center justify-between'>
                    <div>
                      <h3 className='font-medium'>源站组</h3>
                      <p className='text-sm text-muted-foreground'>
                        每组包含主源站和可选备源站；默认组及代理路由会引用组名。
                      </p>
                    </div>
                    <Button
                      type='button'
                      variant='outline'
                      size='sm'
                      onClick={createOriginGroup}
                    >
                      <Plus /> 新建源站组
                    </Button>
                  </div>
                  {originGroups.map((group) => (
                    <section
                      key={group.name}
                      className='overflow-hidden rounded-lg border'
                    >
                      <div className='flex flex-col gap-3 border-b bg-muted/30 p-3 sm:flex-row sm:items-center sm:justify-between'>
                        <div className='flex min-w-0 flex-wrap items-center gap-2'>
                          <Input
                            key={group.name}
                            aria-label={`${originGroupLabel(group.name)} 的源站组名称`}
                            defaultValue={originGroupLabel(group.name)}
                            className='h-8 max-w-52 font-medium'
                            onBlur={(event) => {
                              const draftName =
                                group.name === 'default' &&
                                event.currentTarget.value.trim() === '默认'
                                  ? 'default'
                                  : event.currentTarget.value
                              if (
                                !renameOriginGroup(
                                  group.name,
                                  draftName
                                )
                              ) {
                                event.currentTarget.value = originGroupLabel(
                                  group.name
                                )
                              }
                            }}
                          />
                          {form.watch('default_origin_group') ===
                            group.name && (
                            <Badge variant='secondary'>默认组</Badge>
                          )}
                          <Badge variant='outline'>
                            {group.enabledCount} / {group.indexes.length} 启用
                          </Badge>
                          {group.enabledPrimaryCount === 0 && (
                            <Badge variant='outline'>缺少启用的主源站</Badge>
                          )}
                        </div>
                        <Button
                          type='button'
                          variant='outline'
                          size='sm'
                          onClick={() => appendOrigin(group.name, 'backup')}
                        >
                          <Plus /> 添加源站
                        </Button>
                      </div>
                      {group.indexes.map((index, groupIndex) => {
                        const origin = origins.fields[index]
                        if (!origin) return null

                        return (
                          <div
                            key={origin.formKey}
                            className='space-y-3 border-b p-3 last:border-b-0'
                          >
                            <div className='flex items-center justify-between'>
                              <span className='text-sm font-medium'>
                                源站 {groupIndex + 1}
                              </span>
                              <Button
                                type='button'
                                variant='ghost'
                                size='icon'
                                aria-label={`移除 ${group.name} 的源站 ${groupIndex + 1}`}
                                disabled={origins.fields.length === 1}
                                onClick={() => removeOrigin(index)}
                              >
                                <X />
                              </Button>
                            </div>
                            <div className='grid items-start gap-4 sm:grid-cols-2 lg:grid-cols-3'>
                              <FormField
                                control={form.control}
                                name={`origins.${index}.protocol`}
                                render={({ field }) => (
                                  <FormItem>
                                    <FormLabel>协议</FormLabel>
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
                                        <SelectItem value='http'>
                                          HTTP
                                        </SelectItem>
                                        <SelectItem value='https'>
                                          HTTPS
                                        </SelectItem>
                                      </SelectContent>
                                    </Select>
                                  </FormItem>
                                )}
                              />
                              <FormField
                                control={form.control}
                                name={`origins.${index}.host`}
                                render={({ field }) => (
                                  <FormItem>
                                    <FormLabel>地址</FormLabel>
                                    <FormControl>
                                      <Input
                                        placeholder='origin.example.com'
                                        {...field}
                                      />
                                    </FormControl>
                                    <FormMessage />
                                  </FormItem>
                                )}
                              />
                              <FormField
                                control={form.control}
                                name={`origins.${index}.port`}
                                render={({ field }) => (
                                  <FormItem>
                                    <FormLabel>端口</FormLabel>
                                    <FormControl>
                                      <Input
                                        type='number'
                                        {...field}
                                        onChange={(event) =>
                                          field.onChange(
                                            event.currentTarget.valueAsNumber
                                          )
                                        }
                                      />
                                    </FormControl>
                                    <FormMessage />
                                  </FormItem>
                                )}
                              />
                              <FormField
                                control={form.control}
                                name={`origins.${index}.role`}
                                render={({ field }) => (
                                  <FormItem>
                                    <FormLabel>角色</FormLabel>
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
                                        <SelectItem value='primary'>
                                          主源站
                                        </SelectItem>
                                        <SelectItem value='backup'>
                                          备源站
                                        </SelectItem>
                                      </SelectContent>
                                    </Select>
                                  </FormItem>
                                )}
                              />
                              <FormField
                                control={form.control}
                                name={`origins.${index}.weight`}
                                render={({ field }) => (
                                  <FormItem>
                                    <FormLabel>权重</FormLabel>
                                    <FormControl>
                                      <Input
                                        type='number'
                                        {...field}
                                        onChange={(event) =>
                                          field.onChange(
                                            event.currentTarget.valueAsNumber
                                          )
                                        }
                                      />
                                    </FormControl>
                                  </FormItem>
                                )}
                              />
                              <FormField
                                control={form.control}
                                name={`origins.${index}.status`}
                                render={({ field }) => (
                                  <FormItem>
                                    <FormLabel>状态</FormLabel>
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
                                        <SelectItem value='enabled'>
                                          启用
                                        </SelectItem>
                                        <SelectItem value='disabled'>
                                          停用
                                        </SelectItem>
                                      </SelectContent>
                                    </Select>
                                  </FormItem>
                                )}
                              />
                            </div>
                          </div>
                        )
                      })}
                    </section>
                  ))}
                </TabsContent>
  )
}
