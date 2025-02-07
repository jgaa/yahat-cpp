FROM ubuntu:noble
MAINTAINER Jarle Aase <jgaa@jgaa.com>

ENV DEBIAN_FRONTEND=noninteractive


RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    zlib1g \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY yahatchat /usr/local/bin

# Expose default port (optional)
EXPOSE 8080

# Set default environment variables (can be overridden at runtime)
ENV PORT=8080
ENV ENDPOINT="0.0.0.0"
ENV LOGLEVEL="info"

# Define the entrypoint and allow users to override variables
CMD yahatchat --http-port "$PORT" --http-endpoint "$ENDPOINT" --log-level "$LOGLEVEL"
