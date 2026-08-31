import { X } from 'lucide-react';
import { useDeferredValue, useMemo, useState } from 'react';
import { Combobox, type ComboboxOption } from '@/components/ui/combobox';
import { DNS_PROVIDER_REGISTRY } from '@/config/providers';
import { cn } from '@/lib/utils';
import { useDnsProviderDetail, useDnsProviderList } from '../dns_provider.service';
import type { DnsProviderItem } from '../dns_provider.types';

export interface DnsProviderSelectOption extends ComboboxOption {
    provider: DnsProviderItem;
}

export interface DnsProviderSelectProps {
    value?: string;
    onChange?: (value: string | undefined, option?: DnsProviderSelectOption) => void;
    onDnsProviderChange?: (provider: DnsProviderItem | undefined) => void;
    enabled?: boolean;
    requireVerified?: boolean;
    allowClear?: boolean;
    placeholder?: string;
    searchPlaceholder?: string;
    emptyText?: string;
    className?: string;
    disabled?: boolean;
    invalid?: boolean;
    id?: string;
    name?: string;
    onBlur?: () => void;
}

function optionLabel(provider: DnsProviderItem) {
    return `${DNS_PROVIDER_REGISTRY[provider.provider].label} · ${provider.name}`;
}

export default function DnsProviderSelect({
    value,
    onChange,
    onDnsProviderChange,
    enabled = true,
    requireVerified = false,
    allowClear = false,
    placeholder,
    searchPlaceholder = '搜索账号名称或账户 ID',
    emptyText = '没有匹配的 DNS 服务商账号',
    className,
    disabled = false,
    invalid = false,
    id,
    name,
    onBlur,
}: DnsProviderSelectProps) {
    const [search, setSearch] = useState('');
    const keyword = useDeferredValue(search.trim());
    const query = useDnsProviderList(
        {
            page: 1,
            pageSize: 20,
            keyword: keyword || undefined,
            status: requireVerified ? 'verified' : undefined,
        },
        enabled
    );
    const selected = useDnsProviderDetail(enabled && value ? value : undefined);
    const providers = useMemo(() => {
        const values = new Map((query.data?.list ?? []).map((provider) => [provider.id, provider]));
        if (selected.data) values.set(selected.data.id, selected.data);
        return [...values.values()];
    }, [query.data?.list, selected.data]);
    const options = useMemo<DnsProviderSelectOption[]>(
        () =>
            providers.map((provider) => ({
                value: provider.id,
                label: optionLabel(provider),
                description: provider.account_id,
                disabled: requireVerified && provider.status !== 'verified',
                provider,
            })),
        [providers, requireVerified]
    );

    const selectValue = (nextValue: string | undefined) => {
        const option = options.find((candidate) => candidate.value === nextValue);
        onChange?.(nextValue, option);
        onDnsProviderChange?.(option?.provider);
    };

    return (
        <fieldset
            name={name}
            className={cn('relative m-0 min-w-0 border-0 p-0', className)}
            onBlur={onBlur}
        >
            <Combobox
                id={id}
                value={value}
                options={options}
                onValueChange={selectValue}
                onSearchChange={setSearch}
                placeholder={placeholder}
                searchPlaceholder={searchPlaceholder}
                emptyText={emptyText}
                loading={query.isLoading || selected.isLoading}
                disabled={!enabled || disabled}
                invalid={invalid}
                className={cn('w-full', allowClear && value && 'pr-14')}
            />
            {allowClear && value && !disabled && enabled && (
                <button
                    type="button"
                    aria-label="清除已选 DNS 服务商账号"
                    className="absolute top-1/2 right-8 z-10 -translate-y-1/2 rounded-sm p-0.5 text-muted-foreground hover:bg-accent hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
                    onMouseDown={(event) => event.preventDefault()}
                    onClick={() => selectValue(undefined)}
                >
                    <X className="size-3.5" />
                </button>
            )}
        </fieldset>
    );
}
