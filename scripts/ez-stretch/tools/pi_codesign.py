#!/usr/bin/env python3
"""
pi_codesign.py - Standalone PixInsight code signing tool

Signs PixInsight scripts (.js, .scp) and XML files (.xri) using Ed25519.

Usage:
    # Using pre-extracted JSON keys (from DumpKeys.js):
    ./pi_codesign.py --keys ~/.pi_signing_keys.json <files...>

    # Keys file format (JSON):
    {
      "developerId": "username",
      "publicKey": "hex...",
      "privateKey": "hex..."
    }

To extract keys, run DumpKeys.js in PixInsight first.
"""

import argparse
import base64
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False


def load_json_keys(json_path: str) -> dict:
    """Load signing keys from JSON file (extracted by DumpKeys.js)"""
    with open(json_path, 'r') as f:
        data = json.load(f)

    if 'developerId' not in data or 'privateKey' not in data:
        raise ValueError("Invalid keys file: missing developerId or privateKey")

    return {
        'developer_id': data['developerId'],
        'public_key': bytes.fromhex(data.get('publicKey', '')),
        'private_key': bytes.fromhex(data['privateKey']),
    }


def extract_script_id(script_path: str) -> str:
    """Extract #script-id from a JavaScript file"""
    with open(script_path, 'r', encoding='utf-8') as f:
        content = f.read()

    match = re.search(r'#script-id\s+(\S+)', content)
    if match:
        return match.group(1)

    # Fallback to filename without extension
    return Path(script_path).stem


def canonicalize_script(script_path: str) -> bytes:
    """
    Canonicalize script for signing.
    PixInsight normalizes line endings to LF.
    """
    with open(script_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Normalize line endings to LF
    content = content.replace('\r\n', '\n').replace('\r', '\n')

    return content.encode('utf-8')


def sign_data_expanded(scalar_bytes: bytes, prefix: bytes, public_key: bytes, message: bytes) -> bytes:
    """
    Sign data using Ed25519 with expanded key format.

    PixInsight stores keys in expanded format:
    - scalar (32 bytes, clamped): the secret signing scalar
    - prefix (32 bytes): used for deterministic nonce generation

    Standard Ed25519 libraries expect seed format, but PixInsight exports
    the already-expanded key (result of SHA512(seed) with clamping applied).
    """
    import hashlib

    # Ed25519 curve parameters
    p = 2**255 - 19  # Prime field
    L = 2**252 + 27742317777372353535851937790883648493  # Order of base point
    d = -121665 * pow(121666, -1, p) % p  # Curve parameter

    def mod_inv(x, mod):
        return pow(x, mod - 2, mod)

    def point_add(P, Q):
        if P is None: return Q
        if Q is None: return P
        x1, y1 = P
        x2, y2 = Q
        denom = d * x1 * x2 * y1 * y2
        x3 = (x1*y2 + x2*y1) * mod_inv(1 + denom, p) % p
        y3 = (y1*y2 + x1*x2) * mod_inv(1 - denom, p) % p
        return (x3, y3)

    def point_mul(n, P):
        if n == 0: return None
        Q = None
        while n > 0:
            if n & 1:
                Q = point_add(Q, P)
            P = point_add(P, P)
            n >>= 1
        return Q

    def recover_x(y, sign):
        x2 = (y*y - 1) * mod_inv(d*y*y + 1, p) % p
        x = pow(x2, (p+3)//8, p)
        if (x*x - x2) % p != 0:
            x = x * pow(2, (p-1)//4, p) % p
        if x % 2 != sign:
            x = p - x
        return x

    # Base point B
    By = 4 * mod_inv(5, p) % p
    Bx = recover_x(By, 0)
    B = (Bx, By)

    def point_encode(P):
        if P is None: return bytes(32)
        x, y = P
        return (y | ((x & 1) << 255)).to_bytes(32, 'little')

    def int_from_bytes(b):
        return int.from_bytes(b, 'little')

    def sha512(data):
        return hashlib.sha512(data).digest()

    scalar = int_from_bytes(scalar_bytes)

    # r = SHA512(prefix || message) mod L
    r_hash = sha512(prefix + message)
    r = int_from_bytes(r_hash) % L

    # R = r * B
    R_point = point_mul(r, B)
    R = point_encode(R_point)

    # k = SHA512(R || public_key || message) mod L
    k_hash = sha512(R + public_key + message)
    k = int_from_bytes(k_hash) % L

    # s = (r + k * scalar) mod L
    s = (r + k * scalar) % L
    S = s.to_bytes(32, 'little')

    return R + S


def sign_data(keys: dict, data: bytes) -> bytes:
    """Sign data with Ed25519 using PixInsight's expanded key format"""
    private_key_bytes = keys['private_key']
    public_key = keys['public_key']

    if len(private_key_bytes) != 64:
        raise ValueError(f"Invalid private key length: {len(private_key_bytes)}, expected 64 (expanded format)")

    scalar_bytes = private_key_bytes[:32]
    prefix = private_key_bytes[32:]

    return sign_data_expanded(scalar_bytes, prefix, public_key, data)


def sign_script(keys: dict, script_path: str, entitlements: list = None) -> str:
    """Sign a script file and return the signature XML"""
    script_id = extract_script_id(script_path)
    developer_id = keys['developer_id']
    now = datetime.now(timezone.utc)
    timestamp = now.strftime('%Y-%m-%dT%H:%M:%S.') + f"{now.microsecond // 1000:03d}Z"

    # Canonicalize script
    script_content = canonicalize_script(script_path)

    # Build message to sign
    # Format: scriptId + developerId + timestamp + script + entitlements
    message_parts = [
        script_id.encode('utf-8'),
        developer_id.encode('utf-8'),
        timestamp.encode('utf-8'),
        script_content,
    ]

    if entitlements:
        for e in entitlements:
            message_parts.append(e.encode('utf-8'))

    # Sign the raw message (Ed25519 handles hashing internally)
    full_message = b''.join(message_parts)
    signature = sign_data(keys, full_message)
    signature_b64 = base64.b64encode(signature).decode('ascii')

    # Build XML output
    xml_content = f'''<?xml version="1.0" encoding="UTF-8"?>
<!--
PixInsight XML Code Signature Format - XSGN version 1.0
Created with pi_codesign.py - https://pixinsight.com/
-->
<xsgn version="1.0" xmlns="http://www.pixinsight.com/xsgn" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.pixinsight.com/xsgn http://pixinsight.com/xsgn/xsgn-1.0.xsd">
   <CreationTime>{timestamp}</CreationTime>
   <Signature version="1.0" scriptId="{script_id}" developerId="{developer_id}">
      <Timestamp>{timestamp}</Timestamp>
      <CodeSignature encoding="Base64">{signature_b64}</CodeSignature>
   </Signature>
</xsgn>
'''
    return xml_content


def sign_xri(keys: dict, xri_path: str) -> bool:
    """Sign an XRI file by adding/updating signature element.

    PixInsight XRI files have signature OUTSIDE the root element:
    </xri>
    <Signature ...>...</Signature>
    """
    developer_id = keys['developer_id']
    now = datetime.now(timezone.utc)
    timestamp = now.strftime('%Y-%m-%dT%H:%M:%S.') + f"{now.microsecond // 1000:03d}Z"

    # Read the XRI file as text
    with open(xri_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Remove existing signature if present (it's after </xri>)
    content_clean = re.sub(r'\s*<Signature[^>]*>.*?</Signature>\s*$', '', content, flags=re.DOTALL)
    content_clean = content_clean.rstrip() + '\n'

    # Build message to sign (XRI content without signature)
    message_parts = [
        developer_id.encode('utf-8'),
        timestamp.encode('utf-8'),
        content_clean.encode('utf-8'),
    ]

    # Sign the raw message (Ed25519 handles hashing internally)
    full_message = b''.join(message_parts)
    signature = sign_data(keys, full_message)
    signature_b64 = base64.b64encode(signature).decode('ascii')

    # Build signature element (outside root, PixInsight style)
    sig_line = f'<Signature developerId="{developer_id}" timestamp="{timestamp}" encoding="Base64">{signature_b64}</Signature>'

    # Write back: content + signature after closing tag
    with open(xri_path, 'w', encoding='utf-8') as f:
        f.write(content_clean)
        f.write(sig_line)

    return True


def main():
    parser = argparse.ArgumentParser(
        description='Standalone PixInsight code signing tool',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  # Sign scripts using extracted keys:
  %(prog)s -k ~/.pi_signing_keys.json src/*.js

  # Extract keys first by running DumpKeys.js in PixInsight
'''
    )
    parser.add_argument('--keys', '-k', required=True,
                       help='Path to JSON keys file (from DumpKeys.js)')
    parser.add_argument('files', nargs='+', help='Files to sign')
    parser.add_argument('--debug', '-d', action='store_true', help='Debug mode')
    args = parser.parse_args()

    if not HAVE_CRYPTO:
        print("Error: cryptography library required")
        print("Install with: pip install cryptography")
        sys.exit(1)

    # Load keys
    print(f"Loading keys from: {args.keys}")
    try:
        keys = load_json_keys(args.keys)
        print(f"Developer ID: {keys['developer_id']}")
        if args.debug:
            print(f"Public key length: {len(keys['public_key'])} bytes")
            print(f"Private key length: {len(keys['private_key'])} bytes")
    except FileNotFoundError:
        print(f"Error: Keys file not found: {args.keys}")
        print("\nTo create keys file:")
        print("  1. Run DumpKeys.js in PixInsight")
        print("  2. Move the output to ~/.pi_signing_keys.json")
        sys.exit(1)
    except Exception as e:
        print(f"Error loading keys: {e}")
        sys.exit(1)

    # Sign each file
    succeeded = 0
    failed = 0

    for filepath in args.files:
        if not os.path.exists(filepath):
            print(f"Warning: File not found: {filepath}")
            failed += 1
            continue

        ext = os.path.splitext(filepath)[1].lower()

        try:
            if ext in ('.js', '.scp', '.jsh'):
                print(f"Signing: {filepath}")
                signature_xml = sign_script(keys, filepath)
                sig_path = os.path.splitext(filepath)[0] + '.xsgn'
                with open(sig_path, 'w', encoding='utf-8') as f:
                    f.write(signature_xml)
                print(f"  Created: {sig_path}")
                succeeded += 1

            elif ext == '.xri':
                print(f"Signing XRI: {filepath}")
                sign_xri(keys, filepath)
                print(f"  Signed: {filepath}")
                succeeded += 1

            else:
                print(f"Unknown file type: {filepath}")
                failed += 1

        except Exception as e:
            print(f"  Error: {e}")
            failed += 1
            if args.debug:
                import traceback
                traceback.print_exc()

    print(f"\nResults: {succeeded} succeeded, {failed} failed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == '__main__':
    main()
