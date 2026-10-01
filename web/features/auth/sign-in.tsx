import { useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { Globe } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { Field, Notice } from '@/components/forms'
import { login } from './data'

export function SignInPage() {
  const client = useQueryClient(),
    navigate = useNavigate()
  const [username, setUsername] = useState(''),
    [password, setPassword] = useState(''),
    [error, setError] = useState(''),
    [busy, setBusy] = useState(false)
  async function submit(event: React.FormEvent) {
    event.preventDefault()
    if (busy) return
    setBusy(true)
    setError('')
    try {
      await login(client, username.trim(), password)
      setPassword('')
      const redirect = new URLSearchParams(location.search).get('redirect')
      await navigate({
        to:
          redirect?.startsWith('/') && !redirect.startsWith('//')
            ? redirect
            : '/',
        replace: true,
      })
    } catch (reason) {
      setError(apiErrorMessage(reason))
    } finally {
      setBusy(false)
    }
  }
  return (
    <main className='flex min-h-dvh flex-col items-center justify-center bg-surface-secondary px-5 py-12'>
      <div className='mb-6 flex items-center gap-3 text-lg font-semibold'>
        <span className='flex size-9 items-center justify-center rounded-xl bg-accent text-accent-foreground'>
          <Globe className='size-5' />
        </span>
        FlexEdge
      </div>
      <section className='w-full max-w-[26rem] rounded-3xl border border-border bg-surface p-7'>
        <div className='mb-7'>
          <p className='mb-2 text-sm text-muted'>边缘网络控制台</p>
          <h1 className='text-2xl font-semibold tracking-tight'>登录工作台</h1>
          <p className='mt-3 text-sm leading-6 text-muted'>
            管理网站、节点、DNS 与证书。
          </p>
        </div>
        <form onSubmit={submit} className='flex flex-col gap-4'>
          <Notice>{error}</Notice>
          <Field
            label='用户名'
            value={username}
            onChange={setUsername}
            required
            maxLength={64}
            autoComplete='username'
            disabled={busy}
          />
          <Field
            label='密码'
            type='password'
            value={password}
            onChange={setPassword}
            required
            autoComplete='current-password'
            disabled={busy}
          />
          <Button
            size='sm'
            type='submit'
            className='mt-2 w-full'
            isPending={busy}
          >
            登录
          </Button>
        </form>
      </section>
      <p className='mt-7 text-xs text-muted'>使用管理员分配的平台账户</p>
    </main>
  )
}
