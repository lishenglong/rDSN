/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once

#include "mutation_cache.h"

namespace dsn { namespace replication {


class prepare_list : public mutation_cache
{
public:
    typedef std::function<void (mutation_ptr&)> mutation_committer;

public:
    prepare_list(
        decree init_decree, int max_count,
        mutation_committer committer,
        bool allow_prepare_ack_before_logging);

    decree last_committed_decree() const { return _last_committed_decree; }
    void   reset(decree init_decree);
    void   truncate(decree init_decree);
    
    //
    // for two-phase commit
    //
    error_code prepare(mutation_ptr& mu, partition_status status); // unordered prepare
    bool       commit(decree decree, bool force); // ordered commit
    
private:
    void sanity_check();

private:
    bool                     _allow_prepare_ack_before_logging;
    decree                   _last_committed_decree;    
    mutation_committer       _committer;
};
 
}} // namespace

