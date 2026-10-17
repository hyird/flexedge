import { createFileRoute, redirect } from '@tanstack/react-router'

export const Route = createFileRoute('/_authenticated/websites/$websiteId')({
  beforeLoad: ({ params }) => {
    throw redirect({
      to: '/websites',
      state: { website: { id: params.websiteId } },
      replace: true,
    })
  },
})
