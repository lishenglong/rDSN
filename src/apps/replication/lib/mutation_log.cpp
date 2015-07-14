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

#include "mutation_log.h"
#include <boost/filesystem.hpp>
#ifdef _WIN32
#include <io.h>
#endif

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "mutation_log"

namespace dsn { namespace replication {

    using namespace ::dsn::service;

mutation_log::mutation_log(uint32_t log_buffer_size_mb, uint32_t log_pending_max_ms, uint32_t max_log_file_mb, bool batch_write, int write_task_max_count)
{
    _log_buffer_size_bytes = log_buffer_size_mb * 1024 * 1024;
    _log_pending_max_milliseconds = log_pending_max_ms;    
    _max_log_file_size_in_bytes = ((int64_t)max_log_file_mb) * 1024L * 1024L;
    _batch_write = batch_write;
    _write_task_number = write_task_max_count;

    _last_file_number = 0;
    _global_start_offset = 0;
    _global_end_offset = 0;

    _last_log_file = nullptr;
    _current_log_file = nullptr;
    _dir = "";

    _max_staleness_for_commit = 0;
}

mutation_log::~mutation_log()
{
    close();
}

void mutation_log::reset()
{
    _last_file_number = 0;
    _global_start_offset = 0;
    _global_end_offset = 0;

    _last_log_file = nullptr;
    _current_log_file = nullptr;

    for (auto it = _log_files.begin(); it != _log_files.end(); it++)
    {
        it->second->close();
    }
    _log_files.clear();
}

int mutation_log::initialize(const char* dir)
{
    zauto_lock l(_lock);

    //create dir if necessary
    if (!boost::filesystem::exists(dir) && !boost::filesystem::create_directory(dir))
    {
        derror ("open mutation_log: create log path failed");
        return ERR_FILE_OPERATION_FAILED;
    }

    _dir = std::string(dir);

    
    _last_file_number = 0;
    _log_files.clear();

    boost::filesystem::directory_iterator endtr;

    for (boost::filesystem::directory_iterator it(dir);
        it != endtr;
        ++it)
    {
        std::string fullPath = it->path().string();
        log_file_ptr log = log_file::opend_read(fullPath.c_str());
        if (log == nullptr)
        {
            dwarn ("Skip file %s during log init", fullPath.c_str());
            continue;
        }

        dassert (_log_files.find(log->index()) == _log_files.end(), "");
        _log_files[log->index()] = log;
    }

    if (_log_files.size() > 0)
    {
        _last_file_number = _log_files.begin()->first - 1;
        _global_start_offset = _log_files.begin()->second->start_offset();
    }

    for (auto it = _log_files.begin(); it != _log_files.end(); it++)
    {
        if (++_last_file_number != it->first)
        {
            derror ("log file missing with index %u", _last_file_number);
            return ERR_OBJECT_NOT_FOUND;
        }

        _global_end_offset = it->second->end_offset();
    }
    
    return ERR_OK;
}

int mutation_log::create_new_log_file()
{
    //dassert (_lock.IsHeldByCurrentThread(), "");

    if (_current_log_file != nullptr)
    {
        _last_log_file = _current_log_file;
        dassert (_current_log_file->end_offset() == _global_end_offset, "");
    }

    log_file_ptr logFile = log_file::create_write(_dir.c_str(), _last_file_number + 1, _global_end_offset, _max_staleness_for_commit, _write_task_number);
    if (logFile == nullptr)
    {
        derror ("cannot create log file with index %u", _last_file_number);
        return ERR_FILE_OPERATION_FAILED;
    }    

    derror ("create new log file %s", logFile->path().c_str());
        
    _last_file_number++;
    dassert (_log_files.find(_last_file_number) == _log_files.end(), "");
    _log_files[_last_file_number] = logFile;

    dassert (logFile->end_offset() == logFile->start_offset(), "");
    dassert (_global_end_offset == logFile->end_offset(), "");

    _current_log_file = logFile; 

    create_new_pending_buffer();
    auto len = logFile->write_header(_pending_write, _init_prepared_decrees, 
        static_cast<int>(_log_buffer_size_bytes));
    _global_end_offset += len;
    dassert (_pending_write->total_size() == len + MSG_HDR_SERIALIZED_SIZE, "");

    return ERR_OK;
}

void mutation_log::create_new_pending_buffer()
{
    dassert (_pending_write == nullptr, "");
    dassert (_pending_write_callbacks == nullptr, "");
    dassert (_pending_write_timer == nullptr, "");

    _pending_write = message::create_request(RPC_PREPARE, _log_pending_max_milliseconds);
    _pending_write_callbacks.reset(new std::list<aio_task_ptr>);

    if (_batch_write)
    {
        _pending_write_timer = tasking::enqueue(
            LPC_MUTATION_LOG_PENDING_TIMER,
            this,
            std::bind(&mutation_log::internal_pending_write_timer, this, _pending_write->header().id),
            -1, 
            _log_pending_max_milliseconds
            );
    }

    dassert (_pending_write->total_size() == MSG_HDR_SERIALIZED_SIZE, "");
    _global_end_offset += MSG_HDR_SERIALIZED_SIZE;
}

void mutation_log::internal_pending_write_timer(uint64_t id)
{
    zauto_lock l(_lock);
    dassert (nullptr != _pending_write, "");
    dassert (_pending_write->header().id == id, "");
    dassert (task::get_current_task() == _pending_write_timer, "");

    _pending_write_timer = nullptr;
    write_pending_mutations();
}

int mutation_log::write_pending_mutations(bool create_new_log_when_necessary)
{
    dassert (_pending_write != nullptr, "");
    dassert (_pending_write_timer == nullptr, "");
    dassert (_pending_write_callbacks != nullptr, "");

    _pending_write->seal(true);

    auto bb = _pending_write->writer().get_buffer();
    uint64_t offset = end_offset() - bb.length();
    auto buf = bb.buffer();
    blob bb2(buf, bb.length());

    task_ptr aio = _current_log_file->write_log_entry(
        bb2,
        LPC_AIO_IMMEDIATE_CALLBACK,
        this,
        std::bind(
            &mutation_log::internal_write_callback, 
            std::placeholders::_1, 
            std::placeholders::_2, 
            _pending_write_callbacks, bb2),
        offset,
        -1
        );    
    
    if (aio == nullptr)
    {
        internal_write_callback(ERR_FILE_OPERATION_FAILED, 0, _pending_write_callbacks, bb2);
    }
    else
    {
        dassert (_global_end_offset == _current_log_file->end_offset(), "");
    }

    _pending_write = nullptr;
    _pending_write_callbacks = nullptr;
    _pending_write_timer = nullptr;

    if (aio == nullptr)
    {
        return ERR_FILE_OPERATION_FAILED;
    }    

    if (create_new_log_when_necessary && _current_log_file->end_offset() - _current_log_file->start_offset() >= _max_log_file_size_in_bytes)
    {
        int ret = create_new_log_file();
        if (ERR_OK != ret)
        {
            derror ("create new log file failed, err = %d", ret);
        }
        return ret;
    }
    return ERR_OK;
}

void mutation_log::internal_write_callback(error_code err, uint32_t size, mutation_log::pending_callbacks_ptr callbacks, blob data)
{
    for (auto it = callbacks->begin(); it != callbacks->end(); it++)
    {
        (*it)->enqueue(err, size, nullptr);
    }
}

/*
TODO: when there is a log error, the server cannot contain any primary or secondary any more!
*/
int mutation_log::replay(ReplayCallback callback)
{
    zauto_lock l(_lock);

    int64_t offset = start_offset();
    int err = ERR_OK;
    for (auto it = _log_files.begin(); it != _log_files.end(); it++)
    {
        log_file_ptr log = it->second;

        if (log->start_offset() != offset)
        {
            derror("offset mismatch in log file offset and global offset %lld vs %lld", log->start_offset(), offset);
            return ERR_FILE_OPERATION_FAILED;
        }

        _last_log_file = log;

        ::dsn::blob bb;
        err = log->read_next_log_entry(bb);
        if (err != ERR_OK)
        {
            if (err == ERR_HANDLE_EOF)
            {
                err = ERR_OK;
                continue;
            }

            derror(
                "read log header failed for %s, err = %x", log->path().c_str(), err);
            break;
        }


        message_ptr msg(new message(bb));
        offset += MSG_HDR_SERIALIZED_SIZE;

        if (!msg->is_right_body())
        {
            derror("data read crc check failed at offset %llu", offset);
            return ERR_WRONG_CHECKSUM;
        }

        offset += log->read_header(msg);

        while (true)
        {
            while (!msg->reader().is_eof())
            {
                auto oldSz = msg->reader().get_remaining_size();
                mutation_ptr mu = mutation::read_from(msg);
                dassert (nullptr != mu, "");                                
                mu->set_logged();

                if (mu->data.header.log_offset != offset)
                {
                    derror("offset mismatch in log entry and mutation %lld vs %lld", offset, mu->data.header.log_offset);
                    return ERR_FILE_OPERATION_FAILED;
                }

                callback(mu);

                offset += oldSz - msg->reader().get_remaining_size();
            }

            err = log->read_next_log_entry(bb);
            if (err != ERR_OK)
            {
                if (err == ERR_HANDLE_EOF)
                {
                    err = ERR_OK;
                    break;
                }

                derror(
                    "read log entry failed for %s, err = %x", log->path().c_str(), err);
                break;
            }
            
            msg = new message(bb);
            offset += MSG_HDR_SERIALIZED_SIZE;

            if (!msg->is_right_body())
            {
                derror("data read crc check failed at offset %llu", offset);
                return ERR_WRONG_CHECKSUM;
            }
        }

        log->close();

        // tail data corruption is checked by next file's offset checking
        if (err != ERR_INVALID_DATA && err != ERR_OK)
            break;        
    }

    if (err == ERR_INVALID_DATA && offset + _last_log_file->header().log_buffer_size_bytes >= end_offset())
    {
        // remove bad data at tail, but still we may lose data so error code remains unchanged
        _global_end_offset = offset;
    }
    else if (err == ERR_OK)
    {
        dassert (end_offset() == offset, "");
    }

    return err;
}

int mutation_log::start_write_service(multi_partition_decrees& initMaxDecrees, int max_staleness_for_commit)
{
    zauto_lock l(_lock);

    _init_prepared_decrees = initMaxDecrees;
    _max_staleness_for_commit = max_staleness_for_commit;
    
    dassert (_current_log_file == nullptr, "");
    return create_new_log_file();
}

void mutation_log::close()
{
    while (true)
    {
        zauto_lock l(_lock);

        if (nullptr != _pending_write_timer)
        {
            bool finish;
            _pending_write_timer->cancel(false, &finish);
            if (finish)
            {
                _pending_write_timer = nullptr;
                write_pending_mutations(false);
                dassert (nullptr == _pending_write_timer, "");
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(0));
                continue;
            }
        }

        if (nullptr != _current_log_file)
        {
            _current_log_file->close();
            _current_log_file = nullptr;
        }
        break;
    }
}

task_ptr mutation_log::append(mutation_ptr& mu, 
                        task_code callback_code,
                        servicelet* callback_host,
                        aio_handler callback,
                        int hash)
{
    zauto_lock l(_lock);

    dassert(nullptr != _current_log_file, "");

    auto it = _init_prepared_decrees.find(mu->data.header.gpid);
    if (it != _init_prepared_decrees.end())
    {
        if (it->second < mu->data.header.decree)
        {
            it->second = mu->data.header.decree;
        }
    }
    else
    {
        _init_prepared_decrees[mu->data.header.gpid] = mu->data.header.decree;
    }
    
    if (_pending_write == nullptr)
    {
        create_new_pending_buffer();
    }

    auto oldSz = _pending_write->total_size();
    mu->data.header.log_offset = end_offset();
    mu->write_to(_pending_write);
    _global_end_offset += _pending_write->total_size() - oldSz;

    aio_task_ptr tsk(new file::internal_use_only::service_aio_task(callback_code, callback_host, callback, hash));
    
    _pending_write_callbacks->push_back(tsk);

    /*if (dsn::service::spec().traceOptions.PathTracing)
    {
        ddebug( 
            "BATCHTHROUGH mutation write with io callback DstTaskId = %016llx", task->TaskId()
                );
    }*/
    
    // printf ("append: %llu, offset = %llu, global = %llu, pendingSize = %u\n",
    //     mu->data.header.decree, mu->data.header.log_offset, _global_end_offset, _pending_write->total_size());

    if (!_batch_write)
    {
        write_pending_mutations();
    }
    else if ((uint32_t)_pending_write->total_size() >= _log_buffer_size_bytes)
    {
        if (_pending_write_timer->cancel(false))
        {
            _pending_write_timer = nullptr;
            write_pending_mutations();
        }
    }

    return tsk;
}

void mutation_log::on_partition_removed(global_partition_id gpid)
{
    zauto_lock l(_lock);
    _init_prepared_decrees.erase(gpid);
}

int mutation_log::garbage_collection(multi_partition_decrees& durable_decrees)
{
    std::map<int, log_file_ptr> files;
    std::map<int, log_file_ptr>::reverse_iterator itr;
    
    {
        zauto_lock l(_lock);
        files = _log_files;
        if (nullptr != _current_log_file) files.erase(_current_log_file->index());
    }
        
    for (itr = files.rbegin(); itr != files.rend(); itr++)
    {
        log_file_ptr log = itr->second;

        bool deleteOlderFiles = true;
        for (auto it2 = durable_decrees.begin(); it2 != durable_decrees.end(); it2++)
        {
            global_partition_id gpid = it2->first;
            decree lastDurableDecree = it2->second;
        
            auto it3 = log->init_prepare_decrees().find(gpid);
            if (it3 == log->init_prepare_decrees().end())
            {
                // new partition, ok to delete older logs
            }
            else
            {
                decree initPrepareDecree = it3->second;
                decree maxPrepareDecreeBeforeThis = initPrepareDecree;
                
                // when all possible decress are covered by durable decress
                if (lastDurableDecree >= maxPrepareDecreeBeforeThis)
                {
                    // ok to delete older logs
                }
                else
                {
                    deleteOlderFiles = false;
                    break;
                }
            }
        }

        if (deleteOlderFiles)
        {
            break;
        }
    }

    if (itr != files.rend()) itr++;
    
    int count = 0;
    for (; itr != files.rend(); itr++)
    {
        itr->second->close();

        ddebug("remove log segment %s", itr->second->path().c_str());

        boost::filesystem::remove(itr->second->path().c_str());

        count++;

        {
            zauto_lock l(_lock);
            _log_files.erase(itr->first);
        }
    }

    return count;
}


std::map<int, log_file_ptr>& mutation_log::get_logfiles_for_test()
{
    return _log_files;
}


//------------------- log_file --------------------------
/*static */log_file_ptr log_file::opend_read(const char* path)
{
    std::string pt = std::string(path);
    char splitters[] = { '\\', '/', 0 };
    std::string name = utils::get_last_component(pt, splitters);

    // log.index.startOffset
    if (name.length() < strlen("log.")
        || name.substr(0, strlen("log.")) != std::string("log.")
        || (name.length() > strlen(".removed") && name.substr(name.length() - strlen(".removed")) == std::string(".removed"))
        )
    {
        dwarn( "Invalid log path %s", path);
        return nullptr;
    }

    auto pos = name.find_first_of('.');
    auto pos2 = name.find_first_of('.', pos + 1);
    if (pos2 == std::string::npos)
    {
        dwarn( "Invalid log path %s", path);
        return nullptr;
    }

    handle_t hFile = (handle_t)::open(path, O_RDONLY | O_BINARY, 0);

    if (hFile == 0)
    {
        dwarn("open log %s failed", path);
        return nullptr;
    }

    
    int index = atoi(name.substr(pos + 1, pos2 - pos - 1).c_str());
    int64_t startOffset = atol(name.substr(pos2 + 1).c_str());
    
    return new log_file(path, hFile, index, startOffset, 0, true);
}

/*static*/ log_file_ptr log_file::create_write(const char* dir, int index, int64_t startOffset, int max_staleness_for_commit, int write_task_max_count)
{
    char path[512]; 
    sprintf (path, "%s/log.%u.%lld", dir, index, static_cast<long long int>(startOffset));
    
    handle_t hFile = dsn::service::file::open(path, O_RDWR | O_CREAT | O_BINARY, 0666);
    if (hFile == 0)
    {
        dwarn("create log %s failed", path);
        return nullptr;
    }

    return new log_file(path, hFile, index, startOffset, max_staleness_for_commit, false, write_task_max_count);
}

log_file::log_file(const char* path, handle_t handle, int index, int64_t startOffset, int max_staleness_for_commit, bool isRead, int write_task_max_count)
{
    _start_offset = startOffset;
    _end_offset = startOffset;
    _handle = handle;
    _is_read = isRead;
    _path = path;
    _index = index; 
    memset(&_header, 0, sizeof(_header));
    _header.max_staleness_for_commit = max_staleness_for_commit;
    _write_task_itr = 0;    
    _write_tasks.resize(write_task_max_count);

    if (isRead)
    {
        boost::filesystem::path cp(_path);
        _end_offset += boost::filesystem::file_size(cp);
    }
}

void log_file::close()
{
    for (size_t itr = 0; itr < _write_tasks.size(); ++itr)
    {
        if (_write_tasks.at(itr) != nullptr)
        {            
            _write_tasks.at(itr)->wait();
            _write_tasks.at(itr) = nullptr;
        }
    }

    if (0 != _handle)
    {
        if (_is_read)
            ::close((int)(_handle));
        else
            dsn::service::file::close(_handle);

        _handle = 0;
    }
}

int log_file::read_next_log_entry(__out_param ::dsn::blob& bb)
{
    dassert (_is_read, "");

    std::shared_ptr<char> hdrBuffer(new char[MSG_HDR_SERIALIZED_SIZE]);
    
    int read_count = ::read(
        (int)(_handle),
        hdrBuffer.get(),
        MSG_HDR_SERIALIZED_SIZE
        );

    if (MSG_HDR_SERIALIZED_SIZE != read_count)
    {
        if (read_count > 0)
        {
            derror("incomplete read data, size = %d vs %d", read_count, MSG_HDR_SERIALIZED_SIZE);
            return ERR_INVALID_DATA;
        }
        else
        {
            return ERR_HANDLE_EOF;
        }
    }

    message_header hdr;
    ::dsn::blob bb2(hdrBuffer, MSG_HDR_SERIALIZED_SIZE);
    ::dsn::binary_reader reader(bb2);
    hdr.unmarshall(reader);

    if (!hdr.is_right_header((char*)hdrBuffer.get()))
    {
        derror("invalid data header");
        return ERR_INVALID_DATA;
    }

    std::shared_ptr<char> data(new char[MSG_HDR_SERIALIZED_SIZE + hdr.body_length]);
    memcpy(data.get(), hdrBuffer.get(), MSG_HDR_SERIALIZED_SIZE);
    bb.assign(data, 0, MSG_HDR_SERIALIZED_SIZE + hdr.body_length);

    read_count = ::read(
        (int)(_handle),
        (void*)((char*)bb.data() + MSG_HDR_SERIALIZED_SIZE),
        hdr.body_length
        );

    if (hdr.body_length != read_count)
    {
        derror("incomplete read data, size = %d vs %d", read_count, MSG_HDR_SERIALIZED_SIZE);
        return ERR_INVALID_DATA;
    }
    
    return ERR_OK;
}

aio_task_ptr log_file::write_log_entry(
                blob& bb,
                task_code evt,  // to indicate which thread pool to execute the callback
                servicelet* callback_host,
                aio_handler callback,
                int64_t offset,
                int hash
                )
{
    dassert (!_is_read, "");
    dassert (offset == end_offset(), "");

    auto task = file::write(
        _handle, 
        bb.data(),
        bb.length(),
        offset - start_offset(), 
        evt, 
        callback_host,
        callback, 
        hash
        );
    
    _end_offset = offset + bb.length();

    //printf ("WriteBB: size = %u, startoffset = %llu, endOffset = %llu\n", bb.length(), offset, _end_offset);
        
    // !!! dangerous, we are in the middle of a local lock
    // we already have flow control on maximum on-the-fly prepare requests, so flow control here can be disabled
    /*if (_write_tasks.at(_write_task_itr) != nullptr)
    {
        _write_tasks.at(_write_task_itr)->wait();
    }

    _write_tasks.at(_write_task_itr) = task;
    _write_task_itr = (_write_task_itr < static_cast<int>_write_tasks.size() - 1) ? _write_task_itr + 1 : 0;*/

    return task;
}

int log_file::read_header(message_ptr& reader)
{
    
    reader->reader().read_pod(_header);

    int count;
    reader->reader().read(count);
    for (int i = 0; i < count; i++)
    {
        global_partition_id gpid;
        decree decree;
        
        reader->reader().read_pod(gpid);
        reader->reader().read(decree);

        _init_prepared_decrees[gpid] = decree;
    }

    return static_cast<int>(
        sizeof(_header) + sizeof(count) 
        + (sizeof(global_partition_id) + sizeof(decree))*count
        );
}

int log_file::write_header(message_ptr& writer, multi_partition_decrees& initMaxDecrees, int bufferSizeBytes)
{
    _init_prepared_decrees = initMaxDecrees;
    
    _header.magic = 0xdeadbeef;
    _header.version = 0x1;
    _header.start_global_offset = start_offset();
    _header.log_buffer_size_bytes = bufferSizeBytes;
    // staleness set in ctor

    writer->writer().write_pod(_header);
    
    int count = static_cast<int>(_init_prepared_decrees.size());
    writer->writer().write(count);
    for (auto it = _init_prepared_decrees.begin(); it != _init_prepared_decrees.end(); it++)
    {
        writer->writer().write_pod(it->first);
        writer->writer().write(it->second);
    }

    return static_cast<int>(
        sizeof(_header)+sizeof(count)
        +(sizeof(global_partition_id)+sizeof(decree))*count
        );
}

}} // end namespace
