/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
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

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "connmap.h"
#include "executorthread.h"
#include "dcp/backfill-manager.h"
#include "dcp/consumer.h"
#include "dcp/dcpconnmap.h"
#include "dcp/producer.h"

const uint32_t DcpConnMap::dbFileMem = 10 * 1024;
const uint16_t DcpConnMap::numBackfillsThreshold = 4096;
const uint8_t DcpConnMap::numBackfillsMemThreshold = 1;

DcpConnMap::DcpConnMap(EventuallyPersistentEngine &e)
    : ConnMap(e),
      aggrDcpConsumerBufferSize(0) {
    numActiveSnoozingBackfills = 0;
    updateMaxActiveSnoozingBackfills(engine.getEpStats().getMaxDataSize());
    minCompressionRatioForProducer.store(
                    engine.getConfiguration().getDcpMinCompressionRatio());
}

DcpConsumer *DcpConnMap::newConsumer(const void* cookie,
                                     const std::string &name)
{
    LockHolder lh(connsLock);

    std::string conn_name("eq_dcpq:");
    conn_name.append(name);

    std::list<connection_t>::iterator iter;
    for (iter = all.begin(); iter != all.end(); ++iter) {
        if ((*iter)->getName() == conn_name) {
            (*iter)->setDisconnect(true);
            all.erase(iter);
            break;
        }
    }

    DcpConsumer *dcp = new DcpConsumer(engine, cookie, conn_name);
    connection_t dc(dcp);
    LOG(EXTENSION_LOG_INFO, "%s Connection created", dc->logHeader());
    all.push_back(dc);
    map_[cookie] = dc;
    return dcp;

}

bool DcpConnMap::isPassiveStreamConnected_UNLOCKED(uint16_t vbucket) {
    std::list<connection_t>::iterator it;
    for(it = all.begin(); it != all.end(); it++) {
        DcpConsumer* dcpConsumer = dynamic_cast<DcpConsumer*>(it->get());
        if (dcpConsumer && dcpConsumer->isStreamPresent(vbucket)) {
                LOG(EXTENSION_LOG_DEBUG, "(vb %d) A DCP passive stream "
                    "is already exists for the vbucket in connection: %s",
                    vbucket, dcpConsumer->logHeader());
                return true;
        }
    }
    return false;
}

ENGINE_ERROR_CODE DcpConnMap::addPassiveStream(ConnHandler& conn,
                                               uint32_t opaque,
                                               uint16_t vbucket,
                                               uint32_t flags)
{
    LockHolder lh(connsLock);
    /* Check if a stream (passive) for the vbucket is already present */
    if (isPassiveStreamConnected_UNLOCKED(vbucket)) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Failing to add passive stream, "
            "as one already exists for the vbucket!",
            conn.logHeader(), vbucket);
        return ENGINE_KEY_EEXISTS;
    }

    return conn.addStream(opaque, vbucket, flags);
}

DcpProducer *DcpConnMap::newProducer(const void* cookie,
                                     const std::string &name,
                                     bool notifyOnly)
{
    LockHolder lh(connsLock);

    std::string conn_name("eq_dcpq:");
    conn_name.append(name);

    std::list<connection_t>::iterator iter;
    for (iter = all.begin(); iter != all.end(); ++iter) {
        if ((*iter)->getName() == conn_name) {
            (*iter)->setDisconnect(true);
            all.erase(iter);
            break;
        }
    }

    DcpProducer *dcp = new DcpProducer(engine, cookie, conn_name, notifyOnly);
    LOG(EXTENSION_LOG_INFO, "%s Connection created", dcp->logHeader());
    all.push_back(connection_t(dcp));
    map_[cookie] = dcp;

    return dcp;
}

void DcpConnMap::shutdownAllConnections() {
    LOG(EXTENSION_LOG_WARNING, "Shutting down dcp connections!");

    connNotifier_->stop();


    LockHolder lh(connsLock);
    std::vector<connection_t> toRelease(all.begin(), all.end());

    closeAllStreams_UNLOCKED();
    cancelAllTasks_UNLOCKED();
    all.clear();
    map_.clear();

    lh.unlock();

    {
         LockHolder rlh(releaseLock);
         for (auto &ii : toRelease) {
             LOG(EXTENSION_LOG_NOTICE, "Clean up \"%s\"", ii->getName().c_str());
             ii->releaseReference();
         }
    }
    /*
     * Dead connections are cleaned-up by manageConnections.
     * manageconnections is invoked in the run() of ConnManager,
     * which is a NONIO Task.  The task has a MIN_SLEEP_TIME of 2s,
     * which means dead connections will only be clean-up at most
     * every 2s.  Therefore if we delete a bucket it is possible
     * that dead connections exist.  This causes the function
     * responsible for destroying a bucket to wait indefinitely
     * for the dead connections to be disconnected.
     *
     * Therefore before deleting a bucket we need to ensure that
     * manageConnections is called.
     */
    manageConnections();
}

void DcpConnMap::vbucketStateChanged(uint16_t vbucket, vbucket_state_t state,
                                     bool closeInboundStreams) {
    LockHolder lh(connsLock);
    std::map<const void*, connection_t>::iterator itr = map_.begin();
    for (; itr != map_.end(); ++itr) {
        DcpProducer* producer = dynamic_cast<DcpProducer*> (itr->second.get());
        if (producer) {
            producer->vbucketStateChanged(vbucket, state);
        } else if (closeInboundStreams) {
            static_cast<DcpConsumer*>(itr->second.get())->vbucketStateChanged(
                                                                vbucket, state);
        }
    }
}

bool DcpConnMap::closeSlowStream(uint16_t vbid,
                                 const std::string &name) {
    size_t lock_num = vbid % vbConnLockNum;
    SpinLockHolder lh(&vbConnLocks[lock_num]);
    std::list<connection_t> &vb_conns = vbConns[vbid];

    std::list<connection_t>::iterator itr = vb_conns.begin();
    for (; itr != vb_conns.end(); ++itr) {
        DcpProducer* producer = static_cast<DcpProducer*> ((*itr).get());
        if (producer && producer->closeSlowStream(vbid, name)) {
            return true;
        }
    }
    return false;
}

void DcpConnMap::closeAllStreams_UNLOCKED() {
    std::map<const void*, connection_t>::iterator itr = map_.begin();
    for (; itr != map_.end(); ++itr) {
        DcpProducer* producer = dynamic_cast<DcpProducer*> (itr->second.get());
        if (producer) {
            producer->closeAllStreams();
            producer->clearCheckpointProcessorTaskQueues();
        } else {
            static_cast<DcpConsumer*>(itr->second.get())->closeAllStreams();
        }
    }
}

void DcpConnMap::cancelAllTasks_UNLOCKED() {
    std::map<const void*, connection_t>::iterator itr = map_.begin();
    for (; itr != map_.end(); ++itr) {
        DcpConsumer* consumer = dynamic_cast<DcpConsumer*> (itr->second.get());
        if (consumer) {
            consumer->cancelTask();
        }
    }
}

void DcpConnMap::disconnect(const void *cookie) {
    LockHolder lh(connsLock);
    disconnect_UNLOCKED(cookie);
}

void DcpConnMap::disconnect_UNLOCKED(const void *cookie) {
    std::list<connection_t>::iterator iter;
    for (iter = all.begin(); iter != all.end(); ++iter) {
        if ((*iter)->getCookie() == cookie) {
            (*iter)->setDisconnect(true);
            all.erase(iter);
            break;
        }
    }

    std::map<const void*, connection_t>::iterator itr(map_.find(cookie));
    if (itr != map_.end()) {
        connection_t conn = itr->second;
        if (conn.get()) {
            LOG(EXTENSION_LOG_INFO, "%s Removing connection",
                conn->logHeader());
            map_.erase(itr);
        }

        DcpProducer* producer = dynamic_cast<DcpProducer*> (conn.get());
        if (producer) {
            producer->closeAllStreams();
            producer->clearCheckpointProcessorTaskQueues();
        } else {
            // Cancel consumer's processer task before closing all streams
            static_cast<DcpConsumer*>(conn.get())->cancelTask();
            static_cast<DcpConsumer*>(conn.get())->closeAllStreams();
        }

        deadConnections.push_back(conn);
    }
}

void DcpConnMap::manageConnections() {
    std::list<connection_t> release;

    LockHolder lh(connsLock);
    while (!deadConnections.empty()) {
        connection_t conn = deadConnections.front();
        LOG(EXTENSION_LOG_wARNING, "DcpConnMapp::manageConnections dead connection= %p",(void*)conn);
        release.push_back(conn);
        deadConnections.pop_front();
    }

    const int maxIdleTime = 5;
    rel_time_t now = ep_current_time();

    // Collect the list of connections that need to be signaled.
    std::list<connection_t> toNotify;
    std::map<const void*, connection_t>::iterator iter;
    for (iter = map_.begin(); iter != map_.end(); ++iter) {
        connection_t conn = iter->second;
        Notifiable *tp = dynamic_cast<Notifiable*>(conn.get());
        if (tp && (tp->isPaused() || conn->doDisconnect()) &&
            conn->isReserved()) {
            if (!tp->sentNotify() ||
                (conn->getLastWalkTime() + maxIdleTime < now)) {
                toNotify.push_back(iter->second);
            }
        }
    }

    lh.unlock();

    LockHolder rlh(releaseLock);
    std::list<connection_t>::iterator it;
    for (it = toNotify.begin(); it != toNotify.end(); ++it) {
        Notifiable *tp =
            static_cast<Notifiable*>(static_cast<Producer*>((*it).get()));
        if (tp && (*it)->isReserved()) {
            engine.notifyIOComplete((*it)->getCookie(), ENGINE_SUCCESS);
            tp->setNotifySent(true);
        }
    }

    while (!release.empty()) {
        connection_t conn = release.front();
        conn->releaseReference();
        release.pop_front();
        removeVBConnections(conn);
    }
}

void DcpConnMap::removeVBConnections(connection_t &conn) {
    Producer *tp = dynamic_cast<Producer*>(conn.get());
    if (!tp) {
        return;
    }

    DcpProducer *prod = static_cast<DcpProducer*>(tp);
    for (const auto vbid: prod->getVBVector()) {
        size_t lock_num = vbid % vbConnLockNum;
        SpinLockHolder lh (&vbConnLocks[lock_num]);
        std::list<connection_t> &vb_conns = vbConns[vbid];
        std::list<connection_t>::iterator itr = vb_conns.begin();
        for (; itr != vb_conns.end(); ++itr) {
            if (conn->getCookie() == (*itr)->getCookie()) {
                vb_conns.erase(itr);
                break;
            }
        }
    }
}

void DcpConnMap::notifyVBConnections(uint16_t vbid, uint64_t bySeqno) {
    size_t lock_num = vbid % vbConnLockNum;
    SpinLockHolder lh(&vbConnLocks[lock_num]);

    std::list<connection_t> &conns = vbConns[vbid];
    std::list<connection_t>::iterator it = conns.begin();
    for (; it != conns.end(); ++it) {
        DcpProducer *conn = static_cast<DcpProducer*>((*it).get());
        conn->notifySeqnoAvailable(vbid, bySeqno);
    }
}

void DcpConnMap::notifyBackfillManagerTasks() {
    LockHolder lh(connsLock);
    std::map<const void*, connection_t>::iterator itr = map_.begin();
    for (; itr != map_.end(); ++itr) {
        DcpProducer* producer = dynamic_cast<DcpProducer*> (itr->second.get());
        if (producer) {
            producer->notifyBackfillManager();
        }
    }
}

bool DcpConnMap::canAddBackfillToActiveQ()
{
    SpinLockHolder lh(&numBackfillsLock);
    if (numActiveSnoozingBackfills < maxActiveSnoozingBackfills) {
        ++numActiveSnoozingBackfills;
        return true;
    }
    return false;
}

void DcpConnMap::decrNumActiveSnoozingBackfills()
{
    SpinLockHolder lh(&numBackfillsLock);
    if (numActiveSnoozingBackfills > 0) {
        --numActiveSnoozingBackfills;
    } else {
        LOG(EXTENSION_LOG_WARNING, "ActiveSnoozingBackfills already zero!!!");
    }
}

void DcpConnMap::updateMaxActiveSnoozingBackfills(size_t maxDataSize)
{
    double numBackfillsMemThresholdPercent =
                         static_cast<double>(numBackfillsMemThreshold)/100;
    size_t max = maxDataSize * numBackfillsMemThresholdPercent / dbFileMem;
    /* We must have atleast one active/snoozing backfill */
    SpinLockHolder lh(&numBackfillsLock);
    maxActiveSnoozingBackfills =
        std::max(static_cast<size_t>(1),
                 std::min(max, static_cast<size_t>(numBackfillsThreshold)));
    LOG(EXTENSION_LOG_DEBUG, "Max active snoozing backfills set to %d",
        maxActiveSnoozingBackfills);
}

void DcpConnMap::addStats(ADD_STAT add_stat, const void *c)
{
    LockHolder lh(connsLock);
    add_casted_stat("ep_dcp_dead_conn_count", deadConnections.size(), add_stat,
                    c);
}

void DcpConnMap::updateMinCompressionRatioForProducers(float value) {
    minCompressionRatioForProducer.store(value);
}

float DcpConnMap::getMinCompressionRatio() {
    return minCompressionRatioForProducer.load();
}
