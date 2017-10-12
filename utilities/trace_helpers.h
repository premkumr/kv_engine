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

#include <daemon/base_cookie.h>
#include <utilities/tracer.h>

// DEBUGCODE
//#define DISABLE_SESSION_TRACING 1

#ifndef DISABLE_SESSION_TRACING

#define TO_BASECOOKIE(ck) \
    const_cast<BaseCookie*>(reinterpret_cast<const BaseCookie*>(ck))

#define TRACE_CK_BEGIN(ck, name)          \
    {                                     \
        auto c = TO_BASECOOKIE(ck);       \
        if (c && c->isTracingEnabled()) { \
            c->getTracer().begin(name);   \
        }                                 \
    }

#define TRACE_CK_END(ck, name)            \
    {                                     \
        auto c = TO_BASECOOKIE(ck);       \
        if (c && c->isTracingEnabled()) { \
            c->getTracer().end(name);     \
        }                                 \
    }

#define DISABLE_TRACING_CK(ck)                       \
    if (ck) {                                        \
        TO_BASECOOKIE(ck)->setTracingEnabled(false); \
    }
#define ENABLE_TRACING_CK(ck)                       \
    if (ck) {                                       \
        TO_BASECOOKIE(ck)->setTracingEnabled(true); \
    }

/**
 * Traces a scope
 * Usage:
 *   {
 *     TRACE_SCOPE("test1");
 *     ....
 *    }
 */
class ScopedTracer {
public:
    ScopedTracer(const void* ck, const std::string& name) : bck(nullptr) {
        if (ck && TO_BASECOOKIE(ck)->isTracingEnabled()) {
            bck = TO_BASECOOKIE(ck);
            spanId = bck->getTracer().begin(name);
        }
    }

    ~ScopedTracer() {
        if (bck) {
            bck->getTracer().end(spanId);
        }
    }

protected:
    BaseCookie* bck;
    volatile cb::tracing::Tracer::SpanId spanId;
};

/**
 * Trace a block of code
 * Usage:
 *     TRACE_BLOCK("ht.lock.wait") {
 *         lock.lock();
 *     }
 */
class BlockTracer : public ScopedTracer {
public:
    BlockTracer(const void* ck, const std::string& name)
        : ScopedTracer(ck, name), justonce(true) {
    }

    // will return true only once
    // used by TRACE_BLOCK_CK to execute loop just once
    bool once() volatile {
        if (justonce) {
            justonce = false;
            return true;
        }
        return false;
    }

protected:
    bool justonce;
};

#define TRACE_BLOCK_CK(ck, name)                          \
    for (volatile BlockTracer __bt__##__LINE__(ck, name); \
         __bt__##__LINE__.once();)
/**
 * Note: Had to make these variables volatile as we noticed
 * wierd behavior in Release builds but not in Debug.
 * Have not figured the root cause
 */
#define TRACE_SCOPE_CK(ck, name) \
    volatile ScopedTracer __st__##__LINE__(ck, name)

#define ENABLE_TRACING() ENABLE_TRACING_CK(cookie)
#define DISABLE_TRACING() DISABLE_TRACING_CK(cookie)
#define TRACE_BEGIN(name) TRACE_CK_BEGIN(cookie, name)
#define TRACE_END(name) TRACE_CK_END(cookie, name)
#define TRACE_SCOPE(name) TRACE_SCOPE_CK(cookie, name)
#define TRACE_BLOCK(name) TRACE_BLOCK_CK(cookie, name)

#else
/**
 * if DISABLE_SESSION_TRACING is set
 * unset all TRACE macros
 */
#define TRACE_CK_BEGIN(ck, name)
#define TRACE_CK_END(ck, name)
#define DISABLE_TRACING_CK(ck)
#define ENABLE_TRACING_CK(ck)
#define TRACE_BLOCK_CK(ck, name)
#define TRACE_SCOPE_CK(ck, name)
#define ENABLE_TRACING()
#define DISABLE_TRACING()
#define TRACE_BEGIN(name)
#define TRACE_END(name)
#define TRACE_SCOPE(name)
#define TRACE_BLOCK(name)

#endif
