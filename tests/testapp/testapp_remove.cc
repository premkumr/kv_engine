/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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

#include "testapp.h"
#include "testapp_client_test.h"
#include <protocol/connection/client_greenstack_connection.h>
#include <protocol/connection/client_mcbp_connection.h>

#include <algorithm>
#include <platform/compress.h>

class RemoveTest : public TestappClientTest {
public:

protected:

    /**
     * Verify that a path isn't there anymore!
     *
     * @param path the path to check for
     */
    void verifyMissing(const std::string& path) {
        BinprotSubdocCommand cmd;
        cmd.setOp(PROTOCOL_BINARY_CMD_SUBDOC_GET);
        cmd.setKey(name);
        cmd.setPath(path);
        cmd.setFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_ACCESS_DELETED);

        BinprotSubdocResponse resp;
        safe_do_command(cmd, resp,
                        PROTOCOL_BINARY_RESPONSE_SUBDOC_PATH_ENOENT);
    }


    /**
     * Create a document and keep the information about the document in
     * the info member
     */
    void createDocument() {
        Document doc;
        doc.info.cas = Greenstack::CAS::Wildcard;
        doc.info.compression = Greenstack::Compression::None;
        doc.info.datatype = Greenstack::Datatype::Json;
        doc.info.flags = 0xcaffee;
        doc.info.id = name;
        auto content = to_string(memcached_cfg);
        std::copy(content.begin(), content.end(),
                  std::back_inserter(doc.value));
        info = getConnection().mutate(doc, 0, Greenstack::MutationType::Add);
    }

    MutationInfo info;
};

INSTANTIATE_TEST_CASE_P(TransportProtocols,
                        RemoveTest,
                        ::testing::Values(TransportProtocols::McbpPlain,
                                          TransportProtocols::McbpIpv6Plain,
                                          TransportProtocols::McbpSsl,
                                          TransportProtocols::McbpIpv6Ssl
                                         ),
                        ::testing::PrintToStringParamName());

/**
 * Verify that remove of an non-existing object work (and return the expected
 * value)
 */
TEST_P(RemoveTest, RemoveNonexisting) {
    auto& conn = getConnection();

    try {
        conn.remove(name, 0);
    } catch (const ConnectionError& error) {
        EXPECT_TRUE(error.isNotFound()) << error.what();
    }
}

/**
 * Verify that remove of an existing document with setting the CAS value
 * to the wildcard works
 */
TEST_P(RemoveTest, RemoveCasWildcard) {
    auto& conn = getConnection();

    createDocument();
    auto deleted = conn.remove(name, 0);
    EXPECT_NE(info.cas, deleted.cas);
}

/**
 * Verify that remove of an existing document with an incorrect value
 * fails with EEXISTS
 */
TEST_P(RemoveTest, RemoveWithInvalidCas) {
    auto& conn = getConnection();

    createDocument();
    try {
        conn.remove(name, 0, info.cas + 1);
    } catch (const ConnectionError& error) {
        EXPECT_TRUE(error.isAlreadyExists()) << error.what();
    }
}

/**
 * Verify that remove of an existing document with the correct CAS
 * value works
 */
TEST_P(RemoveTest, RemoveWithCas) {
    auto& conn = getConnection();

    createDocument();
    auto deleted = conn.remove(name, 0, info.cas);
    EXPECT_NE(info.cas, deleted.cas);
}

/**
 * Verify that you may access system attributes of a deleted
 * document, and that the user attributes will be nuked off
 */
TEST_P(RemoveTest, RemoveWithXattr) {
    auto& conn = getConnection();

    createDocument();
    createXattr("meta.content-type", "\"application/json; charset=utf-8\"");
    createXattr("_rbac.attribute", "\"read-only\"");
    auto deleted = conn.remove(name, 0, info.cas);
    EXPECT_NE(info.cas, deleted.cas);

    // The system xattr should have been preserved
    EXPECT_EQ("\"read-only\"", getXattr("_rbac.attribute", true));

    // The user xattr should not be there
    verifyMissing("meta.content_type");
}

/**
 * Verify that the server works as expected when it figures out that all
 * of the xattrs it was supposed to rewrite should be stripped off
 */
TEST_P(RemoveTest, RemoveWithOnlyUserAttributres) {
    auto& conn = getConnection();

    createDocument();
    createXattr("meta.content-type", "\"application/json; charset=utf-8\"");
    auto deleted = conn.remove(name, 0, info.cas);
    EXPECT_NE(info.cas, deleted.cas);
}