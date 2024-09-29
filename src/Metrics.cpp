
#include <array>
#include <cassert>

#include "../include/yahat/Metrics.h"

using namespace std;

namespace yahat {

Metrics::Metrics() {}

Metrics::DataType::DataType(Type type, std::string name, std::string help, std::string unit, labels_t labels)
    : name_{name}, help_{help}, unit_{unit}, labels_{std::move(labels)}, metricName_{makeMetricName()} {
};

const std::string_view yahat::Metrics::DataType::typeName() const noexcept
{
    static constexpr std::array<string_view, 7> names = {
        "counter", "gauge", "histogram", "summary", "info", "stateset", "untyped"};

    assert(static_cast<int>(type()) < names.size());
    return names[static_cast<int>(type())];
}

string Metrics::DataType::makeMetricName() const noexcept
{
    string result = name_;
    if (labels_.empty()) {
        return result;
    }
    result += "{"; // Start label list

    uint count = 0;
    for (const auto& label : labels_) {
        if (++count > 1) {
            result += ",";
        }
        result += label.first + "=" + label.second;
    }
    result += "}"; // End label list
    return result;
}


} // ns
