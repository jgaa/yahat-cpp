# yahat-cpp
Yet Another HTTP API Thing - A trivial HTTP server for simple REST APIs and other HTTP interfaces in C++ projects.

This project contains a very simple HTTP server-as-a-library to make it easy
to embed an HTTP/REST API in a C++ project. In recent years, I have found
myself re-inventing this wheel a little too frequently when writing 
microservices or other C++ projects that require an HTTP interface. 

# Features
- Can be used to implement a simple embedded HTTP server
- Designed to be used as an embedded API server using the HTTP protocol
- Supports HTTP and HTTPS
- Native support for Server-Sent Events (SSE)

# Metrics

Compile-time switch to enable metrics using the OpenMetrics format.
If enabled, the server provides a `/metrics` endpoint and updates relevant
metrics, such as the number of HTTP requests. You can use the metrics functionality 
directly in your application to add your own metrics. 
This is demonstrated in the [chatserver](examples/chatserver/) example.

The metrics feature is primarily designed for C++ applications to export
their own metrics.

Supported metric types:

- [x] Counter
- [x] Gauge
- [x] Info
- [x] Histogram
- [x] Summary
- [x] Stateset
- [x] Untyped

Metrics can be scraped by Prometheus and most other metrics collectors.

## Using the Untyped Metric

You can derive your own metrics class from `yahat::Metrics::DataType` and 
add it to the metrics by calling `Metrics::AddUntyped<YourClass>()`.

# Platforms
Currently tested only on Linux. Uses `boost.asio` and `boost.beast`.

Uses C++20.

# Yahat Chat
This is a simple chat server that uses HTTP *Server-Sent Events* (SSE) to 
send chat events to the participants. I wrote this example because I 
wanted a simple chat server running locally on my network to
quickly and securely pass information between local computers and phones/tablets.

By default (on log-level info), the server does not log or store any messages. When a message
arrives, it is sent to all participants and then discarded.

The chat *app* running in the web browser uses JavaScript. However, it is kept to an
absolute minimum, and it does not use any JavaScript libraries or external resources. 
It use only the app's own `index.html` file. 

# Status
Beta
