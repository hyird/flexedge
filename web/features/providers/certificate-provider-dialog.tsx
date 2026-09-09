import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation } from '@tanstack/react-query'
import { ShieldCheck } from 'lucide-react'
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
import type { CertificateProvider } from '@/features/providers/types'
import {
  certificateProviderFormSchema,
  type CertificateProviderFormValues as CertificateValues,
} from './certificate-provider-form'
import { saveCertificateProvider } from './data'

export function CertificateProviderDialog({
  provider,
  open,
  onOpenChange,
}: {
  provider?: CertificateProvider
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const form = useForm<CertificateValues>({
    resolver: zodResolver(certificateProviderFormSchema(provider)),
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
    mutationFn: (values: CertificateValues) =>
      saveCertificateProvider(values, provider),
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
            {provider ? '编辑证书供应商' : '添加证书供应商'}
          </SheetTitle>
          <SheetDescription>配置 ACME 账户接入方式。</SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='certificate-provider-form'
            className='grid gap-4 px-4'
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
                      Access Key{' '}
                      {provider?.credential_mode === 'access_key'
                        ? '（留空保持不变）'
                        : ''}
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
        <SheetFooter>
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
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
