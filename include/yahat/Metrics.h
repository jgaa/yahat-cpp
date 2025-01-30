/**
 * @file Metrics.h
 * @brief OpenMetrics-compatible metrics implementation.
 *
 * This file defines various metric types (Counter, Gauge, Histogram, Summary, Info, Stateset)
 * used for monitoring applications in compliance with OpenMetrics.
 *
 * It is part of the yahat project, but it can be used independently.
 *
 * @see https://openmetrics.io/
 */

#pragma once

#include <chrono>
#include <iostream>
#include <optional>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <cassert>
#include <format>
#include <cmath>

#include <boost/circular_buffer.hpp>

#include "yahat/config.h"

#ifdef YAHAT_ENABLE_METRICS
namespace yahat {

static constexpr auto cache_line_size_ = 64u; //std::hardware_destructive_interference_size;
static constexpr bool show_metrics_timestamps = false;

template <typename T>
concept EnumType = std::is_enum_v<T>;

/**
 * @class Metrics
 * @brief A collection of metrics for OpenMetrics-based monitoring.
 *
 * This class provides an interface for defining and managing different types of metrics,
 * such as counters, gauges, histograms, and more.
 */
class Metrics
{

public:

    /**
     * @class Scoped
     * @brief RAII-based helper class for incrementing and decrementing metrics.
     *
     * The metric is incremented upon creation and decremented upon destruction.
     * This is useful for tracking resource lifetimes or active operations.
     *
     * @tparam T The metric type, which must support `inc()` and `dec()` methods.
     */
    template <typename T>
    class Scoped {
    public:
        Scoped() = default;
        Scoped(T * metric) : metric_(metric) {
            assert(metric_);
            metric->inc();
        }

        Scoped(const Scoped&) = delete;
        Scoped(Scoped&& v) noexcept
            : metric_(v.metric_) {
            v.metric_ = nullptr;
        }

        void operator = (const Scoped&) = delete;
        Scoped& operator = (Scoped&& v) {
            metric_ = v.metric_;
            v.metric_ = nullptr;
            return *this;
        }

        ~Scoped() {
            if (metric_) {
                metric_->dec();
                metric_ = {};
            }
        }

    private:
        T * metric_{};
    };

    /**
     * @class ScopedTimer
     * @brief Measures execution duration and updates a histogram metric.
     *
     * The duration is recorded when the ScopedTimer instance goes out of scope.
     *
     * @tparam metricT The histogram metric type.
     * @tparam valueT The value type used for storing durations.
     */
    template <typename metricT, typename valueT>
    class ScopedTimer {
    public:
        using clock_t = std::chrono::steady_clock;
        ScopedTimer() = delete;
        explicit ScopedTimer(metricT* histogram)
            : metric_{histogram}, start_time_(clock_t::now()) {}

        ScopedTimer(ScopedTimer&&v)
            : metric_{v.metric_}, start_time_{v.start_time_} {
            metric_ = {};
        }

        ScopedTimer(const ScopedTimer&) = delete;

        ScopedTimer& operator=(ScopedTimer&& v) {
            metric_ = v.metric_;
            start_time_ = v.start_time_;
            v.metric_ = {};
            return *this;
        }

        ScopedTimer& operator=(const ScopedTimer&) = delete;

        ~ScopedTimer() {
            if (metric_) {
                auto duration = get_duration();
                metric_->observe(duration);
            }
        }

        valueT get_duration() const {
            const auto end_time = clock_t::now();
            return std::chrono::duration<valueT>(end_time - start_time_).count();
        }

        /*! Cancel the measurement.
         *
         *  This is useful when you start metrics automatically,
         *  for exampe in a request handler, but want to exclude
         *  some requests from the metrics.
         */
        void cancel() {
            metric_ = {};
        }

    private:
        metricT* metric_{};
        clock_t::time_point start_time_;
    };

    using label_t = std::pair<std::string, std::string>;
    using labels_t = std::vector<label_t>;

    /**
     * @class DataType
     * @brief Base class for all metric types.
     */
    class DataType {
    public:
        /**
         * @enum Type
         * @brief Enumerates the types of supported metrics.
         */
        enum Type {
            Counter, ///< Counter metric
            Gauge, ///< Gauge metric
            Histogram, ///< Histogram metric
            Summary, ///< Summary metric
            Info, ///< Info metric
            Stateset, ///< Stateset metric
            Untyped ///< Untyped metric
        };

        DataType(Type type, std::string name, std::string help, std::string unit, labels_t labels);

        DataType() = delete;
        DataType(const DataType&) = delete;
        DataType(DataType&&) = delete;
        DataType& operator=(const DataType&) = delete;
        DataType& operator=(DataType&&) = delete;

        virtual ~DataType() = default;

        Type type() const noexcept { return type_; }
        const std::string_view typeName() const noexcept;
        const std::string& name() const noexcept { return name_; }
        const std::string& help() const noexcept { return help_; }
        const std::string& unit() const noexcept { return unit_; }
        const std::string& metricName() const noexcept { return metricName_; }
        std::string nameWithSuffix(const std::string& suffix) const;
        const labels_t& labels() const noexcept { return labels_; }

        virtual std::ostream& render(std::ostream& target) const = 0;
        std::ostream& renderCreated(std::ostream& target, bool postfix = false) const;

        static std::string makeKey(const std::string& name, const labels_t& labels, std::optional<DataType::Type> type = {});
        static labels_t makeLabels(labels_t source);
        static std::string makeNameWithSuffixAndLabels(const std::string name, const std::string& suffix,
                                                       const labels_t& labels, bool first = false);
        static std::ostream& renderNumber(std::ostream& target, double value, uint maxDecimals = 6);
        static std::ostream& renderNumber(std::ostream& target, uint64_t value, uint maxDecimals = 0) {
            return target << value;
        }

        void touch () noexcept {
            updated_.store(now(), std::memory_order_relaxed);
        }

        std::chrono::system_clock::time_point updated() const noexcept {
            return updated_.load(std::memory_order_relaxed);
        }

        private:
        std::string makeMetricName() const noexcept;
        std::string labelString() const;

        const Type type_;
        const std::string name_;
        const std::string help_;
        const std::string unit_;
        const labels_t labels_;
        const std::string metricName_;
        std::atomic<std::chrono::system_clock::time_point> updated_{now()};
        const std::string created_name_ = makeNameWithSuffixAndLabels(name_, "created", labels_);
    };

    /**
     * @class Counter
     * @brief A monotonically increasing counter metric.
     *
     * Used for counting occurrences of events, such as HTTP requests served.
     * @tparam T The underlying numeric type (default: uint64_t).
     */
    template <typename T = uint64_t>
    class Counter : public DataType {
    public:
        Counter(std::string name, std::string help, std::string unit, labels_t labels = {})
            : DataType(DataType::Type::Counter, std::move(name), std::move(help), std::move(unit), std::move(labels)) {}

        void inc(T value=1) noexcept {
            assert(value >= 0);
            value_.fetch_add(value, std::memory_order_relaxed);
            touch();
        }

        T value() const noexcept {
            return value_.load(std::memory_order_relaxed);
        }

        std::ostream& render(std::ostream& target) const override {
            target << total_name_ << ' ';
            renderNumber(target, value()) << ' '; //<< '\n';
            return renderCreated(target, true);
        }

    private:
        std::string total_name_ = makeNameWithSuffixAndLabels(name(), "total", labels());
        alignas(cache_line_size_) std::atomic<T> value_{T{}};
    }; // Counter

    /**
     * @class Gauge
     * @brief A metric that can increase and decrease.
     *
     * Used for measuring values such as temperatures or queue lengths.
     * @tparam T The underlying numeric type (default: uint64_t).
     */
    template <typename T = uint64_t>
    class Gauge : public DataType {
    public:
        Gauge(std::string name, std::string help, std::string unit, labels_t labels = {})
            : DataType(DataType::Type::Gauge, std::move(name), std::move(help), std::move(unit), std::move(labels)) {}

        void set(T value) noexcept {
            value_.store(value, std::memory_order_relaxed);
            touch();
        }

        void inc(T value=1) noexcept {
            value_.fetch_add(value, std::memory_order_relaxed);
            touch();
        }

        void dec(T value=1) noexcept {
            assert((value_.load() - value) >= 0);
            value_.fetch_sub(value, std::memory_order_relaxed);
            touch();
        }

        T value() const noexcept {
            return value_.load(std::memory_order_relaxed);
        }

        Scoped<Gauge> scoped() {
            return Scoped(this);
        }

        std::ostream& render(std::ostream& target) const override {
            target << metricName() << ' ';
            renderNumber(target, value()) << ' ';
            return renderCreated(target, true);
        }

    private:
        alignas(cache_line_size_) std::atomic<T> value_{T{}};
    }; // Gauge

    /**
     * @class Info
     * @brief A metric representing static informational labels.
     *
     * The `Info` metric is used to expose metadata about the system or application, such as version numbers,
     * build details, or other static attributes that do not change over time.
     *
     * This metric always has a value of `1` and is primarily used for labeling.
     *
     * @note Unlike other metric types, `Info` does not support numerical increments or decrements.
     */

    class Info : public DataType {
    public:
        Info(std::string name, std::string help, std::string unit, labels_t labels = {})
            : DataType(DataType::Type::Info, std::move(name), std::move(help), std::move(unit), std::move(labels)) {}

        std::ostream& render(std::ostream& target) const override {
            target << info_name_ << " 1 ";
            return renderCreated(target, true);
        }

    private:
        std::string info_name_ = makeNameWithSuffixAndLabels(name(), "info", labels());
    }; // Gauge

    /**
     * @class Histogram
     * @brief A metric that samples observations and counts them in configurable buckets.
     *
     * The `Histogram` metric is used to measure and analyze the distribution of values,
     * such as request durations or response sizes, by sorting them into predefined buckets.
     *
     * Each observation is counted in a specific bucket based on its value, and the sum and count
     * of all observations are tracked.
     *
     * @tparam T The numeric type used for the observations (default: `double`).
     */
    template <typename T = double>
    class Histogram : public DataType {
    public:

        Histogram(std::string name, std::string help, std::string unit, labels_t labels, std::vector<T> bucket_bounds)
            : DataType(DataType::Type::Histogram, std::move(name), std::move(help), std::move(unit), std::move(labels))
            , bucket_bounds_(std::move(bucket_bounds)), buckets_(bucket_bounds_.size() + 1, 0) {

            init();
        }

        /**
         * @brief Records an observation in the histogram.
         *
         * The observation is placed in the appropriate bucket, and the total sum
         * and count of observations are updated.
         *
         * @param value The observed value to be recorded.
         */
        void observe(T value) {
            std::lock_guard<std::mutex> lock(mutex_);
            sum_ += value;
            count_++;
            assert(bucket_bounds_.size() + 1 == buckets_.size());

            for (size_t i = 0; i < bucket_bounds_.size(); ++i) {
                if (value <= bucket_bounds_[i]) {
                    buckets_[i]++;
                    return;
                }
            }

            buckets_.back()++; // +Inf bucket
        }


        std::ostream& render(std::ostream& target) const override {
            assert(!bucket_names_.empty());
            std::vector<T> values;
            values.reserve((bucket_names_.size()));
            {
                // Copy the values so we don't hold the lock while doing slow formatting...
                std::lock_guard<std::mutex> lock(mutex_);
                values = buckets_;
                values.emplace_back(count_);
                values.emplace_back(sum_);
            }

            // First rows are the buckets. Then the bucket +Inf, then count and finally sum.
            const auto inf_row = values.size() - 3;
            const auto count_row = values.size() - 2;

            for (auto row = 0u; row < values.size(); ++row) {
                target << bucket_names_[row]  << ' ';
                if (row == inf_row || row == count_row) {
                    target << std::format("{} ", static_cast<uint64_t>(values[row]));
                } else {
                    renderNumber(target, values[row]) << ' ';
                }
                renderCreated(target, true);
            }

            return target;
        }

        auto scoped() {
            return ScopedTimer<Histogram, T>(this);
        }

        auto getCount() const noexcept {
            return count_;
        }

        auto getSum() const noexcept {
            return sum_;
        }

        auto getBucketCounts() const noexcept {
            return buckets_;
        }

    private:
        void init() {
            for(const auto &bound : bucket_bounds_) {
                auto xlabels = labels();
                xlabels.emplace_back("le", std::format("{:g}",bound));
                bucket_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "bucket", xlabels));
            };

            {
                auto xlabels = labels();
                xlabels.emplace_back("le", "+Inf");
                bucket_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "bucket", xlabels));
            }

            bucket_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "count", labels()));
            bucket_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "sum", labels()));
        }

        T sum_;
        uint64_t count_;
        std::vector<T> bucket_bounds_;
        std::vector<T> buckets_;
        std::vector<std::string> bucket_names_;
        alignas(cache_line_size_) mutable std::mutex mutex_;
    };

    /**
     * @class Summary
     * @brief A metric that captures quantiles and aggregates values over time.
     *
     * The `Summary` metric is used to track distributions of values, such as request latencies,
     * by computing quantiles (e.g., p50, p90, p99) from recorded observations.
     *
     * Unlike a Histogram, a Summary does not use predefined buckets but instead calculates quantiles dynamically.
     *
     * @tparam T The numeric type used for the observations (default: `double`).
     */
    template <typename T = double>
    class Summary : public DataType {
    public:
        /**
         * @brief Constructs a Summary metric with specified parameters.
         *
         * @param name The name of the metric.
         * @param help A description of what the metric represents.
         * @param unit The unit of measurement (e.g., "seconds").
         * @param labels Key-value pairs to distinguish instances of the metric.
         * @param quantiles A vector specifying quantiles to be calculated (e.g., {0.5, 0.9, 0.99}).
         * @param max_samples The maximum number of samples stored in the summary (default: 500).
         */
        Summary(std::string name, std::string help, std::string unit, labels_t labels, std::vector<T> quantiles, size_t max_samples = 500)
            : DataType(DataType::Type::Summary, std::move(name), std::move(help), std::move(unit), std::move(labels))
            , quantiles_(std::move(quantiles))
            , observations_(max_samples) { // Initialize circular buffer with fixed capacity
            init();
        }

        /**
         * @brief Records an observation in the summary.
         *
         * Observations are stored in a ring buffer and used to compute quantiles.
         *
         * @param value The observed value to be recorded.
         */
        void observe(T value) {
            std::lock_guard<std::mutex> lock(mutex_);
            sum_ += value;
            count_++;
            observations_.push_back(value); // Automatically evicts oldest value if full
        }

        std::ostream& render(std::ostream& target) const override {
            assert(!summary_names_.empty());
            std::vector<T> values;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                values = calculateQuantiles();
                values.emplace_back(count_);
                values.emplace_back(sum_);
            }

            const auto count_row = values.size() - 2;

            for (size_t row = 0; row < values.size(); ++row) {
                target << summary_names_[row] << ' ';
                if (row == count_row) {
                    target << std::format("{} ", static_cast<uint64_t>(values[row]));
                } else {
                    renderNumber(target, values[row]) << ' ';
                }
                renderCreated(target, true);
            }

            return target;
        }

        auto scoped() {
            return ScopedTimer<Summary, T>(this);
        }

        auto getCount() const noexcept {
            return count_;
        }

        auto getSum() const noexcept {
            return sum_;
        }

        std::vector<T> calculateQuantiles() const {
            if (observations_.empty()) {
                return std::vector<T>(quantiles_.size(), 0.0);
            }

            std::vector<T> sorted_data(observations_.begin(), observations_.end());
            std::sort(sorted_data.begin(), sorted_data.end());

            std::vector<T> results;
            size_t n = sorted_data.size();

            for (const auto &q : quantiles_) {
                double pos = q * n - 0.5;  // Shift index calculation
                size_t index = static_cast<size_t>(std::max(0.0, std::floor(pos)));  // Ensure index is valid
                double fraction = pos - index;

                if (index >= n - 1) {
                    results.push_back(sorted_data[n - 1]);  // Last element case
                } else {
                    results.push_back(sorted_data[index] + fraction * (sorted_data[index + 1] - sorted_data[index]));
                }
            }
            return results;
        }

    private:
        void init() {
            for (const auto &quantile : quantiles_) {
                auto xlabels = labels();
                xlabels.emplace_back("quantile", std::format("{:g}", quantile));
                summary_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "", xlabels));
            }

            summary_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "count", labels()));
            summary_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "sum", labels()));
        }

        T sum_ = 0;
        uint64_t count_ = 0;
        std::vector<T> quantiles_;
        boost::circular_buffer<T> observations_;  // Replaces std::vector for efficient ring buffer
        std::vector<std::string> summary_names_;
        alignas(cache_line_size_) mutable std::mutex mutex_;
    };


    /**
     * @class Stateset
     * @brief A metric that represents a set of mutually exclusive states.
     *
     * The `Stateset` metric is used to track the current state of an entity by marking one or more states as active.
     * This is useful for monitoring system statuses, feature toggles, or process states.
     *
     * Each state in the set is represented as a boolean value (active/inactive).
     *
     * @tparam MaxCapacity The maximum number of states that can be stored in the stateset.
     */
    template <size_t MaxCapacity>
    class Stateset : public DataType {
    public:
        /**
         * @brief Constructs a Stateset metric with specified states.
         *
         * @param name The name of the metric.
         * @param help A description of what the metric represents.
         * @param unit The unit of measurement (typically empty for states).
         * @param labels Key-value pairs to distinguish instances of the metric.
         * @param states A vector of state names that the metric can track.
         * @throws std::invalid_argument If the number of states exceeds MaxCapacity.
         */
        Stateset(std::string name, std::string help, std::string unit, labels_t labels, std::vector<std::string> states)
            : DataType(DataType::Type::Stateset, std::move(name), std::move(help), std::move(unit), std::move(labels))
            , states_(std::move(states)) {
            if (states_.size() > MaxCapacity) {
                throw std::invalid_argument("Too many states for the defined MaxCapacity");
            }
            init();
        }

        /**
         * @brief Sets the state of a specific state name.
         *
         * Marks the given state as either active or inactive.
         *
         * @param state The name of the state to update.
         * @param active Boolean indicating whether the state should be active.
         * @throws std::out_of_range If the provided state name is not in the set.
         */
        void setState(const std::string& state, bool active) {
            auto it = state_index_.find(state);
            if (it != state_index_.end()) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_values_.at(it->second) = active ? 1 : 0;
                return;
            }
            throw std::out_of_range("Invalid state name");
        }

        /**
         * @brief Sets the state using its index in the stateset.
         *
         * Marks the state at the given index as either active or inactive.
         *
         * @param index The index of the state to update.
         * @param active Boolean indicating whether the state should be active.
         * @throws std::out_of_range If the index is out of bounds.
         */
        void setState(size_t index, bool active) {
            if (index >= states_.size()) {
                throw std::out_of_range("Invalid state index");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            state_values_[index] = active ? 1 : 0;
        }

        /**
         * @brief Sets the state using an enumeration value.
         *
         * Marks the state at the given index as either active or inactive.
         *
         * @tparam T The enumeration type used for the state index.
         * @param index The index of the state to update.
         * @param active Boolean indicating whether the state should be active.
         */
        template <EnumType T>
        void setState(T index, bool active) {
            setState(static_cast<size_t>(index), active);
        }

        /**
         * @brief Sets one state as active and deactivates all others.
         *
         * This is useful for mutually exclusive states, such as operational statuses.
         *
         * @param index The index of the state to activate.
         * @throws std::out_of_range If the index is out of bounds.
         */
        void setExclusiveState(size_t index) {
            if (index >= states_.size()) {
                throw std::out_of_range("Invalid state index");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; i < states_.size(); ++i) {
                state_values_[i] = (i == index) ? 1 : 0;
            }
        }

        /**
         * @brief Sets one state as active and deactivates all others.
         *
         * This is useful for mutually exclusive states, such as operational statuses.
         *
         * @tparam T The enumeration type used for the state index.
         * @param index The index of the state to activate.
         */
        template <EnumType T>
        void setExclusiveState(T index) {
            setExclusiveState(static_cast<size_t>(index));
        }

        /**
         * @brief Retrieves the activation status of a state by name.
         *
         * @param state The name of the state to query.
         * @return `true` if the state is active, `false` otherwise.
         * @throws std::out_of_range If the provided state name is not in the set.
         */
        bool getState(const std::string& state) const {
            auto it = state_index_.find(state);
            if (it != state_index_.end()) {
                assert(it->second < state_values_.size());
                std::lock_guard<std::mutex> lock(mutex_);
                return state_values_[it->second] == 1;
            }
            throw std::out_of_range("Invalid state name");
        }

        /**
         * @brief Retrieves the activation status of a state by index.
         *
         * @param index The index of the state to query.
         * @return `true` if the state is active, `false` otherwise.
         * @throws std::out_of_range If the index is out of bounds.
         */
        bool getState(size_t index) const {
            if (index >= states_.size()) {
                throw std::out_of_range("Invalid state index");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            return state_values_[index] == 1;
        }

        std::ostream& render(std::ostream& target) const override {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; i < states_.size(); ++i) {
                target << state_names_[i] << " " << state_values_[i] << " ";
                renderCreated(target, true);
            }
            return target;
        }

    private:
        void init() {
            assert(states_.size() <= MaxCapacity);
            for (size_t i = 0; i < states_.size(); ++i) {
                auto xlabels = labels();
                xlabels.emplace_back("state", states_[i]);
                state_names_.emplace_back(makeNameWithSuffixAndLabels(name(), "stateset", xlabels));
                state_index_[states_[i]] = i;
            }
        }

        std::array<uint8_t, MaxCapacity> state_values_{};

        // Immutable data after init()
        std::vector<std::string> state_names_;
        std::vector<std::string> states_;
        std::map<std::string, size_t> state_index_;
        alignas(cache_line_size_) mutable std::mutex mutex_;
    };


    Metrics();
    ~Metrics() = default;

    /**
     * @brief Adds a Counter metric.
     *
     * A Counter is a monotonically increasing metric used to count occurrences of events.
     *
     * @tparam T The numeric type used for counting (default: `uint64_t`).
     * @param name The name of the counter metric.
     * @param help A description of what the counter represents.
     * @param unit The unit of measurement (default: empty).
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @return A pointer to the created Counter metric. The Metrics instance owns the metric.
     */
    template<typename T = uint64_t>
    Counter<T> *AddCounter(std::string name, std::string help, std::string unit = {}, labels_t labels = {}) {
        return AddMetric<Counter<T>>(std::move(name), std::move(help), std::move(unit), std::move(labels));
    }


    /**
     * @brief Adds a Gauge metric.
     *
     * A Gauge is a metric that can increase or decrease over time, suitable for measuring values like temperature or queue size.
     *
     * @tparam T The numeric type used for the gauge (default: `uint64_t`).
     * @param name The name of the gauge metric.
     * @param help A description of what the gauge represents.
     * @param unit The unit of measurement (default: empty).
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @return A pointer to the created Gauge metric. The Metrics instance owns the metric
     */
    template<typename T = uint64_t>
    Gauge<T> *AddGauge(std::string name, std::string help, std::string unit = {}, labels_t labels = {}) {
        return AddMetric<Gauge<T>>(std::move(name), std::move(help), std::move(unit), std::move(labels));
    }

    /**
     * @brief Adds an Info metric.
     *
     * An Info metric is used to expose metadata or labels about the system without changing values dynamically.
     *
     * @tparam T The numeric type (default: `uint64_t`).
     * @param name The name of the info metric.
     * @param help A description of what the info metric represents.
     * @param unit The unit of measurement (default: empty).
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @return A pointer to the created Info metric. The Metrics instance owns the metric
     */
    template<typename T = uint64_t>
    Info *AddInfo(std::string name, std::string help, std::string unit = {}, labels_t labels = {}) {
        return AddMetric<Info>(std::move(name), std::move(help), std::move(unit), std::move(labels));
    }

    /**
     * @brief Adds a Histogram metric.
     *
     * A Histogram is used to sample observations and bucket them into predefined intervals.
     * This is useful for tracking distributions, such as request durations.
     *
     * @tparam T The numeric type used for the histogram (default: `double`).
     * @param name The name of the histogram metric.
     * @param help A description of what the histogram represents.
     * @param unit The unit of measurement.
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @param bucket_bounds A vector defining the upper bounds of each histogram bucket.
     * @return A pointer to the created Histogram metric. The Metrics instance owns the metric
     */
    template<typename T = double>
    Histogram<T> *AddHistogram(std::string name, std::string help, std::string unit, labels_t labels, std::vector<T> bucket_bounds) {
        return AddMetric<Histogram<T>>(std::move(name), std::move(help), std::move(unit), std::move(labels), bucket_bounds);
    }

    /**
     * @brief Adds a Summary metric.
     *
     * A Summary tracks quantiles of observed values over time, such as request latencies.
     * Unlike a Histogram, it does not use predefined buckets but dynamically calculates quantiles.
     *
     * @tparam T The numeric type used for the summary (default: `double`).
     * @param name The name of the summary metric.
     * @param help A description of what the summary represents.
     * @param unit The unit of measurement.
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @param quantiles A vector specifying quantiles to be calculated (e.g., {0.5, 0.9, 0.99}).
     * @param max_samples The maximum number of observations stored in the summary (default: 500).
     * @return A pointer to the created Summary metric. The Metrics instance owns the metric
     */
    template<typename T = double>
    Summary<T> *AddSummary(std::string name, std::string help, std::string unit, labels_t labels,
                             std::vector<T> quantiles, size_t max_samples = 500) {
        return AddMetric<Summary<T>>(std::move(name), std::move(help), std::move(unit), std::move(labels),
                                       quantiles, max_samples);
    }

    /**
     * @brief Adds a Stateset metric.
     *
     * A Stateset tracks a set of mutually exclusive states, marking one or more as active at a time.
     * This is useful for monitoring system statuses, feature toggles, or process states.
     *
     * @tparam MaxCapacity The maximum number of states that can be stored in the stateset.
     * @param name The name of the stateset metric.
     * @param help A description of what the stateset represents.
     * @param unit The unit of measurement (typically empty for states).
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @param states A vector of state names that the metric can track.
     * @return A pointer to the created Stateset metric. The Metrics instance owns the metric
     */
    template<size_t MaxCapacity>
    Stateset<MaxCapacity> *AddStateset(std::string name, std::string help, std::string unit,
                                       labels_t labels, std::vector<std::string> states) {
        return AddMetric<Stateset<MaxCapacity>>(std::move(name), std::move(help), std::move(unit),
                                                std::move(labels), states);
    }

    /**
     * @brief Adds an Untyped metric.
     *
     * An Untyped metric is used when the type of metric is unknown or variable.
     * It allows for flexible metric recording that doesn't fit into predefined categories.
     *
     * @tparam T The metric class to use for the Untyped metric. Must be derived from `DataType`.
     * @tparam argsT Additional arguments required for constructing the metric.
     * @param name The name of the metric.
     * @param help A description of what the metric represents.
     * @param unit The unit of measurement.
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @param args Additional constructor arguments for the metric.
     * @return A pointer to the created Untyped metric. The Metrics instance owns the metric
     */
    template<typename T, typename ...argsT>
    auto *AddUntyped(std::string name, std::string help, std::string unit, labels_t labels, argsT&&... args) {
        return AddMetric<T>(std::move(name), std::move(help), std::move(unit), std::move(labels),
                                     std::forward<argsT>(args)...);
    }

    /**
     * @brief A helper method for creating and adding metrics of various types.
     *
     * This method is used internally by `AddCounter`, `AddGauge`, `AddHistogram`, `AddSummary`, `AddInfo`,
     * `AddStateset`, and `AddUntyped` to instantiate a metric object and store it in the internal metric collection.
     *
     * @tparam T The metric type to be created.
     * @tparam argsT Additional arguments required for constructing the metric.
     * @param name The name of the metric.
     * @param help A description of what the metric represents.
     * @param unit The unit of measurement (default: empty).
     * @param labels Key-value pairs to distinguish instances of the metric.
     * @param args Additional constructor arguments specific to the metric type.
     * @return A pointer to the created metric instance. The Metrics instance owns the metric
     */
    template<typename T, typename ...argsT>
    T *AddMetric(std::string name, std::string help, std::string unit = {}, labels_t labels = {}, argsT&&... args) {
        auto c = std::make_unique<T>(std::move(name), std::move(help), std::move(unit), std::move(labels), std::forward<argsT>(args)...);
        auto * ptr = c.get();
        std::lock_guard lock(mutex_);
        auto key = DataType::makeKey(c->name(), c->labels(), c->type());
        metrics_.emplace(key, std::move(c));
        return ptr;
    }

    Metrics(const Metrics&) = delete;
    Metrics(Metrics&&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    Metrics& operator=(Metrics&&) = delete;

    /*! Clone a metric, but not its labels.
     *
     *  Used to create a new metric with the same name, but different labels.
     */
    template <typename T>
    T * clone(T& source, labels_t labels) {
        auto c = std::make_unique<T>(source.name(), source.help(), source.unit(), std::move(labels));
        auto * ptr = c.get();
        const auto key = DataType::makeKey(c->name(), c->labels(), c->type());

        std::lock_guard lock(mutex_);
        auto [it, added] = metrics_.emplace(key, std::move(c));
        if (!added) {
            throw std::invalid_argument("Metric already exists with the same labels");
        }
        return ptr;
    }

    DataType * lookup(const std::string& name, labels_t labels = {}, std::optional<DataType::Type> type = {}) {
        const auto my_labels = DataType::makeLabels(labels);
        const auto key = DataType::makeKey(name, my_labels, type);

        std::lock_guard lock(mutex_);
        auto it = metrics_.find(key);
        if (it == metrics_.end()) {
            return nullptr;
        }

        if (type && type.value() != it->second->type()) {
            return nullptr;
        }

        return it->second.get();
    }

    /*! Generate the Metrics in OpenMetrics format
     *
     *  @param target The stream to write the metrics to
     */
    void generate(std::ostream& target);

    static std::chrono::system_clock::time_point now() {
        // if (now_) [[unlikely]] {
        //     return *now_;
        // }

        return std::chrono::system_clock::now();
    }

    // void setNow(std::chrono::system_clock::time_point now) {
    //     now_ = now;
    // }

    std::string_view contentType() const noexcept {
        return "application/openmetrics-text; version=1.0.0; charset=utf-8";
    }

private:
    std::map<std::string, std::unique_ptr<DataType>> metrics_;
    static std::optional<std::chrono::system_clock::time_point> now_; // Fot unit tests

    alignas(cache_line_size_) std::mutex mutex_;
    std::array<char, cache_line_size_ - sizeof(std::mutex)> mpadding_{};
};

} // ns

#endif // YAHAT_ENABLE_METRICS

