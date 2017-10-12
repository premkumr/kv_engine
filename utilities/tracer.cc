/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "config.h"
#include <utilities/tracer.h>

#include <iostream>
#include <limits>
#include <sstream>

std::chrono::microseconds to_micros(const ProcessClock::time_point tp) {
    return std::chrono::time_point_cast<std::chrono::microseconds>(tp)
            .time_since_epoch();
}

namespace cb {
namespace tracing {

Tracer::SpanId Tracer::invalidSpanId() {
    return std::numeric_limits<SpanId>::max();
}

Tracer::SpanId Tracer::begin(const std::string& name) {
    std::lock_guard<std::mutex> lock(spanMutex);
    auto t = to_micros(ProcessClock::now());
    // vecSpans.emplace_back(name, t);
    vecSpans.push_back(Span(name, t));
    return vecSpans.size() - 1;
}

bool Tracer::end(SpanId spanId) {
    std::lock_guard<std::mutex> lock(spanMutex);
    if (spanId >= vecSpans.size())
        return false;
    auto& span = vecSpans[spanId];
    span.duration = to_micros(ProcessClock::now()) - span.start;
    return true;
}

bool Tracer::end(const std::string& name) {
    std::lock_guard<std::mutex> lock(spanMutex);
    SpanId spanId = 0;
    for (const auto& span : vecSpans) {
        if (span.name == name) {
            return end(spanId);
        }
        spanId++;
    }
    return false;
}

const std::vector<Span>& Tracer::getDurations() const {
    return vecSpans;
}

void Tracer::clear() {
    vecSpans.clear();
}

} // end namespace tracing
} // end namespace cb

MEMCACHED_PUBLIC_API std::ostream& operator<<(
        std::ostream& os, const cb::tracing::Tracer& tracer) {
    return os << to_string(tracer);
}

MEMCACHED_PUBLIC_API std::string to_string(const cb::tracing::Tracer& tracer,
                                           bool raw) {
    const auto& vecSpans = tracer.getDurations();
    std::ostringstream os;
    auto size = vecSpans.size();
    for (const auto& span : vecSpans) {
        os << span.name << "=" << span.start.count() << ":"
           << span.duration.count();
        size--;
        if (size > 0) {
            os << (raw ? " " : "\n");
        }
    }
    std::cerr << "prem:" << os.str() << std::endl;
    return os.str();
}
