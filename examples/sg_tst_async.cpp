/*
 * Copyright (c) 2014 Douglas Gilbert.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <iostream>
#include <vector>
#include <map>
#include <list>
#include <system_error>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <random>

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "sg_lib.h"
#include "sg_io_linux.h"

static const char * version_str = "1.05 20140819";
static const char * util_name = "sg_tst_async";

/* This is a test program for checking the async usage of the Linux sg
 * driver. Each thread opens 1 file descriptor to the next sg device (1
 * or more can be given on the command line) and then starts up to 16
 * commands while checking with the poll command (or
 * ioctl(SG_GET_NUM_WAITING) ) for the completion of those commands. Each
 * command has a unique "pack_id" which is a sequence starting at 1.
 * Either TEST UNIT UNIT, READ(16) or WRITE(16) commands are issued.
 *
 * This is C++ code with some things from C++11 (e.g. threads) and was
 * only just able to compile (when some things were reverted) with gcc/g++
 * version 4.7.3 found in Ubuntu 13.04 . C++11 "feature complete" support
 * was not available until g++ version 4.8.1 . It should build okay on
 * recent distributions.
 *
 * The build uses various object files from the <sg3_utils>/lib directory
 * which is assumed to be a sibling of this examples directory. Those
 * object files in the lib directory can be built with:
 *   cd <sg3_utils_package_root> ; ./configure ; cd lib; make
 *   cd ../examples
 * Then to build sg_tst_async concatenate the next 3 lines:
 *   g++ -Wall -std=c++11 -pthread -I ../include ../lib/sg_lib.o
 *     ../lib/sg_lib_data.o ../lib/sg_io_linux.o -o sg_tst_async
 *     sg_tst_async.cpp
 * or use the C++ Makefile in that directory:
 *   make -f Makefile.cplus sg_tst_async
 *
 * Currently this utility is Linux only and uses the sg driver. The bsg
 * driver is known to be broken (it doesn't match responses to the
 * correct file descriptor that requested them) so this utility won't
 * be extended to bsg until that is fixed.
 *
 * BEWARE: >>> This utility will modify a logical block (default LBA 1000)
 * on the given device when the '-W' option is given.
 *
 */

using namespace std;
using namespace std::chrono;

#define DEF_NUM_PER_THREAD 1000
#define DEF_NUM_THREADS 4
#define DEF_WAIT_MS 10          /* 0: yield or no wait */
#define DEF_TIMEOUT_MS 20000    /* 20 seconds */
#define DEF_LB_SZ 512
#define DEF_BLOCKING 0
#define DEF_DIRECT 0
#define DEF_NO_XFER 0
#define DEF_LBA 1000

#define MAX_Q_PER_FD 16     /* sg driver per file descriptor limit */
#define MAX_CONSEC_NOMEMS 16
#define URANDOM_DEV "/dev/urandom"

#ifndef SG_FLAG_Q_AT_TAIL
#define SG_FLAG_Q_AT_TAIL 0x10
#endif
#ifndef SG_FLAG_Q_AT_HEAD
#define SG_FLAG_Q_AT_HEAD 0x20
#endif



#define EBUFF_SZ 256

static mutex console_mutex;
static mutex rand_lba_mutex;
static atomic<int> async_starts(0);
static atomic<int> async_finishes(0);
static atomic<int> ebusy_count(0);
static atomic<int> eagain_count(0);
static atomic<int> uniq_pack_id(1);

static int page_size = 4096;   /* rough guess, will ask sysconf() */

enum command2execute {SCSI_TUR, SCSI_READ16, SCSI_WRITE16};
enum blkQDiscipline {BQ_DEFAULT, BQ_AT_HEAD, BQ_AT_TAIL};
enum myQDiscipline {MQD_LOW, MQD_MEDIUM, MQD_HIGH};

struct opts_t {
    vector<const char *> dev_names;
    bool direct;
    int maxq_per_thread;
    int num_per_thread;
    bool block;
    uint64_t lba;
    unsigned int hi_lba;        /* last one, inclusive range */
    vector<unsigned int> hi_lbas; /* only used when hi_lba=-1 */
    int lb_sz;
    bool no_xfer;
    int verbose;
    int wait_ms;
    command2execute c2e;
    blkQDiscipline bqd;
    myQDiscipline mqd;
};

#if 0
class Rand_uint {
public:
    Rand_uint(unsigned int lo, unsigned int hi) : p{lo, hi} {}
    unsigned int operator()() const { return r(); }
private:
    uniform_int_distribution<unsigned int>::param_type p;
    auto r = bind(uniform_int_distribution<unsigned int>{p},
                  default_random_engine());
    /* compiler thinks auto should be a static, bs again? */
};
#endif

#if 0
class Rand_uint {
public:
    Rand_uint(unsigned int lo, unsigned int hi, unsigned int my_seed)
        : r(bind(uniform_int_distribution<unsigned int>{lo, hi},
                 default_random_engine())) { r.seed(myseed); }
    unsigned int operator()() const { return r(); }
private:
    function<unsigned int()> r;
};
#endif

class Rand_uint {
public:
    Rand_uint(unsigned int lo, unsigned int hi, unsigned int my_seed)
        : uid(lo, hi), dre(my_seed) { }
    unsigned int get() { return uid(dre); }
private:
    uniform_int_distribution<unsigned int> uid;
    default_random_engine dre;
};


static void
usage(void)
{
    printf("Usage: %s [-d] [-f] [-h] [-l <lba+>] [-M <maxq_per_thr>]\n"
           "                    [-n <n_per_thr>] [-N] [-q 0|1] [-Q 0|1|2] "
           "[-R]\n"
           "                    [-s <lb_sz>] [-t <num_thrs>] [-T] [-v] "
           "[-V]\n"
           "                    [-w <wait_ms>] [-W] <sg_disk_device>*\n",
           util_name);
    printf("  where\n");
    printf("    -d                do direct_io (def: indirect)\n");
    printf("    -f                force: any sg device (def: only scsi_debug "
           "owned)\n");
    printf("                      WARNING: <lba> written to if '-W' given\n");
    printf("    -h                print this usage message then exit\n");
    printf("    -l <lba>          logical block to access (def: %u)\n",
           DEF_LBA);
    printf("    -l <lba,hi_lba>    logical block range (inclusive), if "
           "hi_lba=-1\n"
           "                       assume last block on device\n");
    printf("    -M <maxq_per_thr>    maximum commands queued per thread "
           "(def:%d)\n", MAX_Q_PER_FD);
    printf("    -n <n_per_thr>    number of commands per thread "
           "(def: %d)\n", DEF_NUM_PER_THREAD);
    printf("    -N                no data xfer (def: xfer on READ and "
           "WRITE)\n");
    printf("    -q 0|1            0: blk q_at_head; 1: q_at_tail\n");
    printf("    -Q 0|1|2          0: favour completions (smaller q), 1: "
           "medium,\n"
           "                      2: favour submissions (larger q, "
           "default)\n");
    printf("    -s <lb_sz>        logical block size (def: 512)\n");
    printf("    -R                do READs (def: TUR)\n");
    printf("    -t <num_thrs>     number of threads (def: %d)\n",
           DEF_NUM_THREADS);
    printf("    -T                do TEST UNIT READYs (default is TURs)\n");
    printf("    -v                increase verbosity\n");
    printf("    -V                print version number then exit\n");
    printf("    -w <wait_ms>      >0: poll(<wait_ms>); =0: poll(0); (def: "
           "%d)\n", DEF_WAIT_MS);
    printf("    -W                do WRITEs (def: TUR)\n\n");
    printf("Multiple threads send READ(16), WRITE(16) or TEST UNIT READY "
           "(TUR) SCSI\ncommands. There can be 1 or more <sg_disk_device>s "
           "and each thread takes\nthe next in a round robin fashion. "
           "Each thread queues up to 16 commands.\nOne block is transferred "
           "by each READ and WRITE; zeros are written. If a\nlogical block "
           "range is given, a uniform distribution generates a pseudo\n"
           "random sequence of LBAs.\n");
}

#ifdef __GNUC__
static int pr2serr_lk(const char * fmt, ...)
        __attribute__ ((format (printf, 1, 2)));
static void pr_errno_lk(int e_no, const char * fmt, ...)
        __attribute__ ((format (printf, 2, 3)));
#else
static int pr2serr_lk(const char * fmt, ...);
static void pr_errno_lk(int e_no, const char * fmt, ...);
#endif


static int
pr2serr_lk(const char * fmt, ...)
{
    int n;

    console_mutex.lock();
    {
        va_list args;

        va_start(args, fmt);
        n = vfprintf(stderr, fmt, args);
        va_end(args);
    }
    console_mutex.unlock();
    return n;
}

static void
pr_errno_lk(int e_no, const char * fmt, ...)
{
    char b[160];

    console_mutex.lock();
    {
    va_list args;

        va_start(args, fmt);
        vsnprintf(b, sizeof(b), fmt, args);
        fprintf(stderr, "%s: %s\n", b, strerror(e_no));
        va_end(args);
    }
    console_mutex.unlock();
}

static unsigned int
get_urandom_uint(void)
{
    unsigned int res = 0;
    int n;
    unsigned char b[sizeof(unsigned int)];
    int fd = open(URANDOM_DEV, O_RDONLY);

    if (fd >= 0) {
        /* assume this read is atomic */
        n = read(fd, b, sizeof(unsigned int));
        if (sizeof(unsigned int) == n)
            memcpy(&res, b, sizeof(unsigned int));
        close(fd);
    }
    return res;
}

#define TUR_CMD_LEN 6
#define READ16_REPLY_LEN 512
#define READ16_CMD_LEN 16
#define WRITE16_REPLY_LEN 512
#define WRITE16_CMD_LEN 16

/* Returns 0 if command injected okay, else -1 */
static int
start_sg3_cmd(int sg_fd, command2execute cmd2exe, int pack_id, uint64_t lba,
              unsigned char * lbp, int xfer_bytes, int flags)
{
    struct sg_io_hdr pt;
    unsigned char turCmdBlk[TUR_CMD_LEN] = {0, 0, 0, 0, 0, 0};
    unsigned char r16CmdBlk[READ16_CMD_LEN] =
                {0x88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
    unsigned char w16CmdBlk[WRITE16_CMD_LEN] =
                {0x8a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
    unsigned char sense_buffer[64];
    const char * np = NULL;

    memset(&pt, 0, sizeof(pt));
    switch (cmd2exe) {
    case SCSI_TUR:
        np = "TEST UNIT READY";
        pt.cmdp = turCmdBlk;
        pt.cmd_len = sizeof(turCmdBlk);
        pt.dxfer_direction = SG_DXFER_NONE;
        break;
    case SCSI_READ16:
        np = "READ(16)";
        if (lba > 0xffffffff) {
            r16CmdBlk[2] = (lba >> 56) & 0xff;
            r16CmdBlk[3] = (lba >> 48) & 0xff;
            r16CmdBlk[4] = (lba >> 40) & 0xff;
            r16CmdBlk[5] = (lba >> 32) & 0xff;
        }
        r16CmdBlk[6] = (lba >> 24) & 0xff;
        r16CmdBlk[7] = (lba >> 16) & 0xff;
        r16CmdBlk[8] = (lba >> 8) & 0xff;
        r16CmdBlk[9] = lba & 0xff;
        pt.cmdp = r16CmdBlk;
        pt.cmd_len = sizeof(r16CmdBlk);
        pt.dxfer_direction = SG_DXFER_FROM_DEV;
        pt.dxferp = lbp;
        pt.dxfer_len = xfer_bytes;
        break;
    case SCSI_WRITE16:
        np = "WRITE(16)";
        if (lba > 0xffffffff) {
            w16CmdBlk[2] = (lba >> 56) & 0xff;
            w16CmdBlk[3] = (lba >> 48) & 0xff;
            w16CmdBlk[4] = (lba >> 40) & 0xff;
            w16CmdBlk[5] = (lba >> 32) & 0xff;
        }
        w16CmdBlk[6] = (lba >> 24) & 0xff;
        w16CmdBlk[7] = (lba >> 16) & 0xff;
        w16CmdBlk[8] = (lba >> 8) & 0xff;
        w16CmdBlk[9] = lba & 0xff;
        pt.cmdp = w16CmdBlk;
        pt.cmd_len = sizeof(w16CmdBlk);
        pt.dxfer_direction = SG_DXFER_TO_DEV;
        pt.dxferp = lbp;
        pt.dxfer_len = xfer_bytes;
        break;
    }
    pt.interface_id = 'S';
    pt.mx_sb_len = sizeof(sense_buffer);
    pt.sbp = sense_buffer;      /* ignored .... */
    pt.timeout = DEF_TIMEOUT_MS;
    pt.pack_id = pack_id;
    pt.flags = flags;

    for (int k = 0; write(sg_fd, &pt, sizeof(pt)) < 0; ++k) {
        if ((ENOMEM == errno) && (k < MAX_CONSEC_NOMEMS)) {
            this_thread::yield();
            continue;
        }
        pr_errno_lk(errno, "%s: %s, pack_id=%d", __func__, np, pack_id);
        return -1;
    }
    return 0;
}

static int
finish_sg3_cmd(int sg_fd, command2execute cmd2exe, int & pack_id, int wait_ms,
               unsigned int & eagains)
{
    int ok, res;
    struct sg_io_hdr pt;
    unsigned char sense_buffer[64];
    const char * np = NULL;

    memset(&pt, 0, sizeof(pt));
    switch (cmd2exe) {
    case SCSI_TUR:
        np = "TEST UNIT READY";
        break;
    case SCSI_READ16:
        np = "READ(16)";
        break;
    case SCSI_WRITE16:
        np = "WRITE(16)";
        break;
    }
    pt.interface_id = 'S';
    pt.mx_sb_len = sizeof(sense_buffer);
    pt.sbp = sense_buffer;
    pt.timeout = DEF_TIMEOUT_MS;
    pt.pack_id = 0;

    while (((res = read(sg_fd, &pt, sizeof(pt))) < 0) &&
           (EAGAIN == errno)) {
        ++eagains;
        if (wait_ms > 0)
            this_thread::sleep_for(milliseconds{wait_ms});
        else if (0 == wait_ms)
            this_thread::yield();
        else if (-2 == wait_ms)
            sleep(0);                   // process yield ??
    }
    if (res < 0) {
        pr_errno_lk(errno, "%s: %s", __func__, np);
        return -1;
    }
    /* now for the error processing */
    pack_id = pt.pack_id;
    ok = 0;
    switch (sg_err_category3(&pt)) {
    case SG_LIB_CAT_CLEAN:
        ok = 1;
        break;
    case SG_LIB_CAT_RECOVERED:
        pr2serr_lk("%s: Recovered error on %s, continuing\n", __func__, np);
        ok = 1;
        break;
    default: /* won't bother decoding other categories */
        console_mutex.lock();
        sg_chk_n_print3(np, &pt, 1);
        console_mutex.unlock();
        break;
    }
    return ok ? 0 : -1;
}

/* Should have page alignment if direct_io chosen */
static unsigned char *
get_aligned_heap(int bytes_at_least)
{
    int n;
    void * wp;

    if (bytes_at_least < page_size)
        n = page_size;
    else
        n = bytes_at_least;
#if 1
    int err = posix_memalign(&wp, page_size, n);
    if (err) {
        pr2serr_lk("posix_memalign: error [%d] out of memory?\n", err);
        return NULL;
    }
    memset(wp, 0, n);
    return (unsigned char *)wp;
#else
    if (n == page_size) {
        wp = calloc(page_size, 1);
        memset(wp, 0, n);
        return (unsigned char *)wp;
    } else {
        pr2serr_lk("get_aligned_heap: too fiddly to align, choose smaller "
                "lb_sz\n");
        return NULL;
    }
#endif
}

static void
work_thread(int id, struct opts_t * op)
{
    int thr_async_starts = 0;
    int thr_async_finishes = 0;
    unsigned int thr_eagain_count = 0;
    unsigned int seed = 0;
    int k, n, res, sg_fd, num_outstanding, do_inc, num, pack_id, sg_flags;
    int num_waiting_read, num_to_read;
    int open_flags = O_RDWR;
    bool is_rw = (SCSI_TUR != op->c2e);
    char ebuff[EBUFF_SZ];
    uint64_t lba;
    unsigned char * lbp;
    const char * dev_name;
    const char * err = NULL;
    Rand_uint * ruip = NULL;
    struct pollfd  pfd[1];
    list<unsigned char *> free_lst;
    map<int, unsigned char *> pi_2_buff;
    map<int, uint64_t> pi_2_lba;

    n = op->dev_names.size();
    dev_name = op->dev_names[id % n];
    if (op->verbose) {
        if ((op->verbose > 1) && op->hi_lba)
            pr2serr_lk("Enter work_thread id=%d using %s\n"
                       "    LBA range: 0x%x to 0x%x (inclusive)\n",
                       id, dev_name, (unsigned int)op->lba,
                       (UINT_MAX == op->hi_lba) ? op->hi_lbas[id % n]
                                                : op->hi_lba);
        else
            pr2serr_lk("Enter work_thread id=%d using %s\n", id, dev_name);
    }
    if (! op->block)
        open_flags |= O_NONBLOCK;

    sg_fd = open(dev_name, open_flags);
    if (sg_fd < 0) {
        pr_errno_lk(errno, "%s: id=%d, error opening file: %s", __func__, id,
                 dev_name);
        return;
    }
    pfd[0].fd = sg_fd;
    pfd[0].events = POLLIN;
    if (is_rw && op->hi_lba) {
        seed = get_urandom_uint();
        if (op->verbose > 1)
            pr2serr_lk("  id=%d, /dev/urandom seed=0x%x\n", id, seed);
        if (UINT_MAX == op->hi_lba)
            ruip = new Rand_uint((unsigned int)op->lba, op->hi_lbas[id % n],
                                 seed);
        else
            ruip = new Rand_uint((unsigned int)op->lba, op->hi_lba, seed);
    }

    sg_flags = 0;
    if (BQ_AT_TAIL == op->bqd)
        sg_flags |= SG_FLAG_Q_AT_TAIL;
    else if (BQ_AT_HEAD == op->bqd)
        sg_flags |= SG_FLAG_Q_AT_HEAD;
    if (op->direct)
        sg_flags |= SG_FLAG_DIRECT_IO;
    if (op->no_xfer)
        sg_flags |= SG_FLAG_NO_DXFER;
    if (op->verbose > 1)
        pr2serr_lk("  id=%d, sg_flags=0x%x, %s cmds\n", id, sg_flags,
                   ((SCSI_TUR == op->c2e) ? "TUR":
                    ((SCSI_READ16 == op->c2e) ? "READ" : "WRITE")));

    num = op->num_per_thread;
    for (k = 0, num_outstanding = 0; (k < num) || num_outstanding;
         k = do_inc ? k + 1 : k) {
        do_inc = 0;
        if ((num_outstanding < op->maxq_per_thread) && (k < num)) {
            do_inc = 1;
            pack_id = uniq_pack_id.fetch_add(1);
            if (is_rw) {
                if (free_lst.empty()) {
                    lbp = get_aligned_heap(op->lb_sz);
                    if (NULL == lbp) {
                        err = "out of memory";
                        break;
                    }
                } else {
                    lbp = free_lst.back();
                    free_lst.pop_back();
                }
            } else
                lbp = NULL;
            if (is_rw) {
                if (ruip) {
                    lba = ruip->get();
                    if (op->verbose > 3)
                        pr2serr_lk("  id=%d: start IO at lba=0x%" PRIx64 "\n",
                                   id, lba);
                } else
                    lba = op->lba;
            } else
                lba = 0;
            if (start_sg3_cmd(sg_fd, op->c2e, pack_id, lba, lbp, op->lb_sz,
                              sg_flags)) {
                err = "start_sg3_cmd()";
                break;
            }
            ++thr_async_starts;
            ++num_outstanding;
            pi_2_buff[pack_id] = lbp;
            if (ruip)
                pi_2_lba[pack_id] = lba;
        }
        num_to_read = 0;
        if ((num_outstanding >= op->maxq_per_thread) || (k >= num)) {
            /* full queue or finished injecting */
            num_waiting_read = 0;
            if (ioctl(sg_fd, SG_GET_NUM_WAITING, &num_waiting_read) < 0) {
                err = "ioctl(SG_GET_NUM_WAITING) failed";
                break;
            }
            if (1 == num_waiting_read)
                num_to_read = num_waiting_read;
            else if (num_waiting_read > 0) {
                if (k >= num)
                    num_to_read = num_waiting_read;
                else {
                    switch (op->mqd) {
                    case MQD_LOW:
                        num_to_read = num_waiting_read;
                        break;
                    case MQD_MEDIUM:
                        num_to_read = num_waiting_read / 2;
                        break;
                    case MQD_HIGH:
                    default:
                        num_to_read = 1;
                        break;
                    }
                }
            } else {
                n = (op->wait_ms > 0) ? op->wait_ms : 0;
                while (0 == (res = poll(pfd, 1, n))) {
                    if (res < 0) {
                        err = "poll(wait_ms) failed";
                        break;
                    }
                }
                if (err)
                    break;
            }
        } else {        /* not full, not finished injecting */
            if (MQD_HIGH == op->mqd)
                num_to_read = 0;
            else {
                num_waiting_read = 0;
                if (ioctl(sg_fd, SG_GET_NUM_WAITING, &num_waiting_read) < 0) {
                    err = "ioctl(SG_GET_NUM_WAITING) failed";
                    break;
                }
                if (num_waiting_read > 0)
                    num_to_read = num_waiting_read /
                                  ((MQD_LOW == op->mqd) ? 1 : 2);
                else
                    num_to_read = 0;
            }
        }

        while (num_to_read-- > 0) {
            if (finish_sg3_cmd(sg_fd, op->c2e, pack_id, op->wait_ms,
                               thr_eagain_count)) {
                err = "finish_sg3_cmd()";
                if (ruip && (pack_id > 0)) {
                    auto q = pi_2_lba.find(pack_id);

                    if (q != pi_2_lba.end()) {
                        snprintf(ebuff, sizeof(ebuff), "%s: lba=0x%" PRIx64 ,
                                 err, q->second);
                        err = ebuff;
                    }
                }
                break;
            }
            ++thr_async_finishes;
            --num_outstanding;
            auto p = pi_2_buff.find(pack_id);

            if (p == pi_2_buff.end()) {
                snprintf(ebuff, sizeof(ebuff), "pack_id=%d from "
                         "finish_sg3_cmd() not found\n", pack_id);
                if (! err)
                    err = ebuff;
            } else {
                lbp = p->second;
                pi_2_buff.erase(p);
                if (lbp)
                    free_lst.push_front(lbp);
            }
            if (ruip && (pack_id > 0)) {
                auto q = pi_2_lba.find(pack_id);

                if (q != pi_2_lba.end()) {
                    if (op->verbose > 3)
                        pr2serr_lk("    id=%d: finish IO at lba=0x%" PRIx64
                                   "\n", id, q->second);
                    pi_2_lba.erase(q);
                }
            }
            if (err)
                break;
        }
        if (err)
            break;
    }
    close(sg_fd);       // sg driver will handle any commands "in flight"
    if (ruip)
        delete ruip;

    if (err || (k < num)) {
        if (k < num)
            pr2serr_lk("thread id=%d FAILed at iteration %d%s%s\n", id, k,
                    (err ? ", Reason: " : ""), (err ? err : ""));
        else
            pr2serr_lk("thread id=%d FAILed on last%s%s\n", id,
                    (err ? ", Reason: " : ""), (err ? err : ""));
    }
    n = pi_2_buff.size();
    if (n > 0)
        pr2serr_lk("thread id=%d Still %d elements in pi_2_buff map on exit\n",
                id, n);
    for (k = 0; ! free_lst.empty(); ++k) {
        lbp = free_lst.back();
        free_lst.pop_back();
        if (lbp)
            free(lbp);
    }
    if ((op->verbose > 2) && (k > 0))
        pr2serr_lk("thread id=%d Maximum number of READ/WRITEs queued: %d\n",
                id, k);
    async_starts += thr_async_starts;
    async_finishes += thr_async_finishes;
    eagain_count += thr_eagain_count;
}

#define INQ_REPLY_LEN 96
#define INQ_CMD_LEN 6

/* Send INQUIRY and fetches response. If okay puts PRODUCT ID field
 * in b (up to m_blen bytes). Does not use O_EXCL flag. Returns 0 on success,
 * else -1 . */
static int
do_inquiry_prod_id(const char * dev_name, int block, char * b, int b_mlen)
{
    int sg_fd, ok, ret;
    struct sg_io_hdr pt;
    unsigned char inqCmdBlk [INQ_CMD_LEN] =
                                {0x12, 0, 0, 0, INQ_REPLY_LEN, 0};
    unsigned char inqBuff[INQ_REPLY_LEN];
    unsigned char sense_buffer[64];
    int open_flags = O_RDWR;    /* O_EXCL | O_RDONLY fails with EPERM */

    if (! block)
        open_flags |= O_NONBLOCK;
    sg_fd = open(dev_name, open_flags);
    if (sg_fd < 0) {
        pr_errno_lk(errno, "%s: error opening file: %s", __func__, dev_name);
        return -1;
    }
    /* Prepare INQUIRY command */
    memset(&pt, 0, sizeof(pt));
    pt.interface_id = 'S';
    pt.cmd_len = sizeof(inqCmdBlk);
    /* pt.iovec_count = 0; */  /* memset takes care of this */
    pt.mx_sb_len = sizeof(sense_buffer);
    pt.dxfer_direction = SG_DXFER_FROM_DEV;
    pt.dxfer_len = INQ_REPLY_LEN;
    pt.dxferp = inqBuff;
    pt.cmdp = inqCmdBlk;
    pt.sbp = sense_buffer;
    pt.timeout = 20000;     /* 20000 millisecs == 20 seconds */
    /* pt.flags = 0; */     /* take defaults: indirect IO, etc */
    /* pt.pack_id = 0; */
    /* pt.usr_ptr = NULL; */

    if (ioctl(sg_fd, SG_IO, &pt) < 0) {
        pr_errno_lk(errno, "%s: Inquiry SG_IO ioctl error", __func__);
        close(sg_fd);
        return -1;
    }

    /* now for the error processing */
    ok = 0;
    switch (sg_err_category3(&pt)) {
    case SG_LIB_CAT_CLEAN:
        ok = 1;
        break;
    case SG_LIB_CAT_RECOVERED:
        pr2serr_lk("Recovered error on INQUIRY, continuing\n");
        ok = 1;
        break;
    default: /* won't bother decoding other categories */
        console_mutex.lock();
        sg_chk_n_print3("INQUIRY command error", &pt, 1);
        console_mutex.unlock();
        break;
    }
    if (ok) {
        /* Good, so fetch Product ID from response, copy to 'b' */
        if (b_mlen > 0) {
            if (b_mlen > 16) {
                memcpy(b, inqBuff + 16, 16);
                b[16] = '\0';
            } else {
                memcpy(b, inqBuff + 16, b_mlen - 1);
                b[b_mlen - 1] = '\0';
            }
        }
        ret = 0;
    } else
        ret = -1;
    close(sg_fd);
    return ret;
}

/* Only allow ranges up to 2**32-1 upper limit, so READ CAPACITY(10)
 * sufficient. Return of 0 -> success, -1 -> failure, 2 -> try again */
static int
do_read_capacity(const char * dev_name, int block, unsigned int * last_lba,
                 unsigned int * blk_sz)
{
    int res, sg_fd;
    unsigned char rcCmdBlk [10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned char rcBuff[64];
    unsigned char sense_b[64];
    sg_io_hdr_t io_hdr;
    int open_flags = O_RDWR;    /* O_EXCL | O_RDONLY fails with EPERM */

    if (! block)
        open_flags |= O_NONBLOCK;
    sg_fd = open(dev_name, open_flags);
    if (sg_fd < 0) {
        pr_errno_lk(errno, "%s: error opening file: %s", __func__, dev_name);
        return -1;
    }
    /* Prepare READ CAPACITY(10) command */
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(rcCmdBlk);
    io_hdr.mx_sb_len = sizeof(sense_b);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = sizeof(rcBuff);
    io_hdr.dxferp = rcBuff;
    io_hdr.cmdp = rcCmdBlk;
    io_hdr.sbp = sense_b;
    io_hdr.timeout = 20000;     /* 20000 millisecs == 20 seconds */;

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
        pr_errno_lk(errno, "%s (SG_IO) error", __func__);
        close(sg_fd);
        return -1;
    }
    res = sg_err_category3(&io_hdr);
    if (SG_LIB_CAT_UNIT_ATTENTION == res) {
        console_mutex.lock();
        sg_chk_n_print3("read capacity", &io_hdr, 1);
        console_mutex.unlock();
        close(sg_fd);
        return 2; /* probably have another go ... */
    } else if (SG_LIB_CAT_CLEAN != res) {
        console_mutex.lock();
        sg_chk_n_print3("read capacity", &io_hdr, 1);
        console_mutex.unlock();
        close(sg_fd);
        return -1;
    }
    *last_lba = ((rcBuff[0] << 24) | (rcBuff[1] << 16) |
                 (rcBuff[2] << 8) | rcBuff[3]);
    *blk_sz = (rcBuff[4] << 24) | (rcBuff[5] << 16) |
               (rcBuff[6] << 8) | rcBuff[7];
    close(sg_fd);
    return 0;
}


int
main(int argc, char * argv[])
{
    int k, n, res;
    int force = 0;
    int64_t ll;
    int num_threads = DEF_NUM_THREADS;
    char b[128];
    struct timespec start_tm, end_tm;
    struct opts_t opts;
    struct opts_t * op;
    const char * cp;

    op = &opts;
    op->direct = !! DEF_DIRECT;
    op->lba = DEF_LBA;
    op->hi_lba = 0;
    op->lb_sz = DEF_LB_SZ;
    op->maxq_per_thread = MAX_Q_PER_FD;
    op->num_per_thread = DEF_NUM_PER_THREAD;
    op->no_xfer = !! DEF_NO_XFER;
    op->verbose = 0;
    op->wait_ms = DEF_WAIT_MS;
    op->c2e = SCSI_TUR;
    op->bqd = BQ_DEFAULT;
    op->block = !! DEF_BLOCKING;
    op->mqd = MQD_HIGH;
    page_size = sysconf(_SC_PAGESIZE);

    for (k = 1; k < argc; ++k) {
        if (0 == memcmp("-d", argv[k], 2))
            op->direct = true;
        else if (0 == memcmp("-f", argv[k], 2))
            ++force;
        else if (0 == memcmp("-h", argv[k], 2)) {
            usage();
            return 0;
        } else if (0 == memcmp("-l", argv[k], 2)) {
            ++k;
            if ((k < argc) && isdigit(*argv[k])) {
                ll = sg_get_llnum(argv[k]);
                if (-1 == ll) {
                    pr2serr_lk("could not decode lba\n");
                    return 1;
                } else
                    op->lba = (uint64_t)ll;
                cp = strchr(argv[k], ',');
                if (cp) {
                    if (0 == strcmp("-1", cp + 1))
                        op->hi_lba = UINT_MAX;
                    else {
                        ll = sg_get_llnum(cp + 1);
                        if ((-1 == ll) || (ll > UINT_MAX)) {
                            pr2serr_lk("could not decode hi_lba, or > "
                                       "UINT_MAX\n");
                            return 1;
                        } else
                            op->hi_lba = (unsigned int)ll;
                    }
                }
            } else
                break;
        } else if (0 == memcmp("-M", argv[k], 2)) {
            ++k;
            if ((k < argc) && isdigit(*argv[k])) {
                n = atoi(argv[k]);
                if ((n < 1) || (n > MAX_Q_PER_FD)) {
                    pr2serr_lk("-M expects a value from 1 to %d\n",
                               MAX_Q_PER_FD);
                    return 1;
                }
                op->maxq_per_thread = n;
            } else
                break;
        } else if (0 == memcmp("-n", argv[k], 2)) {
            ++k;
            if ((k < argc) && isdigit(*argv[k]))
                op->num_per_thread = atoi(argv[k]);
            else
                break;
        } else if (0 == memcmp("-N", argv[k], 2))
            op->no_xfer = true;
        else if (0 == memcmp("-q", argv[k], 2)) {
            ++k;
            if ((k < argc) && isdigit(*argv[k])) {
                n = atoi(argv[k]);
                if (0 == n)
                    op->bqd = BQ_AT_HEAD;
                else if (1 == n)
                    op->bqd = BQ_AT_TAIL;
            } else
                break;
        } else if (0 == memcmp("-Q", argv[k], 2)) {
            ++k;
            if ((k < argc) && isdigit(*argv[k])) {
                n = atoi(argv[k]);
                if (0 == n)
                    op->mqd = MQD_LOW;
                else if (1 == n)
                    op->mqd = MQD_MEDIUM;
                else if (2 == n)
                    op->mqd = MQD_HIGH;
            } else
                break;
        } else if (0 == memcmp("-R", argv[k], 2))
            op->c2e = SCSI_READ16;
        else if (0 == memcmp("-s", argv[k], 2)) {
            ++k;
            if ((k < argc) && isdigit(*argv[k])) {
                op->lb_sz = atoi(argv[k]);
                if (op->lb_sz < 256) {
                    cerr << "Strange lb_sz, using 256" << endl;
                    op->lb_sz = 256;
                }
            } else
                break;
        } else if (0 == memcmp("-t", argv[k], 2)) {
            ++k;
            if ((k < argc) && isdigit(*argv[k]))
                num_threads = atoi(argv[k]);
            else
                break;
        } else if (0 == memcmp("-T", argv[k], 2))
            op->c2e = SCSI_TUR;
        else if (0 == memcmp("-vvvv", argv[k], 5))
            op->verbose += 4;
        else if (0 == memcmp("-vvv", argv[k], 4))
            op->verbose += 3;
        else if (0 == memcmp("-vv", argv[k], 3))
            op->verbose += 2;
        else if (0 == memcmp("-v", argv[k], 2))
            ++op->verbose;
        else if (0 == memcmp("-V", argv[k], 2)) {
            printf("%s version: %s\n", util_name, version_str);
            return 0;
        } else if (0 == memcmp("-w", argv[k], 2)) {
            ++k;
            if ((k < argc) && (isdigit(*argv[k]) || ('-' == *argv[k]))) {
                if ('-' == *argv[k])
                    op->wait_ms = - atoi(argv[k] + 1);
                else
                    op->wait_ms = atoi(argv[k]);
            } else
                break;
        } else if (0 == memcmp("-W", argv[k], 2))
            op->c2e = SCSI_WRITE16;
        else if (*argv[k] == '-') {
            pr2serr_lk("Unrecognized switch: %s\n", argv[k]);
            return 1;
        } else
            op->dev_names.push_back(argv[k]);
    }
    if (0 == op->dev_names.size()) {
        usage();
        return 1;
    }
    if (op->hi_lba && (op->lba > op->hi_lba)) {
        cerr << "lba,hi_lba range is illegal" << endl;
        return 1;
    }

    try {
        struct stat a_stat;

        for (k = 0; k < (int)op->dev_names.size(); ++k) {
            if (stat(op->dev_names[k], &a_stat) < 0) {
                snprintf(b, sizeof(b), "could not stat() %s",
                         op->dev_names[k]);
                perror(b);
                return 1;
            }
            if (! S_ISCHR(a_stat.st_mode)) {
                pr2serr_lk("%s should be a sg device which is a char "
                        "device. %s\n", op->dev_names[k], op->dev_names[k]);
                pr2serr_lk("is not a char device and damage could be "
                        "done if it is a BLOCK\ndevice, exiting ...\n");
                return 1;
            }
            if (! force) {
                res = do_inquiry_prod_id(op->dev_names[k], op->block, b,
                                         sizeof(b));
                if (res) {
                    pr2serr_lk("INQUIRY failed on %s\n", op->dev_names[k]);
                    return 1;
                }
                // For safety, since <lba> written to, only permit scsi_debug
                // devices. Bypass this with '-f' option.
                if (0 != memcmp("scsi_debug", b, 10)) {
                    pr2serr_lk("Since this utility may write to LBAs, "
                               "only devices with the\n"
                               "product ID 'scsi_debug' accepted. Use '-f' "
                               "to override.\n");
                    return 2;
                }
            }
            if (UINT_MAX == op->hi_lba) {
                unsigned int last_lba;
                unsigned int blk_sz;

                res = do_read_capacity(op->dev_names[k], op->block,
                                       &last_lba, &blk_sz);
                if (2 == res)
                    res = do_read_capacity(op->dev_names[k], op->block,
                                           &last_lba, &blk_sz);
                if (res) {
                    pr2serr_lk("READ CAPACITY(10) failed on %s\n",
                               op->dev_names[k]);
                    return 1;
                }
                op->hi_lbas.push_back(last_lba);
                if (blk_sz != (unsigned int)op->lb_sz)
                    pr2serr_lk(">>> warning: Logical block size (%d) of %s\n"
                               "    differs from command line option (or "
                               "default)\n", blk_sz, op->dev_names[k]);
            }
        }

        start_tm.tv_sec = 0;
        start_tm.tv_nsec = 0;
        if (clock_gettime(CLOCK_MONOTONIC, &start_tm) < 0)
            perror("clock_gettime failed");

        vector<thread *> vt;

        /* start multi-threaded section */
        for (k = 0; k < num_threads; ++k) {
            thread * tp = new thread {work_thread, k, op};
            vt.push_back(tp);
        }

        // g++ 4.7.3 didn't like range-for loop here
        for (k = 0; k < (int)vt.size(); ++k)
            vt[k]->join();
        /* end multi-threaded section, just this main thread left */

        for (k = 0; k < (int)vt.size(); ++k)
            delete vt[k];

        n = uniq_pack_id.load() - 1;
        if ((n > 0) && (0 == clock_gettime(CLOCK_MONOTONIC, &end_tm))) {
            struct timespec res_tm;
            double a, b;

            res_tm.tv_sec = end_tm.tv_sec - start_tm.tv_sec;
            res_tm.tv_nsec = end_tm.tv_nsec - start_tm.tv_nsec;
            if (res_tm.tv_nsec < 0) {
                --res_tm.tv_sec;
                res_tm.tv_nsec += 1000000000;
            }
            a = res_tm.tv_sec;
            a += (0.000001 * (res_tm.tv_nsec / 1000));
            b = (double)n;
            if (a > 0.000001) {
                printf("Time to complete %d commands was %d.%06d seconds\n",
                       n, (int)res_tm.tv_sec, (int)(res_tm.tv_nsec / 1000));
                cout << "Implies " << (b / a) << " IOPS" << endl;
            }
        }

        if (op->verbose) {
            cout << "Number of async_starts: " << async_starts.load() << endl;
            cout << "Number of async_finishes: " << async_finishes.load() <<
                    endl;
            cout << "Last pack_id: " << n << endl;
            cout << "Number of EBUSYs: " << ebusy_count.load() << endl;
            cout << "Number of EAGAINs: " << eagain_count.load() << endl;
        }
    }
    catch(system_error& e)  {
        cerr << "got a system_error exception: " << e.what() << '\n';
        auto ec = e.code();
        cerr << "category: " << ec.category().name() << '\n';
        cerr << "value: " << ec.value() << '\n';
        cerr << "message: " << ec.message() << '\n';
        cerr << "\nNote: if g++ may need '-pthread' or similar in "
                "compile/link line" << '\n';
    }
    catch(...) {
        cerr << "got another exception: " << '\n';
    }
    return 0;
}
