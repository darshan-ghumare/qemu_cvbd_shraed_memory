
#ifndef _SHARED_MEM_SHARED_H_
#define _SHARED_MEM_SHARED_H_

namespace serverinterface
{

const std::string vd_request_mq_name_format("##equest_mq_");
const std::string vd_reply_mq_name_format("##Reply_mq_");


const uint64_t max_queue_num = 1;

enum RequestType
{
    REQ_UNKNOW,
    REQ_READ,
    REQ_WRITE,
    REQ_OPEN,
    REQ_CLOSE,
    REQ_GETSIZE,
    REQ_FSYNC
};

struct Header
{
    bool stop = false;
    RequestType type;
    uint64_t id_ = 0;
    //vdiskid_t volume_id = 0;
    uint64_t volume = 0;
    uint64_t acb;

    Header()
    : stop(false)
    , type(REQ_UNKNOW)
    , id_(0)
    , volume(0)
    , acb(0)
    { }

    Header(bool _stop, RequestType _type, uint64_t _id_, uint64_t _volume, uint64_t _acb)
    {
        stop = _stop;
        type = _type;
        id_ = _id_;
        volume = _volume;
        acb = _acb;
    }

    void init(Header& _header)
    {
        stop = _header.stop;
        type = _header.type;
        id_ = _header.id_;
        volume = _header.volume;
        acb = _header.acb;
    }
};

const uint64_t max_queue_size = 32;
const uint64_t max_sectors_write = 128;

struct WriteRequest
{
    static const uint64_t max_data_size = 4096 * max_sectors_write;
    Header header;
    off_t offset = 0;
    uint64_t size_in_bytes = 0;
    uint8_t data[max_data_size];

    void init(off_t offset_, uint64_t size_in_bytes_)
    {
        offset = offset_;
        size_in_bytes = size_in_bytes_;
    }
};

static const uint64_t writerequest_size = sizeof(WriteRequest);

/*inline uint64_t
get_effective_size_in_bytes(const WriteRequest& wr)
{
    return sizeof(off_t) + sizeof(uint64_t) + wr.size_in_bytes;

}*/

struct ReadRequest
{
    static const uint64_t max_data_size = 4096 * max_sectors_write;
    Header header;
    off_t offset = 0;
    uint64_t size_in_bytes = 0;

    void init(off_t offset_, uint64_t size_in_bytes_)
    {
        offset = offset_;
        size_in_bytes = size_in_bytes_;
    }
};

static const uint64_t readrequest_size = sizeof(ReadRequest);

struct OpenRequest
{
     Header header;
     char volume_name[PATH_MAX];
     char reply_mq_name[PATH_MAX];

    void init(const char *volume_name_, const char *reply_mq_name_)
    {
        memcpy(volume_name, volume_name_, strlen(volume_name_));
        memcpy(reply_mq_name, volume_name_, strlen(volume_name_));
    }
};

static const uint64_t openrequest_size = sizeof(OpenRequest);

struct CloseRequest
{
     Header header;
};

static const uint64_t closerequest_size = sizeof(CloseRequest);

struct GetSizeRequest
{
     Header header;
};

static const uint64_t getsizerequest_size = sizeof(GetSizeRequest);

struct FsyncRequest
{
     Header header;
};

static const uint64_t fsyncrequest_size = sizeof(FsyncRequest);

union Request
{
    WriteRequest writerequest;
    ReadRequest readrequest;
    OpenRequest openrequest;
    CloseRequest closerequest;
    GetSizeRequest getsizerequest;
    FsyncRequest fsyncrequest;

    Request() {}
    ~Request() {}
};

static const uint64_t request_size = sizeof(Request);

struct ReadReply
{
    static const uint64_t max_data_size = 4096 * max_sectors_write;
    Header header;
    uint64_t size_in_bytes;
    uint8_t data[max_data_size];
};

static const uint64_t readreply_size = sizeof(ReadReply);

struct OpenReply
{
     Header header;
     //vdiskid_t volume_id;
     uint64_t volume;
     int err;
};

static const uint64_t openreply_size = sizeof(OpenReply);

struct GetSizeReply
{
     Header header;
     off_t size_in_bytes;
};

static const uint64_t getsizereply_size = sizeof(GetSizeReply);

struct FsyncReply
{
     Header header;
     int err;
};

static const uint64_t fsyncreply_size = sizeof(FsyncReply);

union Reply
{
    ReadReply readreply;
    OpenReply openreply;
    GetSizeReply getsizereply;
    FsyncReply fsyncreply;

    Reply() {}
    ~Reply() {}
};

static const uint64_t reply_size = sizeof(Reply);

}

#endif // _SHARED_MEM_SHARED_H_

