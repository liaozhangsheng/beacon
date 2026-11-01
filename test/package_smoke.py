"""Run an extracted package through its launcher without the build's library search paths."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

archive = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='beacon-package-') as directory:
    with zipfile.ZipFile(archive) as package:
        package.extractall(directory)
    pointers = list(Path(directory).rglob('.beacon-current'))
    if len(pointers) != 1:
        raise RuntimeError(f'Expected one .beacon-current, found {pointers}')
    root = pointers[0].parent
    version = pointers[0].read_text().strip()
    name = 'beacon.exe' if os.name == 'nt' else 'beacon'
    launcher = root / name
    program = root / 'versions' / version / name
    for executable in (launcher, program):
        if not executable.is_file():
            raise RuntimeError(f'Executable is missing: {executable}')
        executable.chmod(0o755)
    assets = root / 'versions' / version / 'assets'
    if not assets.is_dir():
        raise RuntimeError('Program assets are missing from the version directory')
    for required in ('app-icon.png', 'default-avatar.png', 'settings-background.png',
                     'fonts/Minecraft.otf', 'widgets/button.png', 'progress/background.png'):
        if not (assets / required).is_file():
            raise RuntimeError(f'Application UI asset is missing: {required}')
    for removed in ('ui', 'avatars', 'vender'):
        if (assets / removed).exists():
            raise RuntimeError(f'Template resource directory is still bundled: {removed}')
    skill = root / '.agents' / 'skills' / 'beacon-custom-template' / 'SKILL.md'
    if not skill.is_file():
        raise RuntimeError(f'Bundled template skill is missing: {skill}')
    for required in ('template.json', 'lang.json', 'langs/en.json', 'layout.json'):
        if not (root / 'templates' / '1.16.1' / required).is_file():
            raise RuntimeError(f'Bundled 1.16.1 template file is missing: {required}')
    environment = dict(os.environ)
    for key in ('LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH'):
        environment.pop(key, None)
    if os.name == 'nt':
        system_root = next(value for key, value in environment.items()
                           if key.lower() == 'systemroot')
        environment['PATH'] = str(Path(system_root) / 'System32')
    else:
        environment['PATH'] = '/usr/bin:/bin'
    # The launcher forwards arguments and the exit status of the selected version.
    subprocess.run([str(launcher), '--smoke-test'], cwd=directory,
                   env=environment, check=True, timeout=30)
    # A pointer damaged by a power loss falls back to the newest usable version.
    pointers[0].write_bytes(b'')
    subprocess.run([str(launcher), '--smoke-test'], cwd=directory,
                   env=environment, check=True, timeout=30)
    # Normal startup must still find user templates/settings when the pointer is
    # missing. Unlike --smoke-test, this runs installation-root discovery.
    pointers[0].unlink()
    startup_environment = dict(environment, SDL_VIDEODRIVER='dummy', HOME=directory,
                               APPDATA=directory, XDG_DATA_HOME=directory)
    process = subprocess.Popen([str(launcher)], cwd=directory, env=startup_environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    startup_timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        startup_timed_out = True
        if os.name == 'nt':
            # The Windows launcher waits for a child; stop the entire process tree.
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        process.kill()
        stdout, stderr = process.communicate()
    if not startup_timed_out:
        raise RuntimeError(f'Normal startup exited before the smoke timeout: {process.returncode}: '
                           f'{stdout!r} {stderr!r}')
    if not (root / 'config/settings.json').is_file():
        raise RuntimeError(f'Missing-pointer startup did not save settings in the installation root: '
                           f'{stdout!r} {stderr!r}')
    if (program.parent / '.beacon-running.lock').exists():
        raise RuntimeError('Missing-pointer startup locked the version directory instead of the installation root')
    updater_name = 'beacon-updater.exe' if os.name == 'nt' else 'beacon-updater'
    updater = root / 'updater' / updater_name
    if updater.is_file():
        updater.chmod(0o755)
        subprocess.run([str(updater), '--help'], cwd=directory,
                       env=environment, check=True, timeout=30)
