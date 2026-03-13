FROM alpine:latest
ARG BIN_NAME
COPY ${BIN_NAME} /usr/local/bin/client
RUN chmod +x /usr/local/bin/client
ENTRYPOINT ["/usr/local/bin/client"]
