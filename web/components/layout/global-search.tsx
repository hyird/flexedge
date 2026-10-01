import { useEffect, useState } from 'react'
import { useNavigate } from '@tanstack/react-router'
import { Magnifier } from '@gravity-ui/icons'
import { Command } from '@heroui-pro/react'
import { Button, Kbd } from '@heroui/react'
import { navigation } from './navigation'

export function GlobalSearch() {
  const [open, setOpen] = useState(false)
  const navigate = useNavigate()
  useEffect(() => {
    const listener = (event: KeyboardEvent) => {
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault()
        setOpen((value) => !value)
      }
    }
    window.addEventListener('keydown', listener)
    return () => window.removeEventListener('keydown', listener)
  }, [])
  return (
    <>
      <Button
        size='sm'
        variant='ghost'
        aria-label='搜索页面'
        onPress={() => setOpen(true)}
      >
        <Magnifier className='size-4' />
        <span className='hidden sm:inline'>搜索页面</span>
        <Kbd className='ml-2 hidden text-xs lg:inline-flex'>
          <Kbd.Content>Ctrl K</Kbd.Content>
        </Kbd>
      </Button>
      <Command>
        <Command.Backdrop isOpen={open} onOpenChange={setOpen}>
          <Command.Container>
            <Command.Dialog aria-label='搜索页面'>
              <Command.InputGroup>
                <Command.InputGroup.Prefix>
                  <Magnifier />
                </Command.InputGroup.Prefix>
                <Command.InputGroup.Input
                  aria-label='搜索页面'
                  placeholder='搜索页面…'
                />
                <Command.InputGroup.ClearButton />
              </Command.InputGroup>
              <Command.List>
                <Command.Group heading='页面'>
                  {navigation.map((item) => (
                    <Command.Item
                      id={item.href}
                      key={item.href}
                      textValue={item.label}
                      onAction={() => {
                        setOpen(false)
                        void navigate({ to: item.href })
                      }}
                    >
                      <item.icon className='size-4 text-muted' />
                      <span>{item.label}</span>
                    </Command.Item>
                  ))}
                </Command.Group>
              </Command.List>
              <Command.Footer>
                <span className='text-xs text-muted'>
                  使用方向键选择，Enter 打开，Esc 关闭
                </span>
              </Command.Footer>
            </Command.Dialog>
          </Command.Container>
        </Command.Backdrop>
      </Command>
    </>
  )
}
