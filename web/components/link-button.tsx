import { type ReactNode } from 'react'
import type { HistoryState } from '@tanstack/react-router'
import { Link, buttonVariants } from '@heroui/react'
import { cn } from '@/lib/utils'

/** Navigation styled as a button (HeroUI `Button` has no `href`). Use for page
 * actions that only change the URL: `创建网站` → `/websites/new`, `编辑配置`,
 * `前往关联资源`. Routes through TanStack Router via the root RouterProvider;
 * `state` is the arrival request (e.g. the tab to open), never in the URL. */
export function LinkButton({
  href,
  state,
  children,
  variant = 'primary',
  size = 'sm',
  isIconOnly = false,
  className,
  'aria-label': ariaLabel,
}: {
  href: string
  state?: HistoryState
  children: ReactNode
  variant?: 'primary' | 'secondary' | 'tertiary' | 'ghost' | 'outline'
  size?: 'sm' | 'md' | 'lg'
  isIconOnly?: boolean
  className?: string
  'aria-label'?: string
}) {
  return (
    <Link
      href={href}
      routerOptions={state && { state }}
      aria-label={ariaLabel}
      className={cn(
        buttonVariants({ variant, size, isIconOnly }),
        // `.link` resets the button gap; restore the button's icon spacing.
        'gap-2 no-underline [&_svg]:size-4',
        className
      )}
    >
      {children}
    </Link>
  )
}
