FROM alpine:latest
RUN apk add --no-cache libev
ADD pping.static /usr/bin/pping
