# syntax=docker/dockerfile:1

# The ExyokiOffice runtime image: the shared library, the four installed
# programs, and the dispatcher that turns `docker run` into one of them.
#
# The build context is not this directory and not the repository. The
# create_install workflow stages a small tree next to this file — rootfs/, which
# already has the final layout — and points docker build at that. A distroless
# image has no shell, so nothing can be arranged after the fact with RUN; every
# layout decision has to be made beforehand, where there is a shell to make it.

# Stage one exists only to compile the dispatcher and to hand over the two
# libraries the binaries need beyond the C library. It is Debian 13, the release
# distroless/cc-debian13 is itself built from, so the objects match.
FROM debian:13-slim AS entrypoint

ARG EXYOKI_VERSION=0.0.0

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev \
 && rm -rf /var/lib/apt/lists/*

COPY entrypoint.c /tmp/entrypoint.c
RUN gcc -std=c11 -Os -Wall -Wextra -Werror -s \
        -DEXYOKI_VERSION="\"${EXYOKI_VERSION}\"" \
        -o /tmp/exyoki-docker /tmp/entrypoint.c

# cp -L because both names are symbolic links to a versioned file, and COPY
# --from would not resolve them predictably. Copying these unconditionally means
# the image does not depend on what upstream keeps in the cc variant; at worst
# an identical file from the same Debian release is written twice.
RUN mkdir -p /rt/usr/lib/x86_64-linux-gnu \
 && cp -L /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /rt/usr/lib/x86_64-linux-gnu/ \
 && cp -L /lib/x86_64-linux-gnu/libgcc_s.so.1 /rt/usr/lib/x86_64-linux-gnu/

# Debian 13 carries glibc 2.41 and the libstdc++ of GCC 14, both newer than the
# ubuntu-24.04 runner the binaries are built on (glibc 2.39, GCC 13). A runtime
# base older than the builder — cc-debian12, whose glibc is 2.36 — cannot start
# them at all, which is why this line and the runner label in create_install.yml
# have to be read together.
FROM gcr.io/distroless/cc-debian13:nonroot

# There is no shell to ask the image what it is, so the labels are the only
# description of it available from outside. These are the facts that do not
# depend on the run: version, revision and creation time are what does, and
# create_install.yml passes those with `docker build --label` rather than
# letting a default here quietly claim something untrue.
#
# `licenses` is the license of what this repository puts into the image - the
# four programs, the shared library and the dispatcher. It is not a statement
# about the whole filesystem: the distroless base underneath contributes glibc,
# libstdc++ and a handful of Debian base packages, which carry their own terms,
# LGPL and GPL-with-exception among them. Naming those in an SPDX expression
# here would mean pinning a component list this file does not control, so
# `base.name` below points at the image that does, and docs/tools/docker.md
# spells the split out for anyone redistributing the image.
LABEL org.opencontainers.image.title="ExyokiOffice" \
      org.opencontainers.image.description="ExyokiOffice command-line tool and MCP servers for reading and writing OpenXML documents" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.vendor="Jakub Melka and Contributors" \
      org.opencontainers.image.authors="Jakub Melka and Contributors" \
      org.opencontainers.image.url="https://github.com/JakubMelka/ExyokiOffice" \
      org.opencontainers.image.source="https://github.com/JakubMelka/ExyokiOffice" \
      org.opencontainers.image.documentation="https://github.com/JakubMelka/ExyokiOffice/blob/master/docs/tools/docker.md" \
      org.opencontainers.image.base.name="gcr.io/distroless/cc-debian13:nonroot"

COPY --from=entrypoint /rt/ /
COPY --from=entrypoint /tmp/exyoki-docker /opt/exyokioffice/exyoki-docker

# bin/ and the library directory stay siblings under /opt/exyokioffice, which is
# the relation the executables' built-in RPATH ($ORIGIN/../lib) already
# describes. Nothing has to be patched and no LD_LIBRARY_PATH is needed.
COPY rootfs/opt /opt

# The workspace has to exist and belong to the unprivileged user for a run
# without a bind mount to be able to write anything.
COPY --chown=65532:65532 rootfs/work /work

ENV PATH=/opt/exyokioffice/bin:/usr/local/bin:/usr/bin:/bin \
    EXYOKI_MCP_WORKSPACE=/work

WORKDIR /work

ENTRYPOINT ["/opt/exyokioffice/exyoki-docker"]
