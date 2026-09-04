import { useEffect, useMemo, useState } from 'react'
import { z } from 'zod'
import { useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { Eye, FilePenLine, FileText, Plus, Trash2, X } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData, type ApiEnvelope } from '@/lib/api'
import { formatDate } from '@/lib/format'
import type {
  Certificate,
  Cluster,
  PageData,
  Website,
  WebsiteConfig,
} from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import {
  DropdownMenuItem,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
import {
  Form,
  FormControl,
  FormDescription,
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
import { Switch } from '@/components/ui/switch'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { Textarea } from '@/components/ui/textarea'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'

const domainSchema = z.object({
  id: z.string().uuid(),
  hostname: z.string().trim().min(1, '请输入域名').max(253),
  dns_mode: z.enum(['managed', 'external']),
})

const originSchema = z.object({
  id: z.string().uuid(),
  group: z.string().trim().min(1, '请输入源站组').max(100),
  protocol: z.enum(['http', 'https']),
  host: z.string().trim().min(1, '请输入源站地址').max(253),
  port: z.number().int().min(1).max(65535),
  role: z.enum(['primary', 'backup']),
  weight: z.number().int().min(1).max(100),
  status: z.enum(['enabled', 'disabled']),
})

const schema = z.object({
  cluster_id: z.string().uuid('请选择所属集群'),
  status: z.enum(['enabled', 'disabled']),
  name: z.string().trim().min(1, '请输入网站名称').max(100),
  domains: z.array(domainSchema).min(1, '至少添加一个域名').max(100),
  origins: z.array(originSchema).min(1, '至少添加一个源站').max(100),
  default_origin_group: z.string().trim().min(1, '请输入默认源站组').max(100),
  origin_host_header: z.string().trim().min(1, '请输入回源 Host').max(253),
  health_check_enabled: z.boolean(),
  access_log_enabled: z.boolean(),
  https_enabled: z.boolean(),
  certificate_ids: z.array(z.string().uuid()).max(20),
  response_compression_enabled: z.boolean(),
  route_rules_json: z.string().refine(
    (value) => {
      try {
        return Array.isArray(JSON.parse(value))
      } catch {
        return false
      }
    },
    { message: '路由规则必须是 JSON 数组' }
  ),
})

type Values = z.infer<typeof schema>
type AccessLog = {
  id: string
  occurred_at: string
  node_id: string
  node_name: string
  client_ip?: string
  protocol: string
  method: string
  host: string
  target: string
  status_code: number
  response_bytes: number
  duration_ms: number
  user_agent?: string
  referer?: string
}

function defaultConfig(): WebsiteConfig {
  return {
    name: '',
    domains: [],
    origins: [],
    default_origin_group: 'default',
    origin_host_header: '$host',
    origin_connect_timeout_seconds: 10,
    origin_read_timeout_seconds: 30,
    pass_client_ip: true,
    health_check_enabled: true,
    health_check_path: '/',
    health_check_interval_seconds: 10,
    health_check_timeout_seconds: 3,
    health_check_expected_status: 200,
    healthy_threshold: 2,
    unhealthy_threshold: 3,
    access_log_enabled: true,
    access_log_request_headers: false,
    access_log_request_body: false,
    access_log_response_headers: false,
    access_log_query_params: true,
    access_log_cookies: false,
    access_log_referer: true,
    access_log_user_agent: true,
    access_log_status_code_ranges: ['2xx', '3xx', '4xx', '5xx'],
    access_log_client_abort: true,
    https_enabled: false,
    certificate_ids: [],
    minimum_tls_version: '1.2',
    force_https: false,
    http2_enabled: true,
    hsts_enabled: false,
    response_compression_enabled: true,
    response_compression_min_bytes: 1024,
    response_compression_max_bytes: 0,
    response_compression_algorithms: ['br', 'gzip'],
    response_compression_mime_types: [
      'text/*',
      'application/json',
      'application/javascript',
    ],
    response_compression_extensions: [],
    response_compression_excluded_extensions: [
      '.jpg',
      '.jpeg',
      '.png',
      '.gif',
      '.webp',
      '.zip',
      '.gz',
      '.mp4',
    ],
    route_rules: [],
  }
}

export function Websites() {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const [draftKeyword, setDraftKeyword] = useState('')
  const [keyword, setKeyword] = useState('')
  const [clusterId, setClusterId] = useState('all')
  const [status, setStatus] = useState('all')
  const [dialog, setDialog] = useState<Website | 'new' | null>(null)
  const [detailTarget, setDetailTarget] = useState<Website | null>(null)
  const [logTarget, setLogTarget] = useState<Website | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Website | null>(null)

  const clustersQuery = useQuery({
    queryKey: ['clusters', 'options'],
    queryFn: () =>
      getData<PageData<Cluster>>('/clusters/', {
        page: 1,
        page_size: 100,
      }).then((data) => data.list),
  })
  const query = useQuery({
    queryKey: ['websites', page, pageSize, keyword, clusterId, status],
    queryFn: () =>
      getData<PageData<Website>>('/websites/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        cluster_id: clusterId === 'all' ? undefined : clusterId,
        status: status === 'all' ? undefined : status,
      }),
  })
  const remove = useMutation({
    mutationFn: (item: Website) =>
      sendData('delete', `/websites/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: ['websites'] })
    },
  })

  const columns = useMemo<ColumnDef<Website>[]>(
    () => [
      {
        id: 'name',
        accessorFn: (item) => item.config.name,
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='网站' />
        ),
        cell: ({ row }) => (
          <div>
            <div className='font-medium'>
              {row.original.config.name ||
                row.original.config.domains[0]?.hostname}
            </div>
            <div className='max-w-64 truncate text-xs text-muted-foreground'>
              {row.original.config.domains
                .map((item) => item.hostname)
                .join('、')}
            </div>
          </div>
        ),
      },
      {
        accessorKey: 'cluster_name',
        header: '集群',
        cell: ({ row }) => (
          <div>
            <div>{row.original.cluster_name}</div>
            <code className='text-xs text-muted-foreground'>
              {row.original.access_domain}
            </code>
          </div>
        ),
      },
      {
        id: 'deploy',
        header: '部署',
        cell: ({ row }) => (
          <div className='space-y-1'>
            <StatusBadge status={row.original.runtime.deploy_status} />
            <div className='text-xs text-muted-foreground'>
              {row.original.runtime.synced_node_count}/
              {row.original.runtime.target_node_count} 节点
            </div>
          </div>
        ),
      },
      {
        id: 'https',
        header: 'HTTPS',
        cell: ({ row }) => (
          <Badge variant='outline'>
            {row.original.config.https_enabled
              ? `${row.original.certificates.length} 张证书`
              : '未启用'}
          </Badge>
        ),
      },
      {
        accessorKey: 'status',
        header: '状态',
        cell: ({ row }) => <StatusBadge status={row.original.status} />,
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem onSelect={() => setDetailTarget(row.original)}>
              <Eye /> 查看详情
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setDialog(row.original)}>
              <FilePenLine /> 编辑配置
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setLogTarget(row.original)}>
              <FileText /> 访问日志
            </DropdownMenuItem>
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
    []
  )

  return (
    <FeatureShell
      title='网站'
      description='配置域名、源站、TLS 与边缘分发策略。'
      actions={
        <Button onClick={() => setDialog('new')}>
          <Plus /> 创建网站
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
        placeholder='搜索网站或域名…'
        filters={
          <>
            <Select
              value={clusterId}
              onValueChange={(value) => {
                setClusterId(value)
                setPage(1)
              }}
            >
              <SelectTrigger className='w-44'>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value='all'>全部集群</SelectItem>
                {clustersQuery.data?.map((cluster) => (
                  <SelectItem key={cluster.id} value={cluster.id}>
                    {cluster.name}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Select
              value={status}
              onValueChange={(value) => {
                setStatus(value)
                setPage(1)
              }}
            >
              <SelectTrigger className='w-36'>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value='all'>全部状态</SelectItem>
                <SelectItem value='enabled'>已启用</SelectItem>
                <SelectItem value='disabled'>已停用</SelectItem>
              </SelectContent>
            </Select>
          </>
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
        emptyTitle='暂无网站'
        emptyDescription='创建网站并将流量分发到边缘集群。'
      />
      {dialog && (
        <WebsiteDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          website={dialog === 'new' ? undefined : dialog}
          clusters={clustersQuery.data ?? []}
          open
          onOpenChange={(open) => !open && setDialog(null)}
        />
      )}
      <WebsiteDetailSheet
        website={detailTarget}
        onOpenChange={(open) => !open && setDetailTarget(null)}
      />
      {logTarget && (
        <AccessLogSheet
          key={logTarget.id}
          website={logTarget}
          onOpenChange={(open) => !open && setLogTarget(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除网站'
        desc={`确定删除“${removeTarget?.config.name ?? ''}”吗？边缘节点将收到移除配置任务。`}
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}

function WebsiteDialog({
  website,
  clusters,
  open,
  onOpenChange,
}: {
  website?: Website
  clusters: Cluster[]
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const queryClient = useQueryClient()
  const config = website?.config ?? defaultConfig()
  const certificatesQuery = useQuery({
    queryKey: ['certificates', 'usable-options'],
    queryFn: () =>
      getData<PageData<Certificate>>('/certificates/', {
        page: 1,
        page_size: 100,
        usable: true,
      }).then((data) => data.list),
  })
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      cluster_id: website?.cluster_id ?? '',
      status: (website?.status as Values['status']) ?? 'enabled',
      name: config.name ?? '',
      domains:
        config.domains.length > 0
          ? config.domains
          : [
              {
                id: crypto.randomUUID(),
                hostname: '',
                dns_mode: 'managed' as const,
              },
            ],
      origins:
        config.origins.length > 0
          ? config.origins
          : [
              {
                id: crypto.randomUUID(),
                group: 'default',
                protocol: 'http' as const,
                host: '',
                port: 80,
                role: 'primary' as const,
                weight: 100,
                status: 'enabled' as const,
              },
            ],
      default_origin_group: config.default_origin_group,
      origin_host_header: config.origin_host_header,
      health_check_enabled: config.health_check_enabled,
      access_log_enabled: config.access_log_enabled,
      https_enabled: config.https_enabled,
      certificate_ids: config.certificate_ids,
      response_compression_enabled: config.response_compression_enabled,
      route_rules_json: JSON.stringify(config.route_rules, null, 2),
    },
  })
  const domains = useFieldArray({
    control: form.control,
    name: 'domains',
    keyName: 'formKey',
  })
  const origins = useFieldArray({
    control: form.control,
    name: 'origins',
    keyName: 'formKey',
  })
  const httpsEnabled = form.watch('https_enabled')
  const mutation = useMutation({
    mutationFn: (values: Values) => {
      const bodyConfig: WebsiteConfig = {
        ...config,
        name: values.name,
        domains: values.domains,
        origins: values.origins,
        default_origin_group: values.default_origin_group,
        origin_host_header: values.origin_host_header,
        health_check_enabled: values.health_check_enabled,
        access_log_enabled: values.access_log_enabled,
        https_enabled: values.https_enabled,
        certificate_ids: values.certificate_ids,
        response_compression_enabled: values.response_compression_enabled,
        route_rules: JSON.parse(values.route_rules_json) as Array<
          Record<string, unknown>
        >,
      }
      const body = { status: values.status, config: bodyConfig }
      const url = website
        ? `/websites/${website.id}?cluster_id=${values.cluster_id}`
        : `/websites/?cluster_id=${values.cluster_id}`
      return website
        ? sendData('put', url, body, website.revision)
        : sendData('post', url, body)
    },
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({ queryKey: ['websites'] })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='w-full overflow-hidden p-0 sm:max-w-4xl'>
        <SheetHeader className='px-6 pt-6'>
          <SheetTitle>{website ? '编辑网站' : '创建网站'}</SheetTitle>
          <SheetDescription>
            配置会通过后台任务安全分发到目标集群。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='website-form'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <Tabs defaultValue='basic' className='gap-0'>
              <div className='overflow-x-auto border-b px-6 pb-3'>
                <TabsList>
                  <TabsTrigger value='basic'>基础</TabsTrigger>
                  <TabsTrigger value='domains'>域名</TabsTrigger>
                  <TabsTrigger value='origins'>源站</TabsTrigger>
                  <TabsTrigger value='features'>功能</TabsTrigger>
                  <TabsTrigger value='routes'>路由规则</TabsTrigger>
                </TabsList>
              </div>
              <ScrollArea className='h-[58svh] px-6'>
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
                  <div className='grid gap-4 sm:grid-cols-2'>
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
                  <div className='grid gap-4 sm:grid-cols-2'>
                    <FormField
                      control={form.control}
                      name='default_origin_group'
                      render={({ field }) => (
                        <FormItem>
                          <FormLabel>默认源站组</FormLabel>
                          <FormControl>
                            <Input placeholder='default' {...field} />
                          </FormControl>
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
                          <FormControl>
                            <Input placeholder='$host' {...field} />
                          </FormControl>
                          <FormMessage />
                        </FormItem>
                      )}
                    />
                  </div>
                </TabsContent>
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
                      className='grid gap-2 rounded-md border p-3 sm:grid-cols-[1fr_170px_auto]'
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
                <TabsContent value='origins' className='space-y-3 py-4'>
                  <div className='flex items-center justify-between'>
                    <div>
                      <h3 className='font-medium'>源站</h3>
                      <p className='text-sm text-muted-foreground'>
                        每个启用的源站组至少需要一个主源站。
                      </p>
                    </div>
                    <Button
                      type='button'
                      variant='outline'
                      size='sm'
                      onClick={() =>
                        origins.append({
                          id: crypto.randomUUID(),
                          group: form.getValues('default_origin_group'),
                          protocol: 'http',
                          host: '',
                          port: 80,
                          role: 'primary',
                          weight: 100,
                          status: 'enabled',
                        })
                      }
                    >
                      <Plus /> 添加源站
                    </Button>
                  </div>
                  {origins.fields.map((origin, index) => (
                    <div key={origin.formKey} className='rounded-md border p-3'>
                      <div className='mb-3 flex items-center justify-between'>
                        <span className='text-sm font-medium'>
                          源站 {index + 1}
                        </span>
                        <Button
                          type='button'
                          variant='ghost'
                          size='icon'
                          aria-label='移除源站'
                          disabled={origins.fields.length === 1}
                          onClick={() => origins.remove(index)}
                        >
                          <X />
                        </Button>
                      </div>
                      <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-4'>
                        <FormField
                          control={form.control}
                          name={`origins.${index}.group`}
                          render={({ field }) => (
                            <FormItem>
                              <FormLabel>源站组</FormLabel>
                              <FormControl>
                                <Input {...field} />
                              </FormControl>
                              <FormMessage />
                            </FormItem>
                          )}
                        />
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
                                  <SelectItem value='http'>HTTP</SelectItem>
                                  <SelectItem value='https'>HTTPS</SelectItem>
                                </SelectContent>
                              </Select>
                            </FormItem>
                          )}
                        />
                        <FormField
                          control={form.control}
                          name={`origins.${index}.host`}
                          render={({ field }) => (
                            <FormItem className='lg:col-span-2'>
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
                                  <SelectItem value='backup'>备源站</SelectItem>
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
                                  <SelectItem value='enabled'>启用</SelectItem>
                                  <SelectItem value='disabled'>停用</SelectItem>
                                </SelectContent>
                              </Select>
                            </FormItem>
                          )}
                        />
                      </div>
                    </div>
                  ))}
                </TabsContent>
                <TabsContent value='features' className='space-y-3 py-4'>
                  {[
                    {
                      name: 'health_check_enabled' as const,
                      label: '源站健康检查',
                      description: '周期检查源站状态并参与回源选择。',
                    },
                    {
                      name: 'access_log_enabled' as const,
                      label: '访问日志',
                      description: '采集请求结果并提供实时日志流。',
                    },
                    {
                      name: 'response_compression_enabled' as const,
                      label: '响应压缩',
                      description: '按 MIME 类型启用 Brotli/Gzip 压缩。',
                    },
                    {
                      name: 'https_enabled' as const,
                      label: 'HTTPS',
                      description: '启用 TLS、HTTP/2 与证书分发。',
                    },
                  ].map((item) => (
                    <FormField
                      key={item.name}
                      control={form.control}
                      name={item.name}
                      render={({ field }) => (
                        <FormItem className='flex items-center justify-between gap-4 rounded-md border p-4'>
                          <div>
                            <FormLabel>{item.label}</FormLabel>
                            <FormDescription>
                              {item.description}
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
                  ))}
                  {httpsEnabled && (
                    <FormField
                      control={form.control}
                      name='certificate_ids'
                      render={({ field }) => (
                        <FormItem className='rounded-md border p-4'>
                          <FormLabel>绑定证书</FormLabel>
                          <FormDescription>
                            可绑定最多 20 张当前可用的证书。
                          </FormDescription>
                          <div className='mt-3 grid gap-2 sm:grid-cols-2'>
                            {certificatesQuery.data?.map((certificate) => {
                              const checked = field.value.includes(
                                certificate.id
                              )
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
                  )}
                </TabsContent>
                <TabsContent value='routes' className='space-y-3 py-4'>
                  <FormField
                    control={form.control}
                    name='route_rules_json'
                    render={({ field }) => (
                      <FormItem>
                        <FormLabel>路由规则 JSON</FormLabel>
                        <FormDescription>
                          高级规则完整对应后端 route_rules 数组；保存时会校验
                          JSON。
                        </FormDescription>
                        <FormControl>
                          <Textarea
                            className='min-h-80 font-mono text-xs'
                            spellCheck={false}
                            {...field}
                          />
                        </FormControl>
                        <FormMessage />
                      </FormItem>
                    )}
                  />
                </TabsContent>
              </ScrollArea>
            </Tabs>
          </form>
        </Form>
        <SheetFooter className='px-6 py-4'>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            type='submit'
            form='website-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在保存…' : '保存并分发'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}

function WebsiteDetailSheet({
  website,
  onOpenChange,
}: {
  website: Website | null
  onOpenChange: (open: boolean) => void
}) {
  return (
    <Sheet open={!!website} onOpenChange={onOpenChange}>
      <SheetContent className='w-full overflow-y-auto sm:max-w-2xl'>
        <SheetHeader className='text-start'>
          <SheetTitle>{website?.config.name}</SheetTitle>
          <SheetDescription>
            {website?.cluster_name} · {website?.access_domain}
          </SheetDescription>
        </SheetHeader>
        {website && (
          <div className='space-y-6 px-4 pb-6'>
            <div className='flex flex-wrap gap-2'>
              <StatusBadge status={website.status} />
              <StatusBadge status={website.runtime.deploy_status} />
              <Badge variant='outline'>
                {website.runtime.synced_node_count}/
                {website.runtime.target_node_count} 节点已同步
              </Badge>
            </div>
            <div>
              <h3 className='mb-2 text-sm font-semibold'>绑定域名</h3>
              <div className='space-y-2'>
                {website.config.domains.map((domain) => {
                  const runtime = website.runtime.domain_states.find(
                    (item) => item.id === domain.id
                  )
                  return (
                    <div
                      key={domain.id}
                      className='flex items-center justify-between rounded-md border p-3'
                    >
                      <div>
                        <div className='text-sm font-medium'>
                          {domain.hostname}
                        </div>
                        <div className='text-xs text-muted-foreground'>
                          {domain.dns_mode === 'managed'
                            ? '托管解析'
                            : '外部解析'}
                        </div>
                      </div>
                      <StatusBadge status={runtime?.resolution_status} />
                    </div>
                  )
                })}
              </div>
            </div>
            <div>
              <h3 className='mb-2 text-sm font-semibold'>源站</h3>
              <div className='space-y-2'>
                {website.config.origins.map((origin) => {
                  const states = website.runtime.origin_states.filter(
                    (item) => item.origin_id === origin.id
                  )
                  const healthy = states.filter(
                    (item) => item.status === 'healthy'
                  ).length
                  return (
                    <div key={origin.id} className='rounded-md border p-3'>
                      <div className='flex items-center justify-between'>
                        <div className='font-mono text-sm'>
                          {origin.protocol}://{origin.host}:{origin.port}
                        </div>
                        <Badge variant='outline'>
                          {healthy}/{states.length} 健康
                        </Badge>
                      </div>
                      <div className='mt-1 text-xs text-muted-foreground'>
                        {origin.group} · {origin.role} · 权重 {origin.weight}
                      </div>
                    </div>
                  )
                })}
              </div>
            </div>
          </div>
        )}
      </SheetContent>
    </Sheet>
  )
}

function AccessLogSheet({
  website,
  onOpenChange,
}: {
  website: Website
  onOpenChange: (open: boolean) => void
}) {
  const [logs, setLogs] = useState<AccessLog[]>([])
  const [connected, setConnected] = useState(false)

  useEffect(() => {
    const source = new EventSource(
      `/api/websites/${website.id}/access-logs/stream?limit=100`,
      { withCredentials: true }
    )
    source.addEventListener('ready', () => setConnected(true))
    source.addEventListener('logs', (event) => {
      const payload = JSON.parse(
        (event as MessageEvent<string>).data
      ) as ApiEnvelope<{
        list: AccessLog[]
      }>
      setLogs((current) => {
        const merged = [...payload.data.list, ...current]
        return Array.from(
          new Map(merged.map((item) => [item.id, item])).values()
        )
          .sort((a, b) => b.occurred_at.localeCompare(a.occurred_at))
          .slice(0, 200)
      })
    })
    source.onerror = () => setConnected(false)
    return () => source.close()
  }, [website])

  return (
    <Sheet open onOpenChange={onOpenChange}>
      <SheetContent className='flex w-full flex-col sm:max-w-4xl'>
        <SheetHeader className='text-start'>
          <div className='flex items-center gap-2'>
            <SheetTitle>{website?.config.name} · 访问日志</SheetTitle>
            <Badge variant='outline'>{connected ? '已连接' : '连接中'}</Badge>
          </div>
          <SheetDescription>实时显示最近 200 条请求。</SheetDescription>
        </SheetHeader>
        <ScrollArea className='min-h-0 flex-1 px-4'>
          <div className='space-y-2 pb-5 font-mono text-xs'>
            {logs.map((log) => (
              <div key={log.id} className='rounded-md border p-3'>
                <div className='flex flex-wrap items-center gap-2'>
                  <Badge
                    variant={
                      log.status_code >= 500
                        ? 'destructive'
                        : log.status_code >= 400
                          ? 'secondary'
                          : 'outline'
                    }
                  >
                    {log.status_code}
                  </Badge>
                  <span className='font-semibold'>{log.method}</span>
                  <span className='min-w-0 flex-1 truncate'>
                    {log.host}
                    {log.target}
                  </span>
                  <span className='text-muted-foreground'>
                    {log.duration_ms}ms
                  </span>
                </div>
                <div className='mt-2 flex flex-wrap gap-x-4 text-muted-foreground'>
                  <span>{formatDate(log.occurred_at)}</span>
                  <span>{log.node_name}</span>
                  <span>{log.client_ip || '未知 IP'}</span>
                  <span>{log.response_bytes} B</span>
                </div>
              </div>
            ))}
            {!logs.length && (
              <p className='py-16 text-center text-muted-foreground'>
                等待访问日志…
              </p>
            )}
          </div>
        </ScrollArea>
      </SheetContent>
    </Sheet>
  )
}
