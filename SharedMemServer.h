
#ifndef _SHARED_MEM_SERVER_H_
#define _SHARED_MEM_SERVER_H_
#include <assert.h>
#include <limits.h>
#include <time.h>
#include <string>
#include <sstream>
#include <boost/interprocess/ipc/message_queue.hpp>
#include "/home/storagevisor/cl/cv.host/include/utils.h"
#include <boost/thread.hpp>
#include "boost/date_time/posix_time/posix_time_types.hpp"
#include "boost/variant.hpp"
#include "/home/storagevisor/qemu-ledis/block/SharedMemShared.h"

#define VERIFY(X) assert((X))
#define LOG_ERROR(msg, ...) do { \
    fprintf(stderr, "%s:%s():L%d: " msg "\n", \
    __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__); \
    } while (0)

#define LOG_INFO(msg, ...) do { \
    fprintf(stderr, "%s:%s():L%d: " msg "\n", \
    __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__); \
    } while (0)

#define CVBD_REQ_FLUSH_TIMEOUT 1 /* seconds. */

namespace serverinterface
{
namespace ipc = boost::interprocess;

template <typename Handler>
class SharedMemServer
{
public:
    SharedMemServer(Handler& handler, const size_t num_mq): handler(handler)
    {
        // VERIFY(not ipc::message_queue::remove(write_mq_uuid_.str().c_str()));

        for (size_t i = 0; i < num_mq; i++)
        {
            stringstream str;

            str << i <<  vd_request_mq_name_format;

            ipc::message_queue::remove(str.str().c_str());

            request_mqs_.push_back(std::unique_ptr<ipc::message_queue>(new ipc::message_queue(ipc::create_only,
                        str.str().c_str(),
                        max_queue_size,
                        request_size)));
        }

        for (size_t i = 0; i < num_mq; i++)
        {
            request_threads_.push_back(std::unique_ptr<boost::thread>(new boost::thread(&SharedMemServer::handle_requests,
                            this,
                            i))) ;
        }

    }

    ~SharedMemServer()
    {
        // How does this handle -- thread in creation -- thread that has exited??
        // Can we have denial of service attacks here?? client keeps reading writing effectively
        // blocking out this stop?
        for (vector<std::unique_ptr<ipc::message_queue>>::iterator itr = request_mqs_.begin(); itr != request_mqs_.end(); itr++)
        {
            if(not (*itr)->try_send(getStopRequest<Header>(),
                        request_size,
                        0))
            {
                LOG_INFO("Could not send messages to request queue, is the client still active??");

            }

            (*itr).reset();

            stringstream str;

            str << std::distance(request_mqs_.begin(), itr) <<  vd_request_mq_name_format;

            if(not ipc::message_queue::remove(str.str().c_str()))
            {
                LOG_INFO("Could not remove request queue, client still active?");
            }
        }

        for (vector<std::unique_ptr<boost::thread>>::iterator itr = request_threads_.begin(); itr != request_threads_.end(); itr++)
        {
            (*itr)->join();
            (*itr).reset();
        }

    }

    void handle_requests_thread_join()
    {
        for (vector<std::unique_ptr<boost::thread>>::iterator itr = request_threads_.begin(); itr != request_threads_.end(); itr++)
        {
            (*itr)->join();
        }
    }

private:

    template<typename T>
    static T*
    getStopRequest()
    {
        static std::unique_ptr<T> val(new T);
        val->stop = true;
        return val.get();
    }

    void
    handle_requests(size_t index)
    {
        unsigned int priority;
        std::unique_ptr<Request> request_msg(new Request());
        std::unique_ptr<Reply> reply_msg(new Reply());
        ipc::message_queue::size_type received_size;
        const Header *header = (const Header *) (request_msg.get());

        while(true)
        {
            request_mqs_.at(index)->receive(request_msg.get(),
                    request_size,
                    received_size,
                    priority);

            if(header->stop)
            {
                return;
            }

            if (header->type == REQ_WRITE)
            {
                handler.write((WriteRequest *) (request_msg.get()));
            }
            else if (header->type == REQ_READ)
            {
                handler.read((ReadRequest *) (request_msg.get()),
                        (ReadReply *) (reply_msg.get()));
            }
            else if (header->type == REQ_OPEN)
            {
                handler.open((OpenRequest *) (request_msg.get()),
                        (OpenReply *) (reply_msg.get()));
            }
            else if (header->type == REQ_CLOSE)
            {
                handler.close((CloseRequest *) (request_msg.get()));
            }
            else if (header->type == REQ_GETSIZE)
            {
                handler.getsize((GetSizeRequest *) (request_msg.get()),
                        (GetSizeReply *) (reply_msg.get()));
            }
            else if (header->type == REQ_FSYNC)
            {
                handler.fsync((FsyncRequest *) (request_msg.get()),
                        (FsyncReply *) (reply_msg.get()));
            }
            else
            {
                fprintf(stderr, "%s(): ERROR: bad request type= %d\n", __func__, header->type);
            }
        }
    }



    // Put these in a separate class?
    std::vector<std::unique_ptr<ipc::message_queue>> request_mqs_;

    std::vector<std::unique_ptr<boost::thread>> request_threads_;

    Handler& handler;
};


}
#endif // _SHARED_MEM_SERVER_H_

