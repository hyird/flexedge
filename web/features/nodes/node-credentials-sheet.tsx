import { type ReactNode } from 'react'
import { Alert, Button } from '@heroui/react'
import { CopyButton } from '@/components/copy-button'
import { FormSheet } from '@/components/form-sheet'
import { Mono } from '@/components/mono'
import type { NodeCredentials } from './data'
import { nodeInstallCommand } from './install-command'

function SecretValue({
  label,
  value,
  copyLabel,
}: {
  label: string
  value: string
  copyLabel: string
}) {
  return (
    <div className='flex flex-col gap-1.5'>
      <p className='text-xs text-muted'>{label}</p>
      <div className='flex items-start gap-2 rounded-lg border border-border bg-surface-secondary py-2 pr-1.5 pl-3'>
        <Mono className='min-w-0 flex-1 py-1 break-all'>{value}</Mono>
        <CopyButton value={value} label={copyLabel} />
      </div>
    </div>
  )
}

/** Shows freshly issued or revealed agent credentials with the install
 * command. Credentials only live in component state, never in the URL or the
 * query cache. */
export function NodeCredentialsSheet({
  credentials,
  nodeName,
  onClose,
  footer,
}: {
  credentials: NodeCredentials | null
  nodeName?: string
  onClose: () => void
  /** Replaces the default `完成` button. */
  footer?: ReactNode
}) {
  const command = credentials
    ? nodeInstallCommand(window.location.origin, credentials)
    : ''
  return (
    <FormSheet
      isOpen={credentials !== null}
      onClose={onClose}
      title='节点接入凭据'
      description={
        nodeName
          ? `在节点 ${nodeName} 的服务器上以 root 运行安装命令，节点将使用以下凭据接入控制面。`
          : '在节点服务器上以 root 运行安装命令，节点将使用以下凭据接入控制面。'
      }
      footer={
        footer ?? (
          <Button size='sm' onPress={onClose}>
            完成
          </Button>
        )
      }
    >
      {credentials && (
        <>
          <Alert status='warning'>
            <Alert.Indicator />
            <Alert.Content>
              <Alert.Description>
                凭据等同于节点的登录密码，请勿外传。重置凭据后旧凭据立即失效。
              </Alert.Description>
            </Alert.Content>
          </Alert>
          <div className='flex flex-col gap-1.5'>
            <div className='flex items-center justify-between gap-2'>
              <p className='text-sm font-semibold'>安装命令</p>
              <CopyButton value={command} label='复制安装命令' />
            </div>
            <pre
              aria-label='安装命令'
              className='rounded-lg border border-border bg-surface-secondary p-3 font-mono text-[13px] leading-6 break-all whitespace-pre-wrap'
            >
              {command}
            </pre>
            <p className='text-xs text-muted'>
              需要 Linux 与 systemd；脚本会下载节点程序、写入凭据并启动
              flexedge-node 服务。
            </p>
          </div>
          <SecretValue
            label='Node ID'
            value={credentials.node_id}
            copyLabel='复制 Node ID'
          />
          <SecretValue
            label='Secret'
            value={credentials.secret}
            copyLabel='复制 Secret'
          />
        </>
      )}
    </FormSheet>
  )
}
