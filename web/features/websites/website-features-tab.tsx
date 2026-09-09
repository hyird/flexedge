import type { UseFormReturn } from 'react-hook-form'
import { cn } from '@/lib/utils'
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
import type { Certificate } from '@/features/certificates/types'
import { CompressionAlgorithmSelect } from './compression-algorithm-select'
import {
  linesToValues,
  type WebsiteFormValues,
  valuesToLines,
} from './website-form'

export function WebsiteFeaturesTab({
  form,
  certificates,
  category,
}: {
  form: UseFormReturn<WebsiteFormValues>
  category: 'origin-settings' | 'health' | 'logs' | 'https' | 'compression'
  certificates: Certificate[]
}) {
  const httpsEnabled = form.watch('https_enabled')
  const healthCheckEnabled = form.watch('health_check_enabled')
  const accessLogEnabled = form.watch('access_log_enabled')
  const compressionEnabled = form.watch('response_compression_enabled')

  return (
    <TabsContent
      value={category}
      className={cn(
        'space-y-3 py-4',
        category === 'compression' &&
          'flex min-h-[100cqh] flex-col space-y-0 data-[state=inactive]:hidden'
      )}
    >
      {category === 'origin-settings' && (
        <section className='rounded-lg border'>
          <div className='p-4'>
            <h3 className='font-medium'>回源设置</h3>
            <p className='text-sm text-muted-foreground'>
              设置回源超时，以及是否将访客真实 IP 传给源站。
            </p>
          </div>
          <div className='space-y-4 border-t p-4'>
            <div className='grid items-start gap-4 sm:grid-cols-2'>
              {(
                [
                  ['origin_connect_timeout_seconds', '回源连接超时（秒）', 300],
                  ['origin_read_timeout_seconds', '回源读取超时（秒）', 600],
                ] as const
              ).map(([name, label, max]) => (
                <FormField
                  key={name}
                  control={form.control}
                  name={name}
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>{label}</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          min={1}
                          max={max}
                          value={field.value}
                          onChange={(event) =>
                            field.onChange(event.target.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormDescription>范围 1–{max} 秒。</FormDescription>
                      <FormMessage />
                    </FormItem>
                  )}
                />
              ))}
            </div>
            <FormField
              control={form.control}
              name='pass_client_ip'
              render={({ field }) => (
                <FormItem className='flex items-center justify-between gap-4'>
                  <div>
                    <FormLabel>透传访客 IP</FormLabel>
                    <FormDescription>
                      在回源请求中保留客户端地址，供源站日志和访问控制使用。
                    </FormDescription>
                  </div>
                  <FormControl>
                    <Switch
                      checked={field.value}
                      onCheckedChange={field.onChange}
                    />
                  </FormControl>
                </FormItem>
              )}
            />
          </div>
        </section>
      )}
      {category === 'health' && (
        <section className='rounded-lg border'>
          <div className='flex items-center justify-between gap-4 p-4'>
            <div>
              <h3 className='font-medium'>健康检查</h3>
              <p className='text-sm text-muted-foreground'>
                设置源站探测路径、检查间隔和健康状态判定。
              </p>
            </div>
            <FormField
              control={form.control}
              name='health_check_enabled'
              render={({ field }) => (
                <FormItem className='flex items-center gap-3 space-y-0'>
                  <FormLabel>启用检查</FormLabel>
                  <FormControl>
                    <Switch
                      checked={field.value}
                      onCheckedChange={field.onChange}
                    />
                  </FormControl>
                </FormItem>
              )}
            />
          </div>
          <div className='space-y-4 border-t p-4'>
            {healthCheckEnabled && (
              <div className='mt-4 grid gap-3 sm:grid-cols-2 lg:grid-cols-3'>
                <FormField
                  control={form.control}
                  name='health_check_path'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>探测路径</FormLabel>
                      <FormControl>
                        <Input placeholder='/healthz' {...field} />
                      </FormControl>
                      <FormMessage />
                    </FormItem>
                  )}
                />
                <FormField
                  control={form.control}
                  name='health_check_interval_seconds'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>检查间隔（秒）</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          {...field}
                          onChange={(event) =>
                            field.onChange(event.currentTarget.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormMessage />
                    </FormItem>
                  )}
                />
                <FormField
                  control={form.control}
                  name='health_check_timeout_seconds'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>超时（秒）</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          {...field}
                          onChange={(event) =>
                            field.onChange(event.currentTarget.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormMessage />
                    </FormItem>
                  )}
                />
                <FormField
                  control={form.control}
                  name='health_check_expected_status'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>期望状态码</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          {...field}
                          onChange={(event) =>
                            field.onChange(event.currentTarget.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormMessage />
                    </FormItem>
                  )}
                />
                <FormField
                  control={form.control}
                  name='healthy_threshold'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>恢复阈值</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          {...field}
                          onChange={(event) =>
                            field.onChange(event.currentTarget.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormDescription>连续成功次数</FormDescription>
                      <FormMessage />
                    </FormItem>
                  )}
                />
                <FormField
                  control={form.control}
                  name='unhealthy_threshold'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>故障阈值</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          {...field}
                          onChange={(event) =>
                            field.onChange(event.currentTarget.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormDescription>连续失败次数</FormDescription>
                      <FormMessage />
                    </FormItem>
                  )}
                />
              </div>
            )}
          </div>
        </section>
      )}

      {category === 'logs' && (
        <section className='rounded-lg border'>
          <div className='flex items-center justify-between gap-4 p-4'>
            <div>
              <h3 className='font-medium'>访问日志</h3>
              <p className='text-sm text-muted-foreground'>
                选择需要保留的请求上下文，避免采集不必要的敏感信息。
              </p>
            </div>
            <FormField
              control={form.control}
              name='access_log_enabled'
              render={({ field }) => (
                <FormItem className='flex items-center gap-3 space-y-0'>
                  <FormLabel>启用日志</FormLabel>
                  <FormControl>
                    <Switch
                      checked={field.value}
                      onCheckedChange={field.onChange}
                    />
                  </FormControl>
                </FormItem>
              )}
            />
          </div>
          {accessLogEnabled && (
            <div className='space-y-4 border-t p-4'>
              <FormField
                control={form.control}
                name='access_log_status_code_ranges'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>记录状态码</FormLabel>
                    <div className='flex flex-wrap gap-2'>
                      {(['1xx', '2xx', '3xx', '4xx', '5xx'] as const).map(
                        (range) => {
                          const checked = field.value.includes(range)
                          return (
                            <label
                              key={range}
                              className='flex cursor-pointer items-center gap-2 rounded-md border px-3 py-2 text-sm'
                            >
                              <Checkbox
                                checked={checked}
                                onCheckedChange={(next) =>
                                  field.onChange(
                                    next
                                      ? [...field.value, range]
                                      : field.value.filter(
                                          (item) => item !== range
                                        )
                                  )
                                }
                              />
                              {range}
                            </label>
                          )
                        }
                      )}
                    </div>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <p className='text-sm text-muted-foreground'>
                基础信息记录请求地址、方法、状态码、耗时和流量。更多请求上下文可在高级采集中选择；关闭采集不会补齐或删除历史日志。
              </p>
              <Collapsible>
                <CollapsibleTrigger asChild>
                  <Button type='button' variant='outline' size='sm'>
                    高级采集选项
                  </Button>
                </CollapsibleTrigger>
                <CollapsibleContent className='pt-3'>
                  <div className='grid gap-2 sm:grid-cols-2'>
                    {[
                      ['access_log_query_params', '查询参数'],
                      ['access_log_referer', 'Referer'],
                      ['access_log_user_agent', 'User-Agent'],
                      ['access_log_client_abort', '客户端中断'],
                      ['access_log_request_headers', '请求头'],
                      ['access_log_response_headers', '响应头'],
                      ['access_log_cookies', 'Cookie（可能含敏感信息）'],
                      ['access_log_request_body', '请求体（可能含敏感信息）'],
                    ].map(([name, label]) => (
                      <FormField
                        key={name}
                        control={form.control}
                        name={name as keyof WebsiteFormValues}
                        render={({ field }) => (
                          <FormItem className='flex items-center justify-between rounded-md border p-3'>
                            <FormLabel className='cursor-pointer text-sm'>
                              {label}
                            </FormLabel>
                            <FormControl>
                              <Switch
                                checked={field.value as boolean}
                                onCheckedChange={field.onChange}
                              />
                            </FormControl>
                          </FormItem>
                        )}
                      />
                    ))}
                  </div>
                </CollapsibleContent>
              </Collapsible>
            </div>
          )}
        </section>
      )}

      {category === 'https' && (
        <section className='rounded-lg border'>
          <div className='flex items-center justify-between gap-4 p-4'>
            <div>
              <h3 className='font-medium'>HTTPS 与传输协议</h3>
              <p className='text-sm text-muted-foreground'>
                配置证书、最低 TLS 版本，以及 HTTPS 跳转和 HTTP/2。
              </p>
            </div>
            <FormField
              control={form.control}
              name='https_enabled'
              render={({ field }) => (
                <FormItem className='flex items-center gap-3 space-y-0'>
                  <FormLabel>启用 HTTPS</FormLabel>
                  <FormControl>
                    <Switch
                      checked={field.value}
                      onCheckedChange={field.onChange}
                    />
                  </FormControl>
                </FormItem>
              )}
            />
          </div>
          {httpsEnabled && (
            <div className='space-y-4 border-t p-4'>
              <div className='grid gap-3 sm:grid-cols-2'>
                <FormField
                  control={form.control}
                  name='minimum_tls_version'
                  render={({ field }) => (
                    <FormItem className='sm:col-span-2'>
                      <FormLabel>最低 TLS 版本</FormLabel>
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
                          <SelectItem value='1.2'>TLS 1.2</SelectItem>
                          <SelectItem value='1.3'>TLS 1.3</SelectItem>
                        </SelectContent>
                      </Select>
                    </FormItem>
                  )}
                />
                <div className='grid gap-3 sm:col-span-2 sm:grid-cols-3'>
                  {[
                    ['force_https', '强制 HTTPS'],
                    ['http2_enabled', 'HTTP/2'],
                    ['hsts_enabled', 'HSTS'],
                  ].map(([name, label]) => (
                    <FormField
                      key={name}
                      control={form.control}
                      name={name as keyof WebsiteFormValues}
                      render={({ field }) => (
                        <FormItem className='flex min-h-12 items-center justify-between gap-3 rounded-md border p-3'>
                          <FormLabel className='text-sm'>{label}</FormLabel>
                          <FormControl>
                            <Switch
                              checked={field.value as boolean}
                              onCheckedChange={field.onChange}
                            />
                          </FormControl>
                        </FormItem>
                      )}
                    />
                  ))}
                </div>
              </div>
              <FormField
                control={form.control}
                name='certificate_ids'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>绑定证书</FormLabel>
                    <FormDescription>
                      可绑定最多 20 张当前可用的证书。
                    </FormDescription>
                    <div className='grid gap-2 sm:grid-cols-2'>
                      {certificates.map((certificate) => {
                        const checked = field.value.includes(certificate.id)
                        return (
                          <label
                            key={certificate.id}
                            className='flex cursor-pointer items-center gap-3 rounded-md border p-3 text-sm'
                          >
                            <Checkbox
                              checked={checked}
                              onCheckedChange={(next) =>
                                field.onChange(
                                  next
                                    ? [...field.value, certificate.id]
                                    : field.value.filter(
                                        (id) => id !== certificate.id
                                      )
                                )
                              }
                            />
                            <span className='truncate'>
                              {certificate.domains[0]}
                            </span>
                          </label>
                        )
                      })}
                    </div>
                  </FormItem>
                )}
              />
            </div>
          )}
        </section>
      )}

      {category === 'compression' && (
        <section className='flex flex-1 flex-col rounded-lg border'>
          <div className='flex items-center justify-between gap-4 p-4'>
            <div>
              <h3 className='font-medium'>响应压缩</h3>
              <p className='text-sm text-muted-foreground'>
                按内容类型和大小压缩响应，不会改变源站文件。
              </p>
            </div>
            <FormField
              control={form.control}
              name='response_compression_enabled'
              render={({ field }) => (
                <FormItem className='flex items-center gap-3 space-y-0'>
                  <FormLabel>启用压缩</FormLabel>
                  <FormControl>
                    <Switch
                      checked={field.value}
                      onCheckedChange={field.onChange}
                    />
                  </FormControl>
                </FormItem>
              )}
            />
          </div>
          {compressionEnabled && (
            <div className='flex flex-1 flex-col gap-4 border-t p-4'>
              <div className='grid gap-3 sm:grid-cols-2'>
                <FormField
                  control={form.control}
                  name='response_compression_min_bytes'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>最小响应大小（字节）</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          {...field}
                          onChange={(event) =>
                            field.onChange(event.currentTarget.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormMessage />
                    </FormItem>
                  )}
                />
                <FormField
                  control={form.control}
                  name='response_compression_max_bytes'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>最大响应大小（字节）</FormLabel>
                      <FormControl>
                        <Input
                          type='number'
                          {...field}
                          onChange={(event) =>
                            field.onChange(event.currentTarget.valueAsNumber)
                          }
                        />
                      </FormControl>
                      <FormDescription>填 0 表示不设上限</FormDescription>
                      <FormMessage />
                    </FormItem>
                  )}
                />
              </div>
              <FormField
                control={form.control}
                name='response_compression_algorithms'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>压缩算法</FormLabel>
                    <FormDescription>
                      按勾选顺序确定优先级，重新勾选排到末尾；客户端 q
                      权重相同时生效。
                    </FormDescription>
                    <CompressionAlgorithmSelect
                      value={field.value}
                      onChange={field.onChange}
                    />
                    <FormMessage />
                  </FormItem>
                )}
              />
              <div className='grid flex-1 gap-3 sm:grid-cols-3'>
                {[
                  [
                    'response_compression_mime_types',
                    '压缩 MIME 类型',
                    '每行一个，例如 text/*',
                  ],
                  [
                    'response_compression_extensions',
                    '仅压缩扩展名',
                    '每行一个，例如 .html',
                  ],
                  [
                    'response_compression_excluded_extensions',
                    '排除扩展名',
                    '每行一个，例如 .zip',
                  ],
                ].map(([name, label, placeholder]) => (
                  <FormField
                    key={name}
                    control={form.control}
                    name={name as keyof WebsiteFormValues}
                    render={({ field }) => (
                      <FormItem className='flex flex-col'>
                        <FormLabel>{label}</FormLabel>
                        <FormControl>
                          <Textarea
                            className='[field-sizing:fixed] min-h-40 flex-1 resize-none overflow-y-auto font-mono text-xs'
                            placeholder={placeholder}
                            value={valuesToLines(field.value as string[])}
                            onChange={(event) =>
                              field.onChange(
                                linesToValues(event.currentTarget.value)
                              )
                            }
                          />
                        </FormControl>
                      </FormItem>
                    )}
                  />
                ))}
              </div>
            </div>
          )}
        </section>
      )}
    </TabsContent>
  )
}
