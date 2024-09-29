#pragma once

#include <new>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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

        DataType(Type type, std::string name, std::string help, std::string unit, labels_t labels = {});

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

        private:
        std::string makeMetricName() const noexcept;

        const std::string name_;
        const std::string help_;
        const std::string unit_;
        const std::string metricName_;
        const labels_t labels_;
    };

    template <typename T = uint_fast64_t>
    class Counter : public DataType {
        Counter(std::string name, std::string help, std::string unit, labels_t labels = {})
            : DataType(Type::Counter, name, help, unit, std::move(labels)) {}

        void inc(T value=1) noexcept {
            assert(value >= 0);
            value_.fetch_add(value, std::memory_order_relaxed);
        }

        T value() const noexcept {
            return value_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<T> value_{T{}};
    };

    Metrics();
    ~Metrics() = default;

    template<typename T = uint_fast64_t>
    Counter<T> *AddCounter(std::string name, std::string help, std::string unit, labels_t labels = {}) {
        auto c = std::make_unique<Counter<T>>(name, help, unit, std::move(labels));
        std::lock_guard lock(mutex_);
        auto key = c->metricName();
        auto result = dataTypes_.emplace(key, std::move(c));
    }

    Metrics(const Metrics&) = delete;
    Metrics(Metrics&&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    Metrics& operator=(Metrics&&) = delete;

private:
    std::map<std::string, std::unique_ptr<DataType>> dataTypes_;

    alignas(std::hardware_destructive_interference_size) std::mutex mutex_;
    char mpadding_[std::hardware_destructive_interference_size - sizeof(std::mutex)];
};

} // ns

#endif // YAHAT_ENABLE_METRICS

