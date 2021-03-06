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
#include "diske.sim.h"
#include <dsn/service_api.h>

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "aio_provider"

namespace dsn { namespace tools {

DEFINE_TASK_CODE(LPC_NATIVE_AIO_REDIRECT, TASK_PRIORITY_HIGH, THREAD_POOL_DEFAULT)

sim_aio_provider::sim_aio_provider(disk_engine* disk, aio_provider* inner_provider)
: NATIVE_AIO_PROVIDER(disk, inner_provider)
{
}

sim_aio_provider::~sim_aio_provider(void)
{
}

void sim_aio_provider::aio(aio_task_ptr& aio)
{
    error_code err;
    uint32_t bytes;

    err = aio_internal(aio, false, &bytes);
    complete_io(aio, err, bytes, 0);
}

}} // end namespace
