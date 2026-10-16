import { type ReactNode } from 'react'
import { Globe } from '@gravity-ui/icons'

/** Standalone full-page frame for root-level error and not-found pages, which
 * render outside the authenticated shell. */
export function ErrorLayout({
  code,
  title,
  description,
  details,
  actions,
}: {
  code: string
  title: string
  description: string
  details?: ReactNode
  actions: ReactNode
}) {
  return (
    <main className='flex min-h-dvh flex-col bg-background px-5 py-6 sm:px-8'>
      <div className='flex items-center gap-2.5'>
        <span className='flex size-8 items-center justify-center rounded-lg bg-accent text-accent-foreground'>
          <Globe className='size-4' />
        </span>
        <span className='text-sm font-semibold'>FlexEdge</span>
      </div>
      <section className='flex flex-1 items-center justify-center py-12'>
        <div className='flex w-full max-w-md flex-col items-start gap-5'>
          <span className='font-mono text-sm text-muted'>{code}</span>
          <div className='flex flex-col gap-2'>
            <h1 className='page-title'>{title}</h1>
            <p className='text-sm leading-6 text-muted'>{description}</p>
          </div>
          {details}
          <div className='flex flex-wrap gap-2'>{actions}</div>
        </div>
      </section>
    </main>
  )
}
