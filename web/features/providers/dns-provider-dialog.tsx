import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import { sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type { DnsProvider } from '@/lib/types'
import { Button } from '@/components/ui/button'
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
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetFooter,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'

const dnsSchema = z.object({
  name: z.string().trim().min(1, '请输入账号名称').max(100),
  provider: z.enum(['cloudflare', 'aliyun']),
  account_id: z.string().trim().min(8, '账户标识至少 8 个字符').max(128),
  api_token: z.string().max(256),
})

type DnsValues = z.infer<typeof dnsSchema>

export function DnsProviderDialog({
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
      await queryClient.invalidateQueries({
        queryKey: [...queryKeys.providers, 'dns'],
      })
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto'>
        <SheetHeader>
          <SheetTitle>
            {provider ? '编辑 DNS 账号' : '添加 DNS 账号'}
          </SheetTitle>
          <SheetDescription>
            凭据只会提交给 FlexEdge 服务端，保存后仅显示脱敏提示。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='dns-provider-form'
            className='grid gap-4 px-4'
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
        <SheetFooter>
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
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
