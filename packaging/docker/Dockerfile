# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# The plum image: the release's static (musl) binary on Alpine, which brings
# a shell and the CA certificates HTTPS needs (/etc/ssl/cert.pem).
# Built by .github/workflows/docker.yml from a published release; the
# binaries are checked against the release's SHA256SUMS before this runs.
#
#   docker run --rm -v "$PWD":/src plumlang/plum run app.plum
FROM alpine:3.22

ARG TARGETARCH
ARG VERSION=dev

LABEL org.opencontainers.image.title="Plumlang" \
      org.opencontainers.image.description="Plumlang: reads like English, runs fast, ships as one binary." \
      org.opencontainers.image.url="https://plum.elixpo.com" \
      org.opencontainers.image.source="https://github.com/plumlang/plumlang" \
      org.opencontainers.image.documentation="https://plum.elixpo.com/docs/install/" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.vendor="Elixpo" \
      org.opencontainers.image.version="${VERSION}"

COPY bin/${TARGETARCH}/plum /usr/local/bin/plum
RUN plum version

WORKDIR /src
ENTRYPOINT ["plum"]
CMD ["--help"]
