import { ReloadOutlined } from '@ant-design/icons';
import { Alert, Button, Card, Col, Row, Space, Statistic, Typography } from 'antd';
import { useNavigate } from 'react-router-dom';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { StatusBadge } from '@/components/status_badge';
import { cn } from '@/lib/utils';
import { formatDateTime } from '@/utils/date';
import { useOverview } from './overview.service';
import type { OverviewTask } from './overview.types';

const { Paragraph, Text, Title } = Typography;

type StatusTone = 'success' | 'warning' | 'destructive' | 'info' | 'neutral';

interface ResourceCardProps {
    title: string;
    description: string;
    value?: number;
    loading: boolean;
    onClick: () => void;
}

function ResourceCard({ title, description, value, loading, onClick }: ResourceCardProps) {
    return (
        <Card hoverable onClick={onClick}>
            <Statistic title={title} value={value ?? 0} loading={loading} />
            <Text type="secondary">{description}</Text>
        </Card>
    );
}

const taskStatusMeta: Record<
    OverviewTask['status'],
    { tone: StatusTone; label: string; pulse?: boolean }
> = {
    pending: { tone: 'info', label: '待同步', pulse: true },
    running: { tone: 'info', label: '同步中', pulse: true },
    retry: { tone: 'warning', label: '等待重试', pulse: true },
    completed: { tone: 'success', label: '已完成' },
};

function taskOperationLabel(task: OverviewTask) {
    if (task.kind === 'provider') return '检测供应商';
    if (task.kind === 'dns') return task.operation.startsWith('sync') ? '同步 DNS' : '删除 DNS';
    if (task.kind === 'certificate') return task.operation === 'issue' ? '签发证书' : '续签证书';
    return task.operation === 'apply' ? '下发网站配置' : '删除网站配置';
}

function resourcePath(task: OverviewTask) {
    if (task.resource_type === 'website') return '/websites';
    if (task.resource_type === 'certificate') return '/certificates';
    if (task.resource_type === 'dns_zone') return '/dns-zones';
    return undefined;
}

interface IssueAlertProps {
    message: string;
    onClick?: () => void;
}

function IssueAlert({ message, onClick }: IssueAlertProps) {
    return (
        <Alert
            showIcon
            type="error"
            message={message}
            action={
                onClick ? (
                    <Button size="small" onClick={onClick}>
                        查看
                    </Button>
                ) : undefined
            }
        />
    );
}

export default function OverviewPage() {
    const navigate = useNavigate();
    const overview = useOverview();
    const resources = overview.data?.resources;
    const issues = overview.data?.issues;
    const issueCount =
        (issues?.dns_zone_issue_count ?? 0) +
        (issues?.certificate_expiring_count ?? 0) +
        (issues?.certificate_failed_count ?? 0) +
        (issues?.failed_task_count ?? 0);
    const taskColumns: DataTableColumn<OverviewTask>[] = [
        {
            key: 'resource',
            header: '同步对象',
            cell: (task) => {
                const path = resourcePath(task);
                return path ? (
                    <Button
                        type="link"
                        size="small"
                        className="max-w-52"
                        onClick={() => navigate(path)}
                    >
                        <span className="truncate" title={task.resource_name}>
                            {task.resource_name}
                        </span>
                    </Button>
                ) : (
                    <span className="block max-w-52 truncate" title={task.resource_name}>
                        {task.resource_name}
                    </span>
                );
            },
        },
        {
            key: 'operation',
            header: '操作',
            cell: (task) => taskOperationLabel(task),
        },
        {
            key: 'status',
            header: '状态',
            cell: (task) => {
                const meta = taskStatusMeta[task.status];
                return (
                    <StatusBadge tone={meta.tone} pulse={meta.pulse}>
                        {meta.label}
                    </StatusBadge>
                );
            },
        },
        {
            key: 'error',
            header: '错误信息',
            cell: (task) => (
                <span
                    className={cn(
                        'block max-w-96 whitespace-normal break-words',
                        task.last_error && 'text-destructive'
                    )}
                    title={task.last_error}
                >
                    {task.last_error || '—'}
                </span>
            ),
        },
        {
            key: 'updated',
            header: '最后更新',
            cell: (task) => (
                <span className="whitespace-nowrap tabular-nums">
                    {formatDateTime(task.updated_at)}
                </span>
            ),
        },
    ];

    return (
        <div className="h-full overflow-y-auto p-4">
            <div className="flex flex-col gap-6">
                <div className="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
                    <div>
                        <Title level={2}>首页概览</Title>
                        <Paragraph type="secondary">查看控制面资源、异常状态和待处理同步</Paragraph>
                    </div>
                    <Button
                        icon={<ReloadOutlined />}
                        loading={overview.isFetching}
                        onClick={() => overview.refetch()}
                    >
                        刷新
                    </Button>
                </div>

                {overview.isError && (
                    <Alert
                        showIcon
                        type="error"
                        message="概览数据加载失败"
                        description="请刷新页面重试。"
                    />
                )}

                <Row gutter={[16, 16]}>
                    <Col xs={24} sm={12} xl={6}>
                        <ResourceCard
                            title="网站"
                            description="已接入的加速站点"
                            value={resources?.website_count}
                            loading={overview.isLoading}
                            onClick={() => navigate('/websites')}
                        />
                    </Col>
                    <Col xs={24} sm={12} xl={6}>
                        <ResourceCard
                            title="绑定域名"
                            description="当前服务域名总数"
                            value={resources?.domain_count}
                            loading={overview.isLoading}
                            onClick={() => navigate('/websites')}
                        />
                    </Col>
                    <Col xs={24} sm={12} xl={6}>
                        <ResourceCard
                            title="证书"
                            description="托管的 TLS 证书"
                            value={resources?.certificate_count}
                            loading={overview.isLoading}
                            onClick={() => navigate('/certificates')}
                        />
                    </Col>
                    <Col xs={24} sm={12} xl={6}>
                        <ResourceCard
                            title="集群"
                            description="边缘交付集群"
                            value={resources?.cluster_count}
                            loading={overview.isLoading}
                            onClick={() => navigate('/clusters')}
                        />
                    </Col>
                </Row>

                <Row gutter={[16, 16]} align="stretch">
                    <Col xs={24} xl={8}>
                        <Card title="状态检查" className="h-full">
                            <Space orientation="vertical" size="middle" className="w-full">
                                {issueCount === 0 ? (
                                    <Alert
                                        showIcon
                                        type="success"
                                        message="控制面状态正常"
                                        description="没有发现 DNS、证书或同步异常"
                                    />
                                ) : (
                                    <>
                                        {Boolean(issues?.dns_zone_issue_count) && (
                                            <IssueAlert
                                                message={`${issues?.dns_zone_issue_count} 个 DNS 托管域名同步异常`}
                                                onClick={() => navigate('/dns-zones')}
                                            />
                                        )}
                                        {Boolean(issues?.certificate_expiring_count) && (
                                            <IssueAlert
                                                message={`${issues?.certificate_expiring_count} 张证书将在 30 天内到期`}
                                                onClick={() => navigate('/certificates')}
                                            />
                                        )}
                                        {Boolean(issues?.certificate_failed_count) && (
                                            <IssueAlert
                                                message={`${issues?.certificate_failed_count} 张证书签发、续签失败或已过期`}
                                                onClick={() => navigate('/certificates')}
                                            />
                                        )}
                                        {Boolean(issues?.failed_task_count) && (
                                            <IssueAlert
                                                message={`${issues?.failed_task_count} 个同步标记执行失败`}
                                            />
                                        )}
                                    </>
                                )}
                                <Statistic
                                    title="正在处理的同步"
                                    value={issues?.active_task_count ?? 0}
                                />
                            </Space>
                        </Card>
                    </Col>
                    <Col xs={24} xl={16}>
                        <Card
                            title="待处理同步"
                            extra={<Text type="secondary">最近需要执行或关注的资源同步</Text>}
                            styles={{ body: { padding: 0 } }}
                            className="h-full"
                        >
                            <div className="h-80">
                                <DataTable
                                    columns={taskColumns}
                                    data={overview.data?.recent_tasks}
                                    getRowKey={(task) => task.id}
                                    loading={overview.isLoading}
                                    emptyTitle="暂无待处理同步"
                                    emptyDescription="资源发生变更后会显示在这里"
                                />
                            </div>
                        </Card>
                    </Col>
                </Row>
            </div>
        </div>
    );
}
