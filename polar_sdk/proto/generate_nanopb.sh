#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  polar_sdk/proto/generate_nanopb.sh <polar_proto_dir> [output_dir]

Arguments:
  <polar_proto_dir>  Path to Polar BLE SDK proto directory containing pftp_*.proto
  [output_dir]       Destination for generated *.pb.c/*.pb.h (default: build/polar_proto)

Example:
  ./polar_sdk/proto/generate_nanopb.sh \
    /tmp/polar-ble-sdk/sources/Android/android-communications/library/src/sdk/proto
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage
    exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
polar_proto_dir="$1"
out_dir="${2:-${repo_root}/build/polar_proto}"

nanopb_generator="${repo_root}/vendors/nanopb/generator/nanopb_generator.py"
if [[ ! -f "${nanopb_generator}" ]]; then
    echo "error: nanopb generator not found: ${nanopb_generator}" >&2
    exit 1
fi

required_proto=(
    "nanopb.proto"
    "types.proto"
    "structures.proto"
    "pftp_error.proto"
    "pftp_notification.proto"
    "pftp_request.proto"
    "pftp_response.proto"
)

for rel in "${required_proto[@]}"; do
    if [[ ! -f "${polar_proto_dir}/${rel}" ]]; then
        echo "error: missing proto input: ${polar_proto_dir}/${rel}" >&2
        exit 1
    fi
done

if [[ ! -f "${polar_proto_dir}/google/protobuf/descriptor.proto" ]]; then
    echo "error: missing descriptor proto: ${polar_proto_dir}/google/protobuf/descriptor.proto" >&2
    exit 1
fi

tmp_dir="$(mktemp -d)"
cleanup() {
    rm -rf "${tmp_dir}"
}
trap cleanup EXIT

mkdir -p "${tmp_dir}/proto/google/protobuf"

for rel in "${required_proto[@]}"; do
    cp "${polar_proto_dir}/${rel}" "${tmp_dir}/proto/${rel}"
done
cp "${polar_proto_dir}/google/protobuf/descriptor.proto" "${tmp_dir}/proto/google/protobuf/descriptor.proto"

for opt in pftp_request.options pftp_response.options structures.options; do
    if [[ -f "${repo_root}/polar_sdk/proto/options/${opt}" ]]; then
        cp "${repo_root}/polar_sdk/proto/options/${opt}" "${tmp_dir}/proto/${opt}"
    fi
done

rm -rf "${out_dir}"
mkdir -p "${out_dir}"

python3 "${nanopb_generator}" \
    --error-on-unmatched \
    -I "${tmp_dir}/proto" \
    -D "${out_dir}" \
    "${tmp_dir}/proto/types.proto" \
    "${tmp_dir}/proto/structures.proto" \
    "${tmp_dir}/proto/pftp_error.proto" \
    "${tmp_dir}/proto/pftp_notification.proto" \
    "${tmp_dir}/proto/pftp_request.proto" \
    "${tmp_dir}/proto/pftp_response.proto"

echo "Generated nanopb files in: ${out_dir}"
