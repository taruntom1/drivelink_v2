#!/usr/bin/env python3
"""Build and verify the signed DriveLink apt repository (H3d).

    python3 tools/apt_repo.py build --debs DIR --out DIR [--clean] [--url URL]
                                    (--throwaway-key | --key-env NAME [--expect-fingerprint FILE])
                                    [--public-key-out FILE]
    python3 tools/apt_repo.py verify --repo DIR --key FILE

`build` turns the .deb files of one release (H3c names them
`<Package>_<Version>_<Architecture>.deb`, Version `<full>~ubuntu<release>`) into a
static repository: a pool, one suite per Ubuntu release present (noble 24.04,
resolute 26.04; component main; amd64 and arm64), Release signed as InRelease and
Release.gpg, the public key, `.nojekyll` and an index page. The files are plain
static content for GitHub Pages. --key-env reads the armored secret key from that
environment variable into a temporary GNUPGHOME (it never touches disk elsewhere
and is never printed) and refuses to sign unless its fingerprint equals the one
in --expect-fingerprint (the key committed in the repository). --throwaway-key
signs with a key made for the run, for checks. SOURCE_DATE_EPOCH fixes the Release
date, so equal inputs give byte-identical Packages, Packages.gz and Release.

`verify` serves the repository on 127.0.0.1 and runs real apt against it, without
root (a private apt root with APT_CONFIG): update has no warning, every package's
SHA256 matches the pool, the host's own release resolves the CLI and agent, and
three damaged copies are refused (signed by another key, unsigned, a tampered
Packages). Prints one PASS line per check.

Exit 0 ok, 1 failure, 2 missing tool (dpkg-deb, gpg, apt-get). Linux only; standard
library only (the pure helpers also import on Windows for the unit tests).
"""
import argparse
import contextlib
from dataclasses import dataclass
from datetime import datetime, timezone
import functools
import getpass
import gzip
import hashlib
import http.server
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading

from ros_packages import ROS_PACKAGES, ROS_TARGETS, debian_name

ROOT = Path(__file__).resolve().parents[1]
SUITES = {'24.04': 'noble', '26.04': 'resolute'}
ARCHES = ('amd64', 'arm64')
COMPONENT = 'main'
ORIGIN = 'DriveLink'
DEFAULT_URL = 'https://taruntom1.github.io/drivelink_v2'
KEYRING = 'drivelink-archive-keyring.asc'
INDEX_TEMPLATE = ROOT / 'packaging/apt/index.html'
DAYS = ('Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun')
MONTHS = ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec')
# Fields the index states itself, from the file.
DROPPED_FIELDS = frozenset(name.lower() for name in ('Filename', 'Size', 'MD5sum', 'SHA1', 'SHA256', 'SHA512'))


class RepoError(Exception):
    pass


class MissingTool(Exception):
    pass


# Pure helpers

@dataclass(frozen=True)
class Deb:
    path: Path
    fields: tuple        # ((name, value), ...) in control order; a value may hold '\n ' continuation lines
    size: int
    sha256: str

    def field(self, name):
        for key, value in self.fields:
            if key.lower() == name.lower():
                return value
        raise RepoError(f'{self.path.name}: control has no {name} field')

    @property
    def package(self):
        return self.field('Package')

    @property
    def version(self):
        return self.field('Version')

    @property
    def arch(self):
        return self.field('Architecture')

    @property
    def release(self):
        return release_of(self.version)

    @property
    def suite(self):
        return suite_for(self.version)


def release_of(version):
    """'0.1.0~ubuntu26.04' -> '26.04'."""
    match = re.search(r'~ubuntu(\d+\.\d+)$', version)
    if not match:
        raise RepoError(f'version {version!r} has no ~ubuntu<release> suffix')
    return match.group(1)


def suite_for(version):
    release = release_of(version)
    if release not in SUITES:
        raise RepoError(f'version {version!r}: Ubuntu {release} has no suite (known: {", ".join(SUITES)})')
    return SUITES[release]


def base_version(version):
    """'0.1.0+gabc~ubuntu26.04' -> '0.1.0+gabc'."""
    release_of(version)
    return re.sub(r'~ubuntu\d+\.\d+$', '', version)


def pool_path(package, filename):
    prefix = package[:4] if package.startswith('lib') else package[0]
    return f'pool/{COMPONENT}/{prefix}/{package}/{filename}'


def parse_control(text):
    """dpkg-deb --field output -> ((name, value), ...); continuation lines join with '\\n'."""
    fields = []
    for line in text.splitlines():
        if not line.strip():
            continue
        if line[0] in ' \t':
            if not fields:
                raise RepoError(f'control starts with a continuation line: {line!r}')
            name, value = fields[-1]
            fields[-1] = (name, f'{value}\n{line}')
            continue
        name, sep, value = line.partition(':')
        if not sep or not name.strip():
            raise RepoError(f'control line without a field name: {line!r}')
        fields.append((name.strip(), value.strip()))
    return tuple(fields)


def packages_stanza(deb, filename):
    lines = [f'{name}: {value}' if value else f'{name}:' for name, value in deb.fields
             if name.lower() not in DROPPED_FIELDS]
    lines += [f'Filename: {filename}', f'Size: {deb.size}', f'SHA256: {deb.sha256}']
    return ''.join(f'{line}\n' for line in lines)


def packages_index(debs_with_filenames):
    ordered = sorted(debs_with_filenames, key=lambda item: (item[0].package, item[0].version))
    return '\n'.join(packages_stanza(deb, filename) for deb, filename in ordered)


def select(debs):
    """{(suite, arch): [deb, ...]}; one release of the software across all of them."""
    debs = list(debs)
    if not debs:
        raise RepoError('no .deb files')
    chosen = {}
    bases = {}
    for deb in sorted(debs, key=lambda d: (d.package, d.version, d.arch)):
        name = deb.path.name
        if deb.arch not in ARCHES:
            raise RepoError(f'{name}: architecture {deb.arch!r} is not one of {", ".join(ARCHES)}')
        try:
            suite = deb.suite
        except RepoError as error:
            raise RepoError(f'{name}: {error}') from error
        expected = f'{deb.package}_{deb.version}_{deb.arch}.deb'
        if name != expected:
            raise RepoError(f'{name}: the file name must be {expected}')
        group = chosen.setdefault((suite, deb.arch), [])
        if any(other.package == deb.package for other in group):
            raise RepoError(f'{name}: {deb.package} is in {suite} {deb.arch} twice')
        group.append(deb)
        bases.setdefault(base_version(deb.version), name)
    if len(bases) > 1:
        listing = ', '.join(f'{base} ({name})' for base, name in sorted(bases.items()))
        raise RepoError(f'the files are of more than one version, but a repository is one release: {listing}')
    return dict(sorted(chosen.items()))


def check_complete(debs):
    """A published repository carries the full Linux and ROS release matrix."""
    from package_linux import packages_for

    groups = select(debs)
    for release, suite in SUITES.items():
        distro = ROS_TARGETS[release]
        expected = {package.name for package in packages_for('ubuntu', release)}
        expected.update(debian_name(distro, name) for name in ROS_PACKAGES)
        for arch in ARCHES:
            actual = {deb.package for deb in groups.get((suite, arch), [])}
            if actual != expected:
                raise RepoError(f'{suite} {arch}: missing {sorted(expected - actual)}, '
                                f'unexpected {sorted(actual - expected)}')


def format_date(dt):
    """RFC 2822 in UTC, independent of the locale: 'Sat, 21 Nov 2026 09:05:03 UTC'."""
    if dt.tzinfo is not None:
        dt = dt.astimezone(timezone.utc)
    return (f'{DAYS[dt.weekday()]}, {dt.day:02d} {MONTHS[dt.month - 1]} {dt.year} '
            f'{dt.hour:02d}:{dt.minute:02d}:{dt.second:02d} UTC')


def source_date(env):
    epoch = env.get('SOURCE_DATE_EPOCH')
    if epoch:
        try:
            return datetime.fromtimestamp(int(epoch), timezone.utc)
        except (ValueError, OverflowError, OSError) as error:
            raise RepoError(f'SOURCE_DATE_EPOCH {epoch!r} is not a number of seconds') from error
    return datetime.now(timezone.utc).replace(microsecond=0)


def release_file(suite, files, date):
    """files: {path under dists/<suite>/: (size, sha256)}."""
    release = {name: rel for rel, name in SUITES.items()}[suite]
    lines = [f'Origin: {ORIGIN}', f'Label: {ORIGIN}', f'Suite: {suite}', f'Codename: {suite}', f'Version: {release}',
             f'Date: {format_date(date)}', f'Architectures: {" ".join(ARCHES)}', f'Components: {COMPONENT}',
             f'Description: DriveLink packages for Ubuntu {release}', 'SHA256:']
    lines += [f' {digest} {size:>16} {name}' for name, (size, digest) in sorted(files.items())]
    return ''.join(f'{line}\n' for line in lines)


def gzip_bytes(data):
    """No timestamp and no file name: equal input gives equal bytes."""
    return gzip.compress(data, mtime=0)


def normalize_fingerprint(text):
    fingerprint = ''.join(text.split()).upper()
    if not re.fullmatch(r'[0-9A-F]{40}', fingerprint):
        raise RepoError(f'{text.strip()!r} is not a 40 digit hexadecimal fingerprint')
    return fingerprint


def check_fingerprint(actual, expected_text):
    actual, expected = normalize_fingerprint(actual), normalize_fingerprint(expected_text)
    if actual != expected:
        raise RepoError(f'signing key {actual} is not the committed key {expected}')


def render_index(template, url, fingerprint):
    for placeholder in ('@URL@', '@FINGERPRINT@'):
        if placeholder not in template:
            raise RepoError(f'the index template has no {placeholder}')
    digits = normalize_fingerprint(fingerprint)
    grouped = ' '.join(digits[i:i + 4] for i in range(0, 40, 4))
    return template.replace('@URL@', url).replace('@FINGERPRINT@', grouped)


# Linux edges

def run(argv, input=None, env=None, check=True):
    """Run argv (a list); bytes in, bytes out. Failure is a RepoError unless check is false."""
    argv = [str(a) for a in argv]
    if isinstance(input, str):
        input = input.encode('utf-8')
    try:
        done = subprocess.run(argv, input=input, env=env, capture_output=True)
    except FileNotFoundError as error:
        raise MissingTool(argv[0]) from error
    if check and done.returncode != 0:
        tail = '\n'.join(done.stderr.decode('utf-8', 'replace').strip().splitlines()[-8:])
        raise RepoError(f'{" ".join(argv)} exited {done.returncode}:\n{tail}')
    return done


def output_of(done):
    return (done.stdout + done.stderr).decode('utf-8', 'replace')


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def read_deb(path):
    path = Path(path)
    fields = parse_control(run(['dpkg-deb', '--field', path]).stdout.decode('utf-8'))
    return Deb(path, fields, path.stat().st_size, file_sha256(path))


class Gpg:
    """A temporary GNUPGHOME (mode 0700, short path for the agent socket)."""

    def __init__(self):
        self.home = None

    def __enter__(self):
        self.home = Path(tempfile.mkdtemp(prefix='dlapt-', dir='/tmp'))
        self.home.chmod(0o700)
        return self

    def __exit__(self, *exc):
        try:
            run(['gpgconf', '--homedir', self.home, '--kill', 'all'], check=False)
        finally:
            shutil.rmtree(self.home, ignore_errors=True)

    def gpg(self, *args, input=None):
        return run(['gpg', '--homedir', self.home, '--batch', '--no-tty', '--pinentry-mode', 'loopback',
                    '--passphrase', '', *args], input=input)

    def secret_fingerprints(self):
        listing = self.gpg('--with-colons', '--list-secret-keys').stdout.decode('utf-8')
        found, primary = [], False
        for line in listing.splitlines():
            kind = line.split(':', 1)[0]
            if kind == 'sec':
                primary = True
            elif kind == 'fpr' and primary:
                found.append(line.split(':')[9])
                primary = False
        return found

    def generate(self, name):
        self.gpg('--quick-gen-key', name, 'ed25519', 'sign', '1d')
        found = self.secret_fingerprints()
        if len(found) != 1:
            raise RepoError(f'expected one generated key, found {len(found)}')
        return found[0]

    def import_secret(self, armored):
        self.gpg('--import', input=armored)
        found = self.secret_fingerprints()
        if len(found) != 1:
            raise RepoError(f'the signing key must hold exactly one secret key, it holds {len(found)}')
        return found[0]

    def export_public(self, fingerprint):
        key = self.gpg('--armor', '--export', fingerprint).stdout
        if not key.strip():
            raise RepoError(f'gpg exported no public key for {fingerprint}')
        return key

    def sign(self, fingerprint, release):
        release = Path(release)
        common = ['--local-user', f'{fingerprint}!', '--digest-algo', 'SHA512', '--yes']
        self.gpg(*common, '--clearsign', '--output', release.with_name('InRelease'), release)
        self.gpg(*common, '--detach-sign', '--armor', '--output', release.with_name('Release.gpg'), release)


def check_empty(out):
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise RepoError(f'{out} exists and is not empty (use --clean to remove it first)')


def build(debs, out, gpg, fpr, date, url, index_template):
    out = Path(out)
    check_empty(out)
    chosen = select(debs)
    out.mkdir(parents=True, exist_ok=True)
    result = {'suites': {}, 'fingerprint': fpr}
    for suite in sorted({suite for suite, _ in chosen}):
        files = {}
        result['suites'][suite] = {}
        for arch in ARCHES:
            group = chosen.get((suite, arch), [])
            entries = []
            for deb in group:
                relative = pool_path(deb.package, deb.path.name)
                target = out / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(deb.path, target)
                entries.append((deb, relative))
            index = packages_index(entries).encode('utf-8')
            folder = out / 'dists' / suite / COMPONENT / f'binary-{arch}'
            folder.mkdir(parents=True)
            for name, data in (('Packages', index), ('Packages.gz', gzip_bytes(index))):
                (folder / name).write_bytes(data)
                files[f'{COMPONENT}/binary-{arch}/{name}'] = (len(data), hashlib.sha256(data).hexdigest())
            result['suites'][suite][arch] = [deb.path.name for deb in group]
        release = out / 'dists' / suite / 'Release'
        release.write_text(release_file(suite, files, date), encoding='utf-8', newline='\n')
        gpg.sign(fpr, release)
    (out / KEYRING).write_bytes(gpg.export_public(fpr))
    (out / '.nojekyll').write_bytes(b'')
    template = Path(index_template).read_text(encoding='utf-8')
    (out / 'index.html').write_text(render_index(template, url, fpr), encoding='utf-8', newline='\n')
    return result


# Verification with real apt

class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


@contextlib.contextmanager
def serve(directory):
    handler = functools.partial(QuietHandler, directory=str(directory))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()


def apt_root(root, port, suite, pubkey):
    """A private apt root under root (no privileges needed); returns the environment for apt."""
    root = Path(root)
    etc = root / 'etc/apt'
    for folder in (etc / 'sources.list.d', etc / 'apt.conf.d', etc / 'preferences.d', etc / 'trusted.gpg.d',
                   root / 'var/lib/apt/lists/partial', root / 'var/cache/apt/archives/partial', root / 'var/log/apt'):
        folder.mkdir(parents=True)
    (etc / 'sources.list').write_text('', encoding='utf-8')
    (root / 'key.asc').write_bytes(pubkey)
    (etc / 'sources.list.d/drivelink.sources').write_text(
        f'Types: deb\nURIs: http://127.0.0.1:{port}/\nSuites: {suite}\nComponents: {COMPONENT}\n'
        f'Signed-By: {root}/key.asc\n', encoding='utf-8')
    settings = {
        'Dir::Etc': etc, 'Dir::Etc::SourceList': etc / 'sources.list', 'Dir::Etc::SourceParts': etc / 'sources.list.d',
        'Dir::Etc::Parts': etc / 'apt.conf.d', 'Dir::Etc::Preferences': etc / 'preferences',
        'Dir::Etc::PreferencesParts': etc / 'preferences.d', 'Dir::Etc::Trusted': etc / 'trusted.gpg',
        'Dir::Etc::TrustedParts': etc / 'trusted.gpg.d', 'Dir::State': root / 'var/lib/apt',
        'Dir::State::status': '/var/lib/dpkg/status', 'Dir::Cache': root / 'var/cache/apt',
        'Dir::Log': root / 'var/log/apt', 'Debug::NoLocking': 'true', 'APT::Sandbox::User': getpass.getuser(),
        'Acquire::Languages': 'none', 'Acquire::http::Proxy': 'DIRECT'}
    conf = ''.join(f'{key} "{value}";\n' for key, value in settings.items())
    conf += 'APT::Architectures { ' + ' '.join(f'"{arch}";' for arch in ARCHES) + ' };\n'
    (root / 'apt.conf').write_text(conf, encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.lower().endswith('_proxy')}
    env.update(APT_CONFIG=str(root / 'apt.conf'), LC_ALL='C')
    return env


def apt_get(env, *args):
    done = run(['apt-get', *args], env=env, check=False)
    return done.returncode, output_of(done)


def noisy_lines(text):
    return [line for line in text.splitlines() if line.startswith(('W:', 'E:', 'Err:'))]


def read_packages(repo, suite):
    """[(package, version, arch, sha256, filename)] of every Packages file of a suite."""
    found = []
    for arch in ARCHES:
        text = (repo / 'dists' / suite / COMPONENT / f'binary-{arch}' / 'Packages').read_text(encoding='utf-8')
        for stanza in filter(None, (s.strip('\n') for s in text.split('\n\n'))):
            fields = dict(parse_control(stanza))
            found.append((fields['Package'], fields['Version'], fields['Architecture'], fields['SHA256'],
                          fields['Filename']))
    return found


def host_codename():
    try:
        text = Path('/etc/os-release').read_text(encoding='utf-8')
    except OSError:
        return ''
    for line in text.splitlines():
        key, _, value = line.partition('=')
        if key == 'VERSION_CODENAME':
            return value.strip().strip('"\'')
    return ''


def refused(repo, suite, pubkey, damage):
    """apt-get update (exit, output) against a damaged copy of the repository."""
    with tempfile.TemporaryDirectory(prefix='dlapt-bad-') as scratch:
        copy = Path(scratch) / 'repo'
        shutil.copytree(repo, copy)
        damage(copy)
        with serve(copy) as port:
            return apt_get(apt_root(Path(scratch) / 'apt', port, suite, pubkey), 'update')


def verify(repo, pubkey, host_arch=None):
    repo = Path(repo)
    pubkey = Path(pubkey).read_bytes()
    suites = sorted(p.name for p in (repo / 'dists').iterdir() if p.is_dir()) if (repo / 'dists').is_dir() else []
    if not suites:
        raise RepoError(f'{repo} has no dists/<suite>')
    host_arch = host_arch or run(['dpkg', '--print-architecture']).stdout.decode().strip()
    passed = []

    def ok(suite, what):
        passed.append(f'PASS {suite}: {what}')
        print(passed[-1], flush=True)

    def fail(suite, what, text):
        raise RepoError(f'{suite}: {what}\n' + '\n'.join('  ' + line for line in text.strip().splitlines()[-12:]))

    for suite in suites:
        with serve(repo) as port, tempfile.TemporaryDirectory(prefix='dlapt-root-') as scratch:
            env = apt_root(scratch, port, suite, pubkey)
            code, text = apt_get(env, 'update')
            if code != 0 or noisy_lines(text):
                fail(suite, f'apt-get update exited {code} or warned', text)
            ok(suite, 'apt-get update exits 0 without a warning')
            listed = read_packages(repo, suite)
            for package, version, arch, digest, filename in listed:
                if file_sha256(repo / filename) != digest:
                    raise RepoError(f'{suite}: {filename} does not match the SHA256 of its Packages entry')
                done = run(['apt-cache', 'show', f'{package}:{arch}={version}'], env=env, check=False)
                text = output_of(done)
                if done.returncode != 0 or f'SHA256: {digest}' not in text.splitlines():
                    fail(suite, f'apt-cache show {package}:{arch}={version} (exit {done.returncode})', text)
            ok(suite, f'apt-cache shows all {len(listed)} packages with the SHA256 of the pool files')
            names = {package for package, _, arch, _, _ in listed if arch == host_arch}
            if suite == host_codename() and {'drivelink-cli', 'drivelink-agent'} <= names:
                code, text = apt_get(env, 'install', '-s', '--no-install-recommends', 'drivelink-cli',
                                     'drivelink-agent')
                if code != 0:
                    fail(suite, f'apt-get install -s exited {code}', text)
                ok(suite, f'apt-get install -s drivelink-cli drivelink-agent resolves ({host_arch})')
            else:
                print(f'SKIP {suite}: the host is not {suite} {host_arch} with the CLI and agent', flush=True)

        def other_key(copy):
            with Gpg() as other:
                other.sign(other.generate('DriveLink other key'), copy / 'dists' / suite / 'Release')

        def unsigned(copy):
            for name in ('InRelease', 'Release.gpg'):
                (copy / 'dists' / suite / name).unlink()

        def tampered(copy):
            folder = copy / 'dists' / suite / COMPONENT / f'binary-{host_arch}'
            data = (folder / 'Packages').read_bytes() + b'\n'
            (folder / 'Packages').write_bytes(data)
            (folder / 'Packages.gz').write_bytes(gzip_bytes(data))

        code, text = refused(repo, suite, pubkey, other_key)
        if code == 0 or not re.search(r'NO_PUBKEY|not signed|invalid', text):
            fail(suite, f'a repository signed by another key was not refused (exit {code})', text)
        ok(suite, 'signed by another key: refused')
        code, text = refused(repo, suite, pubkey, unsigned)
        if code == 0 or 'not signed' not in text:
            fail(suite, f'an unsigned repository was not refused as unsigned (exit {code})', text)
        ok(suite, 'unsigned: refused')
        code, text = refused(repo, suite, pubkey, tampered)
        if code == 0 or not re.search(r'mismatch|unexpected size', text, re.I):
            fail(suite, f'a tampered Packages file was not refused (exit {code})', text)
        ok(suite, 'tampered Packages: refused')
    return passed


def need_tools(*names):
    missing = [name for name in names if not shutil.which(name)]
    if missing:
        raise MissingTool(', '.join(missing))


def signing_key(args, gpg):
    if args.throwaway_key:
        fpr = gpg.generate('DriveLink throwaway test key')
        print(f'apt_repo: signing with a THROWAWAY key {fpr}; the repository is for checks only')
        return fpr
    armored = os.environ.get(args.key_env, '')
    if not armored.strip():
        raise RepoError(f'environment variable {args.key_env} is unset or empty')
    fpr = gpg.import_secret(armored.encode('utf-8'))
    check_fingerprint(fpr, Path(args.expect_fingerprint).read_text(encoding='utf-8'))
    return fpr


def run_build(args):
    need_tools('dpkg-deb', 'gpg', 'gpgconf')
    sources = [path.resolve() for path in args.debs]
    for source in sources:
        if not source.is_dir():
            raise RepoError(f'{source} is not a directory')
    debs = [read_deb(path) for source in sources for path in sorted(source.glob('*.deb'))]
    if args.complete:
        check_complete(debs)
    out = Path(args.out).resolve()
    if args.clean and out.exists():
        if any(out == source or out in source.parents for source in sources) or out == Path(out.anchor):
            raise RepoError(f'refusing to remove {out}: it holds the .deb files or is a root')
        shutil.rmtree(out)
    check_empty(out)
    with Gpg() as gpg:
        fpr = signing_key(args, gpg)
        result = build(debs, out, gpg, fpr, source_date(os.environ), args.url.rstrip('/'), args.index_template)
        if args.public_key_out:
            Path(args.public_key_out).write_bytes(gpg.export_public(fpr))
    for suite, arches in result['suites'].items():
        print(f'{suite}: ' + ', '.join(f'{arch} {len(names)}' for arch, names in arches.items()))
    print(f'apt_repo: wrote {out} (key {result["fingerprint"]})')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    build_parser = commands.add_parser('build', help='write the repository')
    build_parser.add_argument('--debs', required=True, action='append', type=Path,
                              help='directory of .deb files (repeat for multiple directories)')
    build_parser.add_argument('--complete', action='store_true', help='require the full release matrix')
    build_parser.add_argument('--out', required=True, type=Path)
    build_parser.add_argument('--clean', action='store_true', help='remove --out first')
    build_parser.add_argument('--url', default=DEFAULT_URL, help='public URL, shown on the index page')
    key = build_parser.add_mutually_exclusive_group(required=True)
    key.add_argument('--throwaway-key', action='store_true', help='sign with a key made for this run')
    key.add_argument('--key-env', metavar='NAME', help='environment variable holding the armored secret key')
    build_parser.add_argument('--expect-fingerprint', type=Path, help='file with the committed key fingerprint')
    build_parser.add_argument('--public-key-out', type=Path, help='also write the public key here')
    build_parser.add_argument('--index-template', type=Path, default=INDEX_TEMPLATE, help=argparse.SUPPRESS)
    verify_parser = commands.add_parser('verify', help='run apt against the repository')
    verify_parser.add_argument('--repo', required=True, type=Path)
    verify_parser.add_argument('--key', required=True, type=Path, help='public key file')
    args = parser.parse_args(argv)
    if args.command == 'build' and args.key_env and not args.expect_fingerprint:
        parser.error('--key-env needs --expect-fingerprint')
    try:
        if args.command == 'build':
            run_build(args)
        else:
            need_tools('gpg', 'gpgconf', 'dpkg', 'apt-get', 'apt-cache')
            verify(args.repo, args.key)
    except MissingTool as error:
        print(f'apt_repo: missing tool: {error}', file=sys.stderr)
        return 2
    except RepoError as error:
        print(f'apt_repo: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
