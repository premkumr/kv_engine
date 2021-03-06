/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#include "item.h"
#include "kv_bucket.h"
#include "programs/engine_testapp/mock_server.h"
#include "stats.h"
#include "stored_value_factories.h"
#include "tests/module_tests/test_helpers.h"
#include "threadtests.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <platform/cb_malloc.h>

#include <algorithm>
#include <limits>
#include <signal.h>

EPStats global_stats;

class Counter : public HashTableVisitor {
public:

    size_t count;
    size_t deleted;

    Counter(bool v) : count(), deleted(), verify(v) {}

    bool visit(const HashTable::HashBucketLock& lh, StoredValue& v) {
        if (v.isDeleted()) {
            ++deleted;
        } else {
            ++count;
            if (verify) {
                StoredDocKey key(v.getKey());
                value_t val = v.getValue();
                EXPECT_STREQ(key.c_str(), val->to_s().c_str());
            }
        }
        return true;
    }
private:
    bool verify;
};

static int count(HashTable &h, bool verify=true) {
    Counter c(verify);
    h.visit(c);
    EXPECT_EQ(h.getNumItems(), c.count + c.deleted);
    return c.count;
}

static void store(HashTable &h, const StoredDocKey& k) {
    Item i(k, 0, 0, k.data(), k.size());
    EXPECT_EQ(MutationStatus::WasClean, h.set(i));
}

static void storeMany(HashTable &h, std::vector<StoredDocKey> &keys) {
    for (const auto& key : keys) {
        store(h, key);
    }
}

template <typename T>
static const char* toString(AddStatus a) {
    switch(a) {
    case AddStatus::Success:
        return "AddStatus::Success";
    case AddStatus::NoMem:
        return "AddStatus::NoMem";
    case AddStatus::Exists:
        return "AddStatus::Exists";
    case AddStatus::UnDel:
        return "AddStatus::UnDel";
    case AddStatus::AddTmpAndBgFetch:
        return "AddStatus::AddTmpAndBgFetch";
    case AddStatus::BgFetch:
        return "AddStatus::BgFetch";
    }
    abort();
    return NULL;
}

static std::vector<StoredDocKey> generateKeys(int num, int start=0) {
    std::vector<StoredDocKey> rv;

    for (int i = start; i < num; i++) {
        rv.push_back(makeStoredDocKey(std::to_string(i)));
    }

    return rv;
}

/**
 * Delete the item with the given key.
 *
 * @param key the storage key of the value to delete
 * @return true if the item existed before this call
 */
static bool del(HashTable& ht, const DocKey& key) {
    auto hbl = ht.getLockedBucket(key);
    StoredValue* v = ht.unlocked_find(
            key, hbl.getBucketNum(), WantsDeleted::Yes, TrackReference::No);
    if (!v) {
        return false;
    }
    ht.unlocked_del(hbl, key);
    return true;
}

// ----------------------------------------------------------------------
// Actual tests below.
// ----------------------------------------------------------------------

// Test fixture for HashTable tests.
class HashTableTest : public ::testing::Test {
protected:
    static std::unique_ptr<AbstractStoredValueFactory> makeFactory(
            bool isOrdered = false) {
        if (isOrdered) {
            return std::make_unique<OrderedStoredValueFactory>(global_stats);
        }
        return std::make_unique<StoredValueFactory>(global_stats);
    }
    const size_t defaultHtSize = Configuration().getHtSize();
};

TEST_F(HashTableTest, Size) {
    HashTable h(global_stats, makeFactory(), defaultHtSize, /*locks*/ 1);
    ASSERT_EQ(0, count(h));

    store(h, makeStoredDocKey("testkey"));

    EXPECT_EQ(1, count(h));
}

TEST_F(HashTableTest, SizeTwo) {
    HashTable h(global_stats, makeFactory(), defaultHtSize, /*locks*/ 1);
    ASSERT_EQ(0, count(h));

    auto keys = generateKeys(5);
    storeMany(h, keys);
    EXPECT_EQ(5, count(h));

    h.clear();
    EXPECT_EQ(0, count(h));
}

TEST_F(HashTableTest, ReverseDeletions) {
    size_t initialSize = global_stats.currentSize.load();
    HashTable h(global_stats, makeFactory(), 5, 1);
    ASSERT_EQ(0, count(h));
    const int nkeys = 1000;

    auto keys = generateKeys(nkeys);
    storeMany(h, keys);
    EXPECT_EQ(nkeys, count(h));

    std::reverse(keys.begin(), keys.end());

    for (const auto& key : keys) {
        del(h, key);
    }

    EXPECT_EQ(0, count(h));
    EXPECT_EQ(initialSize, global_stats.currentSize.load());
}

TEST_F(HashTableTest, ForwardDeletions) {
    size_t initialSize = global_stats.currentSize.load();
    HashTable h(global_stats, makeFactory(), 5, 1);
    ASSERT_EQ(5, h.getSize());
    ASSERT_EQ(1, h.getNumLocks());
    ASSERT_EQ(0, count(h));
    const int nkeys = 1000;

    auto keys = generateKeys(nkeys);
    storeMany(h, keys);
    EXPECT_EQ(nkeys, count(h));

    for (const auto& key : keys) {
        del(h, key);
    }

    EXPECT_EQ(0, count(h));
    EXPECT_EQ(initialSize, global_stats.currentSize.load());
}

static void verifyFound(HashTable &h, const std::vector<StoredDocKey> &keys) {
    EXPECT_FALSE(h.find(makeStoredDocKey("aMissingKey"),
                        TrackReference::Yes,
                        WantsDeleted::No));

    for (const auto& key : keys) {
        EXPECT_TRUE(h.find(key, TrackReference::Yes, WantsDeleted::No));
    }
}

static void testFind(HashTable &h) {
    const int nkeys = 1000;

    auto keys = generateKeys(nkeys);
    storeMany(h, keys);

    verifyFound(h, keys);
}

TEST_F(HashTableTest, Find) {
    HashTable h(global_stats, makeFactory(), 5, 1);
    testFind(h);
}

TEST_F(HashTableTest, Resize) {
    HashTable h(global_stats, makeFactory(), 5, 3);

    auto keys = generateKeys(1000);
    storeMany(h, keys);

    verifyFound(h, keys);

    h.resize(6143);
    EXPECT_EQ(6143, h.getSize());

    verifyFound(h, keys);

    h.resize(769);
    EXPECT_EQ(769, h.getSize());

    verifyFound(h, keys);

    h.resize(static_cast<size_t>(std::numeric_limits<int>::max()) + 17);
    EXPECT_EQ(769, h.getSize());

    verifyFound(h, keys);
}

class AccessGenerator : public Generator<bool> {
public:

    AccessGenerator(const std::vector<StoredDocKey> &k,
                    HashTable &h) : keys(k), ht(h), size(10000) {
        std::random_shuffle(keys.begin(), keys.end());
    }

    bool operator()() {
        for (const auto& key : keys) {
            if (rand() % 111 == 0) {
                resize();
            }
            del(ht, key);
        }
        return true;
    }

private:

    void resize() {
        ht.resize(size);
        size = size == 1000 ? 3000 : 1000;
    }

    std::vector<StoredDocKey>  keys;
    HashTable                &ht;
    std::atomic<size_t>       size;
};

TEST_F(HashTableTest, ConcurrentAccessResize) {
    HashTable h(global_stats, makeFactory(), 5, 3);

    auto keys = generateKeys(2000);
    h.resize(keys.size());
    storeMany(h, keys);

    verifyFound(h, keys);

    srand(918475);
    AccessGenerator gen(keys, h);
    getCompletedThreads(4, &gen);
}

TEST_F(HashTableTest, AutoResize) {
    HashTable h(global_stats, makeFactory(), 5, 3);

    ASSERT_EQ(5, h.getSize());

    auto keys = generateKeys(1000);
    storeMany(h, keys);

    verifyFound(h, keys);

    h.resize();
    EXPECT_EQ(769, h.getSize());
    verifyFound(h, keys);
}

TEST_F(HashTableTest, DepthCounting) {
    HashTable h(global_stats, makeFactory(), 5, 1);
    const int nkeys = 5000;

    auto keys = generateKeys(nkeys);
    storeMany(h, keys);

    HashTableDepthStatVisitor depthCounter;
    h.visitDepth(depthCounter);
    // std::cout << "Max depth:  " << depthCounter.maxDepth << std::endl;
    EXPECT_GT(depthCounter.max, 1000);
}

TEST_F(HashTableTest, PoisonKey) {
    HashTable h(global_stats, makeFactory(), 5, 1);

    store(h, makeStoredDocKey("A\\NROBs_oc)$zqJ1C.9?XU}Vn^(LW\"`+K/4lykF[ue0{ram;fvId6h=p&Zb3T~SQ]82'ixDP"));
    EXPECT_EQ(1, count(h));
}

// Test fixture for HashTable statistics tests.
class HashTableStatsTest
        : public HashTableTest,
          public ::testing::WithParamInterface<item_eviction_policy_t> {
protected:
    HashTableStatsTest()
        : ht(stats, makeFactory(), 5, 1),
          initialSize(0),
          key(makeStoredDocKey("somekey")),
          itemSize(16 * 1024),
          item(key, 0, 0, std::string(itemSize, 'x').data(), itemSize),
          evictionPolicy(GetParam()) {
    }

    void SetUp() override {
        global_stats.reset();
        ASSERT_EQ(0, ht.memSize.load());
        ASSERT_EQ(0, ht.cacheSize.load());
        initialSize = stats.currentSize.load();

        EXPECT_EQ(0, ht.getNumItems());
        EXPECT_EQ(0, ht.getNumInMemoryItems());
        EXPECT_EQ(0, ht.getNumInMemoryNonResItems());
        EXPECT_EQ(0, ht.getNumTempItems());
        EXPECT_EQ(0, ht.getNumDeletedItems());
    }

    void TearDown() override {
        EXPECT_EQ(0, ht.memSize.load());
        EXPECT_EQ(0, ht.cacheSize.load());
        EXPECT_EQ(initialSize, stats.currentSize.load());

        if (evictionPolicy == VALUE_ONLY) {
            // Only check is zero for ValueOnly; under full eviction getNumItems
            // return the total number of items (including those fully evicted).
            EXPECT_EQ(0, ht.getNumItems());
        }
        EXPECT_EQ(0, ht.getNumInMemoryItems());
        EXPECT_EQ(0, ht.getNumTempItems());
        EXPECT_EQ(0, ht.getNumDeletedItems());
    }

    EPStats stats;
    HashTable ht;
    size_t initialSize;
    const StoredDocKey key;
    const size_t itemSize;
    Item item;
    const item_eviction_policy_t evictionPolicy;
};

TEST_P(HashTableStatsTest, Size) {
    EXPECT_EQ(MutationStatus::WasClean, ht.set(item));

    del(ht, key);
}

TEST_P(HashTableStatsTest, SizeFlush) {
    EXPECT_EQ(MutationStatus::WasClean, ht.set(item));

    ht.clear();
}

TEST_P(HashTableStatsTest, SizeEject) {
    EXPECT_EQ(MutationStatus::WasClean, ht.set(item));

    StoredValue* v(ht.find(key, TrackReference::Yes, WantsDeleted::No));
    EXPECT_TRUE(v);
    v->markClean();
    EXPECT_TRUE(ht.unlocked_ejectItem(v, evictionPolicy));

    del(ht, key);
}

TEST_P(HashTableStatsTest, EjectFlush) {
    EXPECT_EQ(MutationStatus::WasClean, ht.set(item));

    StoredValue* v(ht.find(key, TrackReference::Yes, WantsDeleted::No));
    EXPECT_TRUE(v);
    v->markClean();
    EXPECT_TRUE(ht.unlocked_ejectItem(v, evictionPolicy));

    ht.clear();
}

INSTANTIATE_TEST_CASE_P(
        ValueAndFullEviction,
        HashTableStatsTest,
        ::testing::Values(VALUE_ONLY, FULL_EVICTION),
        [](const ::testing::TestParamInfo<item_eviction_policy_t>& info) {
            switch (info.param) {
            case VALUE_ONLY:
                return "VALUE_ONLY";
            case FULL_EVICTION:
                return "FULL_EVICTION";
            }
            throw std::invalid_argument("Unknown eviction_policy:" +
                                        std::to_string(info.param));
        });

TEST_F(HashTableTest, ItemAge) {
    // Setup
    HashTable ht(global_stats, makeFactory(), 5, 1);
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key, 0, 0, "value", strlen("value"));
    EXPECT_EQ(MutationStatus::WasClean, ht.set(item));

    // Test
    StoredValue* v(ht.find(key, TrackReference::Yes, WantsDeleted::No));
    EXPECT_EQ(0, v->getValue()->getAge());
    v->getValue()->incrementAge();
    EXPECT_EQ(1, v->getValue()->getAge());

    // Check saturation of age.
    for (int ii = 0; ii < 300; ii++) {
        v->getValue()->incrementAge();
    }
    EXPECT_EQ(0xff, v->getValue()->getAge());

    // Check reset of age after reallocation.
    v->reallocate();
    EXPECT_EQ(0, v->getValue()->getAge());

    // Check changing age when new value is used.
    Item item2(key, 0, 0, "value2", strlen("value2"));
    item2.getValue()->incrementAge();
    ht.setValue(item2, *v);
    EXPECT_EQ(1, v->getValue()->getAge());
}

// Check not specifying results in the INITIAL_NRU_VALUE.
TEST_F(HashTableTest, NRUDefault) {
    // Setup
    HashTable ht(global_stats, makeFactory(), 5, 1);
    StoredDocKey key = makeStoredDocKey("key");

    Item item(key, 0, 0, "value", strlen("value"));
    EXPECT_EQ(MutationStatus::WasClean, ht.set(item));

    // trackReferenced=false so we don't modify the NRU while validating it.
    StoredValue* v(ht.find(key, TrackReference::No, WantsDeleted::No));
    EXPECT_NE(nullptr, v);
    EXPECT_EQ(INITIAL_NRU_VALUE, v->getNRUValue());

    // Check that find() by default /does/ update NRU.
    v = ht.find(key, TrackReference::Yes, WantsDeleted::No);
    EXPECT_NE(nullptr, v);
    EXPECT_EQ(INITIAL_NRU_VALUE - 1, v->getNRUValue());
}

// Check a specific NRU value (minimum)
TEST_F(HashTableTest, NRUMinimum) {
    // Setup
    HashTable ht(global_stats, makeFactory(), 5, 1);
    StoredDocKey key = makeStoredDocKey("key");

    Item item(key, 0, 0, "value", strlen("value"));
    item.setNRUValue(MIN_NRU_VALUE);
    EXPECT_EQ(MutationStatus::WasClean, ht.set(item));

    // trackReferenced=false so we don't modify the NRU while validating it.
    StoredValue* v(ht.find(key, TrackReference::No, WantsDeleted::No));
    EXPECT_NE(nullptr, v);
    EXPECT_EQ(MIN_NRU_VALUE, v->getNRUValue());
}

/* Test release from HT (but not deletion) of an (HT) element */
TEST_F(HashTableTest, ReleaseItem) {
    /* Setup with 2 hash buckets and 1 lock */
    HashTable ht(global_stats, makeFactory(), 2, 1);

    /* Write 5 items (there are 2 hash buckets, we want to test removing a head
       element and a non-head element) */
    const int numItems = 5;
    std::string val("value");

    for (int i = 0; i < numItems; ++i) {
        StoredDocKey key =
                makeStoredDocKey(std::string("key" + std::to_string(i)));
        Item item(key, 0, 0, val.data(), val.length());
        EXPECT_EQ(MutationStatus::WasClean, ht.set(item));
    }
    EXPECT_EQ(numItems, ht.getNumItems());

    /* Remove the element added first. This is not (most likely because we
       might have a rare case wherein 4 items are hashed to one bucket and
       "removeKey1" would be a lone element in the other hash bucket) a head
       element of a hash bucket */
    StoredDocKey releaseKey1 =
            makeStoredDocKey(std::string("key" + std::to_string(0)));

    auto hbl = ht.getLockedBucket(releaseKey1);
    StoredValue* vToRelease1 = ht.unlocked_find(releaseKey1,
                                                hbl.getBucketNum(),
                                                WantsDeleted::Yes,
                                                TrackReference::No);

    auto releasedSv1 = ht.unlocked_release(hbl, releaseKey1);

    /* Validate the copied contents */
    EXPECT_EQ(*vToRelease1, *(releasedSv1));

    /* Validate the HT element replace (hence released from HT) */
    EXPECT_EQ(vToRelease1, releasedSv1.get());

    /* Validate the HT count */
    EXPECT_EQ(numItems - 1, ht.getNumItems());

    hbl.getHTLock().unlock();

    /* Remove the element added last. This is certainly the head element of a
       hash bucket */
    StoredDocKey releaseKey2 =
            makeStoredDocKey(std::string("key" + std::to_string(numItems - 1)));

    auto hbl2 = ht.getLockedBucket(releaseKey2);
    StoredValue* vToRelease2 = ht.unlocked_find(releaseKey2,
                                                hbl.getBucketNum(),
                                                WantsDeleted::Yes,
                                                TrackReference::No);

    auto releasedSv2 = ht.unlocked_release(hbl2, releaseKey2);

    /* Validate the copied contents */
    EXPECT_EQ(*vToRelease2, *(releasedSv2));

    /* Validate the HT element replace (hence released from HT) */
    EXPECT_EQ(vToRelease2, releasedSv2.get());

    /* Validate the HT count */
    EXPECT_EQ(numItems - 2, ht.getNumItems());
}

/* Test copying an element in HT */
TEST_F(HashTableTest, CopyItem) {
    /* Setup with 2 hash buckets and 1 lock. Note: Copying is allowed only on
       OrderedStoredValues and hence hash table must have
       OrderedStoredValueFactory */
    HashTable ht(global_stats, makeFactory(true), 2, 1);

    /* Write 3 items */
    const int numItems = 3;
    auto keys = generateKeys(numItems);
    storeMany(ht, keys);

    /* Replace the StoredValue in the HT by its copy */
    StoredDocKey copyKey = makeStoredDocKey(std::string(std::to_string(0)));
    auto hbl = ht.getLockedBucket(copyKey);
    StoredValue* replaceSv = ht.unlocked_find(
            copyKey, hbl.getBucketNum(), WantsDeleted::Yes, TrackReference::No);

    /* Record some stats before 'replace by copy' */
    auto metaDataMemBeforeCopy = ht.metaDataMemory.load();
    auto datatypeCountsBeforeCopy = ht.datatypeCounts;
    auto cacheSizeBeforeCopy = ht.cacheSize.load();
    auto memSizeBeforeCopy = ht.memSize.load();
    auto statsCurrSizeBeforeCopy = global_stats.currentSize.load();

    auto res = ht.unlocked_replaceByCopy(hbl, *replaceSv);

    /* Validate the copied contents */
    EXPECT_EQ(*replaceSv, *(res.first));

    /* Validate the HT element replace (hence released from HT) */
    EXPECT_EQ(replaceSv, res.second.get());

    /* Validate the HT count */
    EXPECT_EQ(numItems, ht.getNumItems());

    /* Stats should be equal to that before 'replaceByCopy' */
    EXPECT_EQ(metaDataMemBeforeCopy, ht.metaDataMemory.load());
    EXPECT_EQ(datatypeCountsBeforeCopy, ht.datatypeCounts);
    EXPECT_EQ(cacheSizeBeforeCopy, ht.cacheSize.load());
    EXPECT_EQ(memSizeBeforeCopy, ht.memSize.load());
    EXPECT_EQ(statsCurrSizeBeforeCopy, global_stats.currentSize.load());
}

/* Test copying a deleted element in HT */
TEST_F(HashTableTest, CopyDeletedItem) {
    /* Setup with 2 hash buckets and 1 lock. Note: Copying is allowed only on
       OrderedStoredValues and hence hash table must have
       OrderedStoredValueFactory */
    HashTable ht(global_stats, makeFactory(true), 2, 1);

    /* Write 3 items */
    const int numItems = 3;
    auto keys = generateKeys(numItems);
    storeMany(ht, keys);

    /* Delete a StoredValue */
    StoredDocKey copyKey = makeStoredDocKey(std::string(std::to_string(0)));
    auto hbl = ht.getLockedBucket(copyKey);
    StoredValue* replaceSv = ht.unlocked_find(
            copyKey, hbl.getBucketNum(), WantsDeleted::Yes, TrackReference::No);

    ht.unlocked_softDelete(
            hbl.getHTLock(), *replaceSv, /* onlyMarkDeleted */ false);
    EXPECT_EQ(numItems, ht.getNumItems());
    const int expNumDeletedItems = 1;
    EXPECT_EQ(expNumDeletedItems, ht.getNumDeletedItems());

    /* Record some stats before 'replace by copy' */
    auto metaDataMemBeforeCopy = ht.metaDataMemory.load();
    auto datatypeCountsBeforeCopy = ht.datatypeCounts;
    auto cacheSizeBeforeCopy = ht.cacheSize.load();
    auto memSizeBeforeCopy = ht.memSize.load();
    auto statsCurrSizeBeforeCopy = global_stats.currentSize.load();

    /* Replace the StoredValue in the HT by its copy */
    auto res = ht.unlocked_replaceByCopy(hbl, *replaceSv);

    /* Validate the copied contents */
    EXPECT_EQ(*replaceSv, *(res.first));

    /* Validate the HT element replace (hence released from HT) */
    EXPECT_EQ(replaceSv, res.second.get());

    /* Validate the HT count */
    EXPECT_EQ(numItems, ht.getNumItems());
    EXPECT_EQ(expNumDeletedItems, ht.getNumDeletedItems());

    /* Stats should be equal to that before 'replaceByCopy' */
    EXPECT_EQ(metaDataMemBeforeCopy, ht.metaDataMemory.load());
    EXPECT_EQ(datatypeCountsBeforeCopy, ht.datatypeCounts);
    EXPECT_EQ(cacheSizeBeforeCopy, ht.cacheSize.load());
    EXPECT_EQ(memSizeBeforeCopy, ht.memSize.load());
    EXPECT_EQ(statsCurrSizeBeforeCopy, global_stats.currentSize.load());
}

// Check that an OSV which was deleted and then made alive again has the
// lock expiry correctly reset (lock_expiry is stored in the same place as
// deleted time).
TEST_F(HashTableTest, LockAfterDelete) {
    /* Setup OSVFactory with 2 hash buckets and 1 lock. */
    HashTable ht(global_stats, makeFactory(true), 2, 1);

    // Delete a key, giving it a non-zero delete time.
    auto key = makeStoredDocKey("key");
    StoredValue* sv;
    store(ht, key);
    {
        auto hbl = ht.getLockedBucket(key);
        sv = ht.unlocked_find(
                key, hbl.getBucketNum(), WantsDeleted::No, TrackReference::No);
        TimeTraveller toTheFuture(1985);
        ht.unlocked_softDelete(
                hbl.getHTLock(), *sv, /* onlyMarkDeleted */ false);
    }
    ASSERT_EQ(1, ht.getNumItems());
    ASSERT_EQ(1, ht.getNumDeletedItems());

    // Sanity check - deleted time should be set.
    auto* osv = sv->toOrderedStoredValue();
    ASSERT_GE(osv->getDeletedTime(), 1985);

    // Now re-create the same key (as alive).
    Item i(key, 0, 0, key.data(), key.size());
    EXPECT_EQ(MutationStatus::WasDirty, ht.set(i));
    EXPECT_FALSE(sv->isLocked(1985));
}

// Check that pauseResumeVisit calls with the correct Hash bucket.
TEST_F(HashTableTest, PauseResumeHashBucket) {
    // Two buckets, one lock.
    HashTable ht(global_stats, makeFactory(true), 2, 1);

    // Store keys to both hash buckets - need keys which hash to bucket 0 and 1.
    StoredDocKey key0("c", DocNamespace::DefaultCollection);
    ASSERT_EQ(0, ht.getLockedBucket(key0).getBucketNum());
    store(ht, key0);

    StoredDocKey key1("b", DocNamespace::DefaultCollection);
    ASSERT_EQ(1, ht.getLockedBucket(key1).getBucketNum());
    store(ht, key1);

    class MockHTVisitor : public HashTableVisitor {
    public:
        MOCK_METHOD2(visit,
                     bool(const HashTable::HashBucketLock& lh, StoredValue& v));
    } mockVisitor;

    // Visit the HashTable; should find two keys, one in each bucket and the
    // lockHolder should have the correct hashbucket.
    using namespace ::testing;
    EXPECT_CALL(mockVisitor,
                visit(Property(&HashTable::HashBucketLock::getBucketNum, 0),
                      Property(&StoredValue::getKey, Eq(key0))))
            .Times(1)
            .WillOnce(Return(true));
    EXPECT_CALL(mockVisitor,
                visit(Property(&HashTable::HashBucketLock::getBucketNum, 1),
                      Property(&StoredValue::getKey, Eq(key1))))
            .Times(1)
            .WillOnce(Return(true));

    HashTable::Position start;
    ht.pauseResumeVisit(mockVisitor, start);
}
