
#include <algorithm>
#include <cmath>
#include <array>
#include <cassert>
#include <iostream>
#include <algorithm>

#include "yahat/Metrics.h"

using namespace std;
using namespace ::std::string_literals;

namespace yahat {

std::optional<std::chrono::system_clock::time_point> Metrics::now_;

Metrics::Metrics() {}

void Metrics::generate(std::ostream &target)
{
    target << std::fixed << std::setprecision(6);

    std::vector<const DataType*> metrics;

    {
        // Make a copy of the metrics pointers so we don't keep the mutex locked for too long
        // Metrics may be added, but once added they are never removed.
        lock_guard lock{mutex_};
        metrics.reserve(metrics_.size());
        for (const auto& [name, data] : metrics_) {
            metrics.push_back(data.get());
        }
    }

    string_view current_family;

    for (const auto* metric : metrics) {
        assert(!metric->name().empty());

        // Only print the meta-information for a metric once, even it there are lots of variations.
        if (current_family != metric->name()) {
            current_family = metric->name();

            if (!metric->help().empty()) {
                target << "# HELP " << metric->name() << " " << metric->help() << "\n";
            }

            assert(!metric->typeName().empty());
            target << "# TYPE " << metric->name() << " " << metric->typeName() << "\n";

            if (!metric->unit().empty()) {
                target << "# UNIT " << metric->name() << " " << metric->unit() << "\n";
            }
        }

        metric->render(target);
    }
}

Metrics::DataType::DataType(Type type, std::string name, std::string help, std::string unit, labels_t labels)
    : type_{type}, name_{name}, help_{help}, unit_{unit}, labels_{makeLabels(std::move(labels))}, metricName_{makeMetricName()} {
};

const std::string_view yahat::Metrics::DataType::typeName() const noexcept
{
    static constexpr std::array<string_view, 7> names = {
        "counter", "gauge", "histogram", "summary", "info", "stateset", "untyped"};

    assert(static_cast<int>(type()) < names.size());
    return names[static_cast<int>(type())];
}

ostream &Metrics::DataType::renderCreated(std::ostream &target, bool postfix) const
{
    if (!postfix) {
        target << created_name_ << ' ';
    }
    renderNumber(target, std::chrono::duration<double>(created_.time_since_epoch()).count(), 3) << '\n';
    return target;
}

string Metrics::DataType::makeKey(const std::string &name, const labels_t &labels, std::optional<DataType::Type> type)
{
    return makeNameWithSuffixAndLabels(name, {}, labels, type && type.value() == Type::Info);
}

string Metrics::DataType::makeMetricName() const noexcept
{
    return makeNameWithSuffixAndLabels(name_, {}, labels_, false);
}

string Metrics::DataType::makeNameWithSuffixAndLabels(const std::string name, const std::string &suffix,
                                                      const labels_t &labels, bool first)
{
    // Info nodes are supposed to come first in the output
    string result = first ? ("#"s + name) : name;

    if (!suffix.empty()) {
        result += "_" + suffix;
    }

    if (labels.empty()) {
        return result;
    }
    result += "{"; // Start label list

    uint count = 0;
    for (const auto& label : labels) {
        if (++count > 1) {
            result += ",";
        }
        result += label.first + "=\"" + label.second + "\"";
    }
    result += "}"; // End label list
    return result;
}

// TODO: Optimize/replace this AI generated code
ostream& Metrics::DataType::renderNumber(std::ostream &target, double value, uint maxDecimals)
{
    if (std::floor(value) == value) {
        // If it's an integer, print with at least 1 decimal
        target << std::fixed << std::setprecision(1) << value;
    } else {
        // Count the number of significant decimal places
        int precision = 1;
        double fractionalPart = value - std::floor(value);
        while (std::fabs(fractionalPart) > std::pow(10, -precision) && precision < 15) {
            if (precision >= maxDecimals) {
                break;
            }
            ++precision;
        }

        // Print the number with the calculated precision
        target << std::fixed << std::setprecision(precision) << value;
    }

    return target;
}

Metrics::labels_t Metrics::DataType::makeLabels(labels_t source)
{
    std::sort(source.begin(), source.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    return source;
}


} // ns
