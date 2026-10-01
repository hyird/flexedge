import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import { Drawer } from '@/components/drawer'
import { Choice, Field, FormActions, Notice, Toggle } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { certificateProvidersQuery } from '@/features/providers/data'
import { providerLabel } from '@/features/providers/provider-display'
import {
  certificateFormSchema,
  type CertificateFormValues,
} from './certificate-form'
import { createCertificate, updateCertificateRenewal } from './data'
import type { Certificate } from './types'

export function CertificateDialog({
  certificate,
  onClose,
}: {
  certificate?: Certificate
  onClose: () => void
}) {
  const client = useQueryClient()
  const providers = useQuery({
    ...certificateProvidersQuery,
    enabled: !certificate,
  })
  const zones = useQuery({ ...dnsZoneOptionsQuery, enabled: !certificate })
  const [values, setValues] = useState<CertificateFormValues>({
    domain: certificate?.domains[0] ?? '',
    certificate_provider_id: certificate?.certificate_provider_id ?? '',
    dns_zone_id: certificate?.dns_zone_id ?? '',
    auto_renew: certificate?.config.auto_renew ?? true,
  })
  const [errors, setErrors] = useState<Record<string, string>>({})
  const mutation = useMutation({
    mutationFn: (input: CertificateFormValues) =>
      certificate
        ? updateCertificateRenewal(certificate, input.auto_renew)
        : createCertificate({
            domain: input.domain,
            certificate_provider_id: input.certificate_provider_id,
            dns_zone_id: input.dns_zone_id,
            config: { auto_renew: input.auto_renew },
          }),
    onSuccess: (response) => {
      toast.success(response.message || '已提交')
      void client.invalidateQueries({ queryKey: queryKeys.certificates })
      onClose()
    },
  })
  return (
    <Drawer
      title={certificate ? '续期设置' : '申请证书'}
      onClose={onClose}
      busy={mutation.isPending}
    >
      <form
        className='grid gap-4'
        onSubmit={(event) => {
          event.preventDefault()
          const result = certificateFormSchema(!!certificate).safeParse(values)
          if (!result.success) {
            setErrors(
              Object.fromEntries(
                result.error.issues.map((issue) => [
                  String(issue.path[0]),
                  issue.message,
                ])
              )
            )
            return
          }
          setErrors({})
          mutation.mutate(result.data)
        }}
      >
        <p className='text-sm text-muted'>
          使用 DNS-01 验证，签发任务在后台执行。
        </p>
        <Field
          label='证书域名'
          value={values.domain}
          onChange={(domain) =>
            setValues((current) => ({ ...current, domain }))
          }
          required
          placeholder='*.example.com'
          disabled={!!certificate || mutation.isPending}
          error={errors.domain}
        />
        {!certificate && (
          <>
            <QueryNotice
              query={providers}
              onRetry={() => void providers.refetch()}
            />
            <Choice
              label='证书供应商'
              value={values.certificate_provider_id}
              onChange={(certificate_provider_id) =>
                setValues((current) => ({
                  ...current,
                  certificate_provider_id,
                }))
              }
              items={(providers.data ?? []).map((item) => ({
                id: item.id,
                label: `${providerLabel(item.provider)} · ${item.account_email || item.access_key_hint || 'Access Key'}`,
              }))}
              required
              error={errors.certificate_provider_id}
              disabled={providers.isPending || mutation.isPending}
            />
            <QueryNotice query={zones} onRetry={() => void zones.refetch()} />
            <Choice
              label='DNS 验证域名'
              value={values.dns_zone_id}
              onChange={(dns_zone_id) =>
                setValues((current) => ({ ...current, dns_zone_id }))
              }
              items={(zones.data ?? [])
                .filter((item) => item.available)
                .map((item) => ({ id: item.id, label: item.domain }))}
              required
              error={errors.dns_zone_id}
              disabled={zones.isPending || mutation.isPending}
            />
            {providers.isSuccess && !providers.data.length && (
              <Notice>请先添加证书供应商。</Notice>
            )}
            {zones.isSuccess && !zones.data.some((item) => item.available) && (
              <Notice>暂无可用托管域名，请先配置 DNS 托管并完成同步。</Notice>
            )}
          </>
        )}
        <Toggle
          label='自动续期'
          hint='在到期前自动申请新证书并分发给关联网站。'
          selected={values.auto_renew}
          onChange={(auto_renew) =>
            setValues((current) => ({ ...current, auto_renew }))
          }
          disabled={mutation.isPending}
        />
        {mutation.isError && <Notice>{apiErrorMessage(mutation.error)}</Notice>}
        <FormActions
          onCancel={onClose}
          busy={mutation.isPending}
          label={certificate ? '保存' : '申请证书'}
        />
      </form>
    </Drawer>
  )
}
