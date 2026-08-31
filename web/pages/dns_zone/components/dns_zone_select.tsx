import { useDeferredValue, useEffect, useMemo, useState } from 'react';
import { Combobox } from '@/components/ui/combobox';
import { DNS_PROVIDER_REGISTRY } from '@/config/providers';
import { useDnsZoneOptions } from '../dns_zone.service';
import type { DnsZoneOption, SyncStatus } from '../dns_zone.types';

export interface DnsZoneSelectOption {
    id: string;
    domain: string;
    dns_provider?: DnsZoneOption['dns_provider'];
    dns_provider_name: string;
    sync_status?: SyncStatus;
    available?: boolean;
}

interface DnsZoneSelectProps {
    value?: string;
    onChange?: (value: string) => void;
    enabled?: boolean;
    ownerOf?: string;
    requireAvailable?: boolean;
    seedOptions?: DnsZoneSelectOption[];
    onDnsZoneChange?: (dnsZone: DnsZoneSelectOption | undefined) => void;
    placeholder?: string;
    disabled?: boolean;
    invalid?: boolean;
    className?: string;
    id?: string;
    name?: string;
    onBlur?: () => void;
    'aria-describedby'?: string;
}

function providerLabel(dnsZone: DnsZoneSelectOption) {
    return dnsZone.dns_provider
        ? (DNS_PROVIDER_REGISTRY[dnsZone.dns_provider]?.label ?? dnsZone.dns_provider)
        : undefined;
}

export default function DnsZoneSelect({
    value,
    onChange,
    enabled = true,
    ownerOf,
    requireAvailable = false,
    seedOptions = [],
    onDnsZoneChange,
    placeholder = '选择托管域名',
    disabled,
    invalid,
    className,
    id,
    name,
    onBlur,
    'aria-describedby': ariaDescribedBy,
}: DnsZoneSelectProps) {
    const [search, setSearch] = useState('');
    const [knownOptions, setKnownOptions] = useState<DnsZoneOption[]>([]);
    const keyword = useDeferredValue(search.trim());
    const query = useDnsZoneOptions(
        {
            keyword: keyword || undefined,
            ownerOf: ownerOf || undefined,
            available: requireAvailable ? true : undefined,
        },
        enabled
    );

    useEffect(() => {
        if (!query.data?.length) return;
        setKnownOptions((current) => {
            const values = new Map(current.map((dnsZone) => [dnsZone.id, dnsZone]));
            for (const dnsZone of query.data ?? []) values.set(dnsZone.id, dnsZone);
            return [...values.values()];
        });
    }, [query.data]);

    const dnsZones = useMemo(() => {
        const values = new Map<string, DnsZoneSelectOption>();
        for (const dnsZone of query.data ?? []) values.set(dnsZone.id, dnsZone);
        for (const dnsZone of seedOptions) values.set(dnsZone.id, dnsZone);
        const selectedDnsZone = knownOptions.find((dnsZone) => dnsZone.id === value);
        if (selectedDnsZone) values.set(selectedDnsZone.id, selectedDnsZone);
        return [...values.values()];
    }, [knownOptions, query.data, seedOptions, value]);

    return (
        <Combobox
            id={id}
            name={name}
            value={value}
            options={dnsZones.map((dnsZone) => ({
                value: dnsZone.id,
                label: dnsZone.domain,
                description: [providerLabel(dnsZone), dnsZone.dns_provider_name]
                    .filter(Boolean)
                    .join(' · '),
                disabled: requireAvailable && dnsZone.available !== true,
            }))}
            onSearchChange={setSearch}
            onValueChange={(nextValue) => {
                onChange?.(nextValue);
                onDnsZoneChange?.(dnsZones.find((dnsZone) => dnsZone.id === nextValue));
            }}
            placeholder={placeholder}
            searchPlaceholder="搜索域名或账号…"
            emptyText={ownerOf ? '没有匹配此域名的可用托管域名' : '没有可用的托管域名'}
            loading={query.isLoading || query.isFetching}
            disabled={disabled || !enabled}
            invalid={invalid}
            aria-describedby={ariaDescribedBy}
            className={className}
            onBlur={onBlur}
        />
    );
}
