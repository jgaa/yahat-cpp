# Yahat Chat

[![Make YahatChat container image](https://github.com/jgaa/yahat-cpp/actions/workflows/yahatchat_container_image.yaml/badge.svg)](https://github.com/jgaa/yahat-cpp/actions/workflows/yahatchat_container_image.yaml)

This is a simple chat server that uses HTTP *Server-Sent Events* (SSE) to
send chat events to the participants. I wrote this example because I
wanted a simple chat server running locally on my network to
quickly and securely pass information between local computers and phones/tablets.

By default (on log-level info), the server does not log or store any messages. When a message
arrives, it is sent to all participants and then discarded.

The chat *app* running in the web browser uses JavaScript. However, it is kept to an
absolute minimum, and it does not use any JavaScript libraries or external resources.
It uses only the app's own `index.html` file.

If you can run Docker containers on your machine, there is an official docker image available for yahatchat.

You can run it locally on port 8000 with this command. Since we are binding to `0.0.0.0`, it will be available for all the machines on your local network. If you just want to test it locally, use `127.0.0.1` instead.

```sh
docker run --rm -d --name yahatchat -p 0.0.0.0:8000:8080 ghcr.io/jgaa/yahatchat
```

Source: [chat server example](../examples/chatserver/).
