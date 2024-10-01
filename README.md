# yahat-cpp
Yet Another HTTP API Thing - A trivial HTTP server for simple REST API's and other HTTP interfaces in C++ projects.

This project contains a very simple HTTP server-as-a-library to make it simple
to embed a HTTP/REST API in a C++ project. In recent years, I have found
myself re-inventing this wheel a little to frequently when I have
written micro-services or other C++ project that required a HTTP interface. 

# Features
- Can be used to implement a simple embedded HTTP server
- Desiged to be used as an embedded API server using the HTTP protocol
- Supports http and https
- Native support for Server Side Events

# Metrics

Compile-time switch to enable metrics using the OpenMetrics format.
If used, the server will enable a `/metrics` endpoint and update some
relevant metrics, like number of HTTP requests. You can use the metrics functionality directly in
your application to add your own metrics.

The metrics feature is designed primarily to be used by C++ applications to export
their own metrics.

Supported metrics types:

-[x] Counter
-[x] Gauge
-[x] Info
-[ ] Histogram
-[ ] Summary
-[ ] Stateset
-[ ] Untyped

Metrics can be scraped by Prometheus and most other metrics collectors.

## Platforms
Currently tested only with Linux. Uses boost.asio and boost.beast

Uses C++20.

## Status
Beta
