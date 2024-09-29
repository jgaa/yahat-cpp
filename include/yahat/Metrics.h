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

#include "yahat/config.h"

#ifdef YAHAT_ENABLE_METRICS
namespace yahat {

/*! Metrics are designed to be compatible with OpenMetrics
 *
 *  See: https://openmetrics.io/
 */
class Metrics
{
public:
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

        DataType(std::string name, std::string help, std::string unit, labels_t labels = {});

        DataType() = delete;
        DataType(const DataType&) = delete;
        DataType(DataType&&) = delete;
        DataType& operator=(const DataType&) = delete;
        DataType& operator=(DataType&&) = delete;

        virtual ~DataType() = default;

        virtual Type type() const noexcept = 0;
        const std::string_view typeName() const noexcept;
        const std::string& name() const noexcept { return name_; }
        const std::string& help() const noexcept { return help_; }
        const std::string& unit() const noexcept { return unit_; }
        const std::string& metricName() const noexcept { return metricName_; }
        std::string nameWithSuffix(const std::string& suffix) const;
        const labels_t& labels() const noexcept { return labels_; }

        virtual std::ostream& render(std::ostream& target) const = 0;
        std::ostream& renderCreated(std::ostream& target) const;

        static std::string makeKey(const std::string& name, const labels_t& labels);
        static labels_t makeLabels(labels_t source);
        static std::string makeNameWithSuffixAndLabels(const std::string name, const std::string& suffix, const labels_t& labels);
        static std::ostream& renderNumber(std::ostream& target, double value, uint maxDecimals = 6);
        static std::ostream& renderNumber(std::ostream& target, uint64_t value, uint maxDecimals = 0) {
            return target << value;
        }


        private:
        std::string makeMetricName() const noexcept;
        std::string labelString() const;

        const std::string name_;
        const std::string help_;
        const std::string unit_;
        const labels_t labels_;
        const std::string metricName_;
        const std::chrono::system_clock::time_point created_{now()};
        const std::string created_name_ = makeNameWithSuffixAndLabels(name_, "created", labels_);
    };

    template <typename T = uint64_t>
    class Counter : public DataType {
    public:
        Counter(std::string name, std::string help, std::string unit, labels_t labels = {})
            : DataType(name, help, unit, std::move(labels)) {}

        Type type() const noexcept override { return Type::Counter; }

        void inc(T value=1) noexcept {
            assert(value >= 0);
            value_.fetch_add(value, std::memory_order_relaxed);
        }

        T value() const noexcept {
            return value_.load(std::memory_order_relaxed);
        }

        std::ostream& render(std::ostream& target) const override {
            target << total_name_ << ' ';
            renderNumber(target, value()) << '\n';
            return renderCreated(target);
        }

    private:
        std::atomic<T> value_{T{}};
        std::string total_name_ = makeNameWithSuffixAndLabels(name(), "total", labels());
    };

    Metrics();
    ~Metrics() = default;

    template<typename T = uint64_t>
    Counter<T> *AddCounter(std::string name, std::string help, std::string unit = {}, labels_t labels = {}) {
        auto c = std::make_unique<Counter<T>>(name, help, unit, std::move(labels));
        auto * ptr = c.get();
        std::lock_guard lock(mutex_);
        auto key = c->metricName();
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
        std::lock_guard lock(mutex_);
        auto key = c->metricName();
        auto [it, added] = metrics_.emplace(key, std::move(c));
        if (!added) {
            throw std::invalid_argument("Metric already exists with the same labels");
        }
        return ptr;
    }

    DataType * lookup(const std::string& name, labels_t labels = {}) {
        const auto my_labels = DataType::makeLabels(labels);
        const auto key = DataType::makeKey(name, my_labels);

        std::lock_guard lock(mutex_);
        auto it = metrics_.find(key);
        if (it == metrics_.end()) {
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

private:
    std::map<std::string, std::unique_ptr<DataType>> metrics_;
    static std::optional<std::chrono::system_clock::time_point> now_; // Fot unit tests

    alignas(std::hardware_destructive_interference_size) std::mutex mutex_;
    char mpadding_[std::hardware_destructive_interference_size - sizeof(std::mutex)];
};

} // ns

#endif // YAHAT_ENABLE_METRICS

