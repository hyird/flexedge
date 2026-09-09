import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery } from '@tanstack/react-query'
import { toast } from 'sonner'
import { Button } from '@/components/ui/button'
import { Checkbox } from '@/components/ui/checkbox'
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
import type { Certificate } from '@/features/certificates/types'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { certificateProvidersQuery } from '@/features/providers/data'
import {
  certificateFormSchema,
  type CertificateFormValues as CreateValues,
} from './certificate-form'
import { createCertificate, updateCertificateRenewal } from './data'

export function CertificateDialog({
  certificate,
  open,
  onOpenChange,
}: {
  certificate?: Certificate
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const providersQuery = useQuery({
    ...certificateProvidersQuery,
    enabled: !certificate,
  })
  const zonesQuery = useQuery({ ...dnsZoneOptionsQuery, enabled: !certificate })
  const form = useForm<CreateValues>({
    resolver: zodResolver(certificateFormSchema(!!certificate)),
    defaultValues: {
      domain: certificate?.domains[0] ?? '',
      certificate_provider_id: certificate?.certificate_provider_id ?? '',
      dns_zone_id: certificate?.dns_zone_id ?? '',
      auto_renew: certificate?.config.auto_renew ?? true,
    },
  })
  const mutation = useMutation({
    mutationFn: (values: CreateValues) =>
      certificate
        ? updateCertificateRenewal(certificate, values.auto_renew)
        : createCertificate({
            domain: values.domain,
            certificate_provider_id: values.certificate_provider_id,
            dns_zone_id: values.dns_zone_id,
            config: { auto_renew: values.auto_renew },
          }),
    onSuccess: (response) => {
      toast.success(response.message)
      onOpenChange(false)
    },
  })

  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent className='overflow-y-auto'>
        <SheetHeader>
          <SheetTitle>{certificate ? '续期设置' : '申请证书'}</SheetTitle>
          <SheetDescription>
            使用 DNS-01 验证申请证书，任务将在后台执行。
          </SheetDescription>
        </SheetHeader>
        <Form {...form}>
          <form
            id='certificate-form'
            className='grid gap-4 px-4'
            onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
          >
            <FormField
              control={form.control}
              name='domain'
              render={({ field }) => (
                <FormItem>
                  <FormLabel>证书域名</FormLabel>
                  <FormControl>
                    <Input
                      disabled={!!certificate}
                      placeholder='*.example.com'
                      {...field}
                    />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            {!certificate && (
              <>
                <FormField
                  control={form.control}
                  name='certificate_provider_id'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>证书供应商</FormLabel>
                      <Select
                        value={field.value}
                        onValueChange={field.onChange}
                      >
                        <FormControl>
                          <SelectTrigger>
                            <SelectValue placeholder='选择供应商' />
                          </SelectTrigger>
                        </FormControl>
                        <SelectContent>
                          {providersQuery.data?.map((provider) => (
                            <SelectItem key={provider.id} value={provider.id}>
                              {provider.provider === 'letsencrypt'
                                ? "Let's Encrypt"
                                : 'ZeroSSL'}
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
                  name='dns_zone_id'
                  render={({ field }) => (
                    <FormItem>
                      <FormLabel>DNS 验证域名</FormLabel>
                      <Select
                        value={field.value}
                        onValueChange={field.onChange}
                      >
                        <FormControl>
                          <SelectTrigger>
                            <SelectValue placeholder='选择托管域名' />
                          </SelectTrigger>
                        </FormControl>
                        <SelectContent>
                          {zonesQuery.data?.map((zone) => (
                            <SelectItem key={zone.id} value={zone.id}>
                              {zone.domain}
                            </SelectItem>
                          ))}
                        </SelectContent>
                      </Select>
                      <FormMessage />
                    </FormItem>
                  )}
                />
              </>
            )}
            <FormField
              control={form.control}
              name='auto_renew'
              render={({ field }) => (
                <FormItem className='flex items-start gap-3 rounded-md border p-4'>
                  <FormControl>
                    <Checkbox
                      checked={field.value}
                      onCheckedChange={field.onChange}
                    />
                  </FormControl>
                  <div>
                    <FormLabel>自动续期</FormLabel>
                    <FormDescription>
                      到期前自动提交续签任务并分发到关联网站。
                    </FormDescription>
                  </div>
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
            form='certificate-form'
            disabled={mutation.isPending}
          >
            {mutation.isPending ? '正在提交…' : certificate ? '保存' : '申请'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
