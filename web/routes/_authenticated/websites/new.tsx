import { createFileRoute } from '@tanstack/react-router'
import { WebsiteCreatePage } from '@/features/websites/website-settings'

export const Route = createFileRoute('/_authenticated/websites/new')({
  staticData: { breadcrumb: '创建网站' },
  component: WebsiteCreatePage,
})
