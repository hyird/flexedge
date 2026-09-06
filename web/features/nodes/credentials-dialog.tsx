import { Clipboard } from 'lucide-react'
import { toast } from 'sonner'
import { Button } from '@/components/ui/button'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetFooter,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import type { NodeCredentials } from './node-dialog'

export function CredentialsDialog({
  credentials,
  onOpenChange,
}: {
  credentials: NodeCredentials | null
  onOpenChange: (open: boolean) => void
}) {
  return (
    <Sheet open={!!credentials} onOpenChange={onOpenChange}>
      <SheetContent>
        <SheetHeader>
          <SheetTitle>节点接入凭据</SheetTitle>
          <SheetDescription>
            请安全保存密钥，不要通过公开渠道传输。
          </SheetDescription>
        </SheetHeader>
        <div className='space-y-3 px-4'>
          <div>
            <div className='mb-1 text-xs text-muted-foreground'>Node ID</div>
            <code className='block rounded-md bg-muted p-3 text-xs break-all'>
              {credentials?.node_id}
            </code>
          </div>
          <div>
            <div className='mb-1 text-xs text-muted-foreground'>Secret</div>
            <code className='block rounded-md bg-muted p-3 text-xs break-all'>
              {credentials?.secret}
            </code>
          </div>
        </div>
        <SheetFooter>
          <Button
            variant='outline'
            onClick={async () => {
              if (!credentials) return
              await navigator.clipboard.writeText(
                `NODE_ID=${credentials.node_id}\nNODE_SECRET=${credentials.secret}`
              )
              toast.success('凭据已复制')
            }}
          >
            <Clipboard /> 复制凭据
          </Button>
          <Button onClick={() => onOpenChange(false)}>完成</Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}
