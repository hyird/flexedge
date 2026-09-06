import { z } from 'zod'
import { useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { Plus, X } from 'lucide-react'
import { toast } from 'sonner'
import { sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type { DnsZone } from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import {
  Form,
  FormControl,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import { ScrollArea } from '@/components/ui/scroll-area'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetFooter,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { DnsLineSelect } from '@/components/dns-line-tree'

const recordSchema = z.object({
  id: z.string().uuid(),
  type: z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX']),
  name: z.string().trim().min(1, '请输入主机记录').max(253),
  content: z.string().trim().min(1, '请输入记录值').max(4096),
  ttl: z.number().int().min(1).max(86400),
  priority: z.number().int().min(0).max(65535).optional(),
  proxied: z.boolean(),
  line_code: z.string().trim().min(1, '请输入线路代码').max(64),
})

const recordsSchema = z.object({ records: z.array(recordSchema).max(10000) })
type RecordsValues = z.infer<typeof recordsSchema>

export function RecordsDialog({
  zone,
  open,
  onOpenChange,
}: {
  zone: DnsZone
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const queryClient = useQueryClient()
  const supportsProxy = zone.dns_provider === 'cloudflare'
  const systemRecords = zone.runtime.projected_records
  const form = useForm<RecordsValues>({
    resolver: zodResolver(recordsSchema),
    defaultValues: { records: zone.config.records },
  })
  const records = useFieldArray({
    control: form.control,
    name: 'records',
    keyName: 'formKey',
  })
  const mutation = useMutation({
    mutationFn: (values: RecordsValues) =>
      sendData(
        'put',
        `/dns-zones/${zone.id}`,
        {
          records: supportsProxy
            ? values.records
            : values.records.map((record) => ({ ...record, proxied: false })),
        },
        zone.revision
      ),
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({ queryKey: queryKeys.dnsZones })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='flex h-full w-full flex-col overflow-hidden p-0 sm:max-w-4xl'>
        <SheetHeader className='px-6 pt-6'>
          <SheetTitle>{zone.domain} · DNS 记录</SheetTitle>
          <SheetDescription>
            保存后会自动提交同步任务。系统自动记录由节点和托管网站生成，只读且不可删除。
            {supportsProxy
              ? ' Cloudflare 支持代理开关。'
              : ' 当前服务商仅支持 DNS 解析。'}
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='zone-records-form'
            className='flex min-h-0 flex-1 flex-col'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <ScrollArea className='min-h-0 flex-1 px-6'>
              <div className='space-y-3 pb-4'>
                {!!systemRecords.length && (
                  <section className='space-y-3 rounded-md border border-dashed bg-muted/30 p-3'>
                    <div className='flex flex-wrap items-center gap-2'>
                      <h3 className='text-sm font-medium'>系统自动记录</h3>
                      <Badge variant='secondary'>只读</Badge>
                      <p className='text-xs text-muted-foreground'>
                        随节点、集群或网站托管解析自动更新，不能在此编辑或删除。
                      </p>
                    </div>
                    <div className='space-y-2'>
                      {systemRecords.map((record) => (
                        <div
                          key={record.id}
                          className='grid gap-1 rounded-md border bg-background px-3 py-2 text-sm md:grid-cols-[100px_1fr_1.4fr_90px_100px] md:gap-2'
                        >
                          <span className='font-medium'>{record.type}</span>
                          <span className='break-all'>{record.name}</span>
                          <span className='break-all text-muted-foreground'>{record.content}</span>
                          <span>TTL {record.ttl}</span>
                          <span className='text-muted-foreground'>
                            {record.line_code}
                          </span>
                        </div>
                      ))}
                    </div>
                  </section>
                )}
                <div className='sticky top-0 z-10 hidden gap-2 border-b bg-background px-3 py-2 text-xs font-medium text-muted-foreground md:grid md:grid-cols-[100px_1fr_1.4fr_90px_100px_auto]'>
                  <span>类型</span>
                  <span>主机记录</span>
                  <span>记录值</span>
                  <span>TTL</span>
                  <span>线路</span>
                  <span>{supportsProxy ? '代理 / 操作' : '操作'}</span>
                </div>
                {records.fields.map((record, index) => (
                  <div
                    key={record.formKey}
                    className='grid gap-2 rounded-md border p-3 md:grid-cols-[100px_1fr_1.4fr_90px_100px_auto]'
                  >
                    <FormField
                      control={form.control}
                      name={`records.${index}.type`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel className='md:sr-only'>类型</FormLabel>
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
                              {['A', 'AAAA', 'CNAME', 'TXT', 'MX'].map(
                                (type) => (
                                  <SelectItem key={type} value={type}>
                                    {type}
                                  </SelectItem>
                                )
                              )}
                            </SelectContent>
                          </Select>
                        </FormItem>
                      )}
                    />
                    <FormField
                      control={form.control}
                      name={`records.${index}.name`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel className='md:sr-only'>主机记录</FormLabel>
                          <FormControl>
                            <Input placeholder='@ 或 www' {...field} />
                          </FormControl>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                    <FormField
                      control={form.control}
                      name={`records.${index}.content`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel className='md:sr-only'>记录值</FormLabel>
                          <FormControl>
                            <Input placeholder='记录值' {...field} />
                          </FormControl>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                    <FormField
                      control={form.control}
                      name={`records.${index}.ttl`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel className='md:sr-only'>TTL</FormLabel>
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
                      name={`records.${index}.line_code`}
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel className='md:sr-only'>线路</FormLabel>
                          <FormControl>
                            <DnsLineSelect
                              lines={zone.runtime.lines}
                              value={field.value}
                              onValueChange={field.onChange}
                            />
                          </FormControl>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                    <div className='flex items-center gap-1'>
                      {supportsProxy && (
                        <FormField
                          control={form.control}
                          name={`records.${index}.proxied`}
                          render={({ field }) => (
                            <FormItem className='flex items-center gap-2 space-y-0'>
                              <FormControl>
                                <Checkbox
                                  checked={field.value}
                                  onCheckedChange={field.onChange}
                                  aria-label='代理'
                                />
                              </FormControl>
                              <FormLabel className='cursor-pointer text-sm font-normal'>
                                代理
                              </FormLabel>
                            </FormItem>
                          )}
                        />
                      )}
                      <Button
                        type='button'
                        variant='ghost'
                        size='icon'
                        aria-label='删除 DNS 记录'
                        onClick={() => records.remove(index)}
                      >
                        <X />
                      </Button>
                    </div>
                  </div>
                ))}
                {!records.fields.length && (
                  <div className='rounded-md border border-dashed py-12 text-center text-sm text-muted-foreground'>
                    暂无 DNS 记录
                  </div>
                )}
              </div>
            </ScrollArea>
          </form>
        </Form>
        <SheetFooter className='px-6 py-4'>
          <Button
            variant='outline'
            onClick={() =>
              records.append({
                id: crypto.randomUUID(),
                type: 'A',
                name: '@',
                content: '',
                ttl: 600,
                proxied: false,
                line_code: zone.runtime.lines[0]?.code || 'default',
              })
            }
          >
            <Plus /> 添加记录
          </Button>
          <div className='flex-1' />
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            type='submit'
            form='zone-records-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在保存…' : '保存并同步'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
