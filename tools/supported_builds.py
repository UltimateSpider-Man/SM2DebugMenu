"""Exact executable identities shared with the compiled runtime policy.

No normalization, ignored byte range or externally supplied hash is admitted.
The canonical hash remains immutable provenance for original research data.
"""
import hashlib
from pathlib import Path
import re

POLICY_HEADER = Path(__file__).resolve().parents[1] / 'src/supported_builds.h'
_source = POLICY_HEADER.read_text(encoding='utf-8')
_values = {}
for _name in ('kOriginalHash', 'kBootstrapHash'):
    _matches = re.findall(r'inline constexpr char\s+' + _name + r'\[\]\s*=\s*"([0-9a-f]{64})"\s*;', _source)
    if len(_matches) != 1:
        raise ValueError('Missing or ambiguous compiled executable identity: ' + _name)
    _values[_name] = _matches[0]
CANONICAL_SHA256 = _values['kOriginalHash']
BOOTSTRAP_SHA256 = _values['kBootstrapHash']
if CANONICAL_SHA256 == BOOTSTRAP_SHA256:
    raise ValueError('Canonical and bootstrap executable identities must differ')
SUPPORTED_SHA256 = (CANONICAL_SHA256, BOOTSTRAP_SHA256)

def is_supported_hash(digest):
    return isinstance(digest, str) and digest in SUPPORTED_SHA256

def checked_file_hash(path):
    with Path(path).open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    if not is_supported_hash(digest):
        raise ValueError('Unsupported executable: expected the canonical build or exact approved bootstrap variant')
    return digest
