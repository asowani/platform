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

#include <platform/processclock.h>

cb::ProcessClock::time_point cb::DefaultProcessClockSource::now() {
    return cb::ProcessClock::now();
}

static cb::DefaultProcessClockSource clockSource;

cb::DefaultProcessClockSource& cb::defaultProcessClockSource() {
    return clockSource;
}

std::chrono::nanoseconds cb::to_ns_since_epoch(
        const ProcessClock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                         tp.time_since_epoch());
}