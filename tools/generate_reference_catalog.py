"""Generate presentation-only C++ rows from preserved reference inventories.

No recorded values become live/default retail settings. Empty targets are
unsupported leaves; missing Debug category pages provide explicit unavailable
information. The source port adds presentation data; local.* pages keep their
separately implemented controls in menu_ui.cpp.
Run with --require-all for the final, combined inventory build.
"""
import argparse
import json
import re
from pathlib import Path


DEFAULT_SOURCE_PORT = Path(__file__).resolve().parent.parent / 'data' / 'build15_debug_menu.json'
DEFAULT_BINDING_ROWS = Path(__file__).resolve().parent.parent / 'data' / 'retail_binding_rows.json'
UNAVAILABLE_HEADING = 'Controls unavailable in this build'
ROOT_LABELS = [
    'Common Items', 'Ambient', 'BugIt', 'Camera', 'DebugActor', 'DebugDraw',
    'DebugScriptMessages', 'Designer Debug', 'Enemies', 'Engine', 'GameControls',
    'GameSystems', 'Health', 'Hero', 'HeroTransitions', 'LedgePathDisplay',
    'Loading', 'Nav3D', 'NetworkSystems', 'ObjectiveSystem', 'Prefab Actor Systems',
    'Profiling', 'Rendering', 'Server', 'SonyTest', 'SyncedAnim', 'Traffic',
    'Weapons', 'Reload',
]
# Exact six visible rows in the user-provided Materials screenshot, not values.
MATERIALS = ['Aniso Filtering', 'Force Default Shaders', 'Bypass Material Framegrabs',
             'Bypass Emissive Scaling', 'Bypass Deformers', 'Validate Materials']
PAGE_IDS = {
    'Health': 'debug.health', 'Loading': 'debug.loading', 'Reload Map': 'debug.reload_map',
    'Enemies': 'debug.enemies', 'Aggro': 'debug.aggro', 'Reactions': 'debug.reactions',
    'BehaviorOverrideIfCompEngaged': 'debug.behavior_override_if_comp_engaged',
    'PanicInvisible': 'debug.panic_invisible', 'Stealth': 'debug.stealth',
    'DebugFollow': 'debug.debug_follow',
    'GameSystems': 'debug.game_systems', 'NetworkSystems': 'debug.network_systems',
}
LOCAL_TARGETS = {'local.common', 'local.hero', 'local.health', 'local.rendering', 'local.engine'}
# Exact GotoMenu routes retained in native_dev_catalog.cpp. NativeDevPages()
# appends these populated destination pages in the emitted C++ catalog.
RETAIL_NAVIGATION_TARGETS = {
    ('dev', 'Tracking Demo'): 'dev.retail.tracking_demo',
    ('dev.missions', 'Act 2 Missions'): 'dev.retail.act_2_missions',
    ('dev.missions', 'Act 3 Missions'): 'dev.retail.act_3_missions',
    ('dev.missions', 'Silver Path'): 'dev.retail.silver_path',
}


def label(text):
    text = re.sub(r'\s*\[\?.*?\]', '', text).strip().rstrip('>').strip()
    text = re.sub(r'\s*\(Current:\s*\d+\)', '', text).strip()
    # A blurred fragment is not an invented complete internal field name.
    return text.split('...')[0].strip()


def page_id(name):
    name = label(name)
    return PAGE_IDS.get(name, 'debug.' + re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_'))


def cpp_string(text):
    # Three-digit octal escapes preserve exact UTF-8 bytes without C++ hex
    # escape continuation or JSON-surrogate/universal-character ambiguities.
    result = '"'
    for byte in text.encode('utf-8'):
        if byte in (34, 92):
            result += '\\' + chr(byte)
        elif 32 <= byte < 127:
            result += chr(byte)
        else:
            result += '\\%03o' % byte
    return result + '"'


def read(path, required=True):
    if not path.is_file():
        if required:
            raise ValueError('Missing reference inventory: ' + str(path))
        return None
    return json.loads(path.read_text(encoding='utf-8-sig'))


def merge_source_port(pages, source_port):
    """Merge presentation data without replacing existing controls or routes."""
    source = read(source_port)
    if source.get('schema') != 1:
        raise ValueError('Unsupported source-port schema')
    additions = source.get('pages', [])
    by_id = {}
    for page in additions:
        if set(page) != {'id', 'title', 'developer', 'rows'}:
            raise ValueError('Source-port page contains non-presentation metadata')
        identifier = page['id']
        if not isinstance(identifier, str) or not identifier.startswith(('debug', 'local.')):
            raise ValueError('Invalid source-port page ID: ' + repr(identifier))
        if identifier in by_id:
            raise ValueError('Duplicate source-port page ID: ' + identifier)
        if not isinstance(page['title'], str) or not page['title'] or page['developer'] is not False:
            raise ValueError('Invalid source-port page title/style: ' + identifier)
        by_id[identifier] = page
        for entry in page['rows']:
            if set(entry) != {'label', 'target', 'heading', 'help'}:
                raise ValueError('Source-port row contains non-presentation metadata: ' + identifier)
            if not all(isinstance(entry[k], str) for k in ('label', 'target', 'help')) or type(entry['heading']) is not bool:
                raise ValueError('Invalid source-port row shape: ' + identifier)
            if not entry['label'] or any(ord(c) < 32 for c in entry['label']):
                raise ValueError('Invalid source-port label: ' + repr(entry['label']))
    if 'debug' not in by_id or [r['label'] for r in by_id['debug']['rows']] != ROOT_LABELS:
        raise ValueError('Source port must preserve the 29 Debug roots and order')
    for page in additions:
        for entry in page['rows']:
            if entry['target'] and entry['target'] not in by_id:
                raise ValueError('Unresolved source-port target: ' + entry['target'])
    reached = set()
    pending = ['debug']
    while pending:
        identifier = pending.pop()
        if identifier in reached:
            continue
        reached.add(identifier)
        pending.extend(r['target'] for r in by_id[identifier]['rows'] if r['target'])
    if reached != set(by_id):
        raise ValueError('Unreachable source-port pages: ' + repr(sorted(set(by_id) - reached)))
    if source.get('counts', {}).get('pages') != len(additions) or source.get('counts', {}).get('rows') != sum(len(p['rows']) for p in additions):
        raise ValueError('Source-port counts do not match its presentation data')

    current = {p['id']: p for p in pages}
    root = current['debug']
    if [(r['label'], r['target'], r['heading']) for r in root['rows']] != [
            (r['label'], r['target'], r['heading']) for r in by_id['debug']['rows']]:
        raise ValueError('Source port would change an existing Debug root route')
    for page in additions:
        identifier = page['id']
        if identifier not in current:
            added = {k: v for k, v in page.items() if k != 'rows'}
            added['rows'] = [dict(r) for r in page['rows']] or [
                {'label': 'No additional controls available', 'target': '', 'heading': True, 'help': ''}]
            pages.append(added)
            current[identifier] = added
            continue
        destination = current[identifier]
        # Match only rows that predate this merge. Exact duplicate labels in
        # original source pages retain their separate rows and source order.
        existing_rows = list(destination['rows'])
        if page['rows']:
            destination['rows'] = [r for r in destination['rows'] if not (
                r['heading'] and r['label'] == UNAVAILABLE_HEADING)]
        for entry in page['rows']:
            matches = [r for r in existing_rows if r['label'] == entry['label']]
            if matches:
                for existing in matches:
                    if (existing['target'], existing['heading']) != (entry['target'], entry['heading']):
                        raise ValueError('Conflicting existing source-port row: ' + identifier + '/' + entry['label'])
                    if entry['help'] and not existing.get('help'):
                        existing['help'] = entry['help']
            else:
                destination['rows'].append(dict(entry))


def merge_binding_rows(pages, path):
    additions = read(path)
    if additions.get('schema') != 1:
        raise ValueError('Unsupported binding-row schema')
    indexed = {p['id']: p for p in pages}
    seen = set()
    for item in additions['rows']:
        if set(item) != {'page', 'label', 'help'} or not all(isinstance(v, str) for v in item.values()):
            raise ValueError('Binding rows must contain presentation strings only')
        key = (item['page'], item['label'])
        if key in seen or item['page'] not in indexed or not item['label']:
            raise ValueError('Invalid or duplicate original binding row: ' + repr(key))
        seen.add(key)
        page = indexed[item['page']]
        existing = [r for r in page['rows'] if r['label'] == item['label']]
        if existing:
            if any(r['heading'] or r['target'] for r in existing):
                raise ValueError('Binding would replace navigation or heading: ' + repr(key))
            for row in existing:
                if not row.get('help'):
                    row['help'] = item['help']
        else:
            page['rows'] = [r for r in page['rows'] if not (r['heading'] and r['label'] in
                (UNAVAILABLE_HEADING, 'No additional controls available'))]
            page['rows'].append({'label': item['label'], 'target': '', 'heading': False, 'help': item['help']})


def make_catalog(directory, require_all, source_port=DEFAULT_SOURCE_PORT, binding_rows=DEFAULT_BINDING_ROWS):
    original = read(directory / 'inventory.json')
    validated = read(directory / 'inventory-debug-validated.json')
    late = read(directory / 'inventory-debug-late.json', require_all)
    dev = read(directory / 'inventory-dev.json', require_all)
    observed = {}
    original_pages = {page_id(p['name']): p for p in original['debug_menu']['pages']}
    removed = {page_id(p['page']) for p in (late or {}).get('remove_misattributed_pages', [])}
    for inventory in (original, validated, late):
        if inventory:
            for page in inventory.get('debug_menu', {}).get('pages', []):
                identifier = page_id(page['name'])
                if inventory is original and identifier in removed:
                    continue  # Later Follow belongs to DebugFollow, not PanicInvisible.
                if page.get('merge_entries_into_existing_page') and identifier in observed:
                    current = dict(observed[identifier])
                    current['entries'] = list(current.get('entries', []))
                    existing = {label(e['label']) for e in current['entries']}
                    current['entries'].extend(e for e in page.get('entries', []) if label(e['label']) not in existing)
                    observed[identifier] = current
                else:
                    observed[identifier] = page

    for correction in (late or {}).get('corrections', []):
        identifier = page_id(correction['page'])
        page = observed[identifier]
        entries = page.get('entries', [])
        if any(label(e['label']) == label(correction['label']) for e in entries):
            continue  # Independently resolved by the earlier validation too.
        matching = [e for e in entries if e['label'] == correction['original_label']]
        if not matching:
            # The independent early reviewer may have supplied a different
            # approximate reading of the same row. The source inventory's
            # exact original_label fixes its ordinal without fuzzy guessing.
            baseline = original_pages[identifier].get('entries', [])
            indices = [i for i, e in enumerate(baseline) if e['label'] == correction['original_label']]
            if len(indices) == 1 and len(baseline) == len(entries):
                matching = [entries[indices[0]]]
        if len(matching) != 1:
            raise ValueError('Cannot uniquely apply reference correction: ' + repr(correction))
        matching[0]['label'] = correction['label']

    pages = []
    def add(identifier, title, developer, rows):
        pages.append({'id': identifier, 'title': title, 'developer': developer, 'rows': rows})
    def row(text, target='', heading=False):
        return {'label': label(text), 'target': target, 'heading': heading}

    roots = {'Common Items': 'local.common', 'Engine': 'local.engine', 'Hero': 'local.hero',
             'DebugDraw': 'debug.debug_draw', 'DebugScriptMessages': 'debug.script_messages',
             'Weapons': 'debug.weapons',
             'Traffic': 'debug.traffic', 'Prefab Actor Systems': 'debug.prefab_actor_systems',
             'Rendering': 'debug.rendering', 'Reload': 'debug.reload_map'}
    roots.update({label(p['name']): identifier for identifier, p in observed.items()})
    add('debug', 'Debug Menu', False, [row(name, roots.get(name, '')) for name in ROOT_LABELS])
    names = {label(page['name']): identifier for identifier, page in observed.items()}
    for identifier, page in observed.items():
        rows = [row(e['label'], names.get(label(e['label']), '')) for e in page.get('entries', [])]
        for group in page.get('groups', []):
            rows.append(row(group['name'], heading=True))
            rows.extend(row(e['label'], names.get(label(e['label']), '')) for e in group['entries'])
        if identifier == 'debug.health':
            rows.append(row('Local health controls', 'local.health'))
        add(identifier, label(page['name']), False, rows)
    # These additions come from reviewed retained retail fields, not unseen
    # console screenshot rows. Provenance: debug-runtime-expansion-bindings.json.
    add('debug.debug_draw', 'DebugDraw', False, [row('Show tile boundaries')])
    add('debug.script_messages', 'DebugScriptMessages', False,
        [row('Prototype messages'), row('Debug messages')])
    add('debug.weapons', 'Weapons', False, [row('Infinite ammo')])
    # Traffic/Pedestrians comes from SM2Launcher-main/Debug Menu Guide.docx.
    # The population control is a reviewed retained-retail addition; no unseen
    # development-build options are inferred from its reflected field names.
    add('debug.traffic', 'Traffic', False, [row('Pedestrians', 'debug.traffic.pedestrians')])
    add('debug.traffic.pedestrians', 'Pedestrians', False, [row('Enable Pedestrians')])
    add('debug.prefab_actor_systems', 'Prefab Actor Systems', False, [row('All systems enabled')])
    add('debug.rendering', 'Rendering', False,
        [row('Materials', 'debug.materials'), row('Window tools', 'local.rendering'),
         row('Motion blur'), row('Visual effects processing')])
    add('debug.materials', 'Materials', False, [row(name) for name in MATERIALS])

    if dev:
        descriptors = {p['id']: p for p in dev['pages']}
        for identifier, descriptor in descriptors.items():
            rows = []
            children = sorted((e for e in dev['entries'] if e.get('parent') == identifier), key=lambda e: e['order'])
            for entry in children:
                # A recorded player name and help footer are not menu options.
                if entry['id'] in ('dev.debug_frontend.player', 'dev.debug_frontend.start_help'):
                    continue
                target = ''
                if entry['id'] == 'dev.debug_frontend.open_debug':
                    target = 'debug'  # UI navigation only; never Start Game.
                elif entry['kind'] == 'children' and entry.get('child_content_observed') and entry['id'] in descriptors:
                    target = entry['id']
                if not target:
                    target = RETAIL_NAVIGATION_TARGETS.get((identifier, label(entry['label'])), '')
                rows.append(row(entry['label'], target, entry['kind'] == 'header'))
            add(identifier, descriptor['title'], descriptor['style'] != 'compact_debug', rows)
    else:
        # Initial compile surface while the independent developer inventory is
        # still being transcribed. Unobserved children remain disabled.
        add('dev', 'DEV MENU', True, [row(e['label']) for e in original['dev_menu']['root']])

    # Every listed Debug category is browsable, as requested. Missing original
    # contents are represented by an explicit information page, not invented
    # controls or callable engine actions. Only these category-list pages are
    # expanded: unavailable actions in Health/Reload/etc. remain actions.
    category_lists = {'debug', 'debug.enemies', 'debug.game_systems', 'debug.network_systems'}
    for parent in [p for p in pages if p['id'] in category_lists]:
        for entry in parent['rows']:
            if entry['heading'] or entry['target']:
                continue
            suffix = re.sub(r'[^a-z0-9]+', '_', entry['label'].lower()).strip('_')
            entry['target'] = parent['id'] + '.' + suffix
            add(entry['target'], entry['label'], False,
                [row(UNAVAILABLE_HEADING, heading=True)])

    merge_source_port(pages, source_port)
    merge_binding_rows(pages, binding_rows)

    ids = [p['id'] for p in pages]
    if len(ids) != len(set(ids)):
        raise ValueError('Duplicate catalog page ID')
    valid = set(ids) | LOCAL_TARGETS | set(RETAIL_NAVIGATION_TARGETS.values())
    for page in pages:
        for entry in page['rows']:
            if not entry['label'] or any(ord(c) < 32 for c in entry['label']):
                raise ValueError('Empty/control-character catalog label: ' + repr(entry['label']))
            if entry['target'] and entry['target'] not in valid:
                raise ValueError('Unresolved catalog target: ' + entry['target'])
    return pages


def emit(pages):
    lines = ['// SPDX-License-Identifier: GPL-3.0-or-later',
             '// Generated by tools/generate_reference_catalog.py from preserved reference and source-port evidence.',
             '// Labels/navigation/help only. No recorded values, native addresses or callbacks are embedded.',
             '#include "reference_catalog.h"', '#include "native_dev_catalog.h"', '', 'namespace sm2debug {',
             'const std::vector<ReferencePage>& ReferencePages() {',
             '    static const std::vector<ReferencePage> pages = [] {',
             '    std::vector<ReferencePage> result{']
    for page in pages:
        lines.append('        {%s, %s, %s, {' % (cpp_string(page['id']), cpp_string(page['title']), str(page['developer']).lower()))
        for entry in page['rows']:
            lines.append('            {%s, %s, %s, %s},' % (cpp_string(entry['label']), cpp_string(entry['target']),
                str(entry['heading']).lower(), cpp_string(entry.get('help', ''))))
        lines.append('        }},')
    lines += ['    };',
              '    ReferencePage index{"dev.retail.index", "ALL RETAINED DEV MENUS", true, {}};',
              '    for (const auto& page : NativeDevPages()) {',
              '        index.rows.push_back({page.title, page.id, false});',
              '        result.push_back(page);',
              '    }',
              '    for (auto& page : result) if (page.id == "dev")',
              '        page.rows.push_back({"All retail Dev menus", index.id, false});',
              '    result.push_back(std::move(index));',
              '    return result;', '    }();', '    return pages;', '}',
              'const ReferencePage* FindReferencePage(const std::string& id) {',
              '    for (const auto& page : ReferencePages()) if (page.id == id) return &page;',
              '    return nullptr;', '}', '} // namespace sm2debug', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser()
    project = Path(__file__).resolve().parent.parent
    parser.add_argument('--inventory-directory', type=Path, default=project / 'verification' / 'reference-video')
    parser.add_argument('--output', type=Path, default=project / 'src' / 'reference_catalog.cpp')
    parser.add_argument('--source-port', type=Path, default=DEFAULT_SOURCE_PORT)
    parser.add_argument('--require-all', action='store_true')
    parser.add_argument('--binding-rows', type=Path, default=DEFAULT_BINDING_ROWS)
    args = parser.parse_args()
    pages = make_catalog(args.inventory_directory, args.require_all, args.source_port, args.binding_rows)
    args.output.write_text(emit(pages), encoding='utf-8', newline='\n')
    print(json.dumps({'output': str(args.output), 'reference_pages': len(pages),
                      'reference_rows': sum(len(p['rows']) for p in pages),
                      'native_dev_catalog_included_at_runtime': True,
                      'reference_values_embedded': False}))


if __name__ == '__main__':
    main()
