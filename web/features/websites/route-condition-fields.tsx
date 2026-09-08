import { useFieldArray, type UseFormReturn } from 'react-hook-form'
import { Plus, X } from 'lucide-react'
import { Button } from '@/components/ui/button'
import {
  FormControl,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import type { WebsiteFormValues } from './website-form'

export function RouteConditionFields({
  form,
  index,
}: {
  form: UseFormReturn<WebsiteFormValues>
  index: number
}) {
  const conditions = useFieldArray({
    control: form.control,
    name: `route_rules.${index}.conditions`,
  })
  return (
    <div className='space-y-3 rounded-lg border p-3'>
      <div className='flex items-center justify-between gap-3'>
        <h4 className='text-sm font-medium'>请求条件（全部满足）</h4>
        <Button
          type='button'
          variant='outline'
          size='sm'
          disabled={conditions.fields.length >= 20}
          onClick={() =>
            conditions.append({
              source: 'header',
              name: '',
              op: 'equals',
              value: '',
            })
          }
        >
          <Plus /> 添加条件
        </Button>
      </div>
      <p className='text-xs text-muted-foreground'>
        无条件即不额外筛选。请求头名忽略大小写、值精确匹配；查询参数先 URL
        解码，+
        表示空格。重复字段任一值相等即满足，“不等于”要求字段存在且所有值均不同。
      </p>
      {conditions.fields.map((condition, conditionIndex) => {
        const base =
          `route_rules.${index}.conditions.${conditionIndex}` as const
        const op = form.watch(`${base}.op`)
        const source = form.watch(`${base}.source`)
        return (
          <div
            key={condition.id}
            className='grid items-start gap-3 sm:grid-cols-[1fr_1fr_1fr_1fr_auto]'
          >
            <FormField
              control={form.control}
              name={`${base}.source`}
              render={({ field }) => (
                <FormItem>
                  <FormLabel>条件 {conditionIndex + 1} 来源</FormLabel>
                  <Select value={field.value} onValueChange={field.onChange}>
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      <SelectItem value='header'>请求头</SelectItem>
                      <SelectItem value='query'>查询参数</SelectItem>
                    </SelectContent>
                  </Select>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name={`${base}.name`}
              render={({ field }) => (
                <FormItem>
                  <FormLabel>条件 {conditionIndex + 1} 名称</FormLabel>
                  <FormControl>
                    <Input
                      {...field}
                      placeholder={
                        source === 'header' ? 'X-Channel' : 'channel'
                      }
                    />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name={`${base}.op`}
              render={({ field }) => (
                <FormItem>
                  <FormLabel>条件 {conditionIndex + 1} 操作</FormLabel>
                  <Select
                    value={field.value}
                    onValueChange={(value) => {
                      field.onChange(value)
                      if (value === 'exists' || value === 'absent')
                        form.setValue(`${base}.value`, '', {
                          shouldDirty: true,
                        })
                    }}
                  >
                    <FormControl>
                      <SelectTrigger>
                        <SelectValue />
                      </SelectTrigger>
                    </FormControl>
                    <SelectContent>
                      <SelectItem value='equals'>等于</SelectItem>
                      <SelectItem value='not_equals'>不等于</SelectItem>
                      <SelectItem value='exists'>存在</SelectItem>
                      <SelectItem value='absent'>不存在</SelectItem>
                    </SelectContent>
                  </Select>
                  <FormMessage />
                </FormItem>
              )}
            />
            <FormField
              control={form.control}
              name={`${base}.value`}
              render={({ field }) => (
                <FormItem>
                  <FormLabel>条件 {conditionIndex + 1} 值</FormLabel>
                  <FormControl>
                    <Input
                      {...field}
                      disabled={op === 'exists' || op === 'absent'}
                      placeholder='beta'
                    />
                  </FormControl>
                  <FormMessage />
                </FormItem>
              )}
            />
            <Button
              type='button'
              variant='ghost'
              size='icon'
              className='sm:mt-6'
              aria-label={`移除条件 ${conditionIndex + 1}`}
              onClick={() => conditions.remove(conditionIndex)}
            >
              <X />
            </Button>
          </div>
        )
      })}
    </div>
  )
}
