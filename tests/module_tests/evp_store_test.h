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

/*
 * Unit tests for the EventuallyPersistentStore class.
 */

#pragma once

#include "config.h"

#include "ep.h"
#include "ep_engine.h"
#include "item.h"

#include <memcached/engine.h>

#include <gtest/gtest.h>
#include <memory>

class MockEPStore;

/* A class which subclasses the real EPEngine. Its main purpose is to allow
 * us to construct and setup an EPStore without starting all the various
 * background tasks which are normally started by EPEngine as part of creating
 * EPStore (in the initialize() method).
 *
 * The net result is a (mostly) synchronous environment - while the
 * ExecutorPool's threads exist, none of the normally-created background Tasks
 * should be running. Note however that /if/ any new tasks are created, they
 * will be scheduled on the ExecutorPools' threads asynchronously.
 */
class SynchronousEPEngine : public EventuallyPersistentEngine {
public:
    SynchronousEPEngine(const std::string& extra_config);

    void setEPStore(EventuallyPersistentStore* store);

    // Allow us to call the normally protected method.
    ENGINE_ERROR_CODE public_doTapVbTakeoverStats(const void *cookie,
                                                  ADD_STAT add_stat,
                                                  std::string& key,
                                                  uint16_t vbid) {
        return doTapVbTakeoverStats(cookie, add_stat, key, vbid);
    }
};

/* Subclass of EPStore to expose normally non-public members for test
 * purposes.
 */
class MockEPStore : public EventuallyPersistentStore {
public:
    MockEPStore(EventuallyPersistentEngine &theEngine);

    VBucketMap& getVbMap();
};

/* Actual test fixture class */
class EventuallyPersistentStoreTest : public ::testing::Test {
protected:
    void SetUp() override;

    void TearDown() override;

    // Creates an item with the given vbucket id, key and value.
    static Item make_item(uint16_t vbid, const std::string& key,
                          const std::string& value);

    /* Stores an item into the given vbucket. */
    void store_item(uint16_t vbid, const std::string& key,
                    const std::string& value);

    /* Flush the given vbucket to disk, so any outstanding dirty items are
     * written (and are clean).
     */
    void flush_vbucket_to_disk(uint16_t vbid);

    /* Delete the given item from the given vbucket, verifying it was
     * successfully deleted.
     */
    void delete_item(uint16_t vbid, const std::string& key);

    /* Evict the given key from memory according to the current eviction
     * strategy. Verifies it was successfully evicted.
     */
    void evict_key(uint16_t vbid, const std::string& key);

    static const char test_dbname[];

    std::string config_string;

    const uint16_t vbid = 0;

    // The mock engine (needed to construct the store).
    std::unique_ptr<SynchronousEPEngine> engine;

    // The store under test. Wrapped in a mock to expose some normally
    // protected members. Uses a raw pointer as this is owned by the engine.
    MockEPStore* store;

    // The (mock) server cookie.
    const void* cookie;
};
