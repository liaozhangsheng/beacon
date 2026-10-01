"""End-to-end signed manifests, cached archives, side-by-side versions, smoke checks and rollback."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from package_updates import build_updates


def main():
    parser = argparse.ArgumentParser()
    for name in ('updater', 'key', 'platform', 'openssl', 'smoke-fixture'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--production-updater')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='beacon-update-cli-') as directory:
        workspace = Path(directory).resolve()
        root = workspace / '安装目录 with spaces'
        root.mkdir()
        executable = 'beacon.exe' if args.platform.startswith('windows-') else 'beacon'
        template = root / 'templates' / 'user.txt'
        template.parent.mkdir()
        template.write_bytes(b'user template')
        config = root / 'config' / 'user.json'
        config.parent.mkdir()
        config.write_bytes(b'user config')

        def package(version, binary, asset):
            raw = workspace / f'raw-{version}.zip'
            program = f'versions/{version}/'
            with zipfile.ZipFile(raw, 'w') as archive:
                for name, data in ((executable, b'launcher'), (program + executable, binary)):
                    info = zipfile.ZipInfo(name)
                    info.create_system = 3
                    info.external_attr = 0o100755 << 16
                    archive.writestr(info, data)
                archive.writestr('.beacon-current', version + '\n')
                archive.writestr(program + 'assets/test.txt', asset)
            out = workspace / version
            paths = build_updates(raw, out, args.platform, version, 'https://example.invalid/download')
            manifest = out / f'release-{args.platform}.json'
            return json.loads(manifest.read_text()), paths, out

        def signed(value):
            manifest = workspace / 'release.json'
            manifest.write_text(json.dumps(value))
            signature = workspace / 'release.sig'
            subprocess.run([args.openssl, 'pkeyutl', '-sign', '-rawin', '-inkey', args.key,
                            '-in', str(manifest), '-out', str(signature)], check=True, capture_output=True)
            return manifest, signature

        def run(operation, value=None, success=True, message=None, updater=None, tamper=False, machine=False,
                extra=()):
            command = [updater or args.updater, operation, '--root', str(root), *extra]
            if value is not None:
                manifest, signature = signed(value)
                if tamper:
                    manifest.write_bytes(manifest.read_bytes() + b' ')
                command += ['--manifest', str(manifest), '--signature', str(signature)]
            if machine:
                command += ['--machine-check']
            result = subprocess.run(command, capture_output=True, timeout=45)
            assert (result.returncode == 0) == success, (command, result.stdout, result.stderr)
            if message:
                assert message in result.stderr.decode(), result.stderr
            return result

        def selected():
            return (root / '.beacon-current').read_text().strip()

        def installed():
            return sorted(path.name for path in (root / 'versions').iterdir())

        def cache_components(release, out):
            cache.mkdir(exist_ok=True)
            for component in release['components'].values():
                filename = component['url'].rsplit('/', 1)[-1]
                shutil.copyfile(out / filename, cache / (component['sha256'] + '.zip'))

        # The full package produced by the packaging script is the installation contract.
        old, _, old_out = package('1.0.0', b'old program', b'old asset')
        with zipfile.ZipFile(old_out / 'raw-1.0.0.zip') as archive:
            archive.extractall(root)
        assert selected() == '1.0.0' and installed() == ['1.0.0']
        run('--check', old)
        current_check = run('--check', old, machine=True)
        assert current_check.stdout == b'BEACON_UPDATE_CURRENT\t1.0.0\n', current_check.stdout
        metadata_only, _, _ = package('1.0.1', b'old program', b'old asset')
        metadata_check = run('--check', metadata_only)
        assert b'Update available: 1.0.1' in metadata_check.stdout, metadata_check.stdout
        machine_check = run('--check', metadata_only, machine=True)
        assert machine_check.stdout == b'BEACON_UPDATE_AVAILABLE\t1.0.1\n', machine_check.stdout
        run('--check', old, success=False, message='signature', tamper=True)
        run('--check', dict(old, platform='wrong-platform'), success=False, message='platform')
        run('--check', dict(old, version='0.9.0'), success=False)
        if args.production_updater:
            run('--check', old, success=False, message='no embedded release public key',
                updater=args.production_updater)
        run('--rollback', success=False, message='no earlier version')
        assert selected() == '1.0.0' and installed() == ['1.0.0']

        new, _, new_out = package('1.1.0', Path(args.smoke_fixture).read_bytes(), b'new asset')
        cache = root / '.beacon-downloads'
        cache_components(new, new_out)
        stale = cache / ('0' * 64 + '.zip.part-interrupted')
        stale.write_bytes(b'partial')
        run('--apply', new)  # Both cache hits are verified; no network needed.
        assert not cache.exists(), 'successful apply must clear the download cache'
        program = root / 'versions' / '1.1.0'
        assert (program / executable).read_bytes() == Path(args.smoke_fixture).read_bytes()
        assert (program / 'assets/test.txt').read_bytes() == b'new asset'
        assert selected() == '1.1.0' and installed() == ['1.0.0', '1.1.0']

        next_release = dict(new, version='1.1.1')
        next_manifest, next_signature = signed(next_release)
        result_file = workspace / 'background-update.log'
        launcher = workspace / 'launch_update.py'
        launcher.write_text('''import os, subprocess, sys, time
command = sys.argv[1:-1] + ['--wait-for-parent', str(os.getpid())]
output = open(sys.argv[-1], 'wb')
subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output, stderr=output)
time.sleep(1.0)
''')
        command = [args.updater, '--apply', '--root', str(root), '--manifest', str(next_manifest),
                   '--signature', str(next_signature)]
        parent = subprocess.Popen([sys.executable, str(launcher), *command, str(result_file)])
        time.sleep(0.2)
        assert parent.poll() is None
        assert selected() == '1.1.0'
        assert parent.wait(timeout=45) == 0
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if result_file.exists() and b'Update applied.' in result_file.read_bytes():
                break
            time.sleep(0.05)
        assert result_file.exists() and b'Update applied.' in result_file.read_bytes(), \
            result_file.read_bytes() if result_file.exists() else 'updater produced no output'
        # A version-only update reuses both components; the oldest version is pruned.
        assert selected() == '1.1.1' and installed() == ['1.1.0', '1.1.1']

        bad, _, bad_out = package('1.2.0', b'not an executable', b'bad asset')
        run('--check', dict(bad, version='1.1.1'), success=False)
        cache_components(bad, bad_out)
        run('--apply', bad, success=False, message='health check failed')
        assert cache.exists(), 'failed apply keeps verified archives for a retry'
        assert selected() == '1.1.1' and installed() == ['1.1.0', '1.1.1']
        failure_log = root / '.beacon-update-error.log'
        assert not failure_log.exists(), 'manual runs report errors on the console only'
        exited = subprocess.Popen([sys.executable, '-c', ''])
        exited.wait()
        run('--apply', bad, success=False, extra=['--wait-for-parent', str(exited.pid)])
        assert b'health check failed' in failure_log.read_bytes(), failure_log.read_bytes()

        rolled_back = run('--rollback')
        assert b'1.1.0' in rolled_back.stdout, rolled_back.stdout
        assert selected() == '1.1.0'
        assert template.read_bytes() == b'user template'
        assert config.read_bytes() == b'user config'
        print('PASS signed CLI, side-by-side apply, pruning, health check, rollback and user files')


if __name__ == '__main__':
    main()
