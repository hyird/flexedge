import { Outlet, useRouter } from '@tanstack/react-router'
import {
  Spinner,
  Toast,
  type ToastContentValue,
  type ToastProps,
} from '@heroui/react'
import {
  RouterProvider as AriaRouterProvider,
  I18nProvider,
} from 'react-aria-components'

/** Root frame: HeroUI links (`href` on Sidebar, Breadcrumbs, Tabs, Dropdown and
 * Link items) navigate through TanStack Router instead of reloading the page.
 * The console is Chinese-only, so React Aria's built-in accessible names
 * (overlay dismiss buttons, toast region, date formats) follow `zh-CN` rather
 * than the browser language. */
export function RootLayout() {
  const router = useRouter()
  return (
    <I18nProvider locale='zh-CN'>
      <AriaRouterProvider navigate={(href) => void router.navigate({ href })}>
        <Outlet />
        <Toast.Provider placement='top end' width={340} gap={8}>
          {({ toast }) => <ConsoleToast toast={toast} />}
        </Toast.Provider>
      </AriaRouterProvider>
    </I18nProvider>
  )
}

/** The default toast layout with a Chinese close-button name; HeroUI's
 * default renderer hardcodes the English "Close". */
function ConsoleToast({ toast }: Pick<ToastProps, 'toast'>) {
  const content = toast.content as ToastContentValue
  return (
    <Toast toast={toast} variant={content.variant}>
      {content.indicator === null ? null : (
        <Toast.Indicator variant={content.variant}>
          {content.isLoading ? (
            <Spinner color='current' size='sm' />
          ) : (
            content.indicator
          )}
        </Toast.Indicator>
      )}
      <Toast.Content>
        {content.title ? <Toast.Title>{content.title}</Toast.Title> : null}
        {content.description ? (
          <Toast.Description>{content.description}</Toast.Description>
        ) : null}
      </Toast.Content>
      {content.actionProps?.children ? (
        <Toast.ActionButton {...content.actionProps} />
      ) : null}
      <Toast.CloseButton aria-label='关闭' />
    </Toast>
  )
}
