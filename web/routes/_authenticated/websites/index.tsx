import { createFileRoute, useLocation } from '@tanstack/react-router'
import { WebsiteDetailLayout } from '@/features/websites/website-detail'
import { WebsitesPage } from '@/features/websites/websites-page'
import { websitesSearchSchema } from '@/features/websites/websites-search'

export const Route = createFileRoute('/_authenticated/websites/')({
  validateSearch: websitesSearchSchema,
  staticData: { breadcrumb: '网站' },
  component: function WebsiteWorkspace() {
    const id = useLocation({ select: (location) => location.state.website?.id })
    return id ? <WebsiteDetailLayout key={id} websiteId={id} /> : <WebsitesPage />
  },
})
