import type { UseFormReturn } from 'react-hook-form'
import {
  FormControl,
  FormDescription,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import { Textarea } from '@/components/ui/textarea'
import type { WebsiteFormValues } from './website-form'

export function RouteMetadataFields({
  form,
  index,
}: {
  form: UseFormReturn<WebsiteFormValues>
  index: number
}) {
  return (
    <div className='space-y-4'>
      <FormField
        control={form.control}
        name={`route_rules.${index}.name`}
        render={({ field }) => (
          <FormItem>
            <FormLabel>规则名称（可选）</FormLabel>
            <FormControl>
              <Input placeholder='例如 API 服务分流' {...field} />
            </FormControl>
            <FormMessage />
          </FormItem>
        )}
      />
      <div className='grid items-start gap-4 sm:grid-cols-2'>
        <FormField
          control={form.control}
          name={`route_rules.${index}.hostnames`}
          render={({ field }) => (
            <FormItem>
              <FormLabel>匹配域名（可选）</FormLabel>
              <FormControl>
                <Textarea
                  className='[field-sizing:fixed] h-24 min-h-24 resize-none'
                  placeholder={'api.example.com\nwww.example.com'}
                  value={field.value.join('\n')}
                  onBlur={field.onBlur}
                  ref={field.ref}
                  onChange={(event) =>
                    field.onChange(event.target.value.split('\n'))
                  }
                />
              </FormControl>
              <FormDescription>
                每行一个精确域名；留空匹配此网站全部域名。忽略大小写和请求端口，不支持通配符，且不会自动绑定域名。
              </FormDescription>
              <FormMessage />
            </FormItem>
          )}
        />
        <FormField
          control={form.control}
          name={`route_rules.${index}.description`}
          render={({ field }) => (
            <FormItem>
              <FormLabel>备注（可选）</FormLabel>
              <FormControl>
                <Textarea
                  className='[field-sizing:fixed] h-24 min-h-24 resize-none'
                  placeholder='说明用途或维护注意事项'
                  {...field}
                />
              </FormControl>
              <FormDescription>仅用于管理，不影响请求处理。</FormDescription>
              <FormMessage />
            </FormItem>
          )}
        />
      </div>
    </div>
  )
}
