"""Normalize inherited browser names in compiled Noventra locale packs.

The English resource with the same ID identifies legal credits and external
providers. Internal page URLs follow the English reference; external URLs,
IDs, placeholders, other HTML attributes and non-text resources stay intact.
Run before hashing a new snapshot; never modify a published snapshot in place.
"""
import json
from pathlib import Path
import re
import sys


_URL = re.compile(r'((?:[a-zA-Z][a-zA-Z0-9+.-]*://|mailto:)[^\s<>"\']+)')
_INTERNAL_URL = re.compile(r'(?:chrome|helium)://([a-z0-9-]+)(?=[/?#]|$)')
_HREF = re.compile(r'((?<![\w:-])href\s*=\s*)(["\'])(.*?)\2', re.IGNORECASE)


def _normalize_internal_url(url, hosts):
    internal = _INTERNAL_URL.match(url)
    if internal and internal[1] in hosts and url.startswith('helium://'):
        return 'chrome://' + url[len('helium://'):]
    return url


def normalize_resources(resources, reference):
    result = dict(resources)
    for resource_id, value in resources.items():
        try:
            text = value.decode('utf-8')
            english = reference.get(resource_id, b'').decode('utf-8')
        except UnicodeDecodeError:
            continue
        if 'Helium' not in text and 'helium://' not in text:
            continue
        if 'The Helium Authors' in english or 'Helium filters' in english:
            continue
        # Upstream substitutions also renamed Google's store and OS in some
        # translations. Use their real provider names, not a fictitious store.
        provider = re.search(r'Chrome (?:Web Store|OS)|ChromeOS|Web Store', english)
        name = 'Chrome' if provider else 'Noventra'
        # Only rewrite internal destinations identified by the matching English
        # message. Tokenizing URLs keeps external paths and query values intact.
        hosts = {internal[1] for url in _URL.finditer(english)
                 if (internal := _INTERNAL_URL.match(url[0]))}
        parts = re.split(r'(<[^>]*>)', text)
        for i in range(0, len(parts), 2):
            segments = _URL.split(parts[i])
            for j, segment in enumerate(segments):
                segments[j] = (_normalize_internal_url(segment, hosts) if j % 2
                               else segment.replace('Helium', name))
            parts[i] = ''.join(segments)
        for i in range(1, len(parts), 2):
            parts[i] = _HREF.sub(
                lambda match: (match[1] + match[2] +
                               _normalize_internal_url(match[3], hosts) +
                               match[2]), parts[i])
        result[resource_id] = ''.join(parts).encode('utf-8')
    return result


def normalize_locales(runtime, source_root=None):
    runtime = Path(runtime)
    source_root = Path(source_root) if source_root else Path(__file__).resolve().parents[2] / 'build/src'
    grit_path = str(source_root / 'tools/grit')
    sys.path.insert(0, grit_path)
    try:
        from grit.format import data_pack
    finally:
        sys.path.remove(grit_path)
    reference = data_pack.ReadDataPack(runtime / 'locales/en-US.pak').resources
    report = {}
    for path in sorted((runtime / 'locales').glob('*.pak')):
        pack = data_pack.ReadDataPack(path)
        if pack.encoding != data_pack.UTF8:
            raise ValueError(f'Expected UTF-8 locale pack: {path}')
        resources = normalize_resources(pack.resources, reference)
        changed = [key for key in resources if resources[key] != pack.resources[key]]
        if changed:
            temporary = path.with_suffix('.pak.tmp')
            data_pack.WriteDataPack(resources, temporary, pack.encoding)
            reread = data_pack.ReadDataPack(temporary)
            if reread.resources != resources or reread.encoding != pack.encoding:
                raise ValueError(f'Locale pack failed round-trip verification: {path}')
            temporary.replace(path)
        report[path.name] = changed
    (runtime / 'locale-branding.json').write_text(json.dumps(report, indent=2) + '\n')
    return report
