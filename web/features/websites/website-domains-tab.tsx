import type { UseFieldArrayReturn, UseFormReturn } from 'react-hook-form'
import { Plus, X } from 'lucide-react'
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
import type { WebsiteFormValues } from './website-form'

export function WebsiteDomainsTab({
  form,
  domains,
}: {
  form: UseFormReturn<WebsiteFormValues>
  domains: UseFieldArrayReturn<WebsiteFormValues, 'domains', 'formKey'>
}) {
  return (
    <TabsContent value='domains' className='space-y-3 py-4'>
                  <div className='flex items-center justify-between'>
                    <div>
                      <h3 className='font-medium'>绑定域名</h3>
                      <p className='text-sm text-muted-foreground'>
                        托管解析会自动生成面向集群的记录。
                      </p>
                    </div>
                    <Button
                      type='button'
                      variant='outline'
                      size='sm'
                      onClick={() =>
                        domains.append({
                          id: crypto.randomUUID(),
                          hostname: '',
                          dns_mode: 'managed',
                        })
                      }
                    >
                      <Plus /> 添加域名
                    </Button>
                  </div>
                  {domains.fields.map((domain, index) => (
                    <div
                      key={domain.formKey}
                      className='grid items-start gap-3 rounded-md border p-3 sm:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_auto]'
                    >
                      <FormField
                        control={form.control}
                        name={`domains.${index}.hostname`}
                        render={({ field }) => (
                          <FormItem>
                            <FormLabel className='sm:sr-only'>域名</FormLabel>
                            <FormControl>
                              <Input placeholder='www.example.com' {...field} />
                            </FormControl>
                            <FormMessage />
                          </FormItem>
                        )}
                      />
                      <FormField
                        control={form.control}
                        name={`domains.${index}.dns_mode`}
                        render={({ field }) => (
                          <FormItem>
                            <FormLabel className='sm:sr-only'>
                              解析方式
                            </FormLabel>
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
                                <SelectItem value='managed'>
                                  托管解析
                                </SelectItem>
                                <SelectItem value='external'>
                                  外部解析
                                </SelectItem>
                              </SelectContent>
                            </Select>
                          </FormItem>
                        )}
                      />
                      <Button
                        type='button'
                        variant='ghost'
                        size='icon'
                        aria-label='移除域名'
                        disabled={domains.fields.length === 1}
                        onClick={() => domains.remove(index)}
                      >
                        <X />
                      </Button>
                    </div>
                  ))}
                </TabsContent>
  )
}
