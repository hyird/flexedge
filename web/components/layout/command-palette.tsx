import { useEffect, useState } from 'react'
import { useQueries } from '@tanstack/react-query'
import { useRouter } from '@tanstack/react-router'
import {
  Globe,
  Layers,
  Magnifier,
  Plus,
  Server,
  ShieldCheck,
  Signal,
} from '@gravity-ui/icons'
import { Command } from '@heroui-pro/react'
import { Button, Kbd, Spinner } from '@heroui/react'
import type { ResourceSearchHit, ResourceSearchQuery } from '@/lib/resource'
import { navigation } from './navigation'

/** One resource list the palette searches (assembled by the route). */
export type ResourceSearchSource = {
  label: string
  icon: typeof Globe
  /** Detail route prefix of the found resources. */
  path: string
  /** The owning domain's palette search (`xxxSearchQuery`). */
  search: (keyword: string) => ResourceSearchQuery
}

/** Quick actions only navigate; each target page opens its own create view
 * (URL contract listed in the foundation API notes). */
const quickActions = [
  { href: '/websites/new', label: '创建网站', icon: Globe },
  { href: '/nodes?create=1', label: '添加节点', icon: Server },
  { href: '/clusters?create=1', label: '创建集群', icon: Layers },
  { href: '/certificates?create=1', label: '申请证书', icon: ShieldCheck },
  { href: '/dns-zones?create=1', label: '添加域名', icon: Signal },
]

const keywordDebounceMs = 250
const isMac =
  typeof navigator !== 'undefined' && /Mac|iP(hone|ad)/.test(navigator.platform)

/** Navbar search trigger plus the ⌘K / Ctrl+K command palette. */
export function CommandPalette({
  resourceSearch,
}: {
  resourceSearch: ResourceSearchSource[]
}) {
  const [isOpen, setOpen] = useState(false)
  useEffect(() => {
    const toggle = (event: KeyboardEvent) => {
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault()
        setOpen((open) => !open)
      }
    }
    window.addEventListener('keydown', toggle)
    return () => window.removeEventListener('keydown', toggle)
  }, [])
  return (
    <>
      <Button
        size='sm'
        variant='tertiary'
        aria-label='搜索或跳转'
        aria-keyshortcuts={isMac ? 'Meta+K' : 'Control+K'}
        className='w-9 justify-center px-0 sm:w-64 sm:justify-start sm:px-3'
        onPress={() => setOpen(true)}
      >
        <Magnifier className='size-4 shrink-0' />
        <span className='hidden flex-1 text-left font-normal text-muted sm:inline'>
          搜索或跳转…
        </span>
        <Kbd className='hidden text-xs sm:inline-flex'>
          {isMac ? (
            <Kbd.Abbr keyValue='command' />
          ) : (
            <Kbd.Content>Ctrl</Kbd.Content>
          )}
          <Kbd.Content>K</Kbd.Content>
        </Kbd>
      </Button>
      <Command>
        <Command.Backdrop isOpen={isOpen} onOpenChange={setOpen}>
          <Command.Container size='lg'>
            <CommandPaletteDialog
              resourceSearch={resourceSearch}
              onClose={() => setOpen(false)}
            />
          </Command.Container>
        </Command.Backdrop>
      </Command>
    </>
  )
}

// Mounted only while the palette is open: its resource searches exist only
// during that time.
function CommandPaletteDialog({
  resourceSearch,
  onClose,
}: {
  resourceSearch: ResourceSearchSource[]
  onClose: () => void
}) {
  const router = useRouter()
  const [input, setInput] = useState('')
  const [keyword, setKeyword] = useState('')
  useEffect(() => {
    const timer = window.setTimeout(
      () => setKeyword(input.trim()),
      keywordDebounceMs
    )
    return () => window.clearTimeout(timer)
  }, [input])
  const searches = useQueries({
    queries: resourceSearch.map((source) => ({
      ...source.search(keyword),
      select: (hits: ResourceSearchHit[]) =>
        hits.map((hit) => ({
          key: `${source.path}/${hit.id}`,
          label: hit.label,
          href: `${source.path}/${hit.id}`,
        })),
    })),
  })
  const isSearching = searches.some((search) => search.isFetching)
  const hits = resourceSearch.flatMap((source, index) =>
    (searches[index].data ?? []).map((hit) => ({ ...hit, source }))
  )
  const failed = searches.some((search) => search.isError)
  const targets = new Map<string, string>([
    ...navigation.map((item): [string, string] => [
      `page:${item.href}`,
      item.href,
    ]),
    ...quickActions.map((item): [string, string] => [
      `action:${item.href}`,
      item.href,
    ]),
    ...hits.map((hit): [string, string] => [hit.key, hit.href]),
  ])
  return (
    <Command.Dialog
      aria-label='搜索或跳转'
      inputValue={input}
      onInputChange={setInput}
    >
      <Command.InputGroup>
        <Command.InputGroup.Prefix>
          <Magnifier />
        </Command.InputGroup.Prefix>
        <Command.InputGroup.Input
          aria-label='搜索页面、操作或资源'
          placeholder='搜索页面、操作或资源…'
          // The search field would otherwise spend the first Esc clearing text.
          onKeyDown={(event) => {
            if (event.key === 'Escape') onClose()
          }}
        />
        <Command.InputGroup.ClearButton aria-label='清空搜索' />
        <Command.InputGroup.Suffix>
          {isSearching ? (
            <Spinner size='sm' aria-label='正在搜索资源' />
          ) : (
            <Kbd className='text-xs'>
              <Kbd.Content>Esc</Kbd.Content>
            </Kbd>
          )}
        </Command.InputGroup.Suffix>
      </Command.InputGroup>
      <Command.List
        aria-label='搜索结果'
        onAction={(key) => {
          const href = targets.get(String(key))
          if (!href) return
          onClose()
          void router.navigate({ href })
        }}
        renderEmptyState={() => (
          <p className='py-10 text-center text-sm text-muted'>
            {isSearching ? '正在搜索资源…' : '没有匹配的页面、操作或资源'}
          </p>
        )}
      >
        <Command.Group heading='页面'>
          {navigation.map((item) => (
            <Command.Item
              key={item.href}
              id={`page:${item.href}`}
              textValue={`${item.label} ${item.keywords ?? ''}`}
            >
              <item.icon className='size-4 text-muted' />
              <span>{item.label}</span>
            </Command.Item>
          ))}
        </Command.Group>
        <Command.Group heading='快捷操作'>
          {quickActions.map((item) => (
            <Command.Item
              key={item.href}
              id={`action:${item.href}`}
              textValue={item.label}
            >
              <Plus className='size-4 text-muted' />
              <span>{item.label}</span>
            </Command.Item>
          ))}
        </Command.Group>
        {keyword && hits.length > 0 && (
          <Command.Group heading='资源'>
            {hits.map((hit) => (
              <Command.Item
                key={hit.key}
                id={hit.key}
                // The server already matched these rows (possibly on fields not
                // shown); the keyword keeps them past the palette's text filter.
                textValue={`${hit.label} ${keyword}`}
              >
                <hit.source.icon className='size-4 shrink-0 text-muted' />
                <span className='min-w-0 truncate'>{hit.label}</span>
                <span className='ml-auto shrink-0 text-xs text-muted'>
                  {hit.source.label}
                </span>
              </Command.Item>
            ))}
          </Command.Group>
        )}
      </Command.List>
      <Command.Footer className='justify-between gap-3 text-xs'>
        <span>↑↓ 选择 · Enter 打开 · Esc 关闭</span>
        {failed ? (
          <span className='text-danger'>部分资源搜索失败</span>
        ) : (
          keyword && <span>资源结果为打开时的快照</span>
        )}
      </Command.Footer>
    </Command.Dialog>
  )
}
