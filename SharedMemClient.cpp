#include "SharedMemClient.h"
#include <sstream>
#include "qemu/typedefs.h"

namespace serverinterface
{

SharedMemClient::SharedMemClient(const std::string& volume_name)
    : request_msg_(new Request())
    , reply_msg_(new Reply())
    , volume_name_(volume_name)
{
    stringstream str;

    //TODO:Use RR to select among request queues.
    assert(volume_name.length() > 0);
    size_t queue_num = volume_name.at(volume_name.length() - 1) % max_queue_num;
    str << queue_num << vd_request_mq_name_format;

    ipc::message_queue::remove(volume_name_.c_str());

    request_mq_.reset(new ipc::message_queue(ipc::open_only,
                            str.str().c_str()));
    reply_mq_.reset(new ipc::message_queue(ipc::create_only,
                volume_name.c_str(),
                max_queue_size,
                reply_size));

}

SharedMemClient::~SharedMemClient()
{
    request_msg_.reset();
    reply_msg_.reset();

    if(not ipc::message_queue::remove(volume_name_.c_str()))
    {
        fprintf(stderr, "Could not remove request queue, client still active?");
    }

}

int
SharedMemClient::send_open()
{
    OpenRequest *openrequest_msg = (OpenRequest *) request_msg_.get();
    OpenReply *openreply_msg = (OpenReply *) reply_msg_.get();

    openrequest_msg->header = Header(false, REQ_OPEN, ++id, volume_, 0);
    openrequest_msg->init(volume_name_.c_str(), volume_name_.c_str());

    request_mq_->send(openrequest_msg,
                      openrequest_size,
                      0);

    unsigned int priority;
    ipc::message_queue::size_type received_size;

    reply_mq_->receive(openreply_msg,
                        reply_size,
                        received_size,
                        priority);
    assert(openreply_msg->header.type == REQ_OPEN);

    if (openreply_msg->err == 0)
    {
        volume_ = openreply_msg->volume;
    }

    return openreply_msg->err;
}

void
SharedMemClient::send_close()
{
    CloseRequest *closerequest_msg = (CloseRequest *) request_msg_.get();

    closerequest_msg->header = Header(false, REQ_CLOSE, ++id, volume_, 0);

    request_mq_->send(closerequest_msg,
                      closerequest_size,
                      0);
}

off_t
SharedMemClient::send_getsize()
{
    GetSizeRequest *getsizerequest_msg = (GetSizeRequest *) request_msg_.get();
    GetSizeReply  *getsizereply_msg = (GetSizeReply *) reply_msg_.get();

    getsizerequest_msg->header = Header(false, REQ_GETSIZE, ++id, volume_, 0);

    request_mq_->send(getsizerequest_msg,
                      getsizerequest_size,
                      0);

    unsigned int priority;
    ipc::message_queue::size_type received_size;

    reply_mq_->receive(getsizereply_msg,
                        reply_size,
                        received_size,
                        priority);
    assert(getsizereply_msg->header.type == REQ_GETSIZE);

    return getsizereply_msg->size_in_bytes;
}

void
SharedMemClient::send_write(void* message,
                            const uint64_t size_in_bytes,
                            const off_t offset)
{
    WriteRequest *writerequest_msg = (WriteRequest *) request_msg_.get();

    assert(size_in_bytes <= WriteRequest::max_data_size);
    writerequest_msg->header = Header(false, REQ_WRITE, ++id, volume_, 0);
    writerequest_msg->init(offset, size_in_bytes);
    memcpy(writerequest_msg->data,
           message,
           size_in_bytes);

    request_mq_->send(writerequest_msg,
                      writerequest_size,
                      0);
}


void
SharedMemClient::send_read(void* message,
                           const uint64_t size_in_bytes,
                           const off_t offset)
{
    ReadRequest *readrequest_msg = (ReadRequest *) request_msg_.get();
    ReadReply *readreply_msg = (ReadReply *) reply_msg_.get();

    assert(size_in_bytes <= ReadReply::max_data_size);
    readrequest_msg->header = Header(false, REQ_READ, ++id, volume_, 0);
    readrequest_msg->init(offset, size_in_bytes);

    request_mq_->send(readrequest_msg,
                      readrequest_size,
                      0);

    unsigned int priority;
    ipc::message_queue::size_type received_size;

    reply_mq_->receive(readreply_msg,
                        reply_size,
                        received_size,
                        priority);
    assert(readreply_msg->header.type == REQ_READ);
    assert(size_in_bytes == readreply_msg->size_in_bytes);

    memcpy(message,
           readreply_msg->data,
           size_in_bytes);
}

}


SharedMemHandle
shared_mem_make_client(const char* volume_name)
{
    try
    {
        return new serverinterface::SharedMemClient(volume_name);
    }
    catch(std::exception& e)
    {
        return nullptr;
    }
    catch(...)
    {
        return nullptr;
    }

}

void
shared_mem_destroy_client(SharedMemHandle h)
{
    try
    {
        delete static_cast<serverinterface::SharedMemClient*>(h);
    }
    catch(std::exception& e)
    {
        return;
    }
    catch(...)
    {
        return;
    }
}

int
create_volume(SharedMemHandle h)
{
    try
    {

        int err = static_cast<serverinterface::SharedMemClient*>(h)->send_open();

        return err;
    }
    catch(std::exception& e)
    {
        return -errno;
    }
    catch(...)
    {
        return -errno;
    }
}

bool
release_volume(SharedMemHandle h)
{
    try
    {

        static_cast<serverinterface::SharedMemClient*>(h)->send_close();

        return true;
    }
    catch(std::exception& e)
    {
        return false;
    }
    catch(...)
    {
        return false;
    }
}

off_t
getsize_volume(SharedMemHandle h)
{
    try
    {

        off_t size_in_bytes = static_cast<serverinterface::SharedMemClient*>(h)->send_getsize();

        return size_in_bytes;
    }
    catch(std::exception& e)
    {
        return 0;
    }
    catch(...)
    {
        return 0;
    }
}

bool
shared_mem_write(SharedMemHandle h,
                      void* message,
                      const uint64_t size_in_bytes,
                      const off_t offset)
{
    try
    {

        static_cast<serverinterface::SharedMemClient*>(h)->send_write(message,
                                                                     size_in_bytes,
                                                                     offset);
        return true;
    }
    catch(std::exception& e)
    {
        return false;
    }
    catch(...)
    {
        return false;
    }
}

ool
shared_mem_read(SharedMemHandle h,
                      void* message,
                      const uint64_t size_in_bytes,
                      const off_t offset)
{
    try
    {

        static_cast<serverinterface::SharedMemClient*>(h)->send_read(message,
                                                                     size_in_bytes,
                                                                     offset);
        return true;
    }
    catch(std::exception& e)
    {
        return false;
    }
    catch(...)
    {
        return false;
    }
}

