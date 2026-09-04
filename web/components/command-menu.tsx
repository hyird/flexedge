import { useCallback } from 'react'
import { useNavigate } from '@tanstack/react-router'
import { ArrowRight, Laptop, Moon, Sun } from 'lucide-react'
import { useSearch } from '@/context/search-provider'
import { useTheme } from '@/context/theme-provider'
import {
  CommandDialog,
  CommandEmpty,
  CommandGroup,
  CommandInput,
  CommandItem,
  CommandList,
  CommandSeparator,
} from '@/components/ui/command'
import { sidebarData } from './layout/data/sidebar-data'
import { ScrollArea } from './ui/scroll-area'

export function CommandMenu() {
  const navigate = useNavigate()
  const { setTheme } = useTheme()
  const { open, setOpen } = useSearch()
  const run = useCallback(
    (command: () => unknown) => {
      setOpen(false)
      command()
    },
    [setOpen]
  )

  return (
    <CommandDialog modal open={open} onOpenChange={setOpen}>
      <CommandInput placeholder='搜索页面或命令…' />
      <CommandList>
        <ScrollArea type='hover' className='h-72 pe-1'>
          <CommandEmpty>没有匹配结果。</CommandEmpty>
          {sidebarData.navGroups.map((group) => (
            <CommandGroup key={group.title} heading={group.title}>
              {group.items.flatMap((item) =>
                item.url ? (
                  <CommandItem
                    key={item.url}
                    value={item.title}
                    onSelect={() => run(() => navigate({ to: item.url! }))}
                  >
                    <ArrowRight className='size-3 text-muted-foreground' />
                    {item.title}
                  </CommandItem>
                ) : (
                  (item.items ?? []).map((subItem) => (
                    <CommandItem
                      key={subItem.url}
                      value={`${item.title} ${subItem.title}`}
                      onSelect={() => run(() => navigate({ to: subItem.url }))}
                    >
                      <ArrowRight className='size-3 text-muted-foreground' />
                      {subItem.title}
                    </CommandItem>
                  ))
                )
              )}
            </CommandGroup>
          ))}
          <CommandSeparator />
          <CommandGroup heading='主题'>
            <CommandItem onSelect={() => run(() => setTheme('light'))}>
              <Sun /> 亮色
            </CommandItem>
            <CommandItem onSelect={() => run(() => setTheme('dark'))}>
              <Moon /> 暗色
            </CommandItem>
            <CommandItem onSelect={() => run(() => setTheme('system'))}>
              <Laptop /> 跟随系统
            </CommandItem>
          </CommandGroup>
        </ScrollArea>
      </CommandList>
    </CommandDialog>
  )
}
