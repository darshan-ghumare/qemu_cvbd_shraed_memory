#include "/home/storagevisor/cl/cv.host/include/utils.h"
#include "SharedMemClient.h"
#include <sstream>
#include "qemu/typedefs.h"

namespace serverinterface
{
#define WAIT_TO_COMPLETE \
do { \
    pthread_mutex_lock(&io_complete_mutex_); \
    pthread_cond_wait(&io_complete_, &io_complete_mutex_); \
    pthread_mutex_unlock(&io_complete_mutex_); \
} while (0);

#define SIGNAL_COMPLETION \
do { \
    pthread_cond_signal(&io_complete_); \
} while (0);


int id = 0;

static size_t iov_from_buf(const struct iovec *iov, unsigned int iov_cnt,
                    size_t offset, const void *buf, size_t bytes)
{
    size_t done;
    unsigned int i;
    for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            size_t len = MIN(iov[i].iov_len - offset, bytes - done);
            memcpy((char *) iov[i].iov_base + offset, (char *) buf + done, len);
            done += len;
            offset = 0;
        } else {
            offset -= iov[i].iov_len;
        }
    }
    assert(offset == 0);
    return done;
}

size_t iov_to_buf(const struct iovec *iov, const unsigned int iov_cnt,
                  size_t offset, void *buf, size_t bytes)
{
    size_t done;
    unsigned int i;
    for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
        if (offset < iov[i].iov_len) {
            size_t len = MIN(iov[i].iov_len - offset, bytes - done);
            memcpy((char *) buf + done, (char *) iov[i].iov_base + offset, len);
            done += len;
            offset = 0;
        } else {
            offset -= iov[i].iov_len;
        }
    }
    assert(offset == 0);
    return done;
}

SharedMemClient::SharedMemClient(const std::string& volume_name)
    : request_msg_(new Request())
    , reply_msg_(new Reply())
    , volume_name_(volume_name)
{
    stringstream str;

    //TODO:Use RR to select among request queues.
    assert(volume_name.length() > 0);
    size_t queue_num = volume_name.at(volume_name.length() - 1) % max_queue_num;
    std::cerr << "queue_num: " << queue_num << std::endl;
    std::cerr << "max_queue_size: " << max_queue_num << std::endl;
    std::cerr << "char: " << (int) volume_name.at(volume_name.length() - 1)<< std::endl;
    str << queue_num << vd_request_mq_name_format;

    ipc::message_queue::remove(volume_name_.c_str());

    request_mq_.reset(new ipc::message_queue(ipc::open_only,
                            str.str().c_str()));
    reply_mq_.reset(new ipc::message_queue(ipc::create_only,
                volume_name.c_str(),
                max_queue_size,
                reply_size));

    co_reply_thread_.reset(new boost::thread(boost::bind(
                        &SharedMemClient::handle_co_reply,
                        this)));

    pthread_cond_init(&io_complete_, NULL);
    pthread_mutex_init(&io_complete_mutex_, NULL);
}

SharedMemClient::~SharedMemClient()
{
    request_msg_.reset();
    reply_msg_.reset();

    if(not ipc::message_queue::remove(volume_name_.c_str()))
    {
        fprintf(stderr, "Could not remove request queue, client still active?");
    }

    if(not reply_mq_->try_send(getStopRequest<Header>(),
                reply_size,
                0))
    {
        fprintf(stderr, "Could not send messages to reply queue, is the client still active??");
    }
}

void
SharedMemClient::handle_co_reply()
{
    unsigned int priority;
    int ret = 0;
    ipc::message_queue::size_type received_size;
    CVBDAIOCB *acb = NULL;
    //std::unique_ptr<Reply> reply_msg(new Reply());
    Header *header = (Header *) (reply_msg_.get());

    while (true)
    {
        reply_mq_->receive(reply_msg_.get(),
                reply_size,
                received_size,
                priority);
        //assert(received_size == get_effective_size_in_bytes(*readreply_msg_));

        if(header->stop)
        {
            return;
        }

        acb = (CVBDAIOCB *) header->acb;

        if (header->type == REQ_READ)
        {
            ReadReply *reply = (ReadReply *) header;

            assert(acb);

            iov_from_buf(acb->qiov->iov, acb->qiov->niov,
                    0, reply->data, reply->size_in_bytes);

            ret = 0;
            acb->ret = reply->size_in_bytes;

        }
        else if (header->type == REQ_FSYNC)
        {
            FsyncReply *reply = (FsyncReply *) header;

            assert(acb);

            ret = acb->ret = reply->err;
        }
        else if (header->type == REQ_OPEN
                 || header->type == REQ_GETSIZE)
        {
            SIGNAL_COMPLETION;
            continue;
        }
        else
        {
            fprintf(stderr, "%s(): Invalid reqeust type: %d\n", __func__, header->type);
            assert(0);
        }

        assert(acb->callback);
        acb->callback(ret, acb);
    }
}

int
SharedMemClient::send_open()
{
    OpenRequest *openrequest_msg = (OpenRequest *) request_msg_.get();
    OpenReply *openreply_msg = (OpenReply *) reply_msg_.get();

    openrequest_msg->header = Header(false, REQ_OPEN, ++id, volume_, 0);
    //openrequest_msg->header.volume_id = volume_id_;
    openrequest_msg->init(volume_name_.c_str(), volume_name_.c_str());

    request_mq_->send(openrequest_msg,
                      openrequest_size,
                      0);

    WAIT_TO_COMPLETE;
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
    //closerequest_msg->header.volume_id = volume_id_;

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
    //getsizerequest_msg->header.volume_id = volume_id_;

    request_mq_->send(getsizerequest_msg,
                      getsizerequest_size,
                      0);

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
    //writerequest_msg->header.volume_id = volume_id_;
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
    //readrequest_msg->header.volume_id = volume_id_;
    readrequest_msg->init(offset, size_in_bytes);

    //fprintf(stderr, "%s(): id: %d\n", __func__, id);

    request_mq_->send(readrequest_msg,
                      readrequest_size,
                      0);

    unsigned int priority;
    ipc::message_queue::size_type received_size;

    reply_mq_->receive(readreply_msg,
                        reply_size,
                        received_size,
                        priority);
    //assert(received_size == get_effective_size_in_bytes(*readreply_msg_));
    assert(readreply_msg->header.type == REQ_READ);
    assert(size_in_bytes == readreply_msg->size_in_bytes);

    memcpy(message,
           readreply_msg->data,
           size_in_bytes);
}

void
SharedMemClient::send_readv(const off_t offset,
                           CVBDAIOCB *acb)
{
    ReadRequest *readrequest_msg = (ReadRequest *) request_msg_.get();

    assert(acb->size <= ReadReply::max_data_size);
    readrequest_msg->header = Header(false, REQ_READ, ++id, volume_, (uint64_t) acb);
    //readrequest_msg->header.volume_id = volume_id_;
    readrequest_msg->init(offset, acb->size);

    request_mq_->send(readrequest_msg,
                      readrequest_size,
                      0);
}

void
SharedMemClient::send_writev(const off_t offset,
                             CVBDAIOCB *acb)
{
    WriteRequest *writerequest_msg = (WriteRequest *) request_msg_.get();

    printf("%s() size %lu\n", __func__, acb->size);
    assert(acb->size <= WriteRequest::max_data_size);
    writerequest_msg->header = Header(false, REQ_WRITE, ++id, volume_, (uint64_t) acb);
    //writerequest_msg->header.volume_id = volume_id_;
    writerequest_msg->init(offset, acb->size);
    iov_to_buf(acb->qiov->iov, acb->qiov->niov,
                 0, writerequest_msg->data, writerequest_msg->size_in_bytes);
    assert(acb->size == writerequest_msg->size_in_bytes);

    request_mq_->send(writerequest_msg,
                      writerequest_size,
                      0);

    /* Donot wait for write ack. */
    assert(acb->callback);
    acb->ret = acb->size;
    acb->callback(0, acb);
}

void
SharedMemClient::send_fsync(CVBDAIOCB *acb)
{
    FsyncRequest *fsyncrequest_msg = (FsyncRequest *) request_msg_.get();

    fsyncrequest_msg->header = Header(false, REQ_FSYNC, ++id, volume_, (uint64_t) acb);

    request_mq_->send(fsyncrequest_msg,
                      fsyncrequest_size,
                      0);
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

bool
shared_mem_writev(SharedMemHandle h,
                      const off_t offset,
                      CVBDAIOCB *acb)
{
    try
    {

        static_cast<serverinterface::SharedMemClient*>(h)->send_writev(offset,
                                                                       acb);
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

bool
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

bool
shared_mem_readv(SharedMemHandle h,
                      const off_t offset,
                      CVBDAIOCB *acb)
{
    try
    {

        static_cast<serverinterface::SharedMemClient*>(h)->send_readv(offset,
                                                                       acb);
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

bool
shared_mem_fsync(SharedMemHandle h,
                      CVBDAIOCB *acb)
{
    try
    {

        static_cast<serverinterface::SharedMemClient*>(h)->send_fsync(acb);
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

/*uint64_t
shared_mem_device_size_in_bytes(SharedMemHandle h)
{
    assert(h);
    try
    {
        return static_cast<serverinterface::SharedMemClient*>(h)->device_size_in_bytes();
    }
    catch(std::exception& e)
    {
        return 0;
    }
    catch(...)
    {
        return 0;
    }
}*/

