"""Run an extracted package without the build's runtime-library search paths."""
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
    name = 'beacon.exe' if os.name == 'nt' else 'beacon'
    binaries = [path for path in Path(directory).rglob(name) if path.is_file()]
    if len(binaries) != 1:
        raise RuntimeError(f'Expected one executable, found {binaries}')
    executable = binaries[0]
    executable.chmod(0o755)
    skill = executable.parent / '.agents' / 'skills' / 'beacon-custom-template' / 'SKILL.md'
    if not skill.is_file():
        raise RuntimeError(f'Bundled template skill is missing: {skill}')
    environment = dict(os.environ, SDL_VIDEODRIVER='dummy')
    for key in ('LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH'):
        environment.pop(key, None)
    environment['PATH'] = (str(Path(environment['SystemRoot']) / 'System32')
                           if os.name == 'nt' else '/usr/bin:/bin')
    subprocess.run([str(executable), '--smoke-test'], cwd=directory,
                   env=environment, check=True, timeout=30)
