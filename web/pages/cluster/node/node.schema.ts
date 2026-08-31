import { z } from 'zod';
import { isIpAddress } from '@/utils/ip_address';

export const NODE_NAME_MAX_LENGTH = 100;
export const IP_ADDRESS_MAX_LENGTH = 45;
export const NODE_IP_MAX_COUNT = 8;

const nodeEndpointSchema = z.object({
    id: z.string().optional(),
    ip_address: z
        .string()
        .min(1, 'IP 地址不能为空')
        .max(IP_ADDRESS_MAX_LENGTH, 'IP 地址最多45个字符')
        .refine((value) => isIpAddress(value.trim()), 'IP 地址格式不正确'),
    line_code: z.string().min(1, '请选择 DNS 线路'),
    line_name: z.string().optional(),
});

export const nodeFormSchema = z
    .object({
        cluster_id: z.string().min(1, '请选择所属集群'),
        name: z
            .string()
            .trim()
            .min(1, '节点名称不能为空')
            .max(NODE_NAME_MAX_LENGTH, '节点名称最多100个字符'),
        endpoints: z
            .array(nodeEndpointSchema)
            .min(1, '请至少填写一个 IP')
            .max(NODE_IP_MAX_COUNT, `最多配置${NODE_IP_MAX_COUNT}个 IP`),
        status: z.enum(['enabled', 'disabled'], { error: '节点状态不能为空' }),
    })
    .superRefine((values, context) => {
        const addresses = new Set<string>();
        values.endpoints.forEach((endpoint, index) => {
            const address = endpoint.ip_address.trim();
            if (!address || !addresses.has(address)) {
                addresses.add(address);
                return;
            }
            context.addIssue({
                code: 'custom',
                message: 'IP 地址不能重复',
                path: ['endpoints', index, 'ip_address'],
            });
        });
    });

export type NodeFormValues = z.infer<typeof nodeFormSchema>;
