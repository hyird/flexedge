"""Generate the configurable detector catalog from the pinned upstream CRS sources."""
import argparse
import json
import pathlib
import re

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
args = parser.parse_args()
entries = []
for source in sorted(args.source.glob('REQUEST-*.conf')):
    lines = source.read_text(encoding='utf-8').splitlines()
    index = 0
    while index < len(lines):
        block = lines[index]
        index += 1
        if not block.startswith('SecRule '):
            continue
        while block.rstrip().endswith('\\') and index < len(lines):
            block = block.rstrip()[:-1] + lines[index].strip()
            index += 1
        identity = re.search(r'\bid:(\d+)', block)
        message = re.search(r"\bmsg:'((?:\\.|[^'])*)'", block)
        if not identity or not message:
            continue
        rule_id = int(identity[1])
        # Initialization, PL gates and anomaly-evaluation machinery are not detectors.
        if not 910000 <= rule_id < 949000:
            continue
        level = re.search(r"tag:'paranoia-level/(\d)'", block)
        category = re.search(r"tag:'(attack-[^']+)'", block)
        entries.append((rule_id, message[1].replace("\\'", "'"), category[1] if category else source.stem[12:], int(level[1]) if level else 1))
entries.sort()
assert entries and len({item[0] for item in entries}) == len(entries)
quote = lambda value: json.dumps(value, ensure_ascii=True)
contents = '#pragma once\n#include <array>\n#include <cstdint>\n#include <string_view>\nnamespace flexedge::security {\n'
contents += 'struct crs_rule { std::uint32_t id_; std::string_view name_, category_; std::uint32_t paranoia_level_; };\n'
contents += 'inline constexpr auto crs_catalog = std::to_array<crs_rule>({\n'
contents += ''.join('{'+str(identity)+','+quote(name)+','+quote(category)+','+str(level)+'},\n' for identity, name, category, level in entries)
contents += '});\ninline constexpr std::string_view crs_version = "4.25.0";\n}\n'
args.output.parent.mkdir(parents=True, exist_ok=True)
if not args.output.exists() or args.output.read_text(encoding='utf-8') != contents:
    args.output.write_text(contents, encoding='utf-8')
