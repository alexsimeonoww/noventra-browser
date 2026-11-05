"""Real native Glance size, live resize, controls and document retention.

Run through the owned no-egress namespace and a1920x1080 private Xvfb wrapper.
Uses disposable profiles; never opens, focuses or edits a personal browser.
Native AX dimensions and captured page paint verify geometry independently of
the coordinate calibration used for clicking the preview toolbar.
"""
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time

import glance_workflow_smoke as glance
from glance_workflow_smoke import (
    Budget, Flow, Handler, admit, check, cli, extension_fixture, native,
    native_key, popup_origin, prefs, session, wait_for, write)
from portable_transfer_smoke import protocol
from sidebar_inline_address_smoke import screen_rect


def viewport(flow, source):
    size = flow.b.ev('({w:innerWidth,h:innerHeight})', source)
    tree = flow.tree.read()
    matches = []
    for container in native.visible(tree, r'class_name=ContentsContainerView '):
        identifier = next(key for key, value in tree.nodes.items()
                          if value is container)
        for child in tree.nodes.values():
            if (child['parent'] == identifier and
                    re.search(r'class_name=(?:WebView|ContentsWebView) ', child['line']) and
                    child['bounds'][2:] == (size['w'], size['h'])):
                matches.append({'view': screen_rect(tree, child),
                                'container': screen_rect(tree, container)})
    assert len(matches) == 1, (size, matches)
    return matches[0]


def resize(flow, source, width, height):
    window = protocol(flow.b, 'Browser.getWindowForTarget',
                      {'targetId': source['id']}, source)
    protocol(flow.b, 'Browser.setWindowBounds',
             {'windowId': window['windowId'],
              'bounds': {'width': width, 'height': height}}, source)
    wait_for(lambda: protocol(flow.b, 'Browser.getWindowBounds',
                             {'windowId': window['windowId']}, source)['bounds']['width'] == width,
             flow.b.process, timeout=6)
    time.sleep(.45)


def measure(flow, source, preview, label):
    page = viewport(flow, source)
    tree = flow.tree
    root = native.one(tree, r'class_name=RootView name=Link preview name_from=')
    origin = popup_origin(tree, page['container'], page['view'])
    box = {**origin, 'w': root['bounds'][2], 'h': root['bounds'][3]}
    source_box = page['view']
    inner = flow.b.ev('({w:innerWidth,h:innerHeight})', preview)
    flow.result['native_page_clicks'][-1].update(page)
    image = flow.inp.capture(flow.output / (label + '.png'))
    if label == 'wide':
        # This document measurement is independent of popup coordinate mapping,
        # and makes the legacy fixed-width failure unambiguous.
        check(flow.result, 'wide: preview grows past the old1040px cap',
              inner['w'] > 1150, inner)
    check(flow.result, label + ': screenshot fits the owned wide display',
          source_box['x'] + source_box['w'] <= image.width and
          source_box['y'] + source_box['h'] <= image.height,
          {'display': image.size, 'source': source_box})
    check(flow.result, label + ': preview stays within the source viewport',
          box['x'] >= source_box['x'] and box['y'] >= source_box['y'] and
          box['x'] + box['w'] <= source_box['x'] + source_box['w'] and
          box['y'] + box['h'] <= source_box['y'] + source_box['h'], box)
    check(flow.result, label + ': preview uses nearly full page height',
          box['h'] >= source_box['h'] * .9,
          {'outer': box, 'inner': inner})
    if label == 'wide':
        check(flow.result, 'wide: Zen-style width leaves the source visible',
              .75 <= box['w'] / source_box['w'] <= .86, box)
    else:
        check(flow.result, label + ': narrow and short previews keep usable page space',
              inner['w'] >= min(450, source_box['w'] - 83) and inner['h'] >= 230,
              inner)

    # Named native panes make the page and floating controls measurable without
    # inferring either from the legacy header or an omitted WebView AX node.
    page_nodes = native.visible(tree, r'class_name=\S+ name=Preview page name_from=')
    control_nodes = native.visible(tree, r'class_name=\S+ name=Preview controls name_from=')
    check(flow.result, label + ': headerless preview exposes separate page and controls panes',
          len(page_nodes) == len(control_nodes) == 1,
          {'page': [n['line'] for n in page_nodes],
           'controls': [n['line'] for n in control_nodes]})
    page_rect = tree.rect(page_nodes[0])
    controls = tree.rect(control_nodes[0])
    check(flow.result, label + ': preview has no navigation-arrow buttons',
          not native.visible(tree, r'class_name=ImageButton name=(?:Back|Forward) in preview name_from='))
    check(flow.result, label + ': frameless page starts at the popup edge and fills its height',
          page_rect['x'] == page_rect['y'] == 0 and page_rect['h'] == box['h'],
          {'page': page_rect, 'popup': box})
    check(flow.result, label + ': website viewport loses only its one-pixel border',
          abs(inner['w'] - (page_rect['w'] - 2)) <= 1 and
          abs(inner['h'] - (page_rect['h'] - 2)) <= 1,
          {'page': page_rect, 'inner': inner})
    check(flow.result, label + ': controls occupy a separate compact right gutter',
          controls['w'] == 56 and controls['h'] == 144 and controls['y'] == 15 and
          controls['x'] == page_rect['x'] + page_rect['w'] and
          controls['x'] + controls['w'] == box['w'] and
          0 <= controls['y'] <= controls['y'] + controls['h'] <= box['h'],
          {'page': page_rect, 'controls': controls, 'popup': box})

    preview_root = native.one(tree, r'class_name=PreviewTab ')
    preview_id = next(key for key, node in tree.nodes.items() if node is preview_root)
    def in_preview(node):
        current = node
        while current['parent'] is not None:
            if current['parent'] == preview_id:
                return True
            current = tree.nodes.get(current['parent'])
            if current is None:
                break
        return False
    labels = [node['line'] for node in tree.nodes.values()
              if ' INVISIBLE ' not in node['line'] and
              re.search(r'class_name=Label(?: |$)', node['line']) and in_preview(node)]
    check(flow.result, label + ': preview has no title or URL header labels', not labels, labels)

    # The plain target background verifies native paint independently of the
    # popup-origin calibration. It must reach within one border pixel of the
    # page edges vertically, with no hidden full-width header above it.
    y = box['y'] + page_rect['y'] + page_rect['h'] // 2
    x = box['x'] + page_rect['x'] + page_rect['w'] // 2
    painted_x = [px for px in range(source_box['x'], source_box['x'] + source_box['w'])
                 if image.getpixel((px, y))[:3] == (215, 231, 247)]
    painted_y = [py for py in range(source_box['y'], source_box['y'] + source_box['h'])
                 if image.getpixel((x, py))[:3] == (215, 231, 247)]
    check(flow.result, label + ': captured page paint matches the named native pane',
          bool(painted_x) and
          abs(min(painted_x) - (box['x'] + page_rect['x'] + 1)) <= 2 and
          abs(max(painted_x) - (box['x'] + page_rect['x'] + page_rect['w'] - 2)) <= 2,
          {'paint_span': [min(painted_x), max(painted_x)] if painted_x else [],
           'page': page_rect, 'popup': box})
    check(flow.result, label + ': captured website reaches the top and bottom without a header',
          bool(painted_y) and
          abs(min(painted_y) - (box['y'] + page_rect['y'] + 1)) <= 2 and
          abs(max(painted_y) - (box['y'] + page_rect['y'] + page_rect['h'] - 2)) <= 2,
          {'paint_span': [min(painted_y), max(painted_y)] if painted_y else [],
           'page': page_rect, 'popup': box})
    buttons = {}
    previous = None
    for name in ('Close preview (Esc)', 'Open in tab without reloading',
                 'Open in split without reloading'):
        node = native.one(tree, r'class_name=ImageButton name=' + re.escape(name) + r' name_from=')
        rect = tree.rect(node)
        buttons[name] = rect
        check(flow.result, label + ': ' + name + ' fits the side controls without covering the page',
              rect['w'] == rect['h'] == 32 and
              controls['x'] + 12 == rect['x'] and
              controls['x'] <= rect['x'] < rect['x'] + rect['w'] <= controls['x'] + controls['w'] and
              controls['y'] <= rect['y'] < rect['y'] + rect['h'] <= controls['y'] + controls['h'] and
              rect['x'] >= page_rect['x'] + page_rect['w'] + 12,
              {'button': rect, 'controls': controls, 'page': page_rect})
        check(flow.result, label + ': ' + name + ' keeps its vertical order and spacing',
              rect['y'] == controls['y'] + 12 if previous is None
              else rect['y'] == previous['y'] + previous['h'] + 12, rect)
        previous = rect
    flow.result.setdefault('measurements', {})[label] = {
        'source': source_box, 'popup': box, 'page': page_rect, 'controls': controls,
        'inner': inner, 'buttons': buttons, 'header_labels': labels}
    image.close()
    return box, source_box


def run(flow):
    flow.set_pref(glance.ENABLED, True)
    flow.set_pref(glance.TRIGGER, 0)
    source = flow.create_source('sizing')
    resize(flow, source, 1800, 960)
    flow.b.ev('document.body.style.background="#59256a";true', source)
    source_state = flow.modify_preview(source, 'Source draft survives previews')
    flow.click_page(source, '#external', ('Alt_L',))
    preview = flow.preview(flow.link_url(source, '#external'))
    state = flow.modify_preview(preview, 'Preview draft survives live resize')
    flow.b.ev('document.documentElement.style.background=document.body.style.background="#d7e7f7";true', preview)
    for name, width, height in (('wide', 1800, 960), ('narrow', 700, 480),
                                ('short', 1200, 420), ('restored', 1800, 960)):
        if name != 'wide':
            resize(flow, source, width, height)
        box, source_box = measure(flow, source, preview, name)
        check(flow.result, name + ': live resize retains the same preview document and draft',
              flow.state(preview) == state)
        check(flow.result, name + ': source document and draft remain intact',
              flow.state(source) == source_state)
    # Outside input must stay blocked instead of accidentally clicking the
    # underlying page. Existing explicit Close and Esc behavior is preserved.
    flow.inp.pointer.click(source_box['x'] + 3, source_box['y'] + source_box['h'] // 2)
    time.sleep(.2)
    check(flow.result, 'outside click cannot navigate or modify the underlying source',
          flow.state(source) == source_state)
    # Existing tab-modal behavior can return native focus to the host after an
    # outside click. Send Esc to the preview, rather than assuming that ignored
    # source input also preserves the child widget's keyboard focus.
    flow.inp.pointer.click(box['x'] + box['w'] // 2, box['y'] + box['h'] // 2)
    wait_for(lambda: flow.b.ev('document.hasFocus()', preview), flow.b.process, timeout=4)
    native_key(flow.inp, 'Escape')
    wait_for(lambda: not any(p['id'] == preview['id'] for p in flow.targets()),
             flow.b.process, timeout=6)
    check(flow.result, 'Escape dismisses the resized preview and retains its source',
          flow.state(source) == source_state)
    for action in ('Close preview (Esc)', 'Open in tab without reloading',
                   'Open in split without reloading'):
        flow.click_page(source, '#external', ('Alt_L',))
        preview = flow.preview(flow.link_url(source, '#external'))
        state = flow.modify_preview(preview, action)
        flow.action(action)
        if action == 'Close preview (Esc)':
            wait_for(lambda: not any(p['id'] == preview['id'] for p in flow.targets()),
                     flow.b.process, timeout=6)
            check(flow.result, 'Close icon dismisses the preview', True)
        else:
            wait_for(lambda: any(t['url'] == state['url'] for t in flow.all_tabs()),
                     flow.b.process, timeout=6)
            check(flow.result, action + ' retains the same document and unsaved input',
                  flow.state(preview) == state)
            if action.startswith('Open in split'):
                check(flow.result, 'resized preview joins its source as a real split',
                      flow.tab(source).get('splitViewId', -1) != -1 and
                      flow.tab(preview).get('splitViewId') == flow.tab(source).get('splitViewId'))
            else:
                flow.api('chrome.tabs.remove(' + str(flow.tab(preview)['id']) + ')')
        check(flow.result, action + ' leaves the source document unchanged',
              flow.state(source) == source_state)


def main():
    args = cli(__doc__).parse_args()
    result = admit(args, Path(__file__))
    owner = os.environ.get('NOVENTRA_TRANSFER_OWNED_HOME')
    assert owner and args.output.resolve().is_relative_to(Path(owner).resolve())
    links = json.loads(subprocess.check_output(['/usr/bin/ip', '-j', 'link', 'show'], text=True))
    assert {link['ifname'] for link in links} == {'lo', 'nv-test0'}
    budget = Budget()
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    server.requests = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = args.output / 'profile'
    prefs(profile)
    extension = extension_fixture(profile)
    try:
        with session(args, profile, args.output / 'native', result, budget,
                     (*native.FLAGS, '--load-extension=' + str(extension))) as (b, tree, inp):
            run(Flow(b, tree, inp, result, args.output,
                     f'http://127.0.0.1:{server.server_port}', server.requests))
        check(result, 'owned browser shuts down normally',
              all(r['exit'] == 0 for r in result['normal_shutdowns']))
        result['passed'] = True
    except BaseException as error:
        result['error'] = repr(error)
        raise
    finally:
        server.shutdown()
        result['elapsed_seconds'] = time.monotonic() - budget.started
        budget.close()
        write(args.output / 'result.json', result)


if __name__ == '__main__':
    main()
