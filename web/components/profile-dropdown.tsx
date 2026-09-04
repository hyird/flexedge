import { useQuery } from '@tanstack/react-query'
import { LogOut, UserRound } from 'lucide-react'
import { sessionQueryOptions } from '@/lib/auth'
import { initials } from '@/lib/format'
import useDialogState from '@/hooks/use-dialog-state'
import { Avatar, AvatarFallback } from '@/components/ui/avatar'
import { Button } from '@/components/ui/button'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import { SignOutDialog } from '@/components/sign-out-dialog'

export function ProfileDropdown() {
  const { data: user } = useQuery(sessionQueryOptions)
  const [open, setOpen] = useDialogState()
  const name = user?.nickname || user?.username || '管理员'

  return (
    <>
      <DropdownMenu modal={false}>
        <DropdownMenuTrigger asChild>
          <Button
            variant='ghost'
            size='icon'
            className='relative rounded-full'
            aria-label='打开用户菜单'
          >
            <Avatar className='size-8'>
              <AvatarFallback>{initials(name)}</AvatarFallback>
            </Avatar>
          </Button>
        </DropdownMenuTrigger>
        <DropdownMenuContent className='w-56' align='end' forceMount>
          <DropdownMenuLabel className='font-normal'>
            <div className='flex flex-col gap-1.5'>
              <p className='text-sm leading-none font-medium'>{name}</p>
              <p className='text-xs leading-none text-muted-foreground'>
                {user?.username}
              </p>
            </div>
          </DropdownMenuLabel>
          <DropdownMenuSeparator />
          <DropdownMenuItem disabled>
            <UserRound />
            管理员账户
          </DropdownMenuItem>
          <DropdownMenuSeparator />
          <DropdownMenuItem variant='destructive' onClick={() => setOpen(true)}>
            <LogOut />
            退出登录
          </DropdownMenuItem>
        </DropdownMenuContent>
      </DropdownMenu>
      <SignOutDialog open={!!open} onOpenChange={setOpen} />
    </>
  )
}
