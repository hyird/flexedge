import { z } from 'zod';

export const CLUSTER_NAME_MAX_LENGTH = 100;
export const HOSTNAME_PREFIX_MAX_LENGTH = 63;
export const HOSTNAME_PREFIX_PATTERN = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/;

export const clusterFormSchema = z.object({
    name: z
        .string()
        .trim()
        .min(1, '集群名称不能为空')
        .max(CLUSTER_NAME_MAX_LENGTH, '集群名称最多100个字符'),
    dns_zone_id: z.string().uuid('请选择托管域名'),
    hostname_prefix: z
        .string()
        .trim()
        .min(1, '主机前缀不能为空')
        .max(HOSTNAME_PREFIX_MAX_LENGTH, '主机前缀最多63个字符')
        .regex(HOSTNAME_PREFIX_PATTERN, '主机前缀格式不正确'),
    status: z.enum(['enabled', 'disabled'], { error: '集群状态不能为空' }),
});

export type ClusterFormValues = z.infer<typeof clusterFormSchema>;
