#!/bin/sh
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Make the test-only certificates in this directory (see README.md):
#
#   sh tests/tls/make-certs.sh
#
# Needs openssl. Each run makes new keys, so commit all the files together.
set -eu
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
days=36500 # 100 years: they must not expire under the tests

# The CA. Its key is thrown away: nothing else is ever signed with it.
openssl ecparam -name prime256v1 -genkey -noout -out "$work/ca.key"
openssl req -x509 -new -key "$work/ca.key" -sha256 -days $days \
    -subj "/O=Plumlang tests/CN=Plumlang test CA (not trusted anywhere)" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out ca.pem

# The server, for localhost and 127.0.0.1.
openssl ecparam -name prime256v1 -genkey -noout -out "$work/server.ec"
openssl pkcs8 -topk8 -nocrypt -in "$work/server.ec" -out server.key
openssl req -new -key server.key -subj "/O=Plumlang tests/CN=localhost" -out "$work/server.csr"
printf '%s\n' "basicConstraints=critical,CA:FALSE" "keyUsage=critical,digitalSignature" \
    "extendedKeyUsage=serverAuth" "subjectAltName=DNS:localhost,IP:127.0.0.1" >"$work/ext"
openssl x509 -req -in "$work/server.csr" -CA ca.pem -CAkey "$work/ca.key" \
    -set_serial 1 -sha256 -days $days -extfile "$work/ext" -out server.pem
openssl verify -CAfile ca.pem server.pem
