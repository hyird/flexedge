import { type FormEvent, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Link, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { fieldErrors } from '@/lib/form-errors'
import { queryKeys } from '@/lib/query-keys'
import { FormSheet } from '@/components/form-sheet'
import { Choice, Field, Notice, Toggle } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { certificateProvidersQuery } from '@/features/providers/data'
import { providerLabel } from '@/features/providers/provider-display'
import {
  certificateFormSchema,
  type CertificateFormValues,
} from './certificate-form'
import { createCertificate } from './data'

const emptyValues: CertificateFormValues = {
  domain: '',
  certificate_provider_id: '',
  dns_zone_id: '',
  auto_renew: true,
}

/** "申请证书": domain, ACME provider and the DNS zone used for DNS-01. */
export function RequestCertificateSheet({
  isOpen,
  onClose,
}: {
  isOpen: boolean
  onClose: () => void
}) {
  const client = useQueryClient()
  const [values, setValues] = useState(emptyValues)
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [wasOpen, setWasOpen] = useState(isOpen)
  // Option pickers read one snapshot per opening.
  const providers = useQuery({
    ...certificateProvidersQuery,
    enabled: isOpen,
    gcTime: 0,
    staleTime: 0,
    meta: { inlineError: true },
  })
  const zones = useQuery({
    ...dnsZoneOptionsQuery,
    enabled: isOpen,
    gcTime: 0,
    staleTime: 0,
    meta: { inlineError: true },
  })
  const availableZones = (zones.data ?? []).filter((zone) => zone.available)
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (input: CertificateFormValues) =>
      createCertificate({
        domain: input.domain,
        certificate_provider_id: input.certificate_provider_id,
        dns_zone_id: input.dns_zone_id,
        config: { auto_renew: input.auto_renew },
      }),
    onSuccess: (response) => {
      toast.success(response.message || '证书申请已提交，正在签发')
      void client.invalidateQueries({
        queryKey: queryKeys.certificates,
        refetchType: 'none',
      })
      onClose()
    },
  })
  if (isOpen !== wasOpen) {
    setWasOpen(isOpen)
    if (isOpen) {
      setValues(emptyValues)
      setErrors({})
    }
  }
  const close = () => {
    mutation.reset()
    onClose()
  }
  const setDomain = (domain: string) =>
    setValues((current) => {
      if (current.dns_zone_id) return { ...current, domain }
      // Preselect the managed zone the domain belongs to (longest suffix).
      const host = domain.trim().replace(/^\*\./, '').toLowerCase()
      const zone = availableZones
        .filter(
          (item) => host === item.domain || host.endsWith(`.${item.domain}`)
        )
        .sort((left, right) => right.domain.length - left.domain.length)[0]
      return { ...current, domain, dns_zone_id: zone?.id ?? '' }
    })
  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    const result = certificateFormSchema(false).safeParse(values)
    if (!result.success) {
      setErrors(fieldErrors(result.error.issues))
      return
    }
    setErrors({})
    mutation.mutate(result.data)
  }
  return (
    <FormSheet
      isOpen={isOpen}
      onClose={close}
      title='申请证书'
      description='使用 DNS-01 验证签发，签发任务在后台执行，完成后可绑定到网站。'
      onSubmit={submit}
      submitLabel='申请证书'
      isSubmitting={mutation.isPending}
      error={mutation.isError ? apiErrorMessage(mutation.error) : undefined}
    >
      <Field
        label='证书域名'
        value={values.domain}
        onChange={setDomain}
        required
        placeholder='www.example.com 或 *.example.com'
        hint='支持通配符域名。'
        disabled={mutation.isPending}
        error={errors.domain}
      />
      <QueryNotice
        query={providers}
        onRetry={() => void providers.refetch()}
        title='证书供应商加载失败'
      />
      <Choice
        label='证书供应商'
        required
        value={values.certificate_provider_id}
        onChange={(certificate_provider_id) =>
          setValues((current) => ({ ...current, certificate_provider_id }))
        }
        items={(providers.data ?? []).map((item) => ({
          id: item.id,
          label: `${providerLabel(item.provider)} · ${item.account_email || item.access_key_hint || 'Access Key'}`,
        }))}
        disabled={providers.isPending || mutation.isPending}
        error={errors.certificate_provider_id}
      />
      {providers.isSuccess && !providers.data.length && (
        <Notice>
          还没有证书供应商，请先在
          <Link href='/providers?tab=certificate'>供应商账号 › 证书供应商</Link>
          中添加。
        </Notice>
      )}
      <QueryNotice
        query={zones}
        onRetry={() => void zones.refetch()}
        title='DNS 区域加载失败'
      />
      <Choice
        label='DNS 验证区域'
        required
        value={values.dns_zone_id}
        onChange={(dns_zone_id) =>
          setValues((current) => ({ ...current, dns_zone_id }))
        }
        items={availableZones.map((item) => ({
          id: item.id,
          label: `${item.domain} · ${item.dns_provider_name}`,
        }))}
        hint='证书域名必须属于该区域，签发时在其中写入 _acme-challenge 记录。'
        disabled={zones.isPending || mutation.isPending}
        error={errors.dns_zone_id}
      />
      {zones.isSuccess && !availableZones.length && (
        <Notice>
          没有可用的 DNS 区域，请先
          <Link href='/dns-zones?create=1'>添加 DNS 区域</Link>
          并完成同步。
        </Notice>
      )}
      <Toggle
        label='自动续期'
        hint='到期前自动重新签发，并分发到引用该证书的网站。'
        selected={values.auto_renew}
        onChange={(auto_renew) =>
          setValues((current) => ({ ...current, auto_renew }))
        }
        disabled={mutation.isPending}
      />
    </FormSheet>
  )
}
