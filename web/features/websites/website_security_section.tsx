import { useQuery } from '@tanstack/react-query'
import { useRouter } from '@tanstack/react-router'
import { Button, Link } from '@heroui/react'
import { Choice } from '@/components/forms'
import { QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
import { securityPolicyOptionsQuery } from '@/features/security/data'
import type { WebsiteSectionProps } from './website-sections'

export function WebsiteSecuritySection({
  values,
  errors,
  replace,
}: WebsiteSectionProps) {
  const query = useQuery(securityPolicyOptionsQuery)
  const router = useRouter()
  const selected = query.data?.find(
    (item) => item.id === values.security_policy_id
  )
  return (
    <Section
      title='安全'
      actions={
        <Button
          size='sm'
          variant='tertiary'
          onPress={() => {
            void router.navigate({ href: '/security' })
          }}
        >
          管理安全规则集
        </Button>
      }
    >
      <QueryNotice
        query={query}
        title='安全规则集加载失败'
        onRetry={() => query.refetch()}
      />
      <div className='flex max-w-4xl flex-col gap-4'>
        <Choice
          label='安全规则集'
          value={values.security_policy_id}
          onChange={(id) =>
            replace((current) => ({ ...current, security_policy_id: id }))
          }
          items={[
            { id: '', label: '不应用' },
            ...(query.data ?? []).map((item) => ({
              id: item.id,
              label: item.name,
            })),
          ]}
          error={errors.security_policy_id}
        />
        {selected && (
          <div className='rounded-xl border border-border p-4'>
            <div className='flex flex-wrap items-center justify-between gap-2'>
              <p className='font-medium'>{selected.name}</p>
              <Link
                href={`/security/policies/${selected.id}`}
                className='text-sm'
              >
                查看规则集
              </Link>
            </div>
            <p className='mt-2 text-sm text-muted'>{selected.description}</p>
          </div>
        )}
      </div>
    </Section>
  )
}
