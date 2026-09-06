import { type ReactNode, useEffect, useMemo, useState } from 'react'
import { z } from 'zod'
import { type Resolver, useFieldArray, useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import {
  Activity,
  FilePenLine,
  FileText,
  Globe2,
  Network,
  Plus,
  Server,
  ShieldCheck,
  Trash2,
  X,
  Eye,
  type LucideIcon,
} from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData, type ApiEnvelope } from '@/lib/api'
import { formatBytesPerSecond, formatDate } from '@/lib/format'
import type {
  Certificate,
  Cluster,
  PageData,
  Website,
  WebsiteConfig,
  WebsiteDashboard,
  WebsiteDashboardRanking,
  WebsiteDashboardSeriesPoint,
} from '@/lib/types'
import { useResourceFilters } from '@/hooks/use-resource-filters'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card'
import {
  Collapsible,
  CollapsibleContent,
  CollapsibleTrigger,
} from '@/components/ui/collapsible'
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
import { DataTableCellContent } from '@/components/data-table/cell-content'
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

const routeMethods = [
  'GET',
  'HEAD',
  'POST',
  'PUT',
  'PATCH',
  'DELETE',
  'OPTIONS',
] as const

function routeHeadersToText(value: unknown) {
  if (!Array.isArray(value)) return ''
  return value
    .flatMap((header) => {
      if (!header || typeof header !== 'object') return []
      const record = header as Record<string, unknown>
      if (typeof record.name !== 'string' || typeof record.value !== 'string') {
        return []
      }
      return [`${record.name}: ${record.value}`]
    })
    .join('\n')
}

function parseRouteHeaders(value: string) {
  return value
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const separator = line.indexOf(':')
      return {
        name: line.slice(0, separator).trim(),
        value: line.slice(separator + 1).trim(),
      }
    })
}

function valuesToLines(values: string[]) {
  return values.join('\n')
}

function linesToValues(value: string) {
  return value
    .split(/[\r\n,]+/)
    .map((item) => item.trim())
    .filter(Boolean)
}

function originGroupLabel(value: string) {
  return value === 'default' ? '默认' : value
}

function hasValidRouteHeaderText(value: string) {
  return value.split(/\r?\n/).every((line) => {
    const trimmed = line.trim()
    return !trimmed || trimmed.indexOf(':') > 0
  })
}

const routeRuleSchema = z
  .object({
    id: z.string().uuid(),
    status: z.enum(['enabled', 'disabled']),
    match_type: z.enum(['exact', 'prefix']),
    path: z.string().trim().min(1, '请输入匹配路径').max(2048),
    methods: z.array(z.enum(routeMethods)).max(routeMethods.length),
    action: z.enum(['proxy', 'redirect']),
    rewrite_path: z.string().trim().max(2048),
    redirect_url: z.string().trim().max(2048),
    redirect_status: z.number().int().min(0).max(302),
    origin_group: z.string().trim().max(100),
    request_headers_text: z
      .string()
      .refine(hasValidRouteHeaderText, '每行请填写为 Header: value'),
    response_headers_text: z
      .string()
      .refine(hasValidRouteHeaderText, '每行请填写为 Header: value'),
  })
  .superRefine((rule, context) => {
    if (!rule.path.startsWith('/')) {
      context.addIssue({
        code: 'custom',
        path: ['path'],
        message: '匹配路径必须以 / 开头',
      })
    }
    if (rule.action === 'proxy') {
      if (!rule.origin_group) {
        context.addIssue({
          code: 'custom',
          path: ['origin_group'],
          message: '请选择源站组',
        })
      }
      if (rule.rewrite_path && !rule.rewrite_path.startsWith('/')) {
        context.addIssue({
          code: 'custom',
          path: ['rewrite_path'],
          message: '重写路径必须以 / 开头',
        })
      }
      return
    }
    if (!rule.redirect_url || !/^(\/|https?:\/\/)/.test(rule.redirect_url)) {
      context.addIssue({
        code: 'custom',
        path: ['redirect_url'],
        message: '跳转地址需以 /、http:// 或 https:// 开头',
      })
    }
    if (rule.redirect_status !== 301 && rule.redirect_status !== 302) {
      context.addIssue({
        code: 'custom',
        path: ['redirect_status'],
        message: '请选择 301 或 302',
      })
    }
  })

const schema = z.object({
  cluster_id: z.string().uuid('请选择所属集群'),
  status: z.enum(['enabled', 'disabled']),
  name: z.string().trim().min(1, '请输入网站名称').max(100),
  domains: z.array(domainSchema).min(1, '至少添加一个域名').max(100),
  origins: z.array(originSchema).min(1, '至少添加一个源站').max(100),
  default_origin_group: z.string().trim().min(1, '请选择默认源站组').max(100),
  origin_host_header: z.string().trim().min(1, '请输入回源 Host').max(253),
  pass_client_ip: z.boolean(),
  health_check_enabled: z.boolean(),
  health_check_path: z.string().trim().min(1).max(2048),
  health_check_interval_seconds: z.number().int().min(1).max(3600),
  health_check_timeout_seconds: z.number().int().min(1).max(300),
  health_check_expected_status: z.number().int().min(100).max(599),
  healthy_threshold: z.number().int().min(1).max(10),
  unhealthy_threshold: z.number().int().min(1).max(10),
  access_log_enabled: z.boolean(),
  access_log_request_headers: z.boolean(),
  access_log_request_body: z.boolean(),
  access_log_response_headers: z.boolean(),
  access_log_query_params: z.boolean(),
  access_log_cookies: z.boolean(),
  access_log_referer: z.boolean(),
  access_log_user_agent: z.boolean(),
  access_log_status_code_ranges: z.array(
    z.enum(['1xx', '2xx', '3xx', '4xx', '5xx'])
  ),
  access_log_client_abort: z.boolean(),
  https_enabled: z.boolean(),
  certificate_ids: z.array(z.string().uuid()).max(20),
  minimum_tls_version: z.enum(['1.2', '1.3']),
  force_https: z.boolean(),
  http2_enabled: z.boolean(),
  hsts_enabled: z.boolean(),
  response_compression_enabled: z.boolean(),
  response_compression_min_bytes: z.number().int().min(256).max(1048576),
  response_compression_max_bytes: z.number().int().min(0).max(67108864),
  response_compression_algorithms: z.array(z.enum(['br', 'zstd', 'gzip'])),
  response_compression_mime_types: z.array(z.string().trim()),
  response_compression_extensions: z.array(z.string().trim()),
  response_compression_excluded_extensions: z.array(z.string().trim()),
  route_rules: z.array(routeRuleSchema).max(100),
})

type Values = z.infer<typeof schema>
type RouteRuleForm = z.infer<typeof routeRuleSchema>

function routeRuleToForm(rule: Record<string, unknown>): RouteRuleForm {
  const methods = Array.isArray(rule.methods)
    ? rule.methods.filter((method): method is (typeof routeMethods)[number] =>
        routeMethods.includes(method as (typeof routeMethods)[number])
      )
    : []
  const action = rule.action === 'redirect' ? 'redirect' : 'proxy'

  return {
    id: typeof rule.id === 'string' ? rule.id : crypto.randomUUID(),
    status: rule.status === 'disabled' ? 'disabled' : 'enabled',
    match_type: rule.match_type === 'exact' ? 'exact' : 'prefix',
    path: typeof rule.path === 'string' ? rule.path : '/',
    methods,
    action,
    rewrite_path:
      typeof rule.rewrite_path === 'string' ? rule.rewrite_path : '',
    redirect_url:
      typeof rule.redirect_url === 'string' ? rule.redirect_url : '',
    redirect_status:
      typeof rule.redirect_status === 'number'
        ? rule.redirect_status
        : action === 'redirect'
          ? 302
          : 0,
    origin_group:
      typeof rule.origin_group === 'string' ? rule.origin_group : '',
    request_headers_text: routeHeadersToText(rule.request_headers),
    response_headers_text: routeHeadersToText(rule.response_headers),
  }
}
type AccessLog = {
  id: string
  occurred_at: string
  node_id: string
  node_name: string
  client_ip?: string
  client_ip_location?: string
  protocol: string
  method: string
  host: string
  target: string
  status_code: number
  request_bytes: number
  response_bytes: number
  duration_ms: number
  user_agent?: string
  referer?: string
}

function accessLogStatusLabel(status: number) {
  if (status >= 500) return '服务器错误'
  if (status >= 400) return '客户端错误'
  if (status >= 300) return '重定向'
  if (status >= 200) return '成功'
  return '信息响应'
}

function accessLogStatusClass(status: number) {
  if (status >= 500)
    return 'border-destructive/30 bg-destructive/10 text-destructive'
  if (status >= 400)
    return 'border-amber-500/30 bg-amber-500/10 text-amber-700 dark:text-amber-400'
  if (status >= 300)
    return 'border-sky-500/30 bg-sky-500/10 text-sky-700 dark:text-sky-400'
  return 'border-emerald-500/30 bg-emerald-500/10 text-emerald-700 dark:text-emerald-400'
}

function formatAccessLogBytes(value?: number) {
  if (value === undefined || !Number.isFinite(value)) return '—'
  return `${new Intl.NumberFormat('en-US').format(Math.max(0, value))} B`
}

function accessLogUrlScheme(protocol: string) {
  const normalized = protocol.trim().toLowerCase()
  return normalized === 'https' || normalized === 'h2' || normalized === 'h3'
    ? 'https'
    : 'http'
}

function accessLogHttpVersion(protocol: string) {
  const normalized = protocol.trim().toLowerCase()
  if (normalized === 'h2') return 'HTTP/2'
  if (normalized === 'h3') return 'HTTP/3'
  if (
    normalized === 'http' ||
    normalized === 'https' ||
    normalized === 'http/1.1' ||
    normalized === 'https/1.1'
  ) {
    return 'HTTP/1.1'
  }
  if (normalized === 'http/1.0' || normalized === 'https/1.0') return 'HTTP/1.0'
  return normalized.startsWith('http/') ? normalized.toUpperCase() : '未知协议'
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
  const filter = useResourceFilters({
    keyword: '',
    clusterId: 'all',
    status: 'all',
  })
  const { keyword, clusterId, status } = filter.filters
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
          <DataTableCellContent>
            <div className='font-medium'>
              {row.original.config.name ||
                row.original.config.domains[0]?.hostname}
            </div>
            <div className='max-w-64 truncate text-xs text-muted-foreground'>
              {row.original.config.domains
                .map((item) => item.hostname)
                .join('、')}
            </div>
          </DataTableCellContent>
        ),
      },
      {
        accessorKey: 'cluster_name',
        header: '所属集群',
        cell: ({ row }) => (
          <DataTableCellContent>
            <div>{row.original.cluster_name}</div>
            <code className='text-xs text-muted-foreground'>
              {row.original.access_domain}
            </code>
          </DataTableCellContent>
        ),
      },
      {
        id: 'origin',
        header: '回源',
        cell: ({ row }) => {
          const origins = row.original.config.origins
          const primary =
            origins.find(
              (origin) =>
                origin.role === 'primary' && origin.status === 'enabled'
            ) ?? origins[0]
          if (!primary) return '未配置'
          return (
            <DataTableCellContent>
              <div className='font-medium'>
                {originGroupLabel(primary.group)}
              </div>
              <code
                className='max-w-56 truncate text-xs text-muted-foreground'
                title={`${primary.protocol}://${primary.host}:${primary.port}`}
              >
                {primary.protocol}://{primary.host}:{primary.port}
              </code>
              {origins.length > 1 && (
                <span className='text-xs text-muted-foreground'>
                  共 {origins.length} 个源站
                </span>
              )}
            </DataTableCellContent>
          )
        },
      },
      {
        id: 'deploy',
        header: '节点发布',
        cell: ({ row }) => (
          <DataTableCellContent>
            <StatusBadge status={row.original.runtime.deploy_status} />
            <div className='text-xs text-muted-foreground'>
              {row.original.runtime.synced_node_count}/
              {row.original.runtime.target_node_count} 节点
            </div>
          </DataTableCellContent>
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
        value={filter.draft.keyword}
        onChange={(value) => filter.setField('keyword', value)}
        onSearch={() => {
          const changed = filter.apply()
          setPage(1)
          if (!changed && page === 1) void query.refetch()
        }}
        onReset={() => {
          const changed = filter.reset()
          setPage(1)
          if (!changed && page === 1) void query.refetch()
        }}
        refreshing={query.isFetching}
        placeholder='搜索网站或域名…'
        filters={
          <>
            <Select
              value={filter.draft.clusterId}
              onValueChange={(value) => filter.setField('clusterId', value)}
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
              value={filter.draft.status}
              onValueChange={(value) => filter.setField('status', value)}
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
    resolver: zodResolver(schema) as Resolver<Values>,
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
      pass_client_ip: config.pass_client_ip,
      health_check_enabled: config.health_check_enabled,
      health_check_path: config.health_check_path,
      health_check_interval_seconds: config.health_check_interval_seconds,
      health_check_timeout_seconds: config.health_check_timeout_seconds,
      health_check_expected_status: config.health_check_expected_status,
      healthy_threshold: config.healthy_threshold,
      unhealthy_threshold: config.unhealthy_threshold,
      access_log_enabled: config.access_log_enabled,
      access_log_request_headers: config.access_log_request_headers,
      access_log_request_body: config.access_log_request_body,
      access_log_response_headers: config.access_log_response_headers,
      access_log_query_params: config.access_log_query_params,
      access_log_cookies: config.access_log_cookies,
      access_log_referer: config.access_log_referer,
      access_log_user_agent: config.access_log_user_agent,
      access_log_status_code_ranges:
        config.access_log_status_code_ranges.filter(
          (range): range is Values['access_log_status_code_ranges'][number] =>
            ['1xx', '2xx', '3xx', '4xx', '5xx'].includes(range)
        ),
      access_log_client_abort: config.access_log_client_abort,
      https_enabled: config.https_enabled,
      certificate_ids: config.certificate_ids,
      minimum_tls_version: config.minimum_tls_version,
      force_https: config.force_https,
      http2_enabled: config.http2_enabled,
      hsts_enabled: config.hsts_enabled,
      response_compression_enabled: config.response_compression_enabled,
      response_compression_min_bytes: config.response_compression_min_bytes,
      response_compression_max_bytes: config.response_compression_max_bytes,
      response_compression_algorithms:
        config.response_compression_algorithms.filter(
          (
            algorithm
          ): algorithm is Values['response_compression_algorithms'][number] =>
            ['br', 'zstd', 'gzip'].includes(algorithm)
        ),
      response_compression_mime_types: config.response_compression_mime_types,
      response_compression_extensions: config.response_compression_extensions,
      response_compression_excluded_extensions:
        config.response_compression_excluded_extensions,
      route_rules: config.route_rules.map(routeRuleToForm),
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
  const routeRules = useFieldArray({
    control: form.control,
    name: 'route_rules',
    keyName: 'formKey',
  })
  const watchedOrigins = form.watch('origins')
  const originGroups = useMemo(() => {
    const groups = new Map<
      string,
      {
        name: string
        indexes: number[]
        enabledCount: number
        enabledPrimaryCount: number
      }
    >()

    watchedOrigins.forEach((origin, index) => {
      const name = origin.group.trim()
      if (!name) return

      const group = groups.get(name) ?? {
        name,
        indexes: [],
        enabledCount: 0,
        enabledPrimaryCount: 0,
      }
      group.indexes.push(index)
      if (origin.status === 'enabled') {
        group.enabledCount += 1
        if (origin.role === 'primary') group.enabledPrimaryCount += 1
      }
      groups.set(name, group)
    })

    return Array.from(groups.values())
  }, [watchedOrigins])
  const selectableOriginGroups = useMemo(
    () => originGroups.filter((group) => group.enabledCount > 0),
    [originGroups]
  )
  const appendOrigin = (
    group: string,
    role: Values['origins'][number]['role']
  ) => {
    origins.append({
      id: crypto.randomUUID(),
      group,
      protocol: 'http',
      host: '',
      port: 80,
      role,
      weight: 100,
      status: 'enabled',
    })
  }
  const createOriginGroup = () => {
    let suffix = originGroups.length + 1
    let name = `源站组 ${suffix}`
    while (originGroups.some((group) => group.name === name)) {
      suffix += 1
      name = `源站组 ${suffix}`
    }
    appendOrigin(name, 'primary')
  }
  const renameOriginGroup = (currentName: string, draftName: string) => {
    const nextName = draftName.trim()
    const hasControlCharacter = Array.from(nextName).some((character) => {
      const code = character.charCodeAt(0)
      return code < 32 || code === 127
    })
    if (nextName === currentName) return true
    if (!nextName || nextName.length > 100 || hasControlCharacter) {
      toast.error('源站组名称需为 1–100 个非控制字符')
      return false
    }
    if (originGroups.some((group) => group.name === nextName)) {
      toast.error('源站组名称不能重复')
      return false
    }

    form.getValues('origins').forEach((origin, index) => {
      if (origin.group.trim() === currentName) {
        form.setValue(`origins.${index}.group`, nextName, {
          shouldDirty: true,
          shouldValidate: true,
        })
      }
    })
    if (form.getValues('default_origin_group').trim() === currentName) {
      form.setValue('default_origin_group', nextName, {
        shouldDirty: true,
        shouldValidate: true,
      })
    }
    const rules = form.getValues('route_rules')
    const nextRules = rules.map((rule) =>
      rule.origin_group === currentName
        ? { ...rule, origin_group: nextName }
        : rule
    )
    if (nextRules.some((rule, index) => rule !== rules[index])) {
      form.setValue('route_rules', nextRules, {
        shouldDirty: true,
        shouldValidate: true,
      })
    }
    return true
  }
  const isOriginGroupUsedByRoute = (groupName: string) => {
    return form
      .getValues('route_rules')
      .some(
        (rule) => rule.action === 'proxy' && rule.origin_group === groupName
      )
  }
  const removeOrigin = (index: number) => {
    const currentOrigins = form.getValues('origins')
    const origin = currentOrigins[index]
    if (!origin) return

    const groupName = origin.group.trim()
    const removesGroup = !currentOrigins.some(
      (item, itemIndex) =>
        itemIndex !== index && item.group.trim() === groupName
    )
    if (
      removesGroup &&
      form.getValues('default_origin_group').trim() === groupName
    ) {
      toast.error('请先在“基础”页切换默认源站组，再删除该组')
      return
    }
    if (removesGroup && isOriginGroupUsedByRoute(groupName)) {
      toast.error('该源站组仍被代理路由引用，请先修改路由规则')
      return
    }
    origins.remove(index)
  }
  const httpsEnabled = form.watch('https_enabled')
  const healthCheckEnabled = form.watch('health_check_enabled')
  const accessLogEnabled = form.watch('access_log_enabled')
  const compressionEnabled = form.watch('response_compression_enabled')
  const mutation = useMutation({
    mutationFn: (values: Values) => {
      const bodyConfig: WebsiteConfig = {
        ...config,
        name: values.name,
        domains: values.domains,
        origins: values.origins,
        default_origin_group: values.default_origin_group,
        origin_host_header: values.origin_host_header,
        pass_client_ip: values.pass_client_ip,
        health_check_enabled: values.health_check_enabled,
        health_check_path: values.health_check_path,
        health_check_interval_seconds: values.health_check_interval_seconds,
        health_check_timeout_seconds: values.health_check_timeout_seconds,
        health_check_expected_status: values.health_check_expected_status,
        healthy_threshold: values.healthy_threshold,
        unhealthy_threshold: values.unhealthy_threshold,
        access_log_enabled: values.access_log_enabled,
        access_log_request_headers: values.access_log_request_headers,
        access_log_request_body: values.access_log_request_body,
        access_log_response_headers: values.access_log_response_headers,
        access_log_query_params: values.access_log_query_params,
        access_log_cookies: values.access_log_cookies,
        access_log_referer: values.access_log_referer,
        access_log_user_agent: values.access_log_user_agent,
        access_log_status_code_ranges: values.access_log_status_code_ranges,
        access_log_client_abort: values.access_log_client_abort,
        https_enabled: values.https_enabled,
        certificate_ids: values.certificate_ids,
        minimum_tls_version: values.minimum_tls_version,
        force_https: values.force_https,
        http2_enabled: values.http2_enabled,
        hsts_enabled: values.hsts_enabled,
        response_compression_enabled: values.response_compression_enabled,
        response_compression_min_bytes: values.response_compression_min_bytes,
        response_compression_max_bytes: values.response_compression_max_bytes,
        response_compression_algorithms: values.response_compression_algorithms,
        response_compression_mime_types: values.response_compression_mime_types,
        response_compression_extensions: values.response_compression_extensions,
        response_compression_excluded_extensions:
          values.response_compression_excluded_extensions,
        route_rules: values.route_rules.map(
          ({ request_headers_text, response_headers_text, ...rule }) => ({
            ...rule,
            request_headers: parseRouteHeaders(request_headers_text),
            response_headers: parseRouteHeaders(response_headers_text),
          })
        ),
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
                            <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-[110px_minmax(0,1fr)_92px_110px_96px_110px]'>
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
                <TabsContent value='features' className='space-y-3 py-4'>
                  <section className='rounded-lg border'>
                    <div className='flex items-center justify-between gap-4 p-4'>
                      <div>
                        <h3 className='font-medium'>回源与健康检查</h3>
                        <p className='text-sm text-muted-foreground'>
                          设置源站探测，以及是否将访客真实 IP 传给源站。
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
                    <div className='border-t p-4'>
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
                            name='health_check_timeout_seconds'
                            render={({ field }) => (
                              <FormItem>
                                <FormLabel>超时（秒）</FormLabel>
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
                            name='health_check_expected_status'
                            render={({ field }) => (
                              <FormItem>
                                <FormLabel>期望状态码</FormLabel>
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
                            name='healthy_threshold'
                            render={({ field }) => (
                              <FormItem>
                                <FormLabel>恢复阈值</FormLabel>
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
                                      field.onChange(
                                        event.currentTarget.valueAsNumber
                                      )
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
                                {(
                                  ['1xx', '2xx', '3xx', '4xx', '5xx'] as const
                                ).map((range) => {
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
                                })}
                              </div>
                              <FormMessage />
                            </FormItem>
                          )}
                        />
                        <div className='grid gap-2 sm:grid-cols-2'>
                          {[
                            ['access_log_query_params', '查询参数'],
                            ['access_log_referer', 'Referer'],
                            ['access_log_user_agent', 'User-Agent'],
                            ['access_log_client_abort', '客户端中断'],
                            ['access_log_request_headers', '请求头'],
                            ['access_log_response_headers', '响应头'],
                            ['access_log_cookies', 'Cookie（可能含敏感信息）'],
                            [
                              'access_log_request_body',
                              '请求体（可能含敏感信息）',
                            ],
                          ].map(([name, label]) => (
                            <FormField
                              key={name}
                              control={form.control}
                              name={name as keyof Values}
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
                      </div>
                    )}
                  </section>

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
                              <FormItem>
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
                          <div className='grid gap-2 sm:grid-cols-3'>
                            {[
                              ['force_https', '强制 HTTPS'],
                              ['http2_enabled', 'HTTP/2'],
                              ['hsts_enabled', 'HSTS'],
                            ].map(([name, label]) => (
                              <FormField
                                key={name}
                                control={form.control}
                                name={name as keyof Values}
                                render={({ field }) => (
                                  <FormItem className='flex items-center justify-between rounded-md border px-3 py-2'>
                                    <FormLabel className='text-sm'>
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
                      </div>
                    )}
                  </section>

                  <section className='rounded-lg border'>
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
                      <div className='space-y-4 border-t p-4'>
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
                            name='response_compression_max_bytes'
                            render={({ field }) => (
                              <FormItem>
                                <FormLabel>最大响应大小（字节）</FormLabel>
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
                                <FormDescription>
                                  填 0 表示不设上限
                                </FormDescription>
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
                              <div className='flex flex-wrap gap-2'>
                                {(['br', 'zstd', 'gzip'] as const).map(
                                  (algorithm) => {
                                    const checked =
                                      field.value.includes(algorithm)
                                    return (
                                      <label
                                        key={algorithm}
                                        className='flex cursor-pointer items-center gap-2 rounded-md border px-3 py-2 text-sm'
                                      >
                                        <Checkbox
                                          checked={checked}
                                          onCheckedChange={(next) =>
                                            field.onChange(
                                              next
                                                ? [...field.value, algorithm]
                                                : field.value.filter(
                                                    (item) => item !== algorithm
                                                  )
                                            )
                                          }
                                        />
                                        {algorithm}
                                      </label>
                                    )
                                  }
                                )}
                              </div>
                            </FormItem>
                          )}
                        />
                        <div className='grid gap-3 sm:grid-cols-3'>
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
                              name={name as keyof Values}
                              render={({ field }) => (
                                <FormItem>
                                  <FormLabel>{label}</FormLabel>
                                  <FormControl>
                                    <Textarea
                                      className='min-h-28 font-mono text-xs'
                                      placeholder={placeholder}
                                      value={valuesToLines(
                                        field.value as string[]
                                      )}
                                      onChange={(event) =>
                                        field.onChange(
                                          linesToValues(
                                            event.currentTarget.value
                                          )
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
                </TabsContent>
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

function WebsiteDashboardMetric({
  title,
  value,
  description,
  icon: Icon,
}: {
  title: string
  value: ReactNode
  description: string
  icon: LucideIcon
}) {
  return (
    <div className='min-w-0 border-b p-4 last:border-b-0 sm:border-r sm:last:border-r-0 xl:border-b-0'>
      <div className='flex items-center justify-between gap-2 text-xs text-muted-foreground'>
        <span className='truncate'>{title}</span>
        <Icon className='size-4 shrink-0' aria-hidden='true' />
      </div>
      <div className='mt-3 min-h-6 text-lg font-semibold tabular-nums'>
        {value}
      </div>
      <p className='mt-1 truncate text-xs text-muted-foreground' title={description}>
        {description}
      </p>
    </div>
  )
}

function formatDashboardBytes(value?: number) {
  if (value === undefined || !Number.isFinite(value)) return '—'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let amount = Math.max(0, value)
  let index = 0
  while (amount >= 1024 && index < units.length - 1) {
    amount /= 1024
    index += 1
  }
  return `${amount.toFixed(index === 0 ? 0 : 1)} ${units[index]}`
}

function WebsiteDashboardTrend({
  title,
  description,
  data,
  value,
  format,
}: {
  title: string
  description: string
  data: WebsiteDashboardSeriesPoint[]
  value: (item: WebsiteDashboardSeriesPoint) => number
  format: (value: number) => string
}) {
  const maximum = Math.max(1, ...data.map(value))
  const hasData = data.some((item) => value(item) > 0)

  return (
    <Card>
      <CardHeader className='border-b'>
        <CardTitle>{title}</CardTitle>
        <CardDescription>{description}</CardDescription>
      </CardHeader>
      <CardContent>
        {hasData ? (
          <div className='grid gap-3'>
            <div className='flex h-40 items-end gap-1' aria-label={title}>
              {data.map((item) => {
                const amount = value(item)
                const label = item.timestamp.slice(5, 16).replace('T', ' ')
                return (
                  <div
                    key={item.timestamp}
                    className='group flex h-full min-w-0 flex-1 items-end'
                    title={`${label}：${format(amount)}`}
                  >
                    <div
                      className='w-full rounded-t-sm bg-primary/80 transition-colors group-hover:bg-primary'
                      style={{
                        height: `${Math.max(3, (amount / maximum) * 100)}%`,
                      }}
                    />
                    <span className='sr-only'>
                      {label}：{format(amount)}
                    </span>
                  </div>
                )
              })}
            </div>
            <div className='flex justify-between text-xs text-muted-foreground'>
              <span>{data[0]?.timestamp.slice(5, 10) ?? '—'}</span>
              <span>{data.at(-1)?.timestamp.slice(5, 10) ?? '—'}</span>
            </div>
          </div>
        ) : (
          <p className='py-14 text-center text-sm text-muted-foreground'>
            当前时段暂无访问数据
          </p>
        )}
      </CardContent>
    </Card>
  )
}

function WebsiteDashboardRankingList({
  title,
  description,
  data,
  value,
  format,
}: {
  title: string
  description: string
  data: WebsiteDashboardRanking[]
  value: (item: WebsiteDashboardRanking) => number
  format: (value: number) => string
}) {
  const maximum = Math.max(1, ...data.map(value))

  return (
    <Card>
      <CardHeader className='border-b'>
        <CardTitle>{title}</CardTitle>
        <CardDescription>{description}</CardDescription>
      </CardHeader>
      <CardContent className='space-y-3'>
        {data.map((item) => {
          const amount = value(item)
          return (
            <div key={item.label} className='grid gap-1.5'>
              <div className='flex items-center justify-between gap-3 text-sm'>
                <span className='min-w-0 truncate font-mono' title={item.label}>
                  {item.label}
                </span>
                <span className='shrink-0 tabular-nums text-muted-foreground'>
                  {format(amount)}
                </span>
              </div>
              <div className='h-2 overflow-hidden rounded-full bg-muted'>
                <div
                  className='h-full rounded-full bg-primary'
                  style={{ width: `${(amount / maximum) * 100}%` }}
                />
              </div>
            </div>
          )
        })}
        {!data.length && (
          <p className='py-5 text-center text-sm text-muted-foreground'>
            当前时段暂无访问数据
          </p>
        )}
      </CardContent>
    </Card>
  )
}

function WebsiteDetailSheet({
  website,
  onOpenChange,
}: {
  website: Website | null
  onOpenChange: (open: boolean) => void
}) {
  const websiteId = website?.id
  const detailQuery = useQuery({
    queryKey: ['websites', websiteId, 'detail'],
    enabled: !!websiteId,
    queryFn: () => {
      if (!websiteId) throw new Error('网站不存在')
      return getData<Website>(`/websites/${websiteId}`)
    },
    refetchInterval: 15_000,
  })
  const dashboardQuery = useQuery({
    queryKey: ['websites', websiteId, 'dashboard'],
    enabled: !!websiteId,
    queryFn: () => {
      if (!websiteId) throw new Error('网站不存在')
      return getData<WebsiteDashboard>(`/websites/${websiteId}/dashboard`)
    },
    refetchInterval: 30_000,
  })

  if (!website) return null

  const detail = detailQuery.data ?? website
  const dashboard = dashboardQuery.data
  const summary = dashboard?.summary
  const domainStates = detail.runtime.domain_states
  const originStates = detail.runtime.origin_states

  return (
    <Sheet open onOpenChange={onOpenChange}>
      <SheetContent className='flex w-full flex-col overflow-hidden p-0 sm:w-[80vw] sm:max-w-none'>
        <SheetHeader className='border-b px-6 py-5 text-start'>
          <div className='flex flex-wrap items-center gap-2'>
            <SheetTitle>{detail.config.name || '网站'} · 详情看板</SheetTitle>
            <StatusBadge status={detail.status} />
            <StatusBadge status={detail.runtime.deploy_status} />
          </div>
          <SheetDescription>
            {detail.cluster_name} · {detail.access_domain}
          </SheetDescription>
        </SheetHeader>
        <ScrollArea className='min-h-0 flex-1'>
          <div className='space-y-6 p-6'>
            <section
              className='grid overflow-hidden rounded-lg border sm:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-6'
              aria-label='网站核心运行指标'
            >
              <WebsiteDashboardMetric
                title='上月峰值带宽'
                value={formatBytesPerSecond(summary?.previous_month_peak_bps)}
                description='按分钟响应流量聚合'
                icon={Activity}
              />
              <WebsiteDashboardMetric
                title='当月峰值带宽'
                value={formatBytesPerSecond(summary?.current_month_peak_bps)}
                description='按分钟响应流量聚合'
                icon={Network}
              />
              <WebsiteDashboardMetric
                title='当天峰值带宽'
                value={formatBytesPerSecond(summary?.today_peak_bps)}
                description='按分钟响应流量聚合'
                icon={Globe2}
              />
              <WebsiteDashboardMetric
                title='当前带宽'
                value={formatBytesPerSecond(summary?.current_bandwidth_bps)}
                description='最近一分钟平均值'
                icon={Server}
              />
              <WebsiteDashboardMetric
                title='当天独立 IP'
                value={summary?.today_unique_ips ?? '—'}
                description='按客户端 IP 去重'
                icon={Activity}
              />
              <WebsiteDashboardMetric
                title='当天流量'
                value={formatDashboardBytes(summary?.today_response_bytes)}
                description='当天响应流量累计'
                icon={ShieldCheck}
              />
            </section>

            {dashboardQuery.isError && (
              <Card className='border-destructive/40'>
                <CardContent className='flex flex-wrap items-center justify-between gap-3'>
                  <p className='text-sm text-muted-foreground'>
                    统计数据加载失败，请稍后刷新重试。
                  </p>
                  <Button
                    size='sm'
                    variant='outline'
                    onClick={() => void dashboardQuery.refetch()}
                  >
                    重试
                  </Button>
                </CardContent>
              </Card>
            )}

            <div className='grid gap-6 xl:grid-cols-2'>
              <WebsiteDashboardTrend
                title='24 小时流量趋势'
                description='按小时汇总响应流量。'
                data={dashboard?.hourly ?? []}
                value={(item) => item.response_bytes}
                format={formatDashboardBytes}
              />
              <WebsiteDashboardTrend
                title='15 天流量趋势'
                description='按天汇总响应流量。'
                data={dashboard?.daily ?? []}
                value={(item) => item.response_bytes}
                format={formatDashboardBytes}
              />
              <WebsiteDashboardTrend
                title='24 小时访问量趋势'
                description='按小时汇总请求次数。'
                data={dashboard?.hourly ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
              <WebsiteDashboardTrend
                title='15 天访问量趋势'
                description='按天汇总请求次数。'
                data={dashboard?.daily ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
            </div>

            <div className='grid items-start gap-6 lg:grid-cols-2 xl:grid-cols-4'>
              <WebsiteDashboardRankingList
                title='状态码分布'
                description='最近 24 小时的请求数。'
                data={dashboard?.status_codes ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
              <WebsiteDashboardRankingList
                title='请求方法分布'
                description='最近 24 小时的请求数。'
                data={dashboard?.methods ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
              <WebsiteDashboardRankingList
                title='国家/地区排行'
                description='最近 24 小时按请求数，由本地 XDB 库解析。'
                data={dashboard?.countries ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
              <WebsiteDashboardRankingList
                title='域名访问排行'
                description='最近 24 小时按请求数。'
                data={dashboard?.hosts ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
              <WebsiteDashboardRankingList
                title='请求来源排行'
                description='最近 24 小时按请求数。'
                data={dashboard?.referers ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
              <WebsiteDashboardRankingList
                title='请求路径排行'
                description='最近 24 小时按请求数。'
                data={dashboard?.paths ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
              <WebsiteDashboardRankingList
                title='独立 IP 排行（下行流量）'
                description='最近 24 小时按响应流量。'
                data={dashboard?.client_ips_by_bytes ?? []}
                value={(item) => item.response_bytes}
                format={formatDashboardBytes}
              />
              <WebsiteDashboardRankingList
                title='独立 IP 排行（请求数）'
                description='最近 24 小时按请求数。'
                data={dashboard?.client_ips_by_requests ?? []}
                value={(item) => item.request_count}
                format={(value) => `${value} 次`}
              />
            </div>

            <div className='grid gap-6 xl:grid-cols-2'>
              <Card>
                <CardHeader className='border-b'>
                  <CardTitle>域名解析状态</CardTitle>
                  <CardDescription>节点回传的域名解析运行状态。</CardDescription>
                </CardHeader>
                <CardContent className='divide-y px-4'>
                  {detail.config.domains.map((domain) => {
                    const runtime = domainStates.find(
                      (item) => item.id === domain.id
                    )
                    return (
                      <div
                        key={domain.id}
                        className='flex flex-wrap items-center justify-between gap-3 py-3'
                      >
                        <div className='min-w-0'>
                          <p className='truncate font-mono text-sm font-medium'>
                            {domain.hostname}
                          </p>
                          <p className='mt-1 text-xs text-muted-foreground'>
                            {domain.dns_mode === 'managed'
                              ? '托管解析'
                              : '外部解析'}
                          </p>
                        </div>
                        <StatusBadge status={runtime?.resolution_status ?? 'pending'} />
                      </div>
                    )
                  })}
                </CardContent>
              </Card>

              <Card>
                <CardHeader className='border-b'>
                  <CardTitle>源站健康</CardTitle>
                  <CardDescription>各节点最新回源探测结果。</CardDescription>
                </CardHeader>
                <CardContent className='space-y-3'>
                  {detail.config.origins.map((origin) => {
                    const states = originStates.filter(
                      (state) => state.origin_id === origin.id
                    )
                    const state =
                      origin.status === 'disabled'
                        ? 'disabled'
                        : states.some((item) => item.status === 'unhealthy')
                          ? 'unhealthy'
                          : states.some((item) => item.status === 'healthy')
                            ? 'healthy'
                            : 'pending'
                    return (
                      <div key={origin.id} className='rounded-lg border p-3'>
                        <div className='flex flex-wrap items-start justify-between gap-3'>
                          <div className='min-w-0'>
                            <p className='truncate font-mono text-sm font-medium'>
                              {origin.protocol}://{origin.host}:{origin.port}
                            </p>
                            <p className='mt-1 text-xs text-muted-foreground'>
                              {originGroupLabel(origin.group)} ·{' '}
                              {origin.role === 'primary' ? '主要源站' : '备用源站'} · 权重{' '}
                              {origin.weight}
                            </p>
                          </div>
                          <StatusBadge status={state} />
                        </div>
                        {states.length > 0 && (
                          <div className='mt-3 flex flex-wrap gap-2'>
                            {states.map((item) => (
                              <Badge key={item.node_id} variant='outline'>
                                {item.node_name || '未知节点'} · {item.latency_millis} ms
                              </Badge>
                            ))}
                          </div>
                        )}
                      </div>
                    )
                  })}
                </CardContent>
              </Card>
            </div>
          </div>
        </ScrollArea>
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
      <SheetContent className='flex w-full flex-col sm:max-w-5xl'>
        <SheetHeader className='text-start'>
          <div className='flex items-center gap-2'>
            <SheetTitle>{website?.config.name} · 访问日志</SheetTitle>
            <Badge variant='outline' role='status'>
              {connected ? '已连接' : '正在重连'}
            </Badge>
          </div>
          <SheetDescription>
            实时显示最近 200 条请求，按接收时间倒序；断线后会自动重连。
          </SheetDescription>
        </SheetHeader>
        <ScrollArea className='min-h-0 flex-1 px-4'>
          <div
            className='overflow-hidden rounded-lg border bg-card font-mono text-xs'
            role='region'
            aria-label='访问日志列表'
            tabIndex={0}
          >
            {logs.map((log) => {
              const request = `${accessLogUrlScheme(log.protocol)}://${log.host}${log.target}`
              const statusLabel = accessLogStatusLabel(log.status_code)
              return (
                <div
                  key={log.id}
                  className='grid grid-cols-[minmax(0,1fr)_auto] gap-x-3 gap-y-0 border-b px-3 py-2 last:border-b-0 hover:bg-muted/50'
                >
                  <div className='flex min-w-0 items-center gap-2'>
                    <Badge
                      variant='outline'
                      className={accessLogStatusClass(log.status_code)}
                      aria-label={`响应状态 ${log.status_code}，${statusLabel}`}
                      title={statusLabel}
                    >
                      {log.status_code}
                    </Badge>
                    <span className='font-semibold text-foreground'>
                      {log.method}
                    </span>
                    <code
                      title={request}
                      className='min-w-0 truncate text-foreground'
                    >
                      {request}
                    </code>
                  </div>
                  <div className='row-span-2 flex self-center flex-col items-end gap-0 text-end text-[11px] leading-4 text-muted-foreground tabular-nums'>
                    <span
                      className='font-medium text-muted-foreground'
                      title={`${log.duration_ms} 毫秒`}
                    >
                      {log.duration_ms} ms
                    </span>
                    <span title={`请求体大小：${formatAccessLogBytes(log.request_bytes)}`}>
                      请求体 {formatAccessLogBytes(log.request_bytes)}
                    </span>
                    <span title={`响应大小：${formatAccessLogBytes(log.response_bytes)}`}>
                      响应 {formatAccessLogBytes(log.response_bytes)}
                    </span>
                  </div>
                  <div className='col-start-1 min-w-0 overflow-x-auto text-muted-foreground'>
                    <div className='flex min-w-max items-center gap-x-3 whitespace-nowrap'>
                      <time dateTime={log.occurred_at}>
                        {formatDate(log.occurred_at)}
                      </time>
                      <span>[{log.node_name || '未知节点'}]</span>
                      <span title='用户 IP'>{log.client_ip || '未知 IP'}</span>
                      {log.client_ip_location && (
                        <span title={`IP 归属地：${log.client_ip_location}`}>
                          {log.client_ip_location.replaceAll(' · ', '')}
                        </span>
                      )}
                      <span>{accessLogHttpVersion(log.protocol)}</span>
                    </div>
                  </div>
                </div>
              )
            })}
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
