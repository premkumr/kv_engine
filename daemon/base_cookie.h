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
#include <stdexcept>
#include "utilities/tracer.h"
/**
 * BaseCookie interface is to ensure that different cookie types for
 * test/production expose certain common functionality
 */
class BaseCookie {
public:
    BaseCookie() : enableTracing(false) {
    }

    bool isTracingEnabled() const {
        return enableTracing;
    }

    void setTracingEnabled(bool enable) {
        enableTracing = enable;
    }

    cb::tracing::Tracer& getTracer() {
        return tracer;
    }

    ~BaseCookie() {
    }

protected:
    bool enableTracing;
    cb::tracing::Tracer tracer;
};
