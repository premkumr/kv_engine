/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "ep.hh"
#include "locks.hh"

#include <time.h>
#include <string.h>

extern "C" {
    static void* launch_flusher_thread(void* arg) {
        Flusher *flusher = (Flusher*) arg;
        try {
            flusher->run();
        } catch (std::exception& e) {
            std::cerr << "flusher exception caught: " << e.what() << std::endl;
        } catch(...) {
            std::cerr << "Caught a fatal exception in the flusher thread" << std::endl;
        }
        return NULL;
    }

    static rel_time_t uninitialized_current_time(void) {
        abort();
        return 0;
    }

    rel_time_t (*ep_current_time)() = uninitialized_current_time;
}

EventuallyPersistentStore::EventuallyPersistentStore(KVStore *t,
                                                     size_t est) :
    loadStorageKVPairCallback(storage, stats)
{
    est_size = est;
    towrite = NULL;
    memset(&stats, 0, sizeof(stats));
    initQueue();

    doPersistence = getenv("EP_NO_PERSITENCE") == NULL;
    flusher = new Flusher(this);

    flusherState = STOPPED;
    txnSize = DEFAULT_TXN_SIZE;
    startFlusher();

    underlying = t;
    assert(underlying);
}

EventuallyPersistentStore::~EventuallyPersistentStore() {
    stopFlusher();
    if (flusherState != STOPPED) {
        pthread_join(thread, NULL);
    }
    delete flusher;
    delete towrite;
}

void EventuallyPersistentStore::startFlusher() {
    LockHolder lh(mutex);
    if (flusherState != STOPPED) {
        return;
    }

    // Run in a thread...
    if(pthread_create(&thread, NULL, launch_flusher_thread, flusher)
       != 0) {
        throw std::runtime_error("Error initializing queue thread");
    }
    flusherState = RUNNING;
}

void EventuallyPersistentStore::stopFlusher() {
    LockHolder lh(mutex);
    if (flusherState != RUNNING) {
        return;
    }

    flusherState = SHUTTING_DOWN;
    flusher->stop();
    mutex.notify();
}

flusher_state EventuallyPersistentStore::getFlusherState() {
    LockHolder lh(mutex);
    return flusherState;
}

void EventuallyPersistentStore::initQueue() {
    assert(!towrite);
    stats.queue_size = 0;
    towrite = new std::queue<std::string>;
}

void EventuallyPersistentStore::set(const Item &item, Callback<bool> &cb) {
    mutation_type_t mtype = storage.set(item);

    if (mtype == WAS_CLEAN || mtype == NOT_FOUND) {
        LockHolder lh(mutex);
        queueDirty(item.getKey());
    }
    bool rv = true;
    cb.callback(rv);
}

void EventuallyPersistentStore::reset() {
    flush(false);
    LockHolder lh(mutex);
    underlying->reset();
    delete towrite;
    towrite = NULL;
    memset(&stats, 0, sizeof(stats));
    initQueue();
    storage.clear();
}

void EventuallyPersistentStore::get(const std::string &key,
                                    Callback<GetValue> &cb) {
    int bucket_num = storage.bucket(key);
    LockHolder lh(storage.getMutex(bucket_num));
    StoredValue *v = storage.unlocked_find(key, bucket_num);

    if (v) {
        GetValue rv(new Item(v->getKey(), v->getFlags(), v->getExptime(),
                             v->getValue()));
        cb.callback(rv);
    } else {
        GetValue rv(false);
        cb.callback(rv);
    }
    lh.unlock();
}

void EventuallyPersistentStore::getStats(struct ep_stats *out) {
    LockHolder lh(mutex);
    *out = stats;
}

void EventuallyPersistentStore::resetStats(void) {
    LockHolder lh(mutex);
    memset(&stats, 0, sizeof(stats));
}

void EventuallyPersistentStore::del(const std::string &key, Callback<bool> &cb) {
    bool existed = storage.del(key);
    if (existed) {
        queueDirty(key);
    }
    cb.callback(existed);
}

void EventuallyPersistentStore::flush(bool shouldWait) {
    LockHolder lh(mutex);

    if (towrite->empty()) {
        stats.dirtyAge = 0;
        if (shouldWait) {
            mutex.wait();
        }
    } else {
        rel_time_t flush_start = ep_current_time();

        std::queue<std::string> *q = towrite;
        towrite = NULL;
        initQueue();
        lh.unlock();

        RememberingCallback<bool> cb;
        assert(underlying);

        stats.flusher_todo = q->size();

        while (!q->empty()) {
            flushSome(q, cb);
        }
        rel_time_t complete_time = ep_current_time();

        delete q;

        stats.flushDuration = complete_time - flush_start;
        stats.flushDurationHighWat = stats.flushDuration > stats.flushDurationHighWat
                                     ? stats.flushDuration : stats.flushDurationHighWat;
    }
}

void EventuallyPersistentStore::flushSome(std::queue<std::string> *q,
                                         Callback<bool> &cb) {
    underlying->begin();
    for (int i = 0; i < txnSize && !q->empty(); i++) {
        flushOne(q, cb);
    }
    rel_time_t cstart = ep_current_time();
    underlying->commit();
    rel_time_t complete_time = ep_current_time();
    // One more lock to update a stat.
    LockHolder lh_stat(mutex);
    stats.commit_time = complete_time - cstart;
}

void EventuallyPersistentStore::flushOne(std::queue<std::string> *q,
                                         Callback<bool> &cb) {

    std::string key = q->front();
    q->pop();

    int bucket_num = storage.bucket(key);
    LockHolder lh(storage.getMutex(bucket_num));
    StoredValue *v = storage.unlocked_find(key, bucket_num);

    bool found = v != NULL;
    bool isDirty = (found && v->isDirty());
    Item *val = NULL;
    if (isDirty) {
        rel_time_t queued, dirtied;
        v->markClean(&queued, &dirtied);
        assert(dirtied > 0);
        // Calculate stats if this had a positive time.
        rel_time_t now = ep_current_time();
        stats.dirtyAge = now - queued;
        stats.dataAge = now - dirtied;
        assert(stats.dirtyAge < (86400 * 30));
        assert(stats.dataAge <= stats.dirtyAge);
        stats.dirtyAgeHighWat = stats.dirtyAge > stats.dirtyAgeHighWat
            ? stats.dirtyAge : stats.dirtyAgeHighWat;
        stats.dataAgeHighWat = stats.dataAge > stats.dataAgeHighWat
            ? stats.dataAge : stats.dataAgeHighWat;
        // Copy it for the duration.
        val = new Item(key, v->getFlags(), v->getExptime(), v->getValue());
    }
    stats.flusher_todo--;
    lh.unlock();

    if (found && isDirty) {
        underlying->set(*val, cb);
    } else if (!found) {
        underlying->del(key, cb);
    }

    if (val != NULL) {
        delete val;
    }
}

void EventuallyPersistentStore::flusherStopped() {
    LockHolder lh(mutex);
    flusherState = STOPPED;
}
