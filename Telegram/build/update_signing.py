"""Configured v2 release signing with pinned root and release-key identities."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import sign_update

ROOT = Path(__file__).resolve().parents[2]


def run(*args, capture=False):
    return subprocess.run([str(x) for x in args], check=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def public_coordinates(key, algorithm):
    data = run('openssl', 'pkey', '-in', key, '-pubout', '-outform', 'DER', capture=True)
    encode = lambda value: base64.urlsafe_b64encode(value).rstrip(b'=').decode('ascii')
    if algorithm == 'Ed25519':
        if len(data) != 44 or data[:12].hex() != '302a300506032b6570032100':
            raise RuntimeError('Expected an Ed25519 public key.')
        return dict(alg=algorithm, x=encode(data[-32:]))
    if len(data) != 91 or data[-65] != 4:
        raise RuntimeError('Expected an uncompressed P-256 public key.')
    return dict(alg='ES256', crv='P-256', x=encode(data[-64:-32]), y=encode(data[-32:]))


def root_fingerprint(path):
    data = run('openssl', 'pkey', '-pubin', '-in', path, '-pubout', '-outform', 'DER', capture=True)
    return hashlib.sha256(data).hexdigest()


def load_config(path, keys):
    config = json.loads(path.read_text(encoding='utf-8'))
    if config.get('backend') not in ('openssl', 'azure'):
        raise RuntimeError('Unknown signing backend.')
    if config['backend'] == 'azure' and not config.get('vault_version'):
        raise RuntimeError('Pin the Azure key version before release signing.')
    if config.get('root_sha256', '').lower() != root_fingerprint(keys / 'root-public.pem'):
        raise RuntimeError('Embedded root differs from the protected signing configuration.')
    run('openssl', 'pkeyutl', '-verify', '-rawin', '-pubin',
        '-inkey', keys / 'root-public.pem', '-in', keys / 'manifest.min.json',
        '-sigfile', keys / 'manifest.sig')
    for field in ('local_key', 'es256_key'):
        if config.get(field):
            config[field] = str((path.parent / config[field]).resolve())
    manifest = json.loads((keys / 'manifest.min.json').read_bytes())
    entries = {entry['id']: entry for entry in manifest['keys']}
    for field, identity, algorithm in (
        ('local_key', 'local_id', 'Ed25519'),
        ('es256_key', 'es256_id', 'ES256'),
    ):
        if field == 'es256_key' and config['backend'] == 'azure':
            run(sys.executable, Path(__file__).with_name('sign_update.py'), '--check',
                '--az-vault', config['vault'], '--az-key', config['vault_key'],
                '--az-key-version', config['vault_version'],
                '--keys-loc', keys, '--key-id', config[identity])
            continue
        expected = public_coordinates(config[field], algorithm)
        actual = entries.get(config[identity], {})
        if any(actual.get(k) != v for k, v in expected.items()):
            raise RuntimeError(f'{identity} does not match the signed manifest.')
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--keys-loc', type=Path, default=ROOT / 'Telegram/Resources/update')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--input', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--packer', type=Path)
    parser.add_argument('--unsigned', type=Path)
    parser.add_argument('--channel', default='stable')
    args = parser.parse_args()
    config = load_config(args.config, args.keys_loc)
    if args.check:
        print('Release key identities match the manifest.')
        return
    if not args.input or not args.output:
        parser.error('--input and --output are required')
    command = [sys.executable, Path(__file__).with_name('sign_update.py'),
               '--input', args.input, '--output', args.output]
    if config['backend'] == 'openssl':
        command += ['--openssl-key', config['es256_key']]
    elif config['backend'] == 'azure':
        command += ['--az-vault', config['vault'], '--az-key', config['vault_key'],
                    '--az-key-version', config['vault_version']]
    else:
        raise RuntimeError('Unknown signing backend.')
    run(*command)
    if args.packer:
        if not args.unsigned:
            parser.error('--unsigned is required with --packer')
        run(args.packer.resolve(), '-channel', args.channel, '-keys-loc', args.keys_loc,
            '-unsigned', args.unsigned, '-embed-signatures', f'{config["es256_id"]}:{args.output}',
            '-local-key', config['local_key'], '-local-key-id', config['local_id'])


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
