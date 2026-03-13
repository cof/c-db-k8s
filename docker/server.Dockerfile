FROM alpine:latest
ARG BIN_NAME
COPY ${BIN_NAME} /usr/local/bin/server
RUN chmod +x /usr/local/bin/server
ENTRYPOINT ["/usr/local/bin/server"]
