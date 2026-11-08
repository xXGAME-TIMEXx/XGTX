#!/usr/bin/env python3
"""Build and package XGTX executables without starting services or opening wallets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parent
BINARIES = {'daemon': 'xgtxd', 'simplewallet': 'xgtx-wallet-cli',
            'wallet_rpc_server': 'xgtx-wallet-rpc',
            'xgtx_projection_migrate': 'xgtx-projection-migrate'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', default='build/ubuntu24-clean')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--package', action='store_true')
    parser.add_argument('--seed-output', type=Path, help='copy Cargo cache from a prior output; never modifies it')
    parser.add_argument('--offline', action='store_true', help='require cached Cargo dependencies')
    args = parser.parse_args()
    output = (ROOT / args.output).resolve()
    if not output.is_relative_to(ROOT) or output == ROOT or output.exists():
        parser.error('output must be a new directory inside this source copy')
    if args.jobs < 1:
        parser.error('jobs must be positive')
    output.mkdir(parents=True)
    report = {'status': 'failed', 'reproducibility': 'not established',
              'commands': [], 'scope': 'four executable build; no activation or execution'}
    # Keep dependency writes in this local build, not the user's Cargo home.
    env = dict(os.environ)
    env['CARGO_HOME'] = str(output / 'cargo-home')

    def run(command, name):
        command = [str(x) for x in command]
        report['commands'].append(command)
        print(shlex.join(command), flush=True)
        with (output / (name + '.log')).open('w') as log:
            subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True)

    try:
        if args.seed_output:
            seed = (ROOT / args.seed_output).resolve()
            if not seed.is_relative_to(ROOT) or not (seed / 'cargo-home').is_dir():
                raise RuntimeError('seed must be a local output with cargo-home')
            shutil.copytree(seed / 'cargo-home', output / 'cargo-home')
            report['seed_output'] = str(seed)
        os_release = platform.freedesktop_os_release()
        if os_release.get('ID') != 'ubuntu' or os_release.get('VERSION_ID') != '24.04':
            raise RuntimeError('This recipe targets Ubuntu 24.04')
        if platform.machine() != 'x86_64':
            raise RuntimeError('This recipe targets x86_64')
        required = ['cmake', 'gcc', 'g++', 'make', 'rustc', 'cargo', 'pkg-config']
        missing = [name for name in required if not shutil.which(name)]
        if missing:
            raise RuntimeError('Missing build tools: ' + ', '.join(missing) + '; see BUILD-UBUNTU24.md')
        for tool in required:
            run([tool, '--version'], tool + '-version')
        report['tool_versions'] = {
            tool: (output / (tool + '-version.log')).read_text().splitlines()[0]
            for tool in required}
        run(['cargo', 'fetch', '--locked', *(['--offline'] if args.offline else []), '--manifest-path',
             ROOT / 'src/fcmp_pp/fcmp_pp_rust/Cargo.toml'], 'cargo-fetch')
        build = output / 'native'
        run(['cmake', '-S', ROOT, '-B', build, '-G', 'Unix Makefiles',
             '-DCMAKE_BUILD_TYPE=Release', '-DSTATIC=OFF', '-DARCH=default',
             '-DMANUAL_SUBMODULES=ON', '-DBUILD_TESTS=OFF',
             '-DXGTX_PUBLIC_EXCHANGE_CANDIDATE=OFF',
             '-DCARGO_OPTIONS=' + ('--offline' if args.offline else '')], 'configure')
        run(['cmake', '--build', build, '--target', *BINARIES, '--parallel', args.jobs], 'build')
        report['binary_sha256'] = {}
        dependencies = []
        for name in BINARIES.values():
            binary = build / 'bin' / name
            if not binary.is_file():
                raise RuntimeError('Missing executable: ' + name)
            report['binary_sha256'][name] = hashlib.sha256(binary.read_bytes()).hexdigest()
            run(['ldd', binary], 'dependencies-' + name)
            listing = (output / ('dependencies-' + name + '.log')).read_text()
            if 'not found' in listing:
                raise RuntimeError('Unresolved runtime dependency: ' + name)
            dependencies.append(name + '\n' + listing)
        (output / 'dependencies.log').write_text('\n'.join(dependencies))
        run(['dpkg-query', '-W'], 'packages')
        if args.package:
            package = output / 'xgtx-ubuntu24-x86_64-candidate.tar.gz'
            with tarfile.open(package, 'w:gz') as archive:
                for name in BINARIES.values():
                    archive.add(build / 'bin' / name, arcname='xgtx/bin/' + name)
                for name in ['LICENSE', 'BUILD-UBUNTU24.md', 'CANDIDATE-NOTES.txt']:
                    archive.add(ROOT / name, arcname='xgtx/' + name)
                for name in ['dependencies.log', 'packages.log']:
                    archive.add(output / name, arcname='xgtx/' + name)
            (output / 'SHA256SUMS').write_text(
                hashlib.sha256(package.read_bytes()).hexdigest() + '  ' + package.name + '\n')
            with tarfile.open(package, 'r:gz') as archive:
                members = archive.getmembers()
                for name, expected in report['binary_sha256'].items():
                    matches = [m for m in members if m.name == 'xgtx/bin/' + name]
                    if len(matches) != 1 or not matches[0].isfile() or not matches[0].mode & 0o111:
                        raise RuntimeError('Archive missing executable: ' + name)
                    if hashlib.sha256(archive.extractfile(matches[0]).read()).hexdigest() != expected:
                        raise RuntimeError('Archive binary hash mismatch: ' + name)
            report['archive_verified'] = True
        report['status'] = 'passed'
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        report['error'] = str(error)
        print('BLOCKED: ' + str(error), file=sys.stderr)
    finally:
        (output / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
