
#ifndef _SHARED_MEM_CLIENT_H_
#define _SHARED_MEM_CLIENT_H_
#ifndef ONLY_C
#include <assert.h>
#include <limits.h>
#include <string>
#include <sstream>
#include <boost/interprocess/ipc/message_queue.hpp>
#include "/home/storagevisor/cl/cv.host/include/utils.h"
#include <boost/thread.hpp>
#include "boost/date_time/posix_time/posix_time_types.hpp"
#include "boost/variant.hpp"
#include "/home/storagevisor/qemu-ledis/block/SharedMemShared.h"
#include "SharedMemShared.h"
#include "qemu-common.h"
#include "cvbd.h"

struct _SharedMemStruct
{};


namespace serverinterface
{
namespace ipc = boost::interprocess;

class SharedMemClient final : public _SharedMemStruct
{
public:
    SharedMemClient(const std::string& volume_name);

    ~SharedMemClient();

    void
    handle_co_reply();

    int
    send_open();

    void
    send_close();

    off_t
    send_getsize();

    void
    send_write(void* message,
               const uint64_t size,
               const off_t offset);

    void
    send_read(void* message,
              const uint64_t size_in_bytes,
              const off_t offset);


private:

    template<typename T>
    static T*
    getStopRequest()
    {
        static std::unique_ptr<T> val(new T);
        val->stop = true;
        return val.get();
    }

    std::unique_ptr<ipc::message_queue> request_mq_;
    std::unique_ptr<ipc::message_queue> reply_mq_;

    std::unique_ptr<Request> request_msg_;
    std::unique_ptr<Reply> reply_msg_;

    const std::string volume_name_;
    vdiskid_t volume_id_;
    uint64_t volume_;

};


}
#endif // ONLY_C
#ifdef __cplusplus
extern "C"
{
#endif

    struct _SharedMemStruct;

    typedef struct _SharedMemStruct* SharedMemHandle;

    SharedMemHandle
    shared_mem_make_client(const char* volumename);

    void
    shared_mem_destroy_client(SharedMemHandle h);

    int
    create_volume(SharedMemHandle h);

    bool
    release_volume(SharedMemHandle h);

    off_t
    getsize_volume(SharedMemHandle h);

    bool
    shared_mem_write(SharedMemHandle h,
                          void* message,
                          const uint64_t size,
                          const off_t offset);

    bool
    shared_mem_read(SharedMemHandle h,
                         void* message,
                         const uint64_t size_in_bytes,
                         const off_t offset);

    bool
    shared_mem_fsync(SharedMemHandle h,
                     CVBDAIOCB *acb);

    uint64_t
    shared_mem_device_size_in_bytes(SharedMemHandle h);

#ifdef __cplusplus
}
#endif
#endif // _SHARED_MEM_CLIENT_H_

