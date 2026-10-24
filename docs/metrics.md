# Metrics

Enable with `YAHAT_ENABLE_METRICS` (default **ON**).

Compile-time switch to enable metrics using the OpenMetrics format.
If enabled, the server provides a `/metrics` endpoint and updates relevant
metrics, such as the number of HTTP requests. You can use the metrics functionality
directly in your application to add your own metrics.
This is demonstrated in the [chatserver](../examples/chatserver/) example.

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
add it to the metrics by calling `Metrics::AddUntyped<YourClass>(name, help, unit, labels, ...)`.
