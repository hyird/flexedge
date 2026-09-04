import { useMemo, useState } from 'react'
import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { CheckCircle2, Pencil, Plus, ShieldCheck, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { formatDate } from '@/lib/format'
import type { CertificateProvider, DnsProvider, PageData } from '@/lib/types'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import {
  DropdownMenuItem,
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
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'

const dnsSchema = z.object({
  name: z.string().trim().min(1, '请输入账号名称').max(100),
  provider: z.enum(['cloudflare', 'aliyun']),
  account_id: z.string().trim().min(8, '账户标识至少 8 个字符').max(128),
  api_token: z.string().max(256),
})

type DnsValues = z.infer<typeof dnsSchema>

const certificateSchema = z
  .object({
    provider: z.enum(['letsencrypt', 'zerossl']),
    credential_mode: z.enum(['email', 'access_key']),
    account_email: z.string(),
    access_key: z.string().max(255),
  })
  .superRefine((value, ctx) => {
    if (
      value.credential_mode === 'email' &&
      !z.email().safeParse(value.account_email).success
    ) {
      ctx.addIssue({
        code: 'custom',
        path: ['account_email'],
        message: '请输入有效邮箱',
      })
    }
    if (value.credential_mode === 'access_key' && !value.access_key.trim()) {
      ctx.addIssue({
        code: 'custom',
        path: ['access_key'],
        message: '请输入 API Access Key',
      })
    }
  })

type CertificateValues = z.infer<typeof certificateSchema>

export function Providers() {
  const queryClient = useQueryClient()
  const [dnsPage, setDnsPage] = useState(1)
  const [dnsPageSize, setDnsPageSize] = useState(10)
  const [draftKeyword, setDraftKeyword] = useState('')
  const [keyword, setKeyword] = useState('')
  const [dnsDialog, setDnsDialog] = useState<DnsProvider | 'new' | null>(null)
  const [certificateDialog, setCertificateDialog] = useState<
    CertificateProvider | 'new' | null
  >(null)
  const [removeTarget, setRemoveTarget] = useState<
    | { kind: 'dns'; item: DnsProvider }
    | { kind: 'certificate'; item: CertificateProvider }
    | null
  >(null)

  const dnsQuery = useQuery({
    queryKey: ['providers', 'dns', dnsPage, dnsPageSize, keyword],
    queryFn: () =>
      getData<PageData<DnsProvider>>('/providers/dns', {
        page: dnsPage,
        page_size: dnsPageSize,
        keyword: keyword || undefined,
      }),
  })
  const certificateQuery = useQuery({
    queryKey: ['providers', 'certificate'],
    queryFn: () => getData<CertificateProvider[]>('/providers/certificate'),
  })

  const { mutate: verifyProvider } = useMutation({
    mutationFn: ({
      kind,
      id,
      revision,
    }: {
      kind: 'dns' | 'certificate'
      id: string
      revision: number
    }) =>
      sendData('post', `/providers/${kind}/${id}/verify`, undefined, revision),
    onSuccess: async (response, variables) => {
      toast.success(response.message)
      await queryClient.invalidateQueries({
        queryKey: ['providers', variables.kind],
      })
    },
  })

  const remove = useMutation({
    mutationFn: (target: NonNullable<typeof removeTarget>) =>
      sendData(
        'delete',
        `/providers/${target.kind}/${target.item.id}`,
        undefined,
        target.item.revision
      ),
    onSuccess: async (response, target) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({
        queryKey: ['providers', target.kind],
      })
    },
  })

  const dnsColumns = useMemo<ColumnDef<DnsProvider>[]>(
    () => [
      {
        accessorKey: 'name',
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='账号名称' />
        ),
        cell: ({ row }) => (
          <div>
            <div className='font-medium'>{row.original.name}</div>
            <div className='text-xs text-muted-foreground'>
              {row.original.provider === 'cloudflare' ? 'Cloudflare' : '阿里云'}
            </div>
          </div>
        ),
      },
      {
        accessorKey: 'account_id',
        header: '账户标识',
        cell: ({ row }) => (
          <code className='text-xs'>{row.original.account_id}</code>
        ),
      },
      {
        accessorKey: 'status',
        header: '状态',
        cell: ({ row }) => <StatusBadge status={row.original.status} />,
      },
      {
        accessorKey: 'zone_count',
        header: '托管域名',
      },
      {
        accessorKey: 'last_verified_at',
        header: '最近验证',
        cell: ({ row }) => (
          <span className='whitespace-nowrap text-muted-foreground'>
            {formatDate(row.original.last_verified_at)}
          </span>
        ),
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem onSelect={() => setDnsDialog(row.original)}>
              <Pencil /> 编辑
            </DropdownMenuItem>
            <DropdownMenuItem
              onSelect={() =>
                verifyProvider({
                  kind: 'dns',
                  id: row.original.id,
                  revision: row.original.revision,
                })
              }
            >
              <CheckCircle2 /> 验证凭据
            </DropdownMenuItem>
            <DropdownMenuSeparator />
            <DropdownMenuItem
              variant='destructive'
              onSelect={() =>
                setRemoveTarget({ kind: 'dns', item: row.original })
              }
            >
              <Trash2 /> 删除
            </DropdownMenuItem>
          </RowActions>
        ),
      },
    ],
    [verifyProvider]
  )

  const certificateColumns = useMemo<ColumnDef<CertificateProvider>[]>(
    () => [
      {
        accessorKey: 'provider',
        header: '供应商',
        cell: ({ row }) => (
          <div className='font-medium'>
            {row.original.provider === 'letsencrypt'
              ? "Let's Encrypt"
              : 'ZeroSSL'}
          </div>
        ),
      },
      {
        accessorKey: 'credential_mode',
        header: '接入方式',
        cell: ({ row }) =>
          row.original.credential_mode === 'email' ? '邮箱' : 'Access Key',
      },
      {
        accessorKey: 'account_email',
        header: '账户',
        cell: ({ row }) =>
          row.original.account_email || row.original.access_key_hint || '—',
      },
      {
        accessorKey: 'status',
        header: '状态',
        cell: ({ row }) => <StatusBadge status={row.original.status} />,
      },
      {
        accessorKey: 'last_verified_at',
        header: '最近验证',
        cell: ({ row }) => (
          <span className='whitespace-nowrap text-muted-foreground'>
            {formatDate(row.original.last_verified_at)}
          </span>
        ),
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem
              onSelect={() => setCertificateDialog(row.original)}
            >
              <Pencil /> 编辑
            </DropdownMenuItem>
            <DropdownMenuItem
              onSelect={() =>
                verifyProvider({
                  kind: 'certificate',
                  id: row.original.id,
                  revision: row.original.revision,
                })
              }
            >
              <CheckCircle2 /> 验证凭据
            </DropdownMenuItem>
            <DropdownMenuSeparator />
            <DropdownMenuItem
              variant='destructive'
              onSelect={() =>
                setRemoveTarget({
                  kind: 'certificate',
                  item: row.original,
                })
              }
            >
              <Trash2 /> 删除
            </DropdownMenuItem>
          </RowActions>
        ),
      },
    ],
    [verifyProvider]
  )

  return (
    <FeatureShell title='服务商' description='管理 DNS 与证书供应商凭据。'>
      <Tabs defaultValue='dns' className='space-y-4'>
        <TabsList>
          <TabsTrigger value='dns'>DNS 服务商</TabsTrigger>
          <TabsTrigger value='certificate'>证书供应商</TabsTrigger>
        </TabsList>
        <TabsContent value='dns' className='space-y-4'>
          <div className='flex flex-col gap-2 sm:flex-row sm:items-center'>
            <ResourceToolbar
              value={draftKeyword}
              onChange={setDraftKeyword}
              onSearch={() => {
                setKeyword(draftKeyword.trim())
                setDnsPage(1)
              }}
              onReset={() => {
                setDraftKeyword('')
                setKeyword('')
                setDnsPage(1)
              }}
              onRefresh={() => dnsQuery.refetch()}
              refreshing={dnsQuery.isFetching}
              placeholder='搜索账号名称…'
            />
            <Button className='sm:ms-auto' onClick={() => setDnsDialog('new')}>
              <Plus /> 添加 DNS 账号
            </Button>
          </div>
          <ResourceTable
            columns={dnsColumns}
            data={dnsQuery.data?.list ?? []}
            loading={dnsQuery.isLoading}
            error={dnsQuery.isError}
            onRetry={() => void dnsQuery.refetch()}
            page={dnsPage}
            pageSize={dnsPageSize}
            totalPages={dnsQuery.data?.total_pages ?? 1}
            onPaginationChange={(nextPage, nextSize) => {
              setDnsPage(nextPage)
              setDnsPageSize(nextSize)
            }}
          />
        </TabsContent>
        <TabsContent value='certificate' className='space-y-4'>
          <div className='flex justify-end'>
            <Button onClick={() => setCertificateDialog('new')}>
              <Plus /> 添加证书供应商
            </Button>
          </div>
          <ResourceTable
            columns={certificateColumns}
            data={certificateQuery.data ?? []}
            loading={certificateQuery.isLoading}
            error={certificateQuery.isError}
            onRetry={() => void certificateQuery.refetch()}
            page={1}
            pageSize={Math.max(certificateQuery.data?.length ?? 10, 10)}
            totalPages={1}
            onPaginationChange={() => undefined}
          />
        </TabsContent>
      </Tabs>

      {dnsDialog && (
        <DnsProviderDialog
          key={dnsDialog === 'new' ? 'new' : dnsDialog.id}
          provider={dnsDialog === 'new' ? undefined : dnsDialog}
          open
          onOpenChange={(open) => !open && setDnsDialog(null)}
        />
      )}
      {certificateDialog && (
        <CertificateProviderDialog
          key={certificateDialog === 'new' ? 'new' : certificateDialog.id}
          provider={certificateDialog === 'new' ? undefined : certificateDialog}
          open
          onOpenChange={(open) => !open && setCertificateDialog(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除服务商配置'
        desc={
          removeTarget?.kind === 'dns'
            ? `将删除“${removeTarget.item.name}”，已关联资源会阻止此操作。`
            : '将删除此证书供应商，已关联证书会阻止此操作。'
        }
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}

function DnsProviderDialog({
  provider,
  open,
  onOpenChange,
}: {
  provider?: DnsProvider
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const queryClient = useQueryClient()
  const form = useForm<DnsValues>({
    resolver: zodResolver(dnsSchema),
    defaultValues: {
      name: provider?.name ?? '',
      provider: (provider?.provider as DnsValues['provider']) ?? 'cloudflare',
      account_id: provider?.account_id ?? '',
      api_token: '',
    },
  })
  const mutation = useMutation({
    mutationFn: (values: DnsValues) => {
      if (provider) {
        return sendData(
          'put',
          `/providers/dns/${provider.id}`,
          {
            name: values.name,
            ...(values.api_token ? { api_token: values.api_token } : {}),
          },
          provider.revision
        )
      }
      return sendData('post', '/providers/dns', values)
    },
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({ queryKey: ['providers', 'dns'] })
    },
  })

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>
            {provider ? '编辑 DNS 账号' : '添加 DNS 账号'}
          </DialogTitle>
          <DialogDescription>
            凭据只会提交给 FlexEdge 服务端，保存后仅显示脱敏提示。
          </DialogDescription>
        </DialogHeader>
        <Form {...form}>
          <form
            id='dns-provider-form'
            className='grid gap-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='name'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>账号名称</FormLabel>
                  <FormControl>
                    <Input placeholder='生产 DNS' {...field} />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name='provider'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>服务商</FormLabel>
                  <Select
                    disabled={!!provider}
                    value={field.value}
                    onValueChange={field.onChange}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      <SelectItem value='cloudflare'>Cloudflare</SelectItem>
                      <SelectItem value='aliyun'>阿里云 DNS</SelectItem>
                    </SelectContent>
                  </Select>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name='account_id'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>账户标识</FormLabel>
                  <FormControl>
                    <Input disabled={!!provider} {...field} />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name='api_token'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>
                    API Token {provider ? '（留空保持不变）' : ''}
                  </FormLabel>
                  <FormControl>
                    <Input
                      type='password'
                      autoComplete='new-password'
                      {...field}
                    />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
          </form>
        </Form>
        <DialogFooter>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            type='submit'
            form='dns-provider-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在保存…' : '保存'}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

function CertificateProviderDialog({
  provider,
  open,
  onOpenChange,
}: {
  provider?: CertificateProvider
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const queryClient = useQueryClient()
  const form = useForm<CertificateValues>({
    resolver: zodResolver(certificateSchema),
    defaultValues: {
      provider:
        (provider?.provider as CertificateValues['provider']) ?? 'letsencrypt',
      credential_mode:
        (provider?.credential_mode as CertificateValues['credential_mode']) ??
        'email',
      account_email: provider?.account_email ?? '',
      access_key: '',
    },
  })
  const mode = form.watch('credential_mode')
  const mutation = useMutation({
    mutationFn: (values: CertificateValues) => {
      const body = {
        credential_mode: values.credential_mode,
        account_email:
          values.credential_mode === 'email' ? values.account_email : '',
        access_key:
          values.credential_mode === 'access_key' ? values.access_key : '',
      }
      return provider
        ? sendData(
            'put',
            `/providers/certificate/${provider.id}`,
            body,
            provider.revision
          )
        : sendData('post', '/providers/certificate', {
            provider: values.provider,
            ...body,
          })
    },
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
      await queryClient.invalidateQueries({
        queryKey: ['providers', 'certificate'],
      })
    },
  })

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>
            {provider ? '编辑证书供应商' : '添加证书供应商'}
          </DialogTitle>
          <DialogDescription>配置 ACME 账户接入方式。</DialogDescription>
        </DialogHeader>
        <Form {...form}>
          <form
            id='certificate-provider-form'
            className='grid gap-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='provider'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>供应商</FormLabel>
                  <Select
                    disabled={!!provider}
                    value={field.value}
                    onValueChange={field.onChange}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      <SelectItem value='letsencrypt'>
                        Let&apos;s Encrypt
                      </SelectItem>
                      <SelectItem value='zerossl'>ZeroSSL</SelectItem>
                    </SelectContent>
                  </Select>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name='credential_mode'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>接入方式</FormLabel>
                  <Select value={field.value} onValueChange={field.onChange}>
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      <SelectItem value='email'>账户邮箱</SelectItem>
                      <SelectItem value='access_key'>Access Key</SelectItem>
                    </SelectContent>
                  </Select>
                  <FormMessage />
                </FormItem>
              )}
            />
            {mode === 'email' ? (
              <FormField
                control={form.control}
                name='account_email'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>账户邮箱</FormLabel>
                    <FormControl>
                      <Input type='email' {...field} />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
            ) : (
              <FormField
                control={form.control}
                name='access_key'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>
                      Access Key {provider ? '（留空保持不变）' : ''}
                    </FormLabel>
                    <FormControl>
                      <Input
                        type='password'
                        autoComplete='new-password'
                        {...field}
                      />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
            )}
          </form>
        </Form>
        <DialogFooter>
          <Button variant='outline' onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button
            type='submit'
            form='certificate-provider-form'
            disabled={mutation.isPending}
          >
            <ShieldCheck />
            {mutation.isPending ? '正在保存…' : '保存'}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
