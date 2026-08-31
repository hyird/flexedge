import { RefreshCw } from 'lucide-react';
import { useMemo } from 'react';
import { Button } from '@/components/ui/button';
import { Combobox, type ComboboxOption } from '@/components/ui/combobox';
import type { DnsLineItem } from '../dns_zone.types';

export interface DnsLineTreeNode {
    value: string;
    title: string;
    selectable?: boolean;
    line?: DnsLineItem;
    children?: DnsLineTreeNode[];
}

interface DnsLineTreeSelectProps {
    lines: DnsLineItem[];
    loading?: boolean;
    refreshing?: boolean;
    value?: string;
    onChange?: (lineCode?: string, line?: DnsLineItem) => void;
    onRefresh?: () => void;
    id?: string;
    invalid?: boolean;
    'aria-describedby'?: string;
}

function splitDisplayName(line: DnsLineItem) {
    const separator = line.line_display_name.indexOf('_');
    if (separator < 0) return [line.line_display_name] as const;
    return [
        line.line_display_name.slice(0, separator),
        line.line_display_name.slice(separator + 1),
    ] as const;
}

function commonPrefixLength(left: string, right: string) {
    const limit = Math.min(left.length, right.length);
    let index = 0;
    while (index < limit && left[index] === right[index]) index += 1;
    return index;
}

function findParent(line: DnsLineItem, linesByName: Map<string, DnsLineItem[]>) {
    const path = splitDisplayName(line);
    if (path.length === 1) return undefined;
    const candidates = linesByName.get(path[0]) ?? [];
    return candidates.reduce<DnsLineItem | undefined>((best, candidate) => {
        if (!best) return candidate;
        return commonPrefixLength(line.line_code, candidate.line_code) >
            commonPrefixLength(line.line_code, best.line_code)
            ? candidate
            : best;
    }, undefined);
}

export function buildDnsLineTreeData(lines: DnsLineItem[]) {
    const linesByName = new Map<string, DnsLineItem[]>();
    const nodes = new Map<string, DnsLineTreeNode>();
    for (const line of lines) {
        const matches = linesByName.get(line.line_name) ?? [];
        matches.push(line);
        linesByName.set(line.line_name, matches);
        nodes.set(line.line_code, {
            value: line.line_code,
            title: line.line_name,
            line,
        });
    }

    const parents = new Map<string, string>();
    const syntheticGroups = new Map<string, DnsLineTreeNode>();
    const syntheticChildren = new Map<string, string>();
    for (const line of lines) {
        const parent = findParent(line, linesByName);
        if (parent && parent.line_code !== line.line_code) {
            parents.set(line.line_code, parent.line_code);
            continue;
        }
        const path = splitDisplayName(line);
        if (path.length === 1) continue;
        const groupCode = `__dns_line_group__${path[0]}`;
        if (!syntheticGroups.has(groupCode)) {
            syntheticGroups.set(groupCode, {
                value: groupCode,
                title: path[0],
                selectable: false,
                children: [],
            });
        }
        syntheticChildren.set(line.line_code, groupCode);
    }

    for (const line of lines) {
        const node = nodes.get(line.line_code);
        if (!node) continue;
        const parentCode = parents.get(line.line_code);
        if (parentCode) {
            const parent = nodes.get(parentCode);
            if (parent) {
                parent.children ??= [];
                parent.children.push(node);
            }
            continue;
        }
        const groupCode = syntheticChildren.get(line.line_code);
        if (groupCode) syntheticGroups.get(groupCode)?.children?.push(node);
    }

    const roots: DnsLineTreeNode[] = [];
    const addedGroups = new Set<string>();
    for (const line of lines) {
        if (parents.has(line.line_code)) continue;
        const groupCode = syntheticChildren.get(line.line_code);
        if (groupCode) {
            if (!addedGroups.has(groupCode)) {
                const group = syntheticGroups.get(groupCode);
                if (group) roots.push(group);
                addedGroups.add(groupCode);
            }
            continue;
        }
        const node = nodes.get(line.line_code);
        if (node) roots.push(node);
    }

    return roots;
}

export default function DnsLineTreeSelect({
    lines,
    loading,
    refreshing,
    value,
    onChange,
    onRefresh,
    id,
    invalid,
    'aria-describedby': ariaDescribedBy,
}: DnsLineTreeSelectProps) {
    const treeData = useMemo(() => buildDnsLineTreeData(lines), [lines]);
    const options = useMemo(() => {
        const result: ComboboxOption[] = [];
        const append = (nodes: DnsLineTreeNode[], parents: string[]) => {
            for (const node of nodes) {
                const path = node.selectable === false ? [...parents, node.title] : parents;
                if (node.selectable !== false) {
                    result.push({
                        value: node.value,
                        label: [...parents, node.title].join(' / '),
                        disabled: node.line?.status === 'disabled',
                    });
                }
                if (node.children) append(node.children, path.length ? path : [node.title]);
            }
        };
        append(treeData, []);
        return result;
    }, [treeData]);

    return (
        <div className="flex gap-2">
            <Combobox
                id={id}
                value={value}
                options={options}
                disabled={loading}
                invalid={invalid}
                aria-describedby={ariaDescribedBy}
                placeholder="请选择 DNS 线路"
                searchPlaceholder="搜索线路…"
                emptyText="没有可用的 DNS 线路"
                className="min-w-0 flex-1"
                onValueChange={(lineCode) =>
                    onChange?.(
                        lineCode,
                        lines.find((line) => line.line_code === lineCode)
                    )
                }
            />
            {onRefresh && (
                <Button variant="outline" disabled={loading} onClick={onRefresh}>
                    <RefreshCw className={refreshing ? 'animate-spin' : undefined} />
                    <span className="hidden sm:inline">刷新线路</span>
                </Button>
            )}
        </div>
    );
}
