import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation } from '@tanstack/react-query'
import { toast } from 'sonner'
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
import type { DnsProvider } from '@/features/providers/types'
import { saveDnsProvider } from './data'
import {
  dnsProviderFormSchema,
  type DnsProviderFormValues as DnsValues,
} from './dns-provider-form'

export function DnsProviderDialog({
  provider,
  open,
  onOpenChange,
}: {
  provider?: DnsProvider
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const form = useForm<DnsValues>({
    resolver: zodResolver(dnsProviderFormSchema(!!provider)),
    defaultValues: {
      name: provider?.name ?? '',
      provider: (provider?.provider as DnsValues['provider']) ?? 'cloudflare',
      account_id: provider?.account_id ?? '',
      api_token: '',
    },
  })
  const mutation = useMutation({
    mutationFn: (values: DnsValues) => saveDnsProvider(values, provider),
    onSuccess: async (response) => {
      toast.success(response.message)
      onOpenChange(false)
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
