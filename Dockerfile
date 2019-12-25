FROM alpine:latest
RUN apk add --no-cache libev bash
ADD pping.static /usr/bin/pping
CMD ["/bin/bash"]