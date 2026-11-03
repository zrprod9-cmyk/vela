FROM ubuntu:22.04@sha256:0a9887931b0e38af2e3487ed39e29436b0535c4f3f52e57745baaf79d1898956
# Build environment of build.sh: the script downloads the pinned toolchain and sources itself (see its header).
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl git xz-utils \
    && apt-get clean
ENV DEPS=/deps OUT=/out
ENTRYPOINT ["bash", "/interceptor/build.sh"]
