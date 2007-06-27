/*
 * Copyright (c) 2004-2005 Douglas Gilbert.
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
 *
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include "sg_include.h"
#include "sg_lib.h"
#include "sg_cmds.h"

/* A utility program for the Linux OS SCSI subsystem.
 *
 * This program writes the given mode page contents to the corresponding
 * mode page on the given device.
 */

static char * version_str = "1.04 20050405";

#define ME "sg_wr_mode: "

#define MX_ALLOC_LEN 2048
#define SHORT_ALLOC_LEN 252

#define EBUFF_SZ 256


static struct option long_options[] = {
        {"contents", 1, 0, 'c'},
        {"dbd", 0, 0, 'd'},
        {"force", 0, 0, 'f'},
        {"help", 0, 0, 'h'},
        {"len", 1, 0, 'l'},
        {"mask", 1, 0, 'm'},
        {"page", 1, 0, 'p'},
        {"save", 0, 0, 's'},
        {"verbose", 0, 0, 'v'},
        {"version", 0, 0, 'V'},
        {0, 0, 0, 0},
};

static void usage()
{
    fprintf(stderr, "Usage: "
          "sg_wr_mode [--contents=<h>,<h>...] [--dbd] [--force] [--help]\n"
          "                  [--len=<10|6>] [--mask=<h>,<h>...]\n"
          "                  [--page=<page_code>[,<subpage_code]] [--save]\n"
          "                  [--verbose] [--version] <scsi_device>\n"
          "  where: --contents=<h>,<h>... | -c <h>,<h>...  comma separated "
          "string of hex\n"
          "                               numbers that is mode page contents"
          " to write\n"
          "         --contents=- | -c -   read stdin for mode page contents"
          " to write\n"
          "         --dbd | -d            disable block descriptors (DBD bit"
          " in cdb)\n"
          "         --force | -f          force the contents to be written\n"
          "         --help | -h           print out usage message\n"
          "         --len=<10|6> | -l <10|6>   use 10 byte (def) or 6 byte "
          "variants of\n"
          "                               MODE SENSE/SELECT\n"
          "         --mask=<h>,<h>... | -m <h>,<h>...   comma separated "
          "string of hex\n"
          "                               numbers that mask contents"
          " to write\n"
          "         --page=<page_code> | -p <page_code>  page_code to be "
          "written (in hex)\n"
          "         --page=<page_code>,<subpage_code | -p <pc>,<spc>  page "
          "and subpage\n"
          "                               code to be written (in hex)\n"
          "         --save | -s           set 'save page' (SP) bit; default "
          "don't so\n"
          "                               only 'current' values changed\n"
          "         --verbose | -v        increase verbosity\n"
          "         --version | -V        print version string and exit\n\n"
          "    writes given mode page to device\n"
          );
}


/* Read hex numbers from command line (comma separated list) or from */
/* stdin (one per line, comma separated list or space separated list). */
/* Returns 0 if ok, or 1 if error. */
static int build_mode_page(const char * inp, unsigned char * mp_arr,
                           int * mp_arr_len, int max_arr_len)
{
    int in_len, k, j, m;
    unsigned int h;
    const char * lcp;
    char * cp;

    if ((NULL == inp) || (NULL == mp_arr) ||
        (NULL == mp_arr_len))
        return 1;
    lcp = inp;
    in_len = strlen(inp);
    if (0 == in_len)
        *mp_arr_len = 0;
    if ('-' == inp[0]) {        /* read from stdin */
        char line[512];
        int off = 0;

        for (j = 0; j < 512; ++j) {
            if (NULL == fgets(line, sizeof(line), stdin))
                break;
            in_len = strlen(line);
            if (in_len > 0) {
                if ('\n' == line[in_len - 1]) {
                    --in_len;
                    line[in_len] = '\0';
                }
            }
            if (0 == in_len)
                continue;
            lcp = line;
            m = strspn(lcp, " \t");
            if (m == in_len)
                continue;
            lcp += m;
            in_len -= m;
            if ('#' == *lcp)
                continue;
            k = strspn(lcp, "0123456789aAbBcCdDeEfF ,\t");
            if ((k < in_len) && ('#' != lcp[k])) {
                fprintf(stderr, "build_mode_page: syntax error at "
                        "line %d, pos %d\n", j + 1, m + k + 1);
                return 1;
            }
            for (k = 0; k < 1024; ++k) {
                if (1 == sscanf(lcp, "%x", &h)) {
                    if (h > 0xff) {
                        fprintf(stderr, "build_mode_page: hex number "
                                "larger than 0xff in line %d, pos %d\n",
                                j + 1, (int)(lcp - line + 1));
                        return 1;
                    }
                    if ((off + k) >= max_arr_len) {
                        fprintf(stderr, "build_mode_page: array length "
                                "exceeded\n");
                        return 1;
                    }
                    mp_arr[off + k] = h;
                    lcp = strpbrk(lcp, " ,\t");
                    if (NULL == lcp)
                        break;
                    lcp += strspn(lcp, " ,\t");
                    if ('\0' == *lcp)
                        break;
                } else {
                    if ('#' == *lcp) {
                        --k;
                        break;
                    }
                    fprintf(stderr, "build_mode_page: error in "
                            "line %d, at pos %d\n", j + 1,
                            (int)(lcp - line + 1));
                    return 1;
                }
            }
            off += (k + 1);
        }
        *mp_arr_len = off;
    } else {        /* hex string on command line */
        k = strspn(inp, "0123456789aAbBcCdDeEfF,");
        if (in_len != k) {
            fprintf(stderr, "build_mode_page: error at pos %d\n", k + 1);
            return 1;
        }
        for (k = 0; k < max_arr_len; ++k) {
            if (1 == sscanf(lcp, "%x", &h)) {
                if (h > 0xff) {
                    fprintf(stderr, "build_mode_page: hex number larger "
                            "than 0xff at pos %d\n", (int)(lcp - inp + 1));
                    return 1;
                }
                mp_arr[k] = h;
                cp = strchr(lcp, ',');
                if (NULL == cp)
                    break;
                lcp = cp + 1;
            } else {
                fprintf(stderr, "build_mode_page: error at pos %d\n",
                        (int)(lcp - inp + 1));
                return 1;
            }
        }
        *mp_arr_len = k + 1;
        if (k == max_arr_len) {
            fprintf(stderr, "build_mode_page: array length exceeded\n");
            return 1;
        }
    }
    return 0;
}

/* Read hex numbers from command line (comma separated list). */
/* Returns 0 if ok, or 1 if error. */
static int build_mask(const char * inp, unsigned char * mask_arr,
                      int * mask_arr_len, int max_arr_len)
{
    int in_len, k;
    unsigned int h;
    const char * lcp;
    char * cp;

    if ((NULL == inp) || (NULL == mask_arr) ||
        (NULL == mask_arr_len))
        return 1;
    lcp = inp;
    in_len = strlen(inp);
    if (0 == in_len)
        *mask_arr_len = 0;
    if ('-' == inp[0]) {        /* read from stdin */
        fprintf(stderr, "'--mask' does not accept input from stdin\n");
        return 1;
    } else {        /* hex string on command line */
        k = strspn(inp, "0123456789aAbBcCdDeEfF,");
        if (in_len != k) {
            fprintf(stderr, "build_mode_page: error at pos %d\n", k + 1);
            return 1;
        }
        for (k = 0; k < max_arr_len; ++k) {
            if (1 == sscanf(lcp, "%x", &h)) {
                if (h > 0xff) {
                    fprintf(stderr, "build_mode_page: hex number larger "
                            "than 0xff at pos %d\n", (int)(lcp - inp + 1));
                    return 1;
                }
                mask_arr[k] = h;
                cp = strchr(lcp, ',');
                if (NULL == cp)
                    break;
                lcp = cp + 1;
            } else {
                fprintf(stderr, "build_mode_page: error at pos %d\n",
                        (int)(lcp - inp + 1));
                return 1;
            }
        }
        *mask_arr_len = k + 1;
        if (k == max_arr_len) {
            fprintf(stderr, "build_mode_page: array length exceeded\n");
            return 1;
        }
    }
    return 0;
}


int main(int argc, char * argv[])
{
    int sg_fd, res, c, num, read_in_len, alloc_len, off;
    int k, md_len, hdr_len, bd_len, mask_in_len;
    unsigned u, uu;
    int dbd = 0;
    int got_contents = 0;
    int force = 0;
    int got_mask = 0;
    int mode_6 = 0;
    int pg_code = -1;
    int sub_pg_code = 0;
    int save = 0;
    int verbose = 0;
    char device_name[256];
    unsigned char read_in[MX_ALLOC_LEN];
    unsigned char mask_in[MX_ALLOC_LEN];
    unsigned char ref_md[MX_ALLOC_LEN];
    char ebuff[EBUFF_SZ];
    int ret = 1;

    memset(device_name, 0, sizeof device_name);
    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "c:dfhl:m:p:svV", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'c':
            memset(read_in, 0, sizeof(read_in));
            if (0 != build_mode_page(optarg, read_in, &read_in_len,
                                     sizeof(read_in))) {
                fprintf(stderr, "bad argument to '--contents'\n");
                return 1;
            }
            got_contents = 1;
            break;
        case 'd':
            dbd = 1;
            break;
        case 'f':
            force = 1;
            break;
        case 'h':
        case '?':
            usage();
            return 0;
        case 'l':
            num = sscanf(optarg, "%d", &res);
            if ((1 == num) && ((6 == res) || (10 == res)))
                mode_6 = (6 == res) ? 1 : 0;
            else {
                fprintf(stderr, "length (of cdb) must be 6 or 10\n");
                return 1;
            }
            break;
        case 'm':
            memset(mask_in, 0xff, sizeof(mask_in));
            if (0 != build_mask(optarg, mask_in, &mask_in_len,
                                sizeof(mask_in))) {
                fprintf(stderr, "bad argument to '--mask'\n");
                return 1;
            }
            got_mask = 1;
            break;
        case 'p':
           if (NULL == strchr(optarg, ',')) {
                num = sscanf(optarg, "%x", &u);
                if ((1 != num) || (u > 62)) {
                    fprintf(stderr, "Bad page code value after '--page' "
                            "switch\n");
                    return 1;
                }
                pg_code = u;
            } else if (2 == sscanf(optarg, "%x,%x", &u, &uu)) {
                if (uu > 254) {
                    fprintf(stderr, "Bad sub page code value after '--page'"
                            " switch\n");
                    return 1;
                }
                pg_code = u;
                sub_pg_code = uu;
            } else {
                fprintf(stderr, "Bad page code, subpage code sequence after "
                        "'--page' switch\n");
                return 1;
            }
            break;
        case 's':
            save = 1;
            break;
        case 'v':
            ++verbose;
            break;
        case 'V':
            fprintf(stderr, ME "version: %s\n", version_str);
            return 0;
        default:
            fprintf(stderr, "unrecognised switch code 0x%x ??\n", c);
            usage();
            return 1;
        }
    }
    if (optind < argc) {
        if ('\0' == device_name[0]) {
            strncpy(device_name, argv[optind], sizeof(device_name) - 1);
            device_name[sizeof(device_name) - 1] = '\0';
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                fprintf(stderr, "Unexpected extra argument: %s\n",
                        argv[optind]);
            usage();
            return 1;
        }
    }
    if (0 == device_name[0]) {
        fprintf(stderr, "missing device name!\n");
        usage();
        return 1;
    }
    if (pg_code < 0) {
        fprintf(stderr, "need page code (see '--page=')\n");
        usage();
        return 1;
    }
    if (got_mask && force) {
        fprintf(stderr, "cannot use both '--force' and '--mask'\n");
        usage();
        return 1;
    }

    sg_fd = open(device_name, O_RDWR | O_NONBLOCK);
    if (sg_fd < 0) {
        fprintf(stderr, ME "open error: %s: ", device_name);
        perror("");
        return 1;
    }

    /* do MODE SENSE to fetch current values */
    memset(ref_md, 0, MX_ALLOC_LEN);
    alloc_len = mode_6 ? SHORT_ALLOC_LEN : MX_ALLOC_LEN;
    if (mode_6)
        res = sg_ll_mode_sense6(sg_fd, dbd, 0 /*current */, pg_code,
                                sub_pg_code, ref_md, alloc_len, 1, verbose);
     else
        res = sg_ll_mode_sense10(sg_fd, 0 /* llbaa */, dbd, 0 /* current */,
                                 pg_code, sub_pg_code, ref_md, alloc_len, 1,
                                 verbose);
    if (SG_LIB_CAT_INVALID_OP == res) {
        fprintf(stderr, "MODE SENSE (%d) not supported, try '--len=%d'\n",
                (mode_6 ? 6 : 10), (mode_6 ? 10 : 6));
        goto err_out;
    } else if (SG_LIB_CAT_ILLEGAL_REQ == res) {
        fprintf(stderr, "bad field in MODE SENSE (%d) command\n",
                (mode_6 ? 6 : 10));
        goto err_out;
    } else if (0 != res) {
        fprintf(stderr, "MODE SENSE (%d) failed\n", (mode_6 ? 6 : 10));
        goto err_out;
    }
    off = sg_mode_page_offset(ref_md, alloc_len, mode_6, ebuff, EBUFF_SZ);
    if (off < 0) {
        fprintf(stderr, "MODE SENSE (%d): %s\n", (mode_6 ? 6 : 10), ebuff);
        goto err_out;
    }
    if (mode_6) {
        hdr_len = 4;
        md_len = ref_md[0] + 1;
        bd_len = ref_md[3];
    } else {
        hdr_len = 8;
        md_len = (ref_md[0] << 8) + ref_md[1] + 2;
        bd_len = (ref_md[6] << 8) + ref_md[7];
    }
    if (got_contents) {
        if (read_in_len < 2) {
            fprintf(stderr, "contents length=%d too short\n", read_in_len);
            goto err_out;
        }
        ref_md[0] = 0;  /* mode data length reserved for mode select */
        if (! mode_6)
            ref_md[1] = 0;    /* mode data length reserved for mode select */
        if (md_len > alloc_len) {
            fprintf(stderr, "mode data length=%d exceeds allocation "
                    "length=%d\n", md_len, alloc_len);
            goto err_out;
        }
        if (got_mask) {
            for (k = 0; k < (md_len - off); ++k) {
                if ((0x0 == mask_in[k]) || (k > read_in_len))
                   read_in[k] = ref_md[off + k];
                else if (mask_in[k] < 0xff) {
                   c = (ref_md[off + k] & (0xff & ~mask_in[k]));
                   read_in[k] = (c | (read_in[k] & mask_in[k]));
                }
            }
            read_in_len = md_len - off;
        }
        if (! force) {
            if ((! (ref_md[off] & 0x80)) && save) {
                fprintf(stderr, "PS bit in existing mode page indicates that "
                        "it is not savable\n    but '--save' option given\n");
                goto err_out;
            }
            read_in[0] &= 0x7f; /* mask out PS bit, reserved in mode select */
            if ((md_len - off) != read_in_len) {
                fprintf(stderr, "contents length=%d but reference mode page "
                        "length=%d\n", read_in_len, md_len - off);
                goto err_out;
            }
            if (pg_code != (read_in[0] & 0x3f)) {
                fprintf(stderr, "contents page_code=0x%x but reference "
                        "page_code=0x%x\n", (read_in[0] & 0x3f), pg_code);
                goto err_out;
            }
            if ((read_in[0] & 0x40) != (ref_md[off] & 0x40)) {
                fprintf(stderr, "contents flags subpage but reference page"
                        "does not (or vice versa)\n");
                goto err_out;
            }
            if ((read_in[0] & 0x40) && (read_in[1] != sub_pg_code)) {
                fprintf(stderr, "contents subpage_code=0x%x but reference "
                        "sub_page_code=0x%x\n", read_in[1], sub_pg_code);
                goto err_out;
            }
        } else
            md_len = off + read_in_len; /* force length */

        memcpy(ref_md + off, read_in, read_in_len);
        if (mode_6)
            res = sg_ll_mode_select6(sg_fd, 1, save, ref_md, md_len, 1,
                                     verbose);
        else
            res = sg_ll_mode_select10(sg_fd, 1, save, ref_md, md_len, 1,
                                      verbose);
        if (SG_LIB_CAT_INVALID_OP == res) {
            fprintf(stderr, "MODE SELECT (%d) not supported\n",
                    (mode_6 ? 6 : 10));
            goto err_out;
        } else if (SG_LIB_CAT_ILLEGAL_REQ == res) {
            fprintf(stderr, "bad field in MODE SELECT (%d) command\n",
                    (mode_6 ? 6 : 10));
            goto err_out;
        } else if (0 != res) {
            fprintf(stderr, "MODE SELECT (%d) failed\n", (mode_6 ? 6 : 10));
            goto err_out;
        }
    } else {
        printf(">>> No contents given, so show current mode page data:\n");
        printf("  header:\n");
        dStrHex((const char *)ref_md, hdr_len, -1);
        if (bd_len) {
            printf("  block descriptor(s):\n");
            dStrHex((const char *)(ref_md + hdr_len), bd_len, -1);
        } else
            printf("  << no block descriptors >>\n");
        printf("  mode page:\n");
        dStrHex((const char *)(ref_md + off), md_len - off, -1);
    }
    ret = 0;
err_out:
    res = close(sg_fd);
    if (res < 0) {
        perror(ME "close error");
        return 1;
    }
    return ret;
}
