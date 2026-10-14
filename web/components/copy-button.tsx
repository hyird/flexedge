import { useEffect, useState } from 'react'
import { Check, Copy } from '@gravity-ui/icons'
import { Button, Tooltip, toast } from '@heroui/react'

/** Icon button copying `value` to the clipboard, with tooltip and a brief
 * confirmation state. `label` names what is copied (`复制指纹`). */
export function CopyButton({
  value,
  label = '复制',
}: {
  value: string
  label?: string
}) {
  const [copied, setCopied] = useState(false)
  useEffect(() => {
    if (!copied) return
    const timer = window.setTimeout(() => setCopied(false), 1500)
    return () => window.clearTimeout(timer)
  }, [copied])
  return (
    <Tooltip delay={300}>
      <Button
        size='sm'
        variant='ghost'
        isIconOnly
        aria-label={label}
        className='size-7 min-w-7'
        onPress={() => {
          navigator.clipboard.writeText(value).then(
            () => setCopied(true),
            () => toast.danger('无法写入剪贴板，请手动复制')
          )
        }}
      >
        {copied ? (
          <Check className='size-3.5 text-success' />
        ) : (
          <Copy className='size-3.5' />
        )}
      </Button>
      <Tooltip.Content>{copied ? '已复制' : label}</Tooltip.Content>
    </Tooltip>
  )
}
