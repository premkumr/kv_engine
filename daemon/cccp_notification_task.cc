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
#include "cccp_notification_task.h"
#include "log_macros.h"
#include "memcached.h"
#include "server_event.h"

#include <mcbp/protocol/framebuilder.h>

CccpNotificationTask::CccpNotificationTask(int bucket_, int revision_)
    : Task(),
      bucket(all_buckets.at(bucket_)),
      bucketIndex(bucket_),
      revision(revision_) {
    // Bump a reference so the bucket can't be deleted while we're in the
    // middle of pushing configurations
    std::lock_guard<std::mutex> guard(all_buckets[bucketIndex].mutex);
    all_buckets[bucketIndex].clients++;
}

CccpNotificationTask::~CccpNotificationTask() {
    bool notify = false;
    {
        std::lock_guard<std::mutex> guard(all_buckets[bucketIndex].mutex);
        all_buckets[bucketIndex].clients--;
        if (all_buckets[bucketIndex].clients == 0) {
            notify = true;
        }
    }

    if (notify) {
        all_buckets[bucketIndex].cond.notify_all();
    }
}

class CccpPushNotificationServerEvent : public ServerEvent {
public:
    std::string getDescription() const override {
        return "CccpPushNotificationServerEvent";
    }

    bool execute(Connection& connection) override {
        auto& conn = dynamic_cast<McbpConnection&>(connection);

        auto& bucket = connection.getBucket();
        auto payload = bucket.clusterConfiguration.getConfiguration();
        if (payload.first < conn.getClustermapRevno()) {
            // Ignore.. we've already sent a newer cluster config
            return true;
        }

        conn.setClustermapRevno(payload.first);
        LOG_NOTICE(&conn,
                   "%u: Sending Cluster map revision %u",
                   conn.getId(),
                   payload.first);

        std::string name = bucket.name;

        using namespace cb::mcbp;
        size_t needed = sizeof(Request) + // packet header
                        4 + // rev number in extdata
                        name.size() + // the name of the bucket
                        payload.second->size(); // The actual payload

        conn.write->ensureCapacity(needed);
        FrameBuilder<Request> builder(conn.write->wdata());
        builder.setMagic(Magic::ServerRequest);
        builder.setDatatype(cb::mcbp::Datatype::JSON);
        builder.setOpcode(ServerOpcode::ClustermapChangeNotification);

        // The extras contains the cluster revision number as an uint32_t
        const uint32_t rev = htonl(payload.first);
        builder.setExtras(
                {reinterpret_cast<const uint8_t*>(&rev), sizeof(rev)});
        builder.setKey(
                {reinterpret_cast<const uint8_t*>(name.data()), name.size()});
        builder.setValue(
                {reinterpret_cast<const uint8_t*>(payload.second->data()),
                 payload.second->size()});

        // Inject our packet into the stream!
        conn.addMsgHdr(true);
        conn.addIov(conn.write->wdata().data(), needed);
        conn.write->produced(needed);

        conn.setState(conn_send_data);
        conn.setWriteAndGo(conn_new_cmd);
        return true;
    }
};

Task::Status CccpNotificationTask::execute() {
    LOG_NOTICE(nullptr,
               "Pushing new cluster config for bucket:[%s] revision:[%u]",
               bucket.name,
               revision);

    auto rev = revision;

    // This feels a bit dirty, but the problem is that when we had
    // the task being created we did hold the LIBEVENT_THREAD mutex
    // when we locked the task in order to schedule it.
    // Now we want to iterate over all of the connections, and in
    // order to do that we need to lock the libevent thread so that
    // we can get exclusive access to the connection objects for that
    // thread.
    // No one is using this task so we can safely release the lock
    getMutex().unlock();
    try {
        iterate_all_connections([rev](Connection& c) -> void {
            if (!c.isClustermapChangeNotificationSupported()) {
                // The client hasn't asked to be notified
                return;
            }

            auto* connection = dynamic_cast<McbpConnection*>(&c);
            if (connection == nullptr) {
                // Ignore listening connection objects
                return;
            }

            if (rev <= connection->getClustermapRevno()) {
                LOG_NOTICE(connection,
                           "%u: Client is using %u, no need to push %u",
                           c.getId(),
                           connection->getClustermapRevno(),
                           rev);
                return;
            }

            LOG_NOTICE(connection,
                       "%u: Client is using %u. Push %u",
                       c.getId(),
                       connection->getClustermapRevno(),
                       rev);

            connection->enqueueServerEvent(
                    std::make_unique<CccpPushNotificationServerEvent>());
            connection->signalIfIdle(false, 0);
        });
    } catch (const std::exception& e) {
        LOG_WARNING(nullptr,
                    "CccpNotificationTask::execute: received exception: %s",
                    e.what());
    }
    getMutex().lock();

    return Status::Finished;
}
