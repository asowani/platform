/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include <memory>

namespace Couchbase {
    namespace Compression {
        enum class Algorithm {
            Snappy
        };

        struct Buffer {
            Buffer() : len(0) {}
            std::unique_ptr<char[]> data;
            size_t len;
        };

        /**
         * Inflate the data in the buffer into the output buffer
         *
         * @param algorithm the algorithm to use
         * @param buf pointer to the input data
         * @param len the size of the input data
         * @param output Where to store the result
         * @return true if success, false otherwise
         * @throws std::bad_alloc if we fail to allocate memory for the
         *                        destination buffer
         */
        bool inflate(const Algorithm algorithm,
                     const char* buf,
                     size_t len,
                     Buffer& output);

        /**
         * Deflate the data in the buffer into the output buffer
         *
         * @param algorithm the algorithm to use
         * @param buf pointer to the input data
         * @param len the size of the input data
         * @param output Where to store the result
         * @return true if success, false otherwise
         * @throws std::bad_alloc if we fail to allocate memory for the
         *                        destination buffer
         */
        bool deflate(const Algorithm algorithm,
                     const char* buf,
                     size_t len,
                     Buffer& output);
    }
}