/*
 * QEMU Block driver for CV
 *
 * Copyright (C) 2014 
 *     Author: Sandeep Joshi
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/uri.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qint.h"
#include "qapi/qmp/qstring.h"
#include "qemu/iov.h"
#include "cv_qemu_proto.h"
#include "cvbd.h"

//#include "qemu/crc32c.h"

#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/limits.h>
/*#include <boost/interprocess/ipc/message_queue.hpp>
#include <iostream>
#include <vector>

using namespace boost::interprocess;*/


#define TRACE(msg, ...) do { \
    LOG(msg, ## __VA_ARGS__); \
    } while (0)

#define LOG(msg, ...) do { \
    fprintf(stderr, "%s:%s():L%d: %lu " msg "\n", \
    __FILE__, __FUNCTION__, __LINE__, count++, ## __VA_ARGS__); \
    } while (0)


#define SECTOR_SIZE (512)
#define CVBD_BLKSIZE (4096)
#define BLOCK_NUMBER(off) (off/CVBD_BLKSIZE)

#define MSGQ_MAX_SIZE (4096 * 128)

#define ONLY_C
#include "block/SharedMemClient.h"
#include "NonAlignedCopy.h"

typedef struct CVBDState {
	int socket;
    SharedMemHandle sharedMemHandle;
	int64_t missingReads;
	int64_t reads;
	int64_t writes;
    int64_t readModifyWrites;
} CVBDState;

uint64_t count = 0;

static int cvbd_parse_uri(const char *filename, QDict *dict)
{
    URI *uri;
    const char *p;
    int ret = 0;

    uri = uri_parse(filename);
    if (!uri) {
        return -EINVAL;
    }

    // transport 
    if (strcmp(uri->scheme, "cvbd") != 0) {
        ret = -EINVAL;
        goto out;
    }

    //LOG("query: %s path: %s\n", uri->query, uri->path);

    p = uri->path ? uri->path : "/";
    p += strspn(p, "/");
    if (p[0]) {
        qdict_put(dict, "export", qstring_from_str(p));
    }

out:
    return ret;
}

static void cvbd_parse_filename(const char *filename, QDict *dict,
                               Error **errp)
{
    int ret = -EINVAL;

    if (strstr(filename, "://")) {
        ret = cvbd_parse_uri(filename, dict);
    }

    if (ret < 0) {
        error_setg(errp, "No valid URL specified");
    }
}

static bool is_request_aligned(int64_t sector_num, int nb_sectors)
{
    if ((sector_num * BDRV_SECTOR_SIZE) % CVBD_BLKSIZE ||
            (nb_sectors * BDRV_SECTOR_SIZE) % CVBD_BLKSIZE)
    {
        error_report("CVBD misaligned request: "
                "block_size %u, sector_num %" PRIi64
                ", nb_sectors %d",
                CVBD_BLKSIZE, sector_num, nb_sectors);
        return 0;
    }
    return 1;
}

static QemuOptsList runtime_opts = {
    .name = "cvbd",  
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = "filename",
            .type = QEMU_OPT_STRING,
            .help = "CVBD image name", 
        },
        { /* end of list */ }
    },
};

static void cvbd_config(CVBDState* s, QDict* dict, Error** errp)
{
    Error* local_err = NULL;

    QemuOpts* opts = qemu_opts_create(&runtime_opts, NULL, 0, &error_abort);

    qemu_opts_absorb_qdict(opts, dict, &local_err);

    qemu_opts_del(opts);

    if (qdict_get_try_str(dict, "export")) {
        qdict_del(dict, "export");
    }
}

static int cvbd_open(BlockDriverState *bs, QDict *dict, int flags,
                    Error **errp)
{
    CVBDState *s =  (CVBDState *) bs->opaque;
    const char *export = qdict_get_try_str(bs->options, "export");
    int ret = 0;

    LOG("export: %s\n", (export)? export: "<null>");

    s->reads = s->writes = s->missingReads = s->readModifyWrites = 0;

    // Align IO.
    bs->request_alignment = CVBD_BLKSIZE;

    do
    {
        s->sharedMemHandle = shared_mem_make_client(export);

        if (s->sharedMemHandle == NULL)
        {
            LOG("Failed to create shared memory queue for volume = %s\n", export);
            ret = -EINVAL;
            break;
        }

        ret = create_volume(s->sharedMemHandle);

        if (ret != 0)
        {
            LOG("Failed to create volume = %s, err = %d\n", export, ret);
            break;
        }
    } while (0);

    cvbd_config(s, dict, errp);

    //LOG("opened connection with ret=%d\n", ret);
    return ret;
}

// ============================

static int cvbd_read(BlockDriverState *bs, int64_t sector_num, uint8_t *buffer, int nb_sectors)
{
    CVBDState *s =  (CVBDState *) bs->opaque;
    bool err = true;
    ssize_t size =  (int64_t) nb_sectors * BDRV_SECTOR_SIZE;
    off_t offset = (off_t) sector_num * BDRV_SECTOR_SIZE;

    if (!is_request_aligned(sector_num, nb_sectors))
    {
        return -EINVAL;
    }

    NonAlignedCopy a;
    NonAlignedCopyInit(&a, offset, size, MSGQ_MAX_SIZE);
   // LOG("READ secNum: %ld numSec: %d off: %ld size: %d", sector_num, nb_sectors, sector_num * BDRV_SECTOR_SIZE, nb_sectors * BDRV_SESCTOR_SIZE);

    while (NonAlignedCopyIsValid(&a))
    {
        ssize_t retOff = 0;
        ssize_t retSz = 0;
        NonAlignedCopyNext(&a, &retOff, &retSz);

        ssize_t relativeOff = retOff - (sector_num * BDRV_SECTOR_SIZE);
        assert(relativeOff < nb_sectors * BDRV_SECTOR_SIZE);
        const uint8_t* curBuf = buffer + relativeOff;

        const uint8_t* actualBuf = curBuf;

        err = shared_mem_read(s->sharedMemHandle, (void *) actualBuf, retSz, retOff);

        if (err != true)
        {
            assert(0);
            break;
        }
    }

	return (err == true)? 0: -EIO;
}

static int cvbd_write(BlockDriverState *bs, int64_t sector_num, const uint8_t *buffer, int nb_sectors)
{
    CVBDState *s =  (CVBDState *) bs->opaque;
    bool err = true;
    ssize_t size =  (int64_t) nb_sectors * BDRV_SECTOR_SIZE;
    off_t offset = (off_t) sector_num * BDRV_SECTOR_SIZE;

    if (!is_request_aligned(sector_num, nb_sectors))
    {
        return -EINVAL;
    }

    NonAlignedCopy a;
    NonAlignedCopyInit(&a, offset, size, MSGQ_MAX_SIZE);

    while (NonAlignedCopyIsValid(&a))
    {
        ssize_t retOff = 0;
        ssize_t retSz = 0;
        NonAlignedCopyNext(&a, &retOff, &retSz);

        ssize_t relativeOff = retOff - (sector_num * BDRV_SECTOR_SIZE);
        assert(relativeOff < nb_sectors * BDRV_SECTOR_SIZE);
        const uint8_t* curBuf = buffer + relativeOff;

        const uint8_t* actualBuf = curBuf;

        err = shared_mem_write(s->sharedMemHandle, (void *) actualBuf, retSz, retOff);

        if (err != true)
        {
            assert(0);
            break;
        }
    }

    return (err == true)? 0: -EIO;
}

static void cvbd_close(BlockDriverState *bs)
{
    CVBDState *s =  (CVBDState *) bs->opaque;

    //close(s->socket);
    release_volume(s->sharedMemHandle);
    shared_mem_destroy_client(s->sharedMemHandle);
    LOG("closed connection \n");
}

static int64_t cvbd_getlength(BlockDriverState *bs)
{
    CVBDState *s =  (CVBDState *) bs->opaque;
    uint64_t ret = getsize_volume(s->sharedMemHandle);

    return  ret;
}

static void cvbd_refresh_filename(BlockDriverState *bs)
{
    CVBDState *s =  (CVBDState *) bs->opaque;
    (void) s;

}

static BlockDriver bdrv_cvbd = {
    .format_name                = "cvbd",
    .protocol_name              = "cvbd",
    .instance_size              = sizeof(CVBDState),
    //.bdrv_needs_filename        = true,
    .bdrv_parse_filename        = cvbd_parse_filename,
    .bdrv_file_open             = cvbd_open,
    .bdrv_read                  = cvbd_read,
    .bdrv_write                 = cvbd_write,
    .bdrv_close                 = cvbd_close,
    //.bdrv_co_writev             = cvbd_co_writev,
    //.bdrv_co_readv              = cvbd_co_readv,
    //.bdrv_co_flush_to_disk         = cvbd_co_flush_to_disk,
    //.bdrv_co_flush_to_os        = cvbd_co_flush,
    //.bdrv_co_discard            = cvbd_co_discard,
    .bdrv_getlength             = cvbd_getlength,
    //.bdrv_detach_aio_context    = cvbd_detach_aio_context,
    //.bdrv_attach_aio_context    = cvbd_attach_aio_context,
    .bdrv_refresh_filename      = cvbd_refresh_filename,
    //.create_opts                = cvbd_create_opts,
};

static void bdrv_cvbd_init(void)
{
    bdrv_register(&bdrv_cvbd);
}

block_init(bdrv_cvbd_init);
