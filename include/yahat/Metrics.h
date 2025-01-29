#pragma once

#include <chrono>
#include <new>
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
#include <type_traits>
#include <iomanip>
#include <cassert>
#include <format>

#include "yahat/config.h"

#ifdef YAHAT_ENABLE_METRICS
namespace yahat {

static constexpr auto cache_line_size_ = 64u; //std::hardware_destructive_interference_size;
static constexpr bool show_metrics_timestamps = false;

/*! Metrics are designed to be compatible with OpenMetrics
 *
 *  See: https://openmetrics.io/
 */
class Metrics
{
public:

    /*! Scoped is a helper class to increment and decrement a metric in a scope
     *
     *  The metric is incremented when the Scoped is created and decremented when it is destroyed.
     *
     *  The metric must be a Counter or Gauge.
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
    class DataType {
    public:
        enum Type {
            Counter,
            Gauge,
            Histogram,
            Summary,
            Info,
            Stateset,
            Untyped
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

        Scoped<Counter> scoped() {
            return Scoped(this);
        }

    private:
        std::string total_name_ = makeNameWithSuffixAndLabels(name(), "total", labels());
        alignas(cache_line_size_) std::atomic<T> value_{T{}};
    }; // Counter

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

    template <typename T = double>
    class Histogram : public DataType {
    public:

        Histogram(std::string name, std::string help, std::string unit, labels_t labels, std::vector<T> bucket_bounds)
            : DataType(DataType::Type::Histogram, std::move(name), std::move(help), std::move(unit), std::move(labels))
            , bucket_bounds_(std::move(bucket_bounds)), buckets_(bucket_bounds_.size() + 1, 0) {

            init();
        }

        void observe(T value) {
            std::lock_guard<std::mutex> lock(mutex_);
            sum_ += value;
            count_++;
            assert(bucket_bounds_.size() + 1 == buckets_.size());
            for (size_t i = 0; i < bucket_bounds_.size() -1; ++i) {
                if (value <= bucket_bounds_[i]) {
                    buckets_[i]++;
                    break;
                }
            }
            buckets_.back()++; // The "+Inf" bucket
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


    Metrics();
    ~Metrics() = default;

    template<typename T = uint64_t>
    Counter<T> *AddCounter(std::string name, std::string help, std::string unit = {}, labels_t labels = {}) {
        return AddMetric<Counter<T>>(std::move(name), std::move(help), std::move(unit), std::move(labels));
    }

    template<typename T = uint64_t>
    Gauge<T> *AddGauge(std::string name, std::string help, std::string unit = {}, labels_t labels = {}) {
        return AddMetric<Gauge<T>>(std::move(name), std::move(help), std::move(unit), std::move(labels));
    }

    template<typename T = uint64_t>
    Info *AddInfo(std::string name, std::string help, std::string unit = {}, labels_t labels = {}) {
        return AddMetric<Info>(std::move(name), std::move(help), std::move(unit), std::move(labels));
    }

    template<typename T = uint64_t>
    Histogram<T> *AddHistogram(std::string name, std::string help, std::string unit, labels_t labels, std::vector<T> bucket_bounds) {
        return AddMetric<Histogram<T>>(std::move(name), std::move(help), std::move(unit), std::move(labels), bucket_bounds);
    }

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
        if (now_) [[unlikely]] {
            return *now_;
        }

        return std::chrono::system_clock::now();
    }

    void setNow(std::chrono::system_clock::time_point now) {
        now_ = now;
    }

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

