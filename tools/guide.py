#!/usr/bin/env python3
"""Build, check, serve or combine the DriveLink guide with the signed apt tree."""
from __future__ import annotations

import argparse
import hashlib
import html
from html.parser import HTMLParser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import shutil
from string import Template
import subprocess
import sys
import tempfile
import unicodedata
from urllib.parse import quote, unquote, urljoin, urlsplit

from markdown_it import MarkdownIt

import version


ROOT = Path(__file__).resolve().parents[1]
GUIDE = ROOT / 'guide'
OUT = ROOT / 'build/guide'
PROJECT = 'https://github.com/taruntom1/drivelink_v2'
BASE = '/drivelink_v2/'
PAGES_URL = 'https://taruntom1.github.io' + BASE.rstrip('/')
KEY = 'drivelink-archive-keyring.asc'
FINGERPRINT = ROOT / 'packaging/apt/FINGERPRINT'
APT_PATHS = {'dists', 'pool', KEY}
MERGED = 'drivelink-merged.bin'
FIGURES = {
    'connect': ('The Connect page has separate Serial and Agent tabs.',
                'Connect to your controller with the DriveLink desktop app.'),
    'device': ('The Device page lists the controller status and firmware version.',
               'See the controller’s status and firmware version in the app.'),
    'layout-add-instance': ('The Add instance dialog lists module types and their compatible loops.',
                            'Choose a module type and the loop it will run on.'),
    'layout-review': ('The Layout review lists staged changes with their old and new values.',
                      'Review pending changes before saving and rebooting.'),
    'monitor-command-disarmed': ('The Manual command card shows duty_sp and velocity_sp while disarmed.',
                                 'Set up the duty input while disarmed. The example uses a simulated drive.'),
    'monitor': ('The Monitor plots show live traces beside the signal list and Manual command card.',
                'Monitor plots and manual commands on a simulated drive.'),
    'tune': ('The Tune page shows step traces with the target and measured velocity.',
             'Compare step runs on the same plot.'),
    'tune-identify': ('Plant identification shows a fitted response and suggested PI gains.',
                      'A simulated response and its fit. Use the duty you verified on your own bench.'),
    'layout-calibration-applied': ('The Calibration tab shows four levels at 3 and the sensor orientation.',
                                   'A simulated IMU with four calibration levels at 3.'),
    'update-confirmed': ('The firmware update dialog shows Confirmed after reconnecting.',
                         'The example virtual device has booted and confirmed its new image.'),
    'remote-agent-found': ('The Agent tab lists an agent found on the local network.',
                           'Select a discovered agent to fill in its address.'),
    'remote-agent': ('The Agent tab lists the available devices and their connection state.',
                     'Choose an online device from the agent’s list.'),
    'remote-monitor-observer': ('Monitor shows live data while ROS holds the control lease.',
                                'You can watch the device while another client controls it.'),
    'remote-take-ros': ('The Take lease confirmation identifies ROS as the current holder.',
                        'A takeover stops the current holder. Confirm only when it is safe to stop.'),
}
SUBSTITUTION = re.compile(r'\{\{([^{}]+)\}\}')


class GuideError(ValueError):
    pass


def read(path):
    return path.read_text(encoding='utf-8')


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


def heading_ids(state):
    """Give headings stable IDs and style labeled callouts."""
    used = set()
    for index, token in enumerate(state.tokens):
        if token.type == 'heading_open':
            label = unicodedata.normalize('NFKD', state.tokens[index + 1].content).lower()
            slug = re.sub(r'[^a-z0-9]+', '-', label).strip('-') or 'section'
            identifier = slug
            suffix = 2
            while identifier in used:
                identifier = f'{slug}-{suffix}'
                suffix += 1
            used.add(identifier)
            token.attrSet('id', identifier)
        if token.type == 'blockquote_open':
            label = next((item.content for item in state.tokens[index + 1:]
                          if item.type == 'inline'), '')
            match = re.match(r'\*\*(Note|Warning|Safety)\.\*\*', label)
            if match:
                token.attrSet('class', f'callout {match[1].lower()}')


def markdown():
    # Guide contributors own the templates as well as the Markdown.
    parser = MarkdownIt('commonmark', {'html': True}).enable(['table', 'strikethrough'])
    parser.core.ruler.after('inline', 'guide_ids', heading_ids)
    parser.renderer.rules['table_open'] = lambda *_: '<div class="table-scroll"><table>\n'
    parser.renderer.rules['table_close'] = lambda *_: '</table></div>\n'
    return parser


def navigation():
    groups = json.loads(read(GUIDE / 'navigation.json'))
    pages = [page for group in groups for page in group['pages']]
    slugs = [page['slug'] for page in pages]
    sources = [page['source'] for page in pages]
    if slugs.count('') != 1 or len(set(slugs)) != len(slugs):
        raise GuideError('navigation must have one home page and unique paths')
    if any(slug and not re.fullmatch(r'[a-z0-9]+(?:[-/][a-z0-9]+)*', slug) for slug in slugs):
        raise GuideError('navigation contains an invalid page path')
    expected = {path.relative_to(GUIDE / 'pages').as_posix()
                for path in (GUIDE / 'pages').rglob('*.md')}
    if set(sources) != expected or len(set(sources)) != len(sources):
        raise GuideError('every Markdown page must occur in the navigation exactly once')
    return groups, pages


def nav_html(groups, active):
    parts = []
    for group in groups:
        title = html.escape(group['title'])
        if not group['pages']:
            parts.append(f'<p class="future-section">{title}</p>')
            continue
        parts.extend([f'<h2>{title}</h2>', '<ul>'])
        for page in group['pages']:
            slug = page['slug']
            href = BASE + (slug + '/' if slug else '')
            current = ' aria-current="page"' if slug == active else ''
            parts.append(f'<li><a href="{href}"{current}>{html.escape(page["title"])}</a></li>')
        parts.append('</ul>')
    return '\n'.join(parts)


def fingerprint(text):
    value = ''.join(text.split()).upper()
    if not re.fullmatch(r'[0-9A-F]{40}', value):
        raise GuideError('apt fingerprint must contain 40 hexadecimal digits')
    return ' '.join(value[index:index+4] for index in range(0, len(value), 4))


def asset_url(name, release):
    # GitHub Release normalizes the tilde in Debian asset names to a dot.
    # SHA256SUMS and the apt pool use the original name.
    return f'{PROJECT}/releases/download/v{release}/{quote(name.replace("~", "."))}'


def asset_links(release):
    names = {
        'windows': (f'DriveLink-{release}-setup.exe', 'Download the Windows installer'),
        'gui_amd64': (f'drivelink_{release}~ubuntu26.04_amd64.deb', 'Download the amd64 package'),
        'gui_arm64': (f'drivelink_{release}~ubuntu26.04_arm64.deb', 'Download the arm64 package'),
        'cli_amd64': (f'drivelink-cli_{release}~ubuntu26.04_amd64.deb', 'Download the amd64 dependency'),
        'cli_arm64': (f'drivelink-cli_{release}~ubuntu26.04_arm64.deb', 'Download the arm64 dependency'),
        'firmware': (f'drivelink-firmware-{release}.zip', 'Download the firmware zip'),
        'checksums': ('SHA256SUMS', 'Download SHA256SUMS'),
    }
    return {key: f'[{label}]({asset_url(name, release)})' for key, (name, label) in names.items()}


def firmware_info(folder, release, strict):
    if folder is None:
        if strict:
            raise GuideError('a release build needs the firmware package')
        return None
    info = json.loads(read(folder / 'flash_args.json'))
    settings = info['flash_settings']
    package_version = info['version']
    if package_version != release and (strict or not package_version.startswith(release + '+g')):
        raise GuideError(f'firmware version {package_version} does not match {release}')
    if (info['chip'] != 'esp32s3' or info['merged'] != {'offset': '0x0', 'file': MERGED}
            or not (folder / MERGED).is_file() or (folder / MERGED).stat().st_size == 0):
        raise GuideError('the firmware package needs a merged ESP32-S3 image at offset 0')
    for key, permitted in [('flash_mode', r'qio|qout|dio|dout'),
                           ('flash_size', r'16MB'), ('flash_freq', r'(20|26|40|80)m')]:
        if not re.fullmatch(permitted, settings[key]):
            raise GuideError(f'unexpected firmware setting {key}: {settings[key]}')
    return info


def flash_files(out, folder, info):
    if info is None:
        return ('<div class="flash-control"><p>The browser installer needs a firmware package. '
                'Use the <a href="#flash-with-esptool">esptool steps</a> to flash from a release.</p></div>')
    path = out / 'flash'
    path.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(folder / MERGED, path / MERGED)
    manifest = {
        'name': 'DriveLink', 'version': info['version'], 'new_install_prompt_erase': False,
        'builds': [{'chipFamily': 'ESP32-S3', 'parts': [{'path': MERGED, 'offset': 0}]}],
    }
    write(path / 'manifest.json', json.dumps(manifest, indent=2) + '\n')
    return f"""<div class="flash-control">
<script type="module" src="{BASE}assets/esp-web-tools/install-button.js"></script>
<esp-web-install-button manifest="{BASE}flash/manifest.json">
  <button type="button" slot="activate">Connect</button>
  <span slot="unsupported">This browser does not support Web Serial.</span>
  <span slot="not-allowed">Open this page over HTTPS or on localhost to use Web Serial.</span>
</esp-web-install-button>
<p>If the browser cannot connect, <a href="#flash-with-esptool">use esptool</a>.</p>
</div>"""


def esptool_command(info):
    settings = info['flash_settings'] if info else {
        'flash_mode': 'dio', 'flash_size': '16MB', 'flash_freq': '80m',
    }
    flags = ' '.join(f'--{key} {settings[key]}' for key in ('flash_mode', 'flash_size', 'flash_freq'))
    return f'-m esptool --chip esp32s3 -p PORT -b 921600 write_flash {flags} 0x0 {MERGED}'


def copy_assets(out):
    assets = out / 'assets'
    shutil.copytree(GUIDE / 'assets', assets)
    resources = ROOT / 'host/gui/resources'
    for folder, names in [('fonts', ['Inter-Regular.ttf', 'Inter-SemiBold.ttf', 'JetBrainsMono-Regular.ttf']),
                          ('icons', ['activity.svg'])]:
        destination = assets / folder
        destination.mkdir()
        for name in names:
            shutil.copyfile(resources / folder / name, destination / name)
    vendor = GUIDE / 'vendor/esp-web-tools'
    destination = assets / 'esp-web-tools'
    destination.mkdir()
    for path in vendor.glob('*.js'):
        shutil.copyfile(path, destination / path.name)
    notices = assets / 'third-party'
    notices.mkdir()
    for source, name in [
        (resources / 'fonts/Inter-OFL.txt', 'Inter-OFL.txt'),
        (resources / 'fonts/JetBrainsMono-OFL.txt', 'JetBrainsMono-OFL.txt'),
        (resources / 'icons/LICENSE', 'Lucide-LICENSE.txt'),
        (vendor / 'LICENSE', 'ESP-Web-Tools-LICENSE.txt'),
        (GUIDE / 'vendor/NOTICES.md', 'NOTICES.md'),
    ]:
        shutil.copyfile(source, notices / name)
    for license_file in (GUIDE / 'vendor/licenses').glob('*.txt'):
        shutil.copyfile(license_file, notices / license_file.name)
    links = ''.join(f'<li><a href="{quote(file.name)}">{html.escape(file.name)}</a></li>'
                    for file in sorted(notices.iterdir()))
    write(notices / 'index.html',
          '<!doctype html><html lang="en"><meta charset="utf-8"><title>Third-party notices</title>'
          f'<body><main><h1>Third-party notices</h1><ul>{links}</ul></main></body></html>\n')


def png_size(path):
    with path.open('rb') as stream:
        header = stream.read(24)
    if header[:16] != b'\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR':
        raise GuideError(f'not a PNG: {path}')
    return int.from_bytes(header[16:20], 'big'), int.from_bytes(header[20:24], 'big')


def screenshot(out, shots, scenario, strict):
    alt, caption = FIGURES[scenario]
    if shots is None and not strict:
        return (f'<figure><p class="figure-placeholder">Screenshot preview unavailable.</p>'
                f'<figcaption>{caption}</figcaption></figure>')
    if shots is None:
        raise GuideError('a release build needs the guide screenshots')
    names = {}
    for theme in ('light', 'dark'):
        items = [path for path in (shots / scenario).glob('*.png')
                 if theme in path.stem.lower() and png_size(path) == (1920, 1080)]
        if len(items) != 1:
            raise GuideError(f'{scenario} needs exactly one {theme} 1920×1080 screenshot')
        name = f'{scenario}-{theme}.png'
        target = out / 'assets/screenshots' / name
        target.parent.mkdir(exist_ok=True)
        shutil.copyfile(items[0], target)
        names[theme] = f'{BASE}assets/screenshots/{name}'
    return (f'<figure><picture><source data-dark media="(prefers-color-scheme: dark)" srcset="{names["dark"]}">'
            f'<img src="{names["light"]}" alt="{html.escape(alt, quote=True)}" width="1920" height="1080">'
            f'</picture><figcaption>{caption}</figcaption></figure>')


def protocol_reference():
    # Use the same validated model and Markdown emitters as the wire reference.
    sys.path.insert(0, str(ROOT / 'tools/gen'))
    try:
        import emit_docs
        import model
    finally:
        sys.path.pop(0)
    try:
        defs = model.load_defs(ROOT / 'protocol/defs')
    except model.SpecError as exc:
        raise GuideError(f'protocol reference: {exc}') from exc

    modules = []
    for spec in defs['types']:
        document = emit_docs.type_doc(spec)
        modules.append('#' + document.replace('\n## ', '\n### '))

    enums = defs['registry']['wire_enums']
    status = ['| Code | Result |', '| --- | --- |']
    status.extend(f'| {value} | `{name}` |' for name, value in enums['Result'].items())
    detail = ['| Code | Detail |', '| --- | --- |']
    detail.extend(f'| {value} | `{name}` |' for name, value in enums['Detail'].items())
    diagnostics = emit_docs.diag_block(defs).removeprefix(emit_docs.DIAG_BEGIN)
    diagnostics = diagnostics.removesuffix(emit_docs.DIAG_END).strip()
    return {
        'module_reference': '\n'.join(modules),
        'status_reference': '\n'.join(status),
        'detail_reference': '\n'.join(detail),
        'diagnostic_reference': diagnostics,
    }


def build_site(out, args):
    release = version.base()
    apt_url = args.apt_url.rstrip('/')
    if args.release and apt_url != PAGES_URL:
        raise GuideError('release builds must use the public apt URL')
    if not re.fullmatch(r'(?:https://[A-Za-z0-9_./:-]+|http://localhost(?::\d+)?/[A-Za-z0-9_./-]+)', apt_url):
        raise GuideError('apt URL must use HTTPS or http://localhost')
    groups, pages = navigation()
    template = Template(read(GUIDE / 'templates/page.html'))
    info = firmware_info(args.firmware, release, args.release)
    notice = '' if args.release else ('<p class="preview" role="status">Unreleased preview. '
                                      'Download links refer to the tagged release.</p>')
    release_url = f'{PROJECT}/releases/tag/v{release}'
    public_fingerprint = fingerprint(read(FINGERPRINT))
    key_fingerprint = fingerprint(args.fingerprint or public_fingerprint)
    if args.release and key_fingerprint != public_fingerprint:
        raise GuideError('release builds must use the committed apt fingerprint')
    references = {
        **asset_links(release), 'base': BASE, 'apt_url': apt_url, 'release_url': release_url,
        'release_version': release,
        'release_label': f'[DriveLink {release} release notes]({release_url})',
        'fingerprint': key_fingerprint,
        'fingerprint_source': ('[Check the published fingerprint]'
                               f'({PROJECT}/blob/v{release}/packaging/apt/FINGERPRINT).'
                               if key_fingerprint == public_fingerprint else
                               'This preview uses a temporary signing key. Use it in a disposable Ubuntu environment.'),
        'esptool': esptool_command(info),
        **protocol_reference(),
    }
    copy_assets(out)
    references['flasher'] = flash_files(out, args.firmware, info)
    scenarios = json.loads(read(GUIDE / 'screenshots.json'))
    if set(scenarios) != set(FIGURES) or len(scenarios) != len(FIGURES):
        raise GuideError('each listed screenshot needs one guide figure')
    for name in scenarios:
        references[f'figure:{name}'] = screenshot(out, args.screenshots, name, args.release)
    parser = markdown()
    for page in pages:
        slug = page['slug']
        context = dict(title=html.escape(page['title']), base=BASE, project=PROJECT,
                       preview=notice, navigation=nav_html(groups, slug),
                       page_class='home' if not slug else 'article')
        source = read(GUIDE / 'pages' / page['source'])
        context['content'] = parser.render(SUBSTITUTION.sub(lambda match: references[match[1]], source))
        write(out / slug / 'index.html', template.substitute(context))
    write(out / '.nojekyll', '')
    if {item.name for item in out.iterdir()} & APT_PATHS:
        raise GuideError('guide output collides with apt')
    subprocess.run([sys.executable, '-m', 'pagefind', '--site', str(out),
                    '--exclude-selectors', '.preview'], check=True)
    check_site(out)


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__(convert_charrefs=True)
        self.ids = set()
        self.links = []
        self.problems = []
        self.feed(read(path))
        self.close()

    def handle_starttag(self, tag, attributes):
        attr = dict(attributes)
        if 'id' in attr:
            if attr['id'] in self.ids:
                self.problems.append(f'duplicate id {attr["id"]}')
            self.ids.add(attr['id'])
        if tag == 'img' and attr.get('alt') is None:
            self.problems.append('image is missing alt text')
        if tag in {'base', 'iframe', 'object', 'embed'}:
            self.problems.append(f'unsupported tag <{tag}>')
        if tag in {'a', 'link'} and attr.get('href'):
            self.links.append((attr['href'], tag != 'a'))
        for name in ('src', 'manifest', 'poster'):
            if attr.get(name):
                self.links.append((attr[name], True))
        if tag == 'pagefind-config' and attr.get('bundle-path'):
            self.links.append((urljoin(attr['bundle-path'], 'pagefind-entry.json'), True))
        for part in (attr.get('srcset') or '').split(','):
            if part.strip():
                self.links.append((part.split()[0], True))


def check_site(out):
    if not (out / 'pagefind/pagefind-entry.json').is_file():
        raise GuideError('Pagefind search index is missing')
    _, pages = navigation()
    expected = {out / page['slug'] / 'index.html' for page in pages}
    actual = {path for path in out.rglob('*.html')
              if path.relative_to(out).parts[0] not in {'assets', 'pagefind', *APT_PATHS}}
    if actual != expected:
        raise GuideError('built pages and navigation disagree')
    checked = {path: Page(path) for path in [*expected, out / 'assets/third-party/index.html']}
    problems = []

    def check_link(source, link, local_only):
        split = urlsplit(link)
        if split.scheme or split.netloc:
            if local_only or split.scheme not in {'https', 'http', 'mailto'}:
                problems.append(f'{source}: external or unsupported asset {link}')
            return
        rooted = urlsplit(urljoin(BASE + source.relative_to(out).as_posix(), link))
        if not rooted.path.startswith(BASE):
            problems.append(f'{source}: link leaves the site mount {link}')
            return
        target = (out / unquote(rooted.path[len(BASE):])).resolve()
        if not target.is_relative_to(out.resolve()):
            problems.append(f'{source}: link escapes the site {link}')
            return
        if target.is_dir():
            target /= 'index.html'
        if not target.is_file():
            problems.append(f'{source}: missing target {link}')
        elif rooted.fragment and target in checked and unquote(rooted.fragment) not in checked[target].ids:
            problems.append(f'{source}: missing anchor {link}')

    for source, page in checked.items():
        problems.extend(f'{source}: {problem}' for problem in page.problems)
        for link, local_only in page.links:
            check_link(source, link, local_only)
    css_links = re.compile(r"""url\(\s*['"]?([^'"()\s]+)['"]?\s*\)|@import\s+['"]([^'"]+)['"]""")
    for css in (out / 'assets').rglob('*.css'):
        for first, second in css_links.findall(read(css)):
            check_link(css, first or second, True)
    manifest = out / 'flash/manifest.json'
    if manifest.is_file():
        for build in json.loads(read(manifest))['builds']:
            for part in build['parts']:
                check_link(manifest, part['path'], True)
    if problems:
        raise GuideError('\n'.join(problems))


def apt_snapshot(root):
    for folder in ('dists', 'pool'):
        if not (root / folder).is_dir():
            raise GuideError(f'apt tree is missing {folder}')
    if not (root / KEY).is_file():
        raise GuideError(f'apt tree is missing {KEY}')
    protected = [root / KEY, *(root / 'dists').rglob('*'), *(root / 'pool').rglob('*')]
    snapshot = {}
    for file in protected:
        if file.is_file():
            with file.open('rb') as stream:
                snapshot[file.relative_to(root)] = hashlib.file_digest(stream, 'sha256').hexdigest()
    return snapshot


def combine(site, apt, out):
    check_site(site)
    if not {item.name for item in apt.iterdir()} <= {*APT_PATHS, 'index.html', '.nojekyll'}:
        raise GuideError('apt tree has an unexpected root path')
    if {item.name for item in site.iterdir()} & APT_PATHS:
        raise GuideError('the guide would overwrite a protected apt path')
    if out.exists():
        raise GuideError(f'combined output already exists: {out}')
    snapshot = apt_snapshot(apt)
    shutil.copytree(apt, out)
    shutil.copytree(site, out, dirs_exist_ok=True)
    if apt_snapshot(out) != snapshot or not (out / '.nojekyll').is_file():
        raise GuideError('combined site changed the protected apt files')
    check_site(out)


def serve(out, port):
    check_site(out)

    class MountedGuide(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=out, **kwargs)

        def send_head(self):
            path = urlsplit(self.path).path
            if path in {'/', BASE.rstrip('/')}:
                self.send_response(302)
                self.send_header('Location', BASE)
                self.end_headers()
            elif path.startswith(BASE):
                return super().send_head()
            else:
                self.send_error(404)

        def translate_path(self, path):
            return super().translate_path('/' + path.removeprefix(BASE))

        def list_directory(self, path):
            self.send_error(404)

    with ThreadingHTTPServer(('localhost', port), MountedGuide) as server:
        print(f'Serving http://localhost:{port}{BASE}', flush=True)
        server.serve_forever()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    build = sub.add_parser('build')
    build.add_argument('--firmware', type=Path)
    build.add_argument('--screenshots', type=Path)
    build.add_argument('--release', action='store_true')
    build.add_argument('--apt-url', default=PAGES_URL)
    build.add_argument('--fingerprint')
    check = sub.add_parser('check')
    check.add_argument('--site', type=Path, default=OUT)
    local = sub.add_parser('serve')
    local.add_argument('--site', type=Path, default=OUT)
    local.add_argument('--port', type=int, default=8000)
    merge = sub.add_parser('combine')
    merge.add_argument('--apt', type=Path, required=True)
    merge.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.action == 'build':
            OUT.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix='guide-', dir=OUT.parent) as staging:
                ready = Path(staging) / 'site'
                build_site(ready, args)
                if OUT.is_symlink():
                    raise GuideError(f'refusing symlink at {OUT}')
                if OUT.exists():
                    if not (OUT / 'pagefind/pagefind-entry.json').is_file():
                        raise GuideError(f'refusing to replace non-guide directory {OUT}')
                    shutil.rmtree(OUT)
                ready.rename(OUT)
            print(f'Built {OUT}')
        elif args.action == 'check':
            check_site(args.site.resolve())
            print('Guide pages, links and search index passed')
        elif args.action == 'serve':
            serve(args.site.resolve(), args.port)
        else:
            combine(OUT, args.apt.resolve(), args.out.resolve())
            print(f'Combined guide and verified apt paths in {args.out}')
    except (GuideError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print(f'guide: {error}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
