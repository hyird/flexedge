import { type FormEvent, useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { Globe, Layers, ShieldCheck, Signal } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { Field, Notice } from '@/components/forms'
import { login } from './data'

const highlights = [
  { icon: Layers, text: '集群与边缘节点的接入、配置和实时运行状态' },
  { icon: Globe, text: '网站路由、回源和 HTTPS 配置分发到全部节点' },
  { icon: Signal, text: '域名解析与证书自动同步、签发和续期' },
]

export function SignInPage() {
  const client = useQueryClient()
  const navigate = useNavigate()
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  async function submit(event: FormEvent) {
    event.preventDefault()
    if (busy) return
    setBusy(true)
    setError('')
    try {
      await login(client, username.trim(), password)
      setPassword('')
      // Only same-origin paths are accepted; `//host` would leave the console.
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
    <main className='grid min-h-dvh bg-background lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]'>
      <section
        aria-label='FlexEdge 边缘控制台'
        className='relative hidden flex-col justify-between overflow-hidden border-r border-border bg-surface p-10 lg:flex xl:p-14'
      >
        <div className='flex items-center gap-2.5'>
          <span className='flex size-9 items-center justify-center rounded-lg bg-accent text-accent-foreground'>
            <Globe className='size-5' />
          </span>
          <span className='flex flex-col'>
            <span className='text-sm font-semibold'>FlexEdge</span>
            <span className='text-xs text-muted'>边缘控制台</span>
          </span>
        </div>
        <div className='flex max-w-md flex-col gap-8'>
          <div className='flex flex-col gap-3'>
            <h2 className='text-3xl leading-tight font-semibold tracking-tight'>
              自托管的边缘网络，
              <br />
              一个控制台管理到底。
            </h2>
            <p className='text-sm leading-6 text-muted'>
              配置提交后由控制面分发到边缘节点，运行状态与访问日志实时回传。
            </p>
          </div>
          <ul className='flex flex-col gap-4'>
            {highlights.map((item) => (
              <li key={item.text} className='flex items-start gap-3 text-sm'>
                <span className='flex size-8 shrink-0 items-center justify-center rounded-lg border border-border bg-surface-secondary text-muted'>
                  <item.icon className='size-4' />
                </span>
                <span className='pt-1.5 leading-5'>{item.text}</span>
              </li>
            ))}
          </ul>
        </div>
        <p className='flex items-center gap-2 text-xs text-muted'>
          <ShieldCheck className='size-3.5' />
          仅限获得授权的平台管理员访问。
        </p>
      </section>
      <section className='flex flex-col items-center justify-center px-5 py-12 sm:px-8'>
        <div className='flex w-full max-w-sm flex-col gap-8'>
          <div className='flex items-center gap-2.5 lg:hidden'>
            <span className='flex size-9 items-center justify-center rounded-lg bg-accent text-accent-foreground'>
              <Globe className='size-5' />
            </span>
            <span className='flex flex-col'>
              <span className='text-sm font-semibold'>FlexEdge</span>
              <span className='text-xs text-muted'>边缘控制台</span>
            </span>
          </div>
          <div className='flex flex-col gap-1.5'>
            <h1 className='text-xl font-semibold tracking-tight'>登录控制台</h1>
            <p className='text-sm text-muted'>使用管理员分配的平台账户登录。</p>
          </div>
          <form onSubmit={submit} className='flex flex-col gap-4' noValidate>
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
              type='submit'
              className='mt-2 w-full'
              isPending={busy}
              isDisabled={!username.trim() || !password}
            >
              登录
            </Button>
          </form>
        </div>
      </section>
    </main>
  )
}
