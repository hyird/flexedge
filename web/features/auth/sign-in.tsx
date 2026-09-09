import { z } from 'zod'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { useNavigate, useSearch } from '@tanstack/react-router'
import { Activity, Globe2, Loader2, LockKeyhole, Network } from 'lucide-react'
import { toast } from 'sonner'
import { Button } from '@/components/ui/button'
import {
  Card,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card'
import {
  Form,
  FormControl,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import { PasswordInput } from '@/components/password-input'
import { login } from '@/features/auth/data'

const schema = z.object({
  username: z.string().trim().min(1, '请输入用户名'),
  password: z.string().min(1, '请输入密码'),
})

type Values = z.infer<typeof schema>

export function SignIn() {
  const navigate = useNavigate()
  const search = useSearch({ from: '/sign-in' })
  const queryClient = useQueryClient()
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: { username: '', password: '' },
  })

  const mutation = useMutation({
    mutationFn: (values: Values) =>
      login(queryClient, values.username, values.password),
    onSuccess: async (user) => {
      toast.success(`欢迎回来，${user.nickname || user.username}`)
      await navigate({ to: search.redirect || '/', replace: true })
    },
  })

  return (
    <div className='relative grid min-h-svh lg:grid-cols-2'>
      <div className='flex items-center justify-center px-6 py-12'>
        <div className='w-full max-w-sm space-y-8'>
          <div className='flex items-center gap-3'>
            <div className='flex size-10 items-center justify-center rounded-lg bg-primary text-primary-foreground'>
              <Network className='size-5' />
            </div>
            <div>
              <div className='font-semibold'>FlexEdge</div>
              <div className='text-xs text-muted-foreground'>Edge Console</div>
            </div>
          </div>

          <div>
            <h1 className='text-2xl font-bold tracking-tight'>登录控制台</h1>
            <p className='mt-2 text-sm text-muted-foreground'>
              输入管理员凭据以继续管理边缘网络。
            </p>
          </div>

          <Form {...form}>
            <form
              className='grid gap-4'
              onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
            >
              <FormField
                control={form.control}
                name='username'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>用户名</FormLabel>
                    <FormControl>
                      <Input
                        autoComplete='username'
                        placeholder='请输入用户名'
                        {...field}
                      />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <FormField
                control={form.control}
                name='password'
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>密码</FormLabel>
                    <FormControl>
                      <PasswordInput
                        autoComplete='current-password'
                        placeholder='请输入密码'
                        {...field}
                      />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <Button className='mt-2' disabled={mutation.isPending}>
                {mutation.isPending ? (
                  <Loader2 className='animate-spin' />
                ) : (
                  <LockKeyhole />
                )}
                {mutation.isPending ? '正在登录…' : '登录'}
              </Button>
            </form>
          </Form>

          <p className='text-xs leading-relaxed text-muted-foreground'>
            登录会话由 FlexEdge 服务端通过安全 Cookie 管理。
          </p>
        </div>
      </div>

      <div className='relative hidden overflow-hidden border-s bg-muted/40 lg:block'>
        <div className='absolute inset-0 bg-[radial-gradient(circle_at_75%_15%,color-mix(in_oklab,var(--primary)_12%,transparent),transparent_38%)]' />
        <div className='absolute inset-x-16 top-[14%] rounded-xl border bg-background p-5 shadow-2xl'>
          <div className='mb-7 flex items-center justify-between border-b pb-4'>
            <div>
              <p className='text-lg font-semibold'>边缘资源，统一管理</p>
              <p className='text-sm text-muted-foreground'>
                网站接入、节点分发与运行观测
              </p>
            </div>
          </div>
          <div className='grid grid-cols-3 gap-4'>
            {[
              [Globe2, '网站', '域名与回源'],
              [Network, '节点', '接入与分发'],
              [Activity, '观测', '日志与状态'],
            ].map(([Icon, label, value]) => (
              <Card key={String(label)} className='gap-3 py-5 shadow-none'>
                <CardHeader className='px-5'>
                  <div className='flex items-center justify-between'>
                    <CardDescription>{String(label)}</CardDescription>
                    <Icon className='size-4 text-muted-foreground' />
                  </div>
                  <CardTitle className='text-base'>{String(value)}</CardTitle>
                </CardHeader>
              </Card>
            ))}
          </div>
        </div>
      </div>
    </div>
  )
}
