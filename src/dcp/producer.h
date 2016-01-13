/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc
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

#ifndef SRC_DCP_PRODUCER_H_
#define SRC_DCP_PRODUCER_H_ 1

#include "config.h"

#include "dcp/dcp-types.h"
#include "tapconnection.h"

class BackfillManager;
class DcpResponse;

class DcpProducer : public Producer {
public:

    DcpProducer(EventuallyPersistentEngine &e, const void *cookie,
                const std::string &n, bool notifyOnly);

    ~DcpProducer();

    ENGINE_ERROR_CODE streamRequest(uint32_t flags, uint32_t opaque,
                                    uint16_t vbucket, uint64_t start_seqno,
                                    uint64_t end_seqno, uint64_t vbucket_uuid,
                                    uint64_t last_seqno, uint64_t next_seqno,
                                    uint64_t *rollback_seqno,
                                    dcp_add_failover_log callback);

    ENGINE_ERROR_CODE getFailoverLog(uint32_t opaque, uint16_t vbucket,
                                     dcp_add_failover_log callback);

    ENGINE_ERROR_CODE step(struct dcp_message_producers* producers);

    ENGINE_ERROR_CODE bufferAcknowledgement(uint32_t opaque, uint16_t vbucket,
                                            uint32_t buffer_bytes);

    ENGINE_ERROR_CODE control(uint32_t opaque, const void* key, uint16_t nkey,
                              const void* value, uint32_t nvalue);

    ENGINE_ERROR_CODE handleResponse(protocol_binary_response_header *resp);

    void addStats(ADD_STAT add_stat, const void *c);

    void addTakeoverStats(ADD_STAT add_stat, const void* c, uint16_t vbid);

    // This function adds takeover (TO) stats and returns true if an entry
    // was found in the map that holds the vbucket information for streams
    // that were closed by the checkpoint remover's cursor dropped.
    bool addTOStatsIfStreamTempDisconnected(ADD_STAT add_stat, const void* c,
                                            uint16_t vbid);

    void aggregateQueueStats(ConnCounter* aggregator);

    void setDisconnect(bool disconnect);

    void notifySeqnoAvailable(uint16_t vbucket, uint64_t seqno);

    void vbucketStateChanged(uint16_t vbucket, vbucket_state_t state);

    bool closeSlowStream(uint16_t vbid, const std::string &name);

    void closeAllStreams();

    const char *getType() const;

    bool isTimeForNoop();

    void setTimeForNoop();

    void clearQueues();

    size_t getBackfillQueueSize();

    size_t getItemsSent();

    size_t getTotalBytes();

    bool windowIsFull();

    void flush();

    std::vector<uint16_t> getVBVector(void);

    /**
     * Close the stream for given vbucket stream
     *
     * @param vbucket the if for the vbucket to close
     * @return ENGINE_SUCCESS upon a successful close
     *         ENGINE_NOT_MY_VBUCKET the vbucket stream doesn't exist
     */
    ENGINE_ERROR_CODE closeStream(uint32_t opaque, uint16_t vbucket);

    void notifyStreamReady(uint16_t vbucket, bool schedule);

    BackfillManager* getBackfillManager() {
        return backfillMgr;
    }

    bool isExtMetaDataEnabled () {
        return enableExtMetaData;
    }

    bool isValueCompressionEnabled() {
        return enableValueCompression;
    }

    void notifyPaused(bool schedule);

    class BufferLog {
    public:

        /*
            BufferLog has 3 states.
            Disabled - Flow-control is not in-use.
             This is indicated by setting the size to 0 (i.e. setBufferSize(0)).

            SpaceAvailable - There is *some* space available. You can always
             insert n-bytes even if there's n-1 bytes spare.

            Full - inserts have taken the number of bytes available equal or
             over the buffer size.
        */
        enum State {
            Disabled,
            Full,
            SpaceAvailable
        };

        BufferLog(DcpProducer& p)
            : producer(p), maxBytes(0), bytesSent(0), ackedBytes(0) {}

        void setBufferSize(size_t maxBytes);

        void addStats(ADD_STAT add_stat, const void *c);

        /*
            Return false if the log is full.

            Returns true if the bytes fit or if the buffer log is disabled.
              The tracked bytes is increased.
        */
        bool insert(size_t bytes);

        /*
            Acknowledge the bytes and unpause the producer if full.
              The tracked bytes is decreased.
        */
        void acknowledge(size_t bytes);

        /*
            Pause the producer if full.
        */
        bool pauseIfFull();

        /*
            Unpause the producer if there's space (or disabled).
        */
        void unpauseIfSpaceAvailable();

    private:

        bool isEnabled_UNLOCKED() {
            return maxBytes != 0;
        }

        bool isFull_UNLOCKED() {
            return bytesSent >= maxBytes;
        }

        void release_UNLOCKED(size_t bytes);

        State getState_UNLOCKED();

        RWLock logLock;
        DcpProducer& producer;
        size_t maxBytes;
        size_t bytesSent;
        size_t ackedBytes;
    };

    /*
        Insert bytes into this producer's buffer log.

        If the log is disabled or the insert was successful returns true.
        Else return false.
    */
    bool bufferLogInsert(size_t bytes);

private:

    DcpResponse* getNextItem();

    size_t getItemsRemaining();
    stream_t findStreamByVbid(uint16_t vbid);

    ENGINE_ERROR_CODE maybeSendNoop(struct dcp_message_producers* producers);

    struct {
        rel_time_t sendTime;
        uint32_t opaque;
        uint32_t noopInterval;
        Couchbase::RelaxedAtomic<bool> pendingRecv;
        Couchbase::RelaxedAtomic<bool> enabled;
    } noopCtx;

    std::string priority;

    DcpResponse *rejectResp; // stash response for retry if E2BIG was hit

    bool notifyOnly;
    bool enableExtMetaData;
    bool enableValueCompression;
    bool supportsCursorDropping;

    Couchbase::RelaxedAtomic<rel_time_t> lastSendTime;
    BufferLog log;

    BackfillManager* backfillMgr;

    // Guards all accesses to streams map. If only reading elements in streams
    // (i.e. not adding / removing elements) then can acquire ReadLock, even
    // if a non-const method is called on stream_t.
    RWLock streamsMutex;

    std::vector<AtomicValue<bool> > vbReady;
    AtomicValue<bool> notifiedVbReady;

    std::map<uint16_t, stream_t> streams;

    AtomicValue<size_t> itemsSent;
    AtomicValue<size_t> totalBytesSent;

    size_t roundRobinVbReady;
    static const uint32_t defaultNoopInerval;

    /**
     * This map holds the vbucket id, and the last sent seqno
     * information for streams that have been dropped by the
     * checkpoint remover's cursor dropper, which are awaiting
     * reconnection.
     */
    std::map<uint16_t, uint64_t> tempDroppedStreams;
};

#endif  // SRC_DCP_PRODUCER_H_