import { useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { TrashBin } from '@gravity-ui/icons'
import { Button, toast } from '@heroui/react'
import { queryKeys } from '@/lib/query-keys'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { removeWebsite } from './data'
import { useMarkWebsiteDeleted, useWebsite } from './website-context'
import { websiteName } from './website-display'

/** `删除网站` for the website shown by the detail layout: a centered
 * confirmation states the consequence, then the page returns to the list.
 * Enabling and disabling stay the `状态` field of the basic settings group,
 * saved with the rest of the draft. */
export function WebsiteLifecycleActions() {
  const website = useWebsite()
  const markDeleted = useMarkWebsiteDeleted()
  const client = useQueryClient()
  const navigate = useNavigate()
  const [removing, setRemoving] = useState(false)
  const name = websiteName(website)
  return (
    <>
      <Button
        size='sm'
        variant='danger-soft'
        className='self-start'
        onPress={() => setRemoving(true)}
      >
        <TrashBin />
        删除网站
      </Button>
      <ConfirmDialog
        isOpen={removing}
        onClose={() => setRemoving(false)}
        title={`删除网站 ${name}？`}
        description='网站配置会从所有边缘节点撤下，域名将不再由边缘处理。此操作无法撤销。'
        confirmLabel='删除网站'
        onConfirm={async () => {
          await removeWebsite(website)
          markDeleted()
          toast.success(`网站 ${name} 已删除`)
          // The website is gone, so an unsaved settings draft is moot.
          await navigate({
            to: '/websites',
            replace: true,
            ignoreBlocker: true,
          })
          client.removeQueries({
            queryKey: [...queryKeys.websites, website.id],
          })
          void client.invalidateQueries({ queryKey: queryKeys.websites })
        }}
      />
    </>
  )
}
