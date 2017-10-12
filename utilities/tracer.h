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
#pragma once
#include <cstddef>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <memcached/visibility.h>
#include <platform/platform.h>
#include <platform/processclock.h>

// switching the order for windows
#if defined(_MSC_VER)
#define MC_PUBLIC_CLASS MEMCACHED_PUBLIC_API class
#else
#define MC_PUBLIC_CLASS class MEMCACHED_PUBLIC_API
#endif

namespace cb {
namespace tracing {
// fwd declaration
MC_PUBLIC_CLASS Tracer;
} // namespace tracing
} // namespace cb

// to have this in the global namespace
// get the tracepoints either as raw
// or formatted list of durations
MEMCACHED_PUBLIC_API std::string to_string(const cb::tracing::Tracer& tracer,
                                           bool raw = false);
MEMCACHED_PUBLIC_API std::ostream& operator<<(
        std::ostream& os, const cb::tracing::Tracer& tracer);

namespace cb {
namespace tracing {

MC_PUBLIC_CLASS Span {
public:
    Span(std::string name,
         std::chrono::microseconds start,
         std::chrono::microseconds duration = std::chrono::microseconds(0))
        : name(name), start(start), duration(duration) {
    }
    std::string name;
    std::chrono::microseconds start;
    std::chrono::microseconds duration;
};

/**
 * Tracer maintains an ordered vector of tracepoints
 * with name:time(micros)
 */
MC_PUBLIC_CLASS Tracer {
public:
    using SpanId = std::size_t;

    static SpanId invalidSpanId();

    SpanId begin(const std::string& name);
    bool end(SpanId spanId);
    bool end(const std::string& name);

    // get the tracepoints as ordered durations
    const std::vector<Span>& getDurations() const;

    // clear the collected trace data;
    void clear();

    friend std::string(::to_string)(const cb::tracing::Tracer& tracer,
                                    bool raw);

protected:
    std::vector<Span> vecSpans;
    std::mutex spanMutex;
};

} // namespace tracing
} // namespace cb
