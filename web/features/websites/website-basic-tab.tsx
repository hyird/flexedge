import type { UseFormReturn } from 'react-hook-form'
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
import type { Cluster } from '@/features/clusters/types'
import { originGroupLabel } from './website-display'
import type { WebsiteFormValues } from './website-form'

export function WebsiteBasicTab({
  form,
  clusters,
  selectableOriginGroups,
}: {
  form: UseFormReturn<WebsiteFormValues>
  clusters: Cluster[]
  selectableOriginGroups: Array<{ name: string }>
}) {
  return (
    <TabsContent value='basic' className='space-y-4 py-4'>
                  <FormField
                    control={form.control}
                    name='name'
                    render={({ field }) => (
                      <FormItem>
                        <FormLabel>网站名称</FormLabel>
                        <FormControl>
                          <Input placeholder='主站' {...field} />
                        </FormControl>
                        <FormMessage />
                      </FormItem>
                    )}
                  />
                  <div className='grid items-start gap-4 sm:grid-cols-2'>
                    <FormField
                      control={form.control}
                      name='cluster_id'
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>所属集群</FormLabel>
                          <Select
                            value={field.value}
                            onValueChange={field.onChange}
                          >
                            <FormControl>
                              <SelectTrigger>
                                <SelectValue placeholder='选择集群' />
                              </SelectTrigger>
                            </FormControl>
                            <SelectContent>
                              {clusters.map((cluster) => (
                                <SelectItem key={cluster.id} value={cluster.id}>
                                  {cluster.name}
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
                      name='status'
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
                              <SelectItem value='enabled'>启用</SelectItem>
                              <SelectItem value='disabled'>停用</SelectItem>
                            </SelectContent>
                          </Select>
                        </FormItem>
                      )}
                    />
                  </div>
                  <div className='grid items-start gap-4 sm:grid-cols-2'>
                    <FormField
                      control={form.control}
                      name='default_origin_group'
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>默认源站组</FormLabel>
                          <Select
                            value={field.value}
                            onValueChange={field.onChange}
                          >
                            <FormControl>
                              <SelectTrigger>
                                <SelectValue placeholder='先在“源站”页创建分组' />
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
                          <FormDescription>
                            仅可选择含启用源站的分组；分组可在“源站”页统一管理。
                          </FormDescription>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                    <FormField
                      control={form.control}
                      name='origin_host_header'
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>回源 Host</FormLabel>
                          <Select
                            value={
                              field.value === '$host' ? '$host' : '__custom__'
                            }
                            onValueChange={(value) =>
                              field.onChange(value === '$host' ? '$host' : '')
                            }
                          >
                            <FormControl>
                              <SelectTrigger>
                                <SelectValue placeholder='选择回源 Host 方式' />
                              </SelectTrigger>
                            </FormControl>
                            <SelectContent>
                              <SelectItem value='$host'>
                                透传访问域名（$host）
                              </SelectItem>
                              <SelectItem value='__custom__'>
                                自定义 Host
                              </SelectItem>
                            </SelectContent>
                          </Select>
                          {field.value !== '$host' && (
                            <FormControl>
                              <Input
                                className='mt-2'
                                placeholder='例如 edge.a-z.xin'
                                {...field}
                              />
                            </FormControl>
                          )}
                          <FormDescription>
                            {field.value === '$host' ? (
                              <>
                                使用 <code>$host</code> 透传访问域名。
                              </>
                            ) : (
                              <>
                                填写固定回源 Host，例如{' '}
                                <code>edge.a-z.xin</code>。
                              </>
                            )}
                          </FormDescription>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                  </div>
                </TabsContent>
  )
}
