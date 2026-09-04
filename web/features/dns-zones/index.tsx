import { useMemo, useState } from 'react'
import { z } from 'zod'
import { useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { Eye, FilePenLine, Plus, RefreshCw, Trash2, X } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { formatDate } from '@/lib/format'
import type { DnsProvider, DnsZone, PageData } from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import {
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
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
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { DnsLineSelect, DnsLineTree } from '@/components/dns-line-tree'
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'

const createSchema = z.object({
  dns_provider_id: z.string().uuid('请选择 DNS 服务商'),
  domain: z.string().trim().min(1, '请选择域名'),
})

type CreateValues = z.infer<typeof createSchema>

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

export function DnsZones() {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const [draftKeyword, setDraftKeyword] = useState('')
  const [keyword, setKeyword] = useState('')
  const [providerId, setProviderId] = useState('all')
  const [createOpen, setCreateOpen] = useState(false)
  const [editTarget, setEditTarget] = useState<DnsZone | null>(null)
  const [detailTarget, setDetailTarget] = useState<DnsZone | null>(null)
  const [removeTarget, setRemoveTarget] = useState<DnsZone | null>(null)

  const providersQuery = useQuery({
    queryKey: ['providers', 'dns', 'options'],
    queryFn: () =>
      getData<PageData<DnsProvider>>('/providers/dns', {
        page: 1,
        page_size: 100,
      }).then((data) => data.list),
  })
  const query = useQuery({
    queryKey: ['dns-zones', page, pageSize, keyword, providerId],
    queryFn: () =>
      getData<PageData<DnsZone>>('/dns-zones/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        dns_provider_id: providerId === 'all' ? undefined : providerId,
      }),
  })
  const { mutate: syncZone } = useMutation({
    mutationFn: ({
      item,
      policy,
    }: {
      item: DnsZone
      policy?: 'local' | 'remote'
    }) =>
      sendData(
        'post',
        `/dns-zones/${item.id}/sync`,
        policy ? { conflict_policy: policy } : undefined
      ),
    onSuccess: async (response) => {
      toast.success(response.message)
      await queryClient.invalidateQueries({ queryKey: ['dns-zones'] })
    },
  })
  const remove = useMutation({
    mutationFn: (item: DnsZone) =>
      sendData('delete', `/dns-zones/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: ['dns-zones'] })
    },
  })

  const columns = useMemo<ColumnDef<DnsZone>[]>(
    () => [
      {
        accessorKey: 'domain',
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='托管域名' />
        ),
        cell: ({ row }) => (
          <div>
            <div className='font-medium'>{row.original.domain}</div>
            <div className='text-xs text-muted-foreground'>
              {row.original.dns_provider_name}
            </div>
          </div>
        ),
      },
      {
        accessorKey: 'sync_status',
        header: '同步状态',
        cell: ({ row }) => (
          <div className='space-y-1'>
            <StatusBadge status={row.original.sync_status} />
            <div className='text-xs text-muted-foreground'>
              rev {row.original.synced_revision}/{row.original.desired_revision}
            </div>
          </div>
        ),
      },
      {
        id: 'records',
        header: 'DNS 记录',
        cell: ({ row }) => (
          <span className='tabular-nums'>
            {row.original.config.records.length} 条
          </span>
        ),
      },
      {
        accessorKey: 'website_count',
        header: '网站',
      },
      {
        accessorKey: 'last_synced_at',
        header: '最近同步',
        cell: ({ row }) => (
          <span className='whitespace-nowrap text-muted-foreground'>
            {formatDate(row.original.last_synced_at)}
          </span>
        ),
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem onSelect={() => setDetailTarget(row.original)}>
              <Eye /> 查看详情
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setEditTarget(row.original)}>
              <FilePenLine /> 编辑记录
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => syncZone({ item: row.original })}>
              <RefreshCw /> 立即同步
            </DropdownMenuItem>
            {row.original.runtime.conflicts.length > 0 && (
              <>
                <DropdownMenuLabel>解决冲突</DropdownMenuLabel>
                <DropdownMenuItem
                  onSelect={() =>
                    syncZone({ item: row.original, policy: 'local' })
                  }
                >
                  使用本地记录
                </DropdownMenuItem>
                <DropdownMenuItem
                  onSelect={() =>
                    syncZone({ item: row.original, policy: 'remote' })
                  }
                >
                  使用远端记录
                </DropdownMenuItem>
              </>
            )}
            <DropdownMenuSeparator />
            <DropdownMenuItem
              variant='destructive'
              onSelect={() => setRemoveTarget(row.original)}
            >
              <Trash2 /> 删除
            </DropdownMenuItem>
          </RowActions>
        ),
      },
    ],
    [syncZone]
  )

  return (
    <FeatureShell
      title='DNS 托管'
      description='同步 DNS 服务商区域、记录与线路状态。'
      actions={
        <Button onClick={() => setCreateOpen(true)}>
          <Plus /> 添加域名
        </Button>
      }
    >
      <ResourceToolbar
        value={draftKeyword}
        onChange={setDraftKeyword}
        onSearch={() => {
          setKeyword(draftKeyword.trim())
          setPage(1)
        }}
        onReset={() => {
          setDraftKeyword('')
          setKeyword('')
          setPage(1)
        }}
        onRefresh={() => query.refetch()}
        refreshing={query.isFetching}
        placeholder='搜索域名…'
        filters={
          <Select
            value={providerId}
            onValueChange={(value) => {
              setProviderId(value)
              setPage(1)
            }}
          >
            <SelectTrigger className='w-44'>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value='all'>全部服务商</SelectItem>
              {providersQuery.data?.map((provider) => (
                <SelectItem key={provider.id} value={provider.id}>
                  {provider.name}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
        }
      />
      <ResourceTable
        columns={columns}
        data={query.data?.list ?? []}
        loading={query.isLoading}
        error={query.isError}
        onRetry={() => void query.refetch()}
        page={page}
        pageSize={pageSize}
        totalPages={query.data?.total_pages ?? 1}
        onPaginationChange={(nextPage, nextSize) => {
          setPage(nextPage)
          setPageSize(nextSize)
        }}
        emptyTitle='暂无托管域名'
        emptyDescription='先添加 DNS 服务商，再导入可用域名。'
      />
      <CreateZoneDialog
        open={createOpen}
        onOpenChange={setCreateOpen}
        providers={providersQuery.data ?? []}
      />
      {editTarget && (
        <RecordsDialog
          key={editTarget.id}
          zone={editTarget}
          open
          onOpenChange={(open) => !open && setEditTarget(null)}
        />
      )}
      <ZoneDetailSheet
        zone={detailTarget}
        onOpenChange={(open) => !open && setDetailTarget(null)}
      />
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='移除托管域名'
        desc={`确定移除“${removeTarget?.domain ?? ''}”吗？有关联网站时服务端会拒绝删除。`}
        confirmText={remove.isPending ? '正在删除…' : '确认移除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}

function CreateZoneDialog({
  open,
  onOpenChange,
  providers,
}: {
  open: boolean
  onOpenChange: (open: boolean) => void
  providers: DnsProvider[]
}) {
  const queryClient = useQueryClient()
  const form = useForm<CreateValues>({
    resolver: zodResolver(createSchema),
    defaultValues: { dns_provider_id: '', domain: '' },
  })
  const providerId = form.watch('dns_provider_id')
  const availableQuery = useQuery({
    queryKey: ['dns-zones', 'available', providerId],
    queryFn: () =>
      getData<{ list: Array<{ domain: string; status: string }> }>(
        '/dns-zones/available',
        { dns_provider_id: providerId }
      ).then((data) => data.list),
    enabled: !!providerId,
  })
  const mutation = useMutation({
    mutationFn: (values: CreateValues) =>
      sendData('post', '/dns-zones/', values),
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      form.reset()
      await queryClient.invalidateQueries({ queryKey: ['dns-zones'] })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto'>
        <SheetHeader>
          <SheetTitle>添加托管域名</SheetTitle>
          <SheetDescription>
            从 DNS 服务商账号中选择一个可用区域。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='create-zone-form'
            className='grid gap-4 px-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='dns_provider_id'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>DNS 服务商账号</FormLabel>
                  <Select
                    value={field.value}
                    onValueChange={(value) => {
                      field.onChange(value)
                      form.setValue('domain', '')
                    }}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue placeholder='选择账号' />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      {providers.map((provider) => (
                        <SelectItem key={provider.id} value={provider.id}>
                          {provider.name}
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
              name='domain'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>域名</FormLabel>
                  <Select
                    value={field.value}
                    onValueChange={field.onChange}
                    disabled={!providerId || availableQuery.isLoading}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue
                          placeholder={
                            availableQuery.isLoading
                              ? '正在加载…'
                              : '选择可用域名'
                          }
                        />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      {availableQuery.data?.map((zone) => (
                        <SelectItem key={zone.domain} value={zone.domain}>
                          {zone.domain}
                        </SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                  <FormMessage />
                </FormItem>
              )}
            />
          </form>
        </Form>
        <SheetFooter>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            type='submit'
            form='create-zone-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在添加…' : '添加并同步'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}

function RecordsDialog({
  zone,
  open,
  onOpenChange,
}: {
  zone: DnsZone
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const queryClient = useQueryClient()
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
        { records: values.records },
        zone.revision
      ),
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({ queryKey: ['dns-zones'] })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='w-full overflow-hidden p-0 sm:max-w-4xl'>
        <SheetHeader className='px-6 pt-6'>
          <SheetTitle>{zone.domain} · DNS 记录</SheetTitle>
          <SheetDescription>
            保存后会自动提交同步任务。记录 ID 由控制台生成并保持稳定。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='zone-records-form'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <ScrollArea className='max-h-[64svh] px-6'>
              <div className='space-y-3 pb-4'>
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
                      <FormField
                        control={form.control}
                        name={`records.${index}.proxied`}
                        render={({ field }) => (
                          <FormItem className='flex items-center'>
                            <FormControl>
                              <Checkbox
                                checked={field.value}
                                onCheckedChange={field.onChange}
                                aria-label='代理'
                              />
                            </FormControl>
                          </FormItem>
                        )}
                      />
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

function ZoneDetailSheet({
  zone,
  onOpenChange,
}: {
  zone: DnsZone | null
  onOpenChange: (open: boolean) => void
}) {
  return (
    <Sheet open={!!zone} onOpenChange={onOpenChange}>
      <SheetContent className='w-full overflow-y-auto sm:max-w-xl'>
        <SheetHeader className='text-start'>
          <SheetTitle>{zone?.domain}</SheetTitle>
          <SheetDescription>同步状态、线路与冲突详情。</SheetDescription>
        </SheetHeader>
        {zone && (
          <div className='space-y-6 px-4 pb-6'>
            <div className='grid grid-cols-2 gap-3'>
              <div className='rounded-md border p-3'>
                <div className='text-xs text-muted-foreground'>同步状态</div>
                <div className='mt-2'>
                  <StatusBadge status={zone.sync_status} />
                </div>
              </div>
              <div className='rounded-md border p-3'>
                <div className='text-xs text-muted-foreground'>记录</div>
                <div className='mt-1 text-2xl font-semibold'>
                  {zone.config.records.length}
                </div>
              </div>
            </div>
            <div>
              <h3 className='mb-2 text-sm font-semibold'>DNS 线路</h3>
              <DnsLineTree lines={zone.runtime.lines} />
            </div>
            <div>
              <h3 className='mb-2 text-sm font-semibold'>
                冲突
                <Badge variant='secondary' className='ms-2'>
                  {zone.runtime.conflicts.length}
                </Badge>
              </h3>
              <div className='space-y-2'>
                {zone.runtime.conflicts.map((conflict) => (
                  <div
                    key={conflict.id}
                    className='rounded-md border p-3 text-sm'
                  >
                    <div className='font-medium'>
                      {conflict.type} {conflict.name}
                    </div>
                    <div className='mt-2 grid gap-1 text-xs'>
                      <div>本地：{conflict.local_content}</div>
                      <div>远端：{conflict.remote_content}</div>
                    </div>
                  </div>
                ))}
                {!zone.runtime.conflicts.length && (
                  <p className='text-sm text-muted-foreground'>
                    当前没有冲突。
                  </p>
                )}
              </div>
            </div>
            {zone.last_error && (
              <div className='rounded-md border border-destructive/30 bg-destructive/5 p-3 text-sm text-destructive'>
                {zone.last_error}
              </div>
            )}
          </div>
        )}
      </SheetContent>
    </Sheet>
  )
}
