#!/usr/bin/env python3
"""HTTPS endpoint inspection and logger trust-plan helpers.

The firmware supports two HTTPS trust modes:

- built-in curated public roots (`public_roots`),
- one provisioned CA anchor (`provisioned_anchor`).

This module helps the host choose between them without doing any on-device
TOFU. It verifies the endpoint on the host first, then emits a trust plan that
can be embedded in a config import.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import ssl
import tempfile
from dataclasses import asdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlparse


LOGGER_PUBLIC_ROOT_PROFILE = "logger-public-roots-v1"
LOGGER_TLS_MODE_PUBLIC_ROOTS = "public_roots"
LOGGER_TLS_MODE_PROVISIONED_ANCHOR = "provisioned_anchor"
LOGGER_TLS_ANCHOR_FORMAT_X509_DER_BASE64 = "x509_der_base64"
REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PUBLIC_ROOTS_PEM = REPO_ROOT / "logger_firmware" / "tls" / "public_roots_v1.pem"
_PEM_CERT_RE = re.compile(
    r"-----BEGIN CERTIFICATE-----\s+.*?-----END CERTIFICATE-----\s*",
    re.DOTALL,
)
_DN_ABBREVIATIONS = {
    "businessCategory": "businessCategory",
    "commonName": "CN",
    "countryName": "C",
    "domainComponent": "DC",
    "emailAddress": "emailAddress",
    "localityName": "L",
    "organizationName": "O",
    "organizationalUnitName": "OU",
    "serialNumber": "serialNumber",
    "stateOrProvinceName": "ST",
    "streetAddress": "street",
    "title": "title",
}


class LoggerTlsError(RuntimeError):
    """Base class for TLS planning failures."""


@dataclass(frozen=True)
class CertificateInfo:
    sha256: str
    subject: str
    issuer: str
    not_before: str | None
    not_after: str | None
    self_signed: bool
    der_base64: str


@dataclass(frozen=True)
class TlsProbeResult:
    url: str
    host: str
    port: int
    remote_ip: str
    tls_version: str | None
    cipher_suite: str | None
    verification_source: str
    chain: list[CertificateInfo]


@dataclass(frozen=True)
class TlsTrustPlan:
    mode: str
    root_profile: str | None
    anchor: dict[str, str] | None
    reason: str
    selected_anchor_sha256: str | None
    selected_anchor_subject: str | None


def _decode_name(name: Any) -> str:
    parts: list[str] = []
    for rdn in name or []:
        attrs: list[str] = []
        for item in rdn:
            if len(item) != 2:
                continue
            key, value = item
            attrs.append(f"{_DN_ABBREVIATIONS.get(key, key)}={value}")
        if attrs:
            parts.append("+".join(attrs))
    return ", ".join(parts)


def _decode_der_certificate(der_bytes: bytes) -> CertificateInfo:
    pem_text = ssl.DER_cert_to_PEM_cert(der_bytes)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as handle:
        handle.write(pem_text)
        pem_path = Path(handle.name)
    try:
        info = ssl._ssl._test_decode_cert(str(pem_path))
    finally:
        os.unlink(pem_path)

    subject = _decode_name(info.get("subject"))
    issuer = _decode_name(info.get("issuer"))
    return CertificateInfo(
        sha256=hashlib.sha256(der_bytes).hexdigest(),
        subject=subject,
        issuer=issuer,
        not_before=info.get("notBefore"),
        not_after=info.get("notAfter"),
        self_signed=bool(subject) and subject == issuer,
        der_base64=base64.b64encode(der_bytes).decode("ascii"),
    )


def _load_certificate_file(path: Path) -> list[CertificateInfo]:
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = ""

    pem_blocks = _PEM_CERT_RE.findall(text)
    if pem_blocks:
        return [
            _decode_der_certificate(ssl.PEM_cert_to_DER_cert(block))
            for block in pem_blocks
        ]

    return [_decode_der_certificate(raw)]


def load_builtin_public_roots(path: Path = DEFAULT_PUBLIC_ROOTS_PEM) -> list[CertificateInfo]:
    return _load_certificate_file(path)


def _builtin_public_root_fingerprints(path: Path = DEFAULT_PUBLIC_ROOTS_PEM) -> set[str]:
    return {cert.sha256 for cert in load_builtin_public_roots(path)}


def probe_https_endpoint(url: str, *, ca_cert_path: Path | None = None, timeout_s: float = 10.0) -> TlsProbeResult:
    parsed = urlparse(url)
    if parsed.scheme != "https":
        raise LoggerTlsError("TLS probing requires an https:// URL")
    if not parsed.hostname:
        raise LoggerTlsError("URL must include a hostname")

    port = parsed.port or 443
    verification_source = "system_default"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.verify_mode = ssl.CERT_REQUIRED
    context.check_hostname = True
    context.load_default_certs()
    if ca_cert_path is not None:
        context.load_verify_locations(cafile=str(ca_cert_path))
        verification_source = f"system_default+{ca_cert_path}"

    with socket.create_connection((parsed.hostname, port), timeout=timeout_s) as raw_sock:
        with context.wrap_socket(raw_sock, server_hostname=parsed.hostname) as tls_sock:
            chain_ders = tls_sock.get_verified_chain()
            if not chain_ders:
                leaf_der = tls_sock.getpeercert(binary_form=True)
                if leaf_der is None:
                    raise LoggerTlsError("peer did not provide a certificate")
                chain_ders = [leaf_der]

            chain = [_decode_der_certificate(der) for der in chain_ders]
            cipher = tls_sock.cipher()
            remote_ip = tls_sock.getpeername()[0]
            tls_version = tls_sock.version()

    return TlsProbeResult(
        url=url,
        host=parsed.hostname,
        port=port,
        remote_ip=remote_ip,
        tls_version=tls_version,
        cipher_suite=cipher[0] if cipher else None,
        verification_source=verification_source,
        chain=chain,
    )


def _select_anchor_from_bundle(bundle: list[CertificateInfo], chain: list[CertificateInfo]) -> CertificateInfo:
    bundle_by_sha = {cert.sha256: cert for cert in bundle}
    matched = [bundle_by_sha[cert.sha256] for cert in chain[1:] if cert.sha256 in bundle_by_sha]
    if matched:
        return matched[-1]
    if len(bundle) == 1:
        return bundle[0]
    raise LoggerTlsError(
        "CA bundle did not contain a unique certificate from the verified endpoint chain; "
        "use a single-certificate anchor file or a matching bundle"
    )


def plan_logger_tls(
    url: str,
    *,
    trust_mode: str = "auto",
    ca_cert_path: Path | None = None,
    timeout_s: float = 10.0,
    public_roots_pem: Path = DEFAULT_PUBLIC_ROOTS_PEM,
) -> tuple[TlsProbeResult, TlsTrustPlan]:
    probe = probe_https_endpoint(url, ca_cert_path=ca_cert_path, timeout_s=timeout_s)
    builtin_fingerprints = _builtin_public_root_fingerprints(public_roots_pem)
    builtin_matches = [cert for cert in probe.chain[1:] if cert.sha256 in builtin_fingerprints]
    bundle = _load_certificate_file(ca_cert_path) if ca_cert_path is not None else []

    if trust_mode == "auto":
        if builtin_matches:
            trust_mode = LOGGER_TLS_MODE_PUBLIC_ROOTS
        else:
            trust_mode = LOGGER_TLS_MODE_PROVISIONED_ANCHOR

    if trust_mode == LOGGER_TLS_MODE_PUBLIC_ROOTS:
        if not builtin_matches:
            raise LoggerTlsError(
                "endpoint chain did not terminate at a built-in logger public root; "
                "use --trust-mode provisioned-anchor with --ca-cert or let --trust-mode auto choose"
            )
        return probe, TlsTrustPlan(
            mode=LOGGER_TLS_MODE_PUBLIC_ROOTS,
            root_profile=LOGGER_PUBLIC_ROOT_PROFILE,
            anchor=None,
            reason="verified endpoint chains to a built-in logger public root",
            selected_anchor_sha256=builtin_matches[-1].sha256,
            selected_anchor_subject=builtin_matches[-1].subject,
        )

    if trust_mode != LOGGER_TLS_MODE_PROVISIONED_ANCHOR:
        raise LoggerTlsError(f"unsupported trust mode: {trust_mode}")

    if bundle:
        anchor = _select_anchor_from_bundle(bundle, probe.chain)
        reason = "verified endpoint with host-provided CA bundle; selected matching CA anchor"
    else:
        if len(probe.chain) < 2:
            raise LoggerTlsError("verified chain did not include a CA certificate to provision")
        anchor = probe.chain[-1]
        reason = "verified endpoint does not chain to a built-in logger root; selected top verified CA"

    return probe, TlsTrustPlan(
        mode=LOGGER_TLS_MODE_PROVISIONED_ANCHOR,
        root_profile=None,
        anchor={
            "format": LOGGER_TLS_ANCHOR_FORMAT_X509_DER_BASE64,
            "der_base64": anchor.der_base64,
            "sha256": anchor.sha256,
        },
        reason=reason,
        selected_anchor_sha256=anchor.sha256,
        selected_anchor_subject=anchor.subject,
    )


def _probe_to_dict(probe: TlsProbeResult) -> dict[str, Any]:
    return asdict(probe)


def _plan_to_dict(plan: TlsTrustPlan) -> dict[str, Any]:
    return asdict(plan)


def _print_json(document: dict[str, Any]) -> None:
    print(json.dumps(document, indent=2, sort_keys=True))


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    probe_parser = subparsers.add_parser("probe", help="verify an HTTPS endpoint and print its certificate chain")
    probe_parser.add_argument("url", help="https:// upload URL to inspect")
    probe_parser.add_argument("--ca-cert", type=Path, help="extra CA certificate or bundle to trust while probing")
    probe_parser.add_argument("--timeout", type=float, default=10.0, help="socket timeout in seconds")
    probe_parser.set_defaults(handler=_handle_probe)

    plan_parser = subparsers.add_parser(
        "plan",
        help="emit the recommended logger upload.tls object for an HTTPS endpoint",
    )
    plan_parser.add_argument("url", help="https:// upload URL to inspect")
    plan_parser.add_argument(
        "--trust-mode",
        choices=("auto", LOGGER_TLS_MODE_PUBLIC_ROOTS, LOGGER_TLS_MODE_PROVISIONED_ANCHOR),
        default="auto",
        help="host-side trust planning policy",
    )
    plan_parser.add_argument("--ca-cert", type=Path, help="extra CA certificate or bundle for private endpoints")
    plan_parser.add_argument("--timeout", type=float, default=10.0, help="socket timeout in seconds")
    plan_parser.set_defaults(handler=_handle_plan)

    return parser


def _handle_probe(args: argparse.Namespace) -> int:
    probe = probe_https_endpoint(args.url, ca_cert_path=args.ca_cert, timeout_s=args.timeout)
    _print_json({"probe": _probe_to_dict(probe)})
    return 0


def _handle_plan(args: argparse.Namespace) -> int:
    probe, plan = plan_logger_tls(
        args.url,
        trust_mode=args.trust_mode,
        ca_cert_path=args.ca_cert,
        timeout_s=args.timeout,
    )
    _print_json({"probe": _probe_to_dict(probe), "plan": _plan_to_dict(plan)})
    return 0


def main() -> int:
    parser = _build_arg_parser()
    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())