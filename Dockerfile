FROM ubuntu:bionic AS builder
RUN apt update && apt install -y build-essential curl git
RUN apt-get install -y libssl-dev
ADD . /workdir

WORKDIR /workdir
RUN cd deps && ./download-ssl-deps.sh
RUN OPENSSL_LIB_DIR=/usr/lib/x86_64-linux-gnu/ BUILD_SSL=yes make

FROM ubuntu:bionic
# Copy redis binaries
COPY --from=builder /workdir/src/redis-server /redis-server
COPY --from=builder /workdir/src/redis-cli /redis-cli
COPY --from=builder /workdir/src/redis-sentinel /redis-sentinel
RUN mkdir -p /etc/redis/ssl

CMD ["/redis-server", "--loglevel", "debug",  "--enable-ssl", "yes", "--certificate-file", "/etc/redis/ssl/server.crt", "--private-key-file", "/etc/redis/ssl/server.key", "--root-ca-certs-path", "/etc/redis/ssl/ca.crt", "--dh-params-file", "/etc/redis/ssl/dh_params.dh", "--port", "6401", "--bind", "0.0.0.0"]


