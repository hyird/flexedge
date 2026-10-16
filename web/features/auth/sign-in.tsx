import { type FormEvent, useState } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { Field, Notice } from '@/components/forms'
import { login } from './data'
import styles from './sign-in.module.css'

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
    <main className='relative isolate flex min-h-dvh items-center justify-center bg-background px-5 py-12 sm:px-8'>
      <div aria-hidden='true' className={styles.background} />
      <div className='relative flex w-full max-w-sm flex-col gap-8'>
        <h1 className='page-title'>登录</h1>
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
    </main>
  )
}
