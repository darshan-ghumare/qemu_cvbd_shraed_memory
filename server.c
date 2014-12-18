

/*******select.c*********/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <pthread.h>
#include <map>
#include <sstream>
#include "SharedMemServer.h"
#include "SharedMemShared.h"

#define CVBD_BLKSIZE (4096)
#define BLOCK_NUMBER(off) (off/CVBD_BLKSIZE)

using namespace serverinterface;


struct VolumeState
{
    std::string volume_name_;
    vdiskid_t volume_id_;
    int fd_;
    //FileData *file_data;
    std::unique_ptr<ipc::message_queue> reply_mq_;

    VolumeState(std::string& volume_name, vdiskid_t volume_id, int fd, const char *reply_mq_name):
        volume_name_(volume_name),
        volume_id_(volume_id),
        fd_(fd)
    {
        reply_mq_.reset(new ipc::message_queue(ipc::open_only,
                    reply_mq_name));
    }
};

struct IOHandler
{

public:

    IOHandler() : count(0)
    {
    }


    void open(OpenRequest *req, OpenReply *reply)
    {
        int ret = 0;
        ipc::message_queue *reply_mq = NULL;
        VolumeState *vol = NULL;
        std::string volume_name(req->volume_name);


        do
        {
            stringstream str;

            str << "/ssdfs/vdisk0/" << volume_name;

            ret = ::open(str.str().c_str(), O_RDWR);

            if (ret < 0)
            {
                assert(errno == ENOENT);
                ret = ::open(str.str().c_str(), O_RDWR | O_CREAT, 0777);

                if (ret < 0)
                {
                    ret = -errno;
                    break;
                }
            }

            vol = new VolumeState(volume_name, 0, ret, req->reply_mq_name);
            reply_mq = vol->reply_mq_.get();

            if (ftruncate(ret, (1ULL << 34)) < 0)
            {
                LOG_ERROR("ftruncate failed. volume_name = %s err = %d\n", req->volume_name, errno);
                ret = -errno;
                break;
            }

        } while (0);

        reply->header = req->header;
        reply->volume = reply->header.volume = (uint64_t) vol;
        reply->err = (ret < 0)? ret: 0;

        if (reply_mq)
        {
            reply_mq->send(reply,
                    openreply_size,
                    0);
        }

        if (ret < 0)
        {
            close(vol);
        }
    }

    void close(CloseRequest *req)
    {
        close((VolumeState *) req->header.volume);
    }

    void close(VolumeState *volume)
    {
        if (not volume) return;

        ::close(volume->fd_);
        delete volume;
    }

    void close(vdiskid_t& volume_id)
    {
        int fd = 0;
        VolumeState *vol = find(volume_id);

        if (vol)
        {
            pthread_rwlock_wrlock(&volumeStateMapMutex);
            fd = vol->fd_;
            volumeNameMap.erase(vol->volume_name_);
            volumeStateMap.erase(vol->volume_id_); //This will also release memory of the entry (volume state).
            pthread_rwlock_unlock(&volumeStateMapMutex);

            ::close(fd);
        }
    }

    void getsize(GetSizeRequest *req, GetSizeReply *reply)
    {
        ssize_t ret = 0;
        VolumeState *volState = (VolumeState *) req->header.volume;
        struct stat buf;

        do
        {
            if (not volState)
            {
                ret = -ENOENT;
                break;
            }

            ret = fstat(volState->fd_, &buf);
        } while (0);

        reply->header = req->header;
        reply->size_in_bytes = (ret < 0)? 0: buf.st_size;

        volState->reply_mq_->send(reply,
                getsizereply_size,
                0);
    }

    void fsync(FsyncRequest *req, FsyncReply *reply)
    {
        ssize_t ret = 0;
        VolumeState *volState = (VolumeState *) req->header.volume;

        do
        {
            if (not volState)
            {
                ret = -ENOENT;
                break;
            }

            ret = ::fsync(volState->fd_);
        } while (0);

        reply->header = req->header;
        reply->err = (ret < 0)? -errno: 0;

        volState->reply_mq_->send(reply,
                getsizereply_size,
                0);
    }

    void write(WriteRequest *req)
    {
        VolumeState *volState = (VolumeState *) req->header.volume;

        if (volState)
        {
            ssize_t dataSize = pwrite(volState->fd_, req->data, req->size_in_bytes, req->offset);
            assert(dataSize == req->size_in_bytes);
        }
        else
        {
            LOG_ERROR("Volume not found volume_if = %lu", req->header.volume);
        }

    }

    void read(ReadRequest *req, ReadReply *reply)
    {
        VolumeState *volState = (VolumeState *) req->header.volume;
        ssize_t dataSize = 0;

        do
        {
            if (not volState)
            {
                //LOG_ERROR("Volume not found volume_if = %d", req->header.volume_id);
                LOG_ERROR("Volume not found volume_if = %lu", req->header.volume);
                break;
            }
            
            dataSize = pread(volState->fd_, reply->data, req->size_in_bytes, req->offset);
            assert(dataSize == req->size_in_bytes);
        } while (0);

        reply->header = req->header;
        //reply->header.volume_id = req->header.volume_id;
        reply->size_in_bytes = dataSize;

        volState->reply_mq_->send(reply,
                readreply_size,
                0);

    }

private:

    uint64_t count;
};

int main(int argc, char *argv[])
{

    IOHandler handler;
    SharedMemServer<IOHandler> server(handler, serverinterface::max_queue_num);

    server.handle_requests_thread_join();

	return 0;

}

