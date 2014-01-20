/*
 * Copyright (C) 2010-2013 Red Hat, Inc.
 * Copyright IBM Corp. 2009
 *
 * phyp_driver.c: ssh layer to access Power Hypervisors
 *
 * Authors:
 *  Eduardo Otubo <otubo at linux.vnet.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <libssh2.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <domain_event.h>
#include <poll.h>

#include "internal.h"
#include "virauth.h"
#include "datatypes.h"
#include "virbuffer.h"
#include "viralloc.h"
#include "virlog.h"
#include "driver.h"
#include "virerror.h"
#include "viruuid.h"
#include "domain_conf.h"
#include "storage_conf.h"
#include "nodeinfo.h"
#include "virfile.h"
#include "interface_conf.h"
#include "phyp_driver.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_PHYP

/*
 * URI: phyp://user@[hmc|ivm]/managed_system
 * */

static unsigned const int HMC = 0;
static unsigned const int IVM = 127;
static unsigned const int PHYP_IFACENAME_SIZE = 24;
static unsigned const int PHYP_MAC_SIZE = 12;

static int
waitsocket(int socket_fd, LIBSSH2_SESSION * session)
{
    struct pollfd fds[1];
    int dir;

    memset(fds, 0, sizeof(fds));
    fds[0].fd = socket_fd;

    /* now make sure we wait in the correct direction */
    dir = libssh2_session_block_directions(session);

    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        fds[0].events |= POLLIN;

    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        fds[0].events |= POLLOUT;

    return poll(fds, ARRAY_CARDINALITY(fds), -1);
}

/* this function is the layer that manipulates the ssh channel itself
 * and executes the commands on the remote machine */
static char *phypExec(LIBSSH2_SESSION *, const char *, int *, virConnectPtr)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3)
    ATTRIBUTE_NONNULL(4);
static char *
phypExec(LIBSSH2_SESSION *session, const char *cmd, int *exit_status,
         virConnectPtr conn)
{
    LIBSSH2_CHANNEL *channel;
    ConnectionData *connection_data = conn->networkPrivateData;
    virBuffer tex_ret = VIR_BUFFER_INITIALIZER;
    char *buffer = NULL;
    size_t buffer_size = 16384;
    int exitcode;
    int bytecount = 0;
    int sock = connection_data->sock;
    int rc = 0;

    if (VIR_ALLOC_N(buffer, buffer_size) < 0)
        return NULL;

    /* Exec non-blocking on the remove host */
    while ((channel = libssh2_channel_open_session(session)) == NULL &&
           libssh2_session_last_error(session, NULL, NULL, 0) ==
           LIBSSH2_ERROR_EAGAIN) {
        if (waitsocket(sock, session) < 0 && errno != EINTR) {
            virReportSystemError(errno, "%s",
                                 _("unable to wait on libssh2 socket"));
            goto err;
        }
    }

    if (channel == NULL) {
        goto err;
    }

    while ((rc = libssh2_channel_exec(channel, cmd)) ==
           LIBSSH2_ERROR_EAGAIN) {
        if (waitsocket(sock, session) < 0 && errno != EINTR) {
            virReportSystemError(errno, "%s",
                                 _("unable to wait on libssh2 socket"));
            goto err;
        }
    }

    if (rc != 0) {
        goto err;
    }

    for (;;) {
        /* loop until we block */
        do {
            rc = libssh2_channel_read(channel, buffer, buffer_size);
            if (rc > 0) {
                bytecount += rc;
                virBufferAdd(&tex_ret, buffer, -1);
            }
        }
        while (rc > 0);

        /* this is due to blocking that would occur otherwise so we loop on
         * this condition */
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (waitsocket(sock, session) < 0 && errno != EINTR) {
                virReportSystemError(errno, "%s",
                                     _("unable to wait on libssh2 socket"));
                goto err;
            }
        } else {
            break;
        }
    }

    exitcode = 127;

    while ((rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN) {
        if (waitsocket(sock, session) < 0 && errno != EINTR) {
            virReportSystemError(errno, "%s",
                                 _("unable to wait on libssh2 socket"));
            goto err;
        }
    }

    if (rc == 0) {
        exitcode = libssh2_channel_get_exit_status(channel);
    }

    (*exit_status) = exitcode;
    libssh2_channel_free(channel);
    channel = NULL;
    VIR_FREE(buffer);

    if (virBufferError(&tex_ret)) {
        virBufferFreeAndReset(&tex_ret);
        virReportOOMError();
        return NULL;
    }
    return virBufferContentAndReset(&tex_ret);

err:
    (*exit_status) = SSH_CMD_ERR;
    virBufferFreeAndReset(&tex_ret);
    VIR_FREE(buffer);
    return NULL;
}

/* Convenience wrapper function */
static char *phypExecBuffer(LIBSSH2_SESSION *, virBufferPtr buf, int *,
                            virConnectPtr, bool) ATTRIBUTE_NONNULL(1)
    ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4);
static char *
phypExecBuffer(LIBSSH2_SESSION *session, virBufferPtr buf, int *exit_status,
               virConnectPtr conn, bool strip_newline)
{
    char *cmd;
    char *ret;

    if (virBufferError(buf)) {
        virBufferFreeAndReset(buf);
        virReportOOMError();
        return NULL;
    }
    cmd = virBufferContentAndReset(buf);
    ret = phypExec(session, cmd, exit_status, conn);
    VIR_FREE(cmd);
    if (ret && *exit_status == 0 && strip_newline) {
        char *nl = strchr(ret, '\n');
        if (nl)
            *nl = '\0';
    }
    return ret;
}

/* Convenience wrapper function */
static int phypExecInt(LIBSSH2_SESSION *, virBufferPtr, virConnectPtr, int *)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4);
static int
phypExecInt(LIBSSH2_SESSION *session, virBufferPtr buf, virConnectPtr conn,
            int *result)
{
    char *str;
    int ret;
    char *char_ptr;

    str = phypExecBuffer(session, buf, &ret, conn, true);
    if (!str || ret) {
        VIR_FREE(str);
        return -1;
    }
    ret = virStrToLong_i(str, &char_ptr, 10, result);
    if (ret == 0 && *char_ptr)
        VIR_WARN("ignoring suffix during integer parsing of '%s'", str);
    VIR_FREE(str);
    return ret;
}

static int
phypGetSystemType(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *ret = NULL;
    int exit_status = 0;

    ret = phypExec(session, "lshmc -V", &exit_status, conn);

    VIR_FREE(ret);
    return exit_status;
}

static int
phypGetVIOSPartitionID(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    int id = -1;
    char *managed_system = phyp_driver->managed_system;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lssyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAddLit(&buf, " -r lpar -F lpar_id,lpar_env"
                    "|sed -n '/vioserver/ {\n s/,.*$//\n p\n}'");
    phypExecInt(session, &buf, conn, &id);
    return id;
}


static virCapsPtr
phypCapsInit(void)
{
    virCapsPtr caps;
    virCapsGuestPtr guest;

    if ((caps = virCapabilitiesNew(virArchFromHost(),
                                   0, 0)) == NULL)
        goto no_memory;

    /* Some machines have problematic NUMA toplogy causing
     * unexpected failures. We don't want to break the QEMU
     * driver in this scenario, so log errors & carry on
     */
    if (nodeCapsInitNUMA(caps) < 0) {
        virCapabilitiesFreeNUMAInfo(caps);
        VIR_WARN
            ("Failed to query host NUMA topology, disabling NUMA capabilities");
    }

    if ((guest = virCapabilitiesAddGuest(caps,
                                         "linux",
                                         caps->host.arch,
                                         NULL, NULL, 0, NULL)) == NULL)
        goto no_memory;

    if (virCapabilitiesAddGuestDomain(guest,
                                      "phyp", NULL, NULL, 0, NULL) == NULL)
        goto no_memory;

    return caps;

no_memory:
    virObjectUnref(caps);
    return NULL;
}

/* This is a generic function that won't be used directly by
 * libvirt api. The function returns the number of domains
 * in different states: Running, Not Activated and all:
 *
 * type: 0 - Running
 *       1 - Not Activated
 *       * - All
 * */
static int
phypConnectNumOfDomainsGeneric(virConnectPtr conn, unsigned int type)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    int ndom = -1;
    char *managed_system = phyp_driver->managed_system;
    const char *state;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (type == 0)
        state = "|grep Running";
    else if (type == 1) {
        if (system_type == HMC) {
            state = "|grep \"Not Activated\"";
        } else {
            state = "|grep \"Open Firmware\"";
        }
    } else
        state = " ";

    virBufferAddLit(&buf, "lssyscfg -r lpar");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -F lpar_id,state %s |grep -c '^[0-9][0-9]*'",
                      state);
    phypExecInt(session, &buf, conn, &ndom);
    return ndom;
}

/* This is a generic function that won't be used directly by
 * libvirt api. The function returns the ids of domains
 * in different states: Running, and all:
 *
 * type: 0 - Running
 *       1 - all
 * */
static int
phypConnectListDomainsGeneric(virConnectPtr conn, int *ids, int nids,
                              unsigned int type)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int exit_status = 0;
    int got = -1;
    char *ret = NULL;
    char *line, *next_line;
    const char *state;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (type == 0)
        state = "|grep Running";
    else
        state = " ";

    virBufferAddLit(&buf, "lssyscfg -r lpar");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -F lpar_id,state %s | sed -e 's/,.*$//'",
                      state);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    /* I need to parse the textual return in order to get the ids */
    line = ret;
    got = 0;
    while (*line && got < nids) {
        if (virStrToLong_i(line, &next_line, 10, &ids[got]) == -1) {
            VIR_ERROR(_("Cannot parse number from '%s'"), line);
            got = -1;
            goto cleanup;
        }
        got++;
        line = next_line;
        while (*line == '\n')
            line++; /* skip \n */
    }

cleanup:
    VIR_FREE(ret);
    return got;
}

static int
phypUUIDTable_WriteFile(virConnectPtr conn)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    uuid_tablePtr uuid_table = phyp_driver->uuid_table;
    size_t i = 0;
    int fd = -1;
    char local_file[] = "./uuid_table";

    if ((fd = creat(local_file, 0755)) == -1)
        goto err;

    for (i = 0; i < uuid_table->nlpars; i++) {
        if (safewrite(fd, &uuid_table->lpars[i]->id,
                      sizeof(uuid_table->lpars[i]->id)) !=
            sizeof(uuid_table->lpars[i]->id)) {
            VIR_ERROR(_("Unable to write information to local file."));
            goto err;
        }

        if (safewrite(fd, uuid_table->lpars[i]->uuid, VIR_UUID_BUFLEN) !=
            VIR_UUID_BUFLEN) {
            VIR_ERROR(_("Unable to write information to local file."));
            goto err;
        }
    }

    if (VIR_CLOSE(fd) < 0) {
        virReportSystemError(errno, _("Could not close %s"),
                             local_file);
        goto err;
    }
    return 0;

err:
    VIR_FORCE_CLOSE(fd);
    return -1;
}

static int
phypUUIDTable_Push(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    LIBSSH2_CHANNEL *channel = NULL;
    struct stat local_fileinfo;
    char buffer[1024];
    int rc = 0;
    FILE *f = NULL;
    size_t nread, sent;
    char *ptr;
    char local_file[] = "./uuid_table";
    char *remote_file = NULL;
    int ret = -1;

    if (virAsprintf(&remote_file, "/home/%s/libvirt_uuid_table",
                    NULLSTR(conn->uri->user)) < 0)
        goto cleanup;

    if (stat(local_file, &local_fileinfo) == -1) {
        VIR_WARN("Unable to stat local file.");
        goto cleanup;
    }

    if (!(f = fopen(local_file, "rb"))) {
        VIR_WARN("Unable to open local file.");
        goto cleanup;
    }

    do {
        channel =
            libssh2_scp_send(session, remote_file,
                             0x1FF & local_fileinfo.st_mode,
                             (unsigned long) local_fileinfo.st_size);

        if ((!channel) && (libssh2_session_last_errno(session) !=
                           LIBSSH2_ERROR_EAGAIN))
            goto cleanup;
    } while (!channel);

    do {
        nread = fread(buffer, 1, sizeof(buffer), f);
        if (nread <= 0) {
            if (feof(f)) {
                /* end of file */
                break;
            } else {
                VIR_ERROR(_("Failed to read from %s"), local_file);
                goto cleanup;
            }
        }
        ptr = buffer;
        sent = 0;

        do {
            /* write the same data over and over, until error or completion */
            rc = libssh2_channel_write(channel, ptr, nread);
            if (LIBSSH2_ERROR_EAGAIN == rc) {   /* must loop around */
                continue;
            } else if (rc > 0) {
                /* rc indicates how many bytes were written this time */
                sent += rc;
            }
            ptr += sent;
            nread -= sent;
        } while (rc > 0 && sent < nread);
    } while (1);

    ret = 0;

cleanup:
    if (channel) {
        libssh2_channel_send_eof(channel);
        libssh2_channel_wait_eof(channel);
        libssh2_channel_wait_closed(channel);
        libssh2_channel_free(channel);
        channel = NULL;
    }
    VIR_FORCE_FCLOSE(f);
    return ret;
}

static int
phypUUIDTable_RemLpar(virConnectPtr conn, int id)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    uuid_tablePtr uuid_table = phyp_driver->uuid_table;
    size_t i = 0;

    for (i = 0; i <= uuid_table->nlpars; i++) {
        if (uuid_table->lpars[i]->id == id) {
            uuid_table->lpars[i]->id = -1;
            memset(uuid_table->lpars[i]->uuid, 0, VIR_UUID_BUFLEN);
        }
    }

    if (phypUUIDTable_WriteFile(conn) == -1)
        goto err;

    if (phypUUIDTable_Push(conn) == -1)
        goto err;

    return 0;

err:
    return -1;
}

static int
phypUUIDTable_AddLpar(virConnectPtr conn, unsigned char *uuid, int id)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    uuid_tablePtr uuid_table = phyp_driver->uuid_table;

    uuid_table->nlpars++;
    size_t i = uuid_table->nlpars;
    i--;

    if (VIR_REALLOC_N(uuid_table->lpars, uuid_table->nlpars) < 0)
        goto err;

    if (VIR_ALLOC(uuid_table->lpars[i]) < 0)
        goto err;

    uuid_table->lpars[i]->id = id;
    memcpy(uuid_table->lpars[i]->uuid, uuid, VIR_UUID_BUFLEN);

    if (phypUUIDTable_WriteFile(conn) == -1)
        goto err;

    if (phypUUIDTable_Push(conn) == -1)
        goto err;

    return 0;

err:
    return -1;
}

static int
phypUUIDTable_ReadFile(virConnectPtr conn)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    uuid_tablePtr uuid_table = phyp_driver->uuid_table;
    size_t i = 0;
    int fd = -1;
    char local_file[] = "./uuid_table";
    int rc = 0;
    int id;

    if ((fd = open(local_file, O_RDONLY)) == -1) {
        VIR_WARN("Unable to read information from local file.");
        goto err;
    }

    /* Creating a new data base and writing to local file */
    if (VIR_ALLOC_N(uuid_table->lpars, uuid_table->nlpars) >= 0) {
        for (i = 0; i < uuid_table->nlpars; i++) {

            rc = read(fd, &id, sizeof(int));
            if (rc == sizeof(int)) {
                if (VIR_ALLOC(uuid_table->lpars[i]) < 0)
                    goto err;
                uuid_table->lpars[i]->id = id;
            } else {
                VIR_WARN
                    ("Unable to read from information from local file.");
                goto err;
            }

            rc = read(fd, uuid_table->lpars[i]->uuid, VIR_UUID_BUFLEN);
            if (rc != VIR_UUID_BUFLEN) {
                VIR_WARN("Unable to read information from local file.");
                goto err;
            }
        }
    }

    VIR_FORCE_CLOSE(fd);
    return 0;

err:
    VIR_FORCE_CLOSE(fd);
    return -1;
}

static int
phypUUIDTable_Pull(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    LIBSSH2_CHANNEL *channel = NULL;
    struct stat fileinfo;
    char buffer[1024];
    int rc = 0;
    int fd = -1;
    int got = 0;
    int amount = 0;
    int total = 0;
    int sock = 0;
    char local_file[] = "./uuid_table";
    char *remote_file = NULL;
    int ret = -1;

    if (virAsprintf(&remote_file, "/home/%s/libvirt_uuid_table",
                    NULLSTR(conn->uri->user)) < 0)
        goto cleanup;

    /* Trying to stat the remote file. */
    do {
        channel = libssh2_scp_recv(session, remote_file, &fileinfo);

        if (!channel) {
            if (libssh2_session_last_errno(session) !=
                LIBSSH2_ERROR_EAGAIN) {
                goto cleanup;
            } else {
                if (waitsocket(sock, session) < 0 && errno != EINTR) {
                    virReportSystemError(errno, "%s",
                                         _("unable to wait on libssh2 socket"));
                    goto cleanup;
                }
            }
        }
    } while (!channel);

    /* Creating a new data base based on remote file */
    if ((fd = creat(local_file, 0755)) == -1)
        goto cleanup;

    /* Request a file via SCP */
    while (got < fileinfo.st_size) {
        do {
            amount = sizeof(buffer);

            if ((fileinfo.st_size - got) < amount) {
                amount = fileinfo.st_size - got;
            }

            rc = libssh2_channel_read(channel, buffer, amount);
            if (rc > 0) {
                if (safewrite(fd, buffer, rc) != rc)
                    VIR_WARN
                        ("Unable to write information to local file.");

                got += rc;
                total += rc;
            }
        } while (rc > 0);

        if ((rc == LIBSSH2_ERROR_EAGAIN)
            && (got < fileinfo.st_size)) {
            /* this is due to blocking that would occur otherwise
             * so we loop on this condition */

            /* now we wait */
            if (waitsocket(sock, session) < 0 && errno != EINTR) {
                virReportSystemError(errno, "%s",
                                     _("unable to wait on libssh2 socket"));
                goto cleanup;
            }
            continue;
        }
        break;
    }
    if (VIR_CLOSE(fd) < 0) {
        virReportSystemError(errno, _("Could not close %s"),
                             local_file);
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (channel) {
        libssh2_channel_send_eof(channel);
        libssh2_channel_wait_eof(channel);
        libssh2_channel_wait_closed(channel);
        libssh2_channel_free(channel);
        channel = NULL;
    }
    VIR_FORCE_CLOSE(fd);
    return ret;
}

static int
phypUUIDTable_Init(virConnectPtr conn)
{
    uuid_tablePtr uuid_table = NULL;
    phyp_driverPtr phyp_driver;
    int nids_numdomains = 0;
    int nids_listdomains = 0;
    int *ids = NULL;
    size_t i = 0;
    int ret = -1;
    bool table_created = false;

    if ((nids_numdomains = phypConnectNumOfDomainsGeneric(conn, 2)) < 0)
        goto cleanup;

    if (VIR_ALLOC_N(ids, nids_numdomains) < 0)
        goto cleanup;

    if ((nids_listdomains =
         phypConnectListDomainsGeneric(conn, ids, nids_numdomains, 1)) < 0)
        goto cleanup;

    /* exit early if there are no domains */
    if (nids_numdomains == 0 && nids_listdomains == 0) {
        ret = 0;
        goto cleanup;
    }
    if (nids_numdomains != nids_listdomains) {
        VIR_ERROR(_("Unable to determine number of domains."));
        goto cleanup;
    }

    phyp_driver = conn->privateData;
    uuid_table = phyp_driver->uuid_table;
    uuid_table->nlpars = nids_listdomains;

    /* try to get the table from server */
    if (phypUUIDTable_Pull(conn) == -1) {
        /* file not found in the server, creating a new one */
        table_created = true;
        if (VIR_ALLOC_N(uuid_table->lpars, uuid_table->nlpars) >= 0) {
            for (i = 0; i < uuid_table->nlpars; i++) {
                if (VIR_ALLOC(uuid_table->lpars[i]) < 0)
                    goto cleanup;
                uuid_table->lpars[i]->id = ids[i];

                if (virUUIDGenerate(uuid_table->lpars[i]->uuid) < 0)
                    VIR_WARN("Unable to generate UUID for domain %d",
                             ids[i]);
            }
        } else
            goto cleanup;

        if (phypUUIDTable_WriteFile(conn) == -1)
            goto cleanup;

        if (phypUUIDTable_Push(conn) == -1)
            goto cleanup;
    } else {
        if (phypUUIDTable_ReadFile(conn) == -1)
            goto cleanup;
    }

    ret = 0;

cleanup:
    if (ret < 0 && table_created) {
        for (i = 0; i < uuid_table->nlpars; i++) {
            VIR_FREE(uuid_table->lpars[i]);
        }
        VIR_FREE(uuid_table->lpars);
    }
    VIR_FREE(ids);
    return ret;
}

static void
phypUUIDTable_Free(uuid_tablePtr uuid_table)
{
    size_t i;

    if (uuid_table == NULL)
        return;

    for (i = 0; i < uuid_table->nlpars; i++)
        VIR_FREE(uuid_table->lpars[i]);

    VIR_FREE(uuid_table->lpars);
    VIR_FREE(uuid_table);
}

#define SPECIALCHARACTER_CASES                                                \
    case '&': case ';': case '`': case '@': case '"': case '|': case '*':     \
    case '?': case '~': case '<': case '>': case '^': case '(': case ')':     \
    case '[': case ']': case '{': case '}': case '$': case '%': case '#':     \
    case '\\': case '\n': case '\r': case '\t':

static bool
contains_specialcharacters(const char *src)
{
    size_t len = strlen(src);
    size_t i = 0;

    if (len == 0)
        return false;

    for (i = 0; i < len; i++) {
        switch (src[i]) {
        SPECIALCHARACTER_CASES
            return true;
        default:
            continue;
        }
    }

    return false;
}

static char *
escape_specialcharacters(const char *src)
{
    size_t len = strlen(src);
    size_t i = 0, j = 0;
    char *dst;

    if (len == 0)
        return NULL;

    if (VIR_ALLOC_N(dst, len + 1) < 0)
        return NULL;

    for (i = 0; i < len; i++) {
        switch (src[i]) {
        SPECIALCHARACTER_CASES
            continue;
        default:
            dst[j] = src[i];
            j++;
        }
    }

    dst[j] = '\0';

    return dst;
}

static LIBSSH2_SESSION *
openSSHSession(virConnectPtr conn, virConnectAuthPtr auth,
               int *internal_socket)
{
    LIBSSH2_SESSION *session;
    const char *hostname = conn->uri->server;
    char *username = NULL;
    char *password = NULL;
    int sock = -1;
    int rc;
    struct addrinfo *ai = NULL, *cur;
    struct addrinfo hints;
    int ret;
    char *pubkey = NULL;
    char *pvtkey = NULL;
    char *userhome = virGetUserDirectory();
    struct stat pvt_stat, pub_stat;

    if (userhome == NULL)
        goto err;

    if (virAsprintf(&pubkey, "%s/.ssh/id_rsa.pub", userhome) < 0)
        goto err;

    if (virAsprintf(&pvtkey, "%s/.ssh/id_rsa", userhome) < 0)
        goto err;

    if (conn->uri->user != NULL) {
        if (VIR_STRDUP(username, conn->uri->user) < 0)
            goto err;
    } else {
        if (auth == NULL || auth->cb == NULL) {
            virReportError(VIR_ERR_AUTH_FAILED,
                           "%s", _("No authentication callback provided."));
            goto err;
        }

        username = virAuthGetUsername(conn, auth, "ssh", NULL, conn->uri->server);

        if (username == NULL) {
            virReportError(VIR_ERR_AUTH_FAILED, "%s",
                           _("Username request failed"));
            goto err;
        }
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;

    ret = getaddrinfo(hostname, "22", &hints, &ai);
    if (ret != 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Error while getting %s address info"), hostname);
        goto err;
    }

    cur = ai;
    while (cur != NULL) {
        sock = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (sock >= 0) {
            if (connect(sock, cur->ai_addr, cur->ai_addrlen) == 0) {
                goto connected;
            }
            VIR_FORCE_CLOSE(sock);
        }
        cur = cur->ai_next;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Failed to connect to %s"), hostname);
    freeaddrinfo(ai);
    goto err;

connected:

    (*internal_socket) = sock;

    /* Create a session instance */
    session = libssh2_session_init();
    if (!session)
        goto err;

    /* tell libssh2 we want it all done non-blocking */
    libssh2_session_set_blocking(session, 0);

    while ((rc = libssh2_session_startup(session, sock)) ==
           LIBSSH2_ERROR_EAGAIN);
    if (rc) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Failure establishing SSH session."));
        goto disconnect;
    }

    /* Trying authentication by pubkey */
    if (stat(pvtkey, &pvt_stat) || stat(pubkey, &pub_stat)) {
        rc = LIBSSH2_ERROR_SOCKET_NONE;
        goto keyboard_interactive;
    }

    while ((rc =
            libssh2_userauth_publickey_fromfile(session, username,
                                                pubkey,
                                                pvtkey,
                                                NULL)) ==
           LIBSSH2_ERROR_EAGAIN);

keyboard_interactive:
    if (rc == LIBSSH2_ERROR_SOCKET_NONE
        || rc == LIBSSH2_ERROR_PUBLICKEY_UNRECOGNIZED
        || rc == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED) {
        if (auth == NULL || auth->cb == NULL) {
            virReportError(VIR_ERR_AUTH_FAILED,
                           "%s", _("No authentication callback provided."));
            goto disconnect;
        }

        password = virAuthGetPassword(conn, auth, "ssh", username, conn->uri->server);

        if (password == NULL) {
            virReportError(VIR_ERR_AUTH_FAILED, "%s",
                           _("Password request failed"));
            goto disconnect;
        }

        while ((rc =
                libssh2_userauth_password(session, username,
                                          password)) ==
               LIBSSH2_ERROR_EAGAIN);

        if (rc) {
            virReportError(VIR_ERR_AUTH_FAILED,
                           "%s", _("Authentication failed"));
            goto disconnect;
        } else
            goto exit;

    } else if (rc == LIBSSH2_ERROR_NONE) {
        goto exit;

    } else if (rc == LIBSSH2_ERROR_ALLOC || rc == LIBSSH2_ERROR_SOCKET_SEND
               || rc == LIBSSH2_ERROR_SOCKET_TIMEOUT) {
        goto err;
    }

disconnect:
    libssh2_session_disconnect(session, "Disconnecting...");
    libssh2_session_free(session);
err:
    VIR_FORCE_CLOSE(sock);
    VIR_FREE(userhome);
    VIR_FREE(pubkey);
    VIR_FREE(pvtkey);
    VIR_FREE(username);
    VIR_FREE(password);
    return NULL;

exit:
    VIR_FREE(userhome);
    VIR_FREE(pubkey);
    VIR_FREE(pvtkey);
    VIR_FREE(username);
    VIR_FREE(password);
    return session;
}

static virDrvOpenStatus
phypConnectOpen(virConnectPtr conn,
                virConnectAuthPtr auth, unsigned int flags)
{
    LIBSSH2_SESSION *session = NULL;
    ConnectionData *connection_data = NULL;
    int internal_socket;
    uuid_tablePtr uuid_table = NULL;
    phyp_driverPtr phyp_driver = NULL;
    char *char_ptr;
    char *managed_system = NULL;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (!conn || !conn->uri)
        return VIR_DRV_OPEN_DECLINED;

    if (conn->uri->scheme == NULL || STRNEQ(conn->uri->scheme, "phyp"))
        return VIR_DRV_OPEN_DECLINED;

    if (conn->uri->server == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Missing server name in phyp:// URI"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (VIR_ALLOC(phyp_driver) < 0)
        goto failure;

    if (VIR_ALLOC(uuid_table) < 0)
        goto failure;

    if (VIR_ALLOC(connection_data) < 0)
        goto failure;
    connection_data->sock = -1;

    if (conn->uri->path) {
        /* need to shift one byte in order to remove the first "/" of URI component */
        if (VIR_STRDUP(managed_system,
                       conn->uri->path + (conn->uri->path[0] == '/')) < 0)
            goto failure;

        /* here we are handling only the first component of the path,
         * so skipping the second:
         * */
        char_ptr = strchr(managed_system, '/');

        if (char_ptr)
            *char_ptr = '\0';

        if (contains_specialcharacters(conn->uri->path)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s",
                           _("Error parsing 'path'. Invalid characters."));
            goto failure;
        }
    }

    if ((session = openSSHSession(conn, auth, &internal_socket)) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Error while opening SSH session."));
        goto failure;
    }

    connection_data->session = session;
    connection_data->sock = internal_socket;

    uuid_table->nlpars = 0;
    uuid_table->lpars = NULL;

    if (conn->uri->path)
        phyp_driver->managed_system = managed_system;

    phyp_driver->uuid_table = uuid_table;
    if ((phyp_driver->caps = phypCapsInit()) == NULL)
        goto failure;

    if (!(phyp_driver->xmlopt = virDomainXMLOptionNew(NULL, NULL, NULL)))
        goto failure;

    conn->privateData = phyp_driver;
    conn->networkPrivateData = connection_data;

    if ((phyp_driver->system_type = phypGetSystemType(conn)) == -1)
        goto failure;

    if (phypUUIDTable_Init(conn) == -1)
        goto failure;

    if (phyp_driver->system_type == HMC) {
        if ((phyp_driver->vios_id = phypGetVIOSPartitionID(conn)) == -1)
            goto failure;
    }

    return VIR_DRV_OPEN_SUCCESS;

failure:
    VIR_FREE(managed_system);

    if (phyp_driver != NULL) {
        virObjectUnref(phyp_driver->caps);
        virObjectUnref(phyp_driver->xmlopt);
        VIR_FREE(phyp_driver);
    }

    phypUUIDTable_Free(uuid_table);

    if (session != NULL) {
        libssh2_session_disconnect(session, "Disconnecting...");
        libssh2_session_free(session);
    }

    if (connection_data)
        VIR_FORCE_CLOSE(connection_data->sock);
    VIR_FREE(connection_data);

    return VIR_DRV_OPEN_ERROR;
}

static int
phypConnectClose(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;

    libssh2_session_disconnect(session, "Disconnecting...");
    libssh2_session_free(session);

    virObjectUnref(phyp_driver->caps);
    virObjectUnref(phyp_driver->xmlopt);
    phypUUIDTable_Free(phyp_driver->uuid_table);
    VIR_FREE(phyp_driver->managed_system);
    VIR_FREE(phyp_driver);

    VIR_FORCE_CLOSE(connection_data->sock);
    VIR_FREE(connection_data);
    return 0;
}


static int
phypConnectIsEncrypted(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Phyp uses an SSH tunnel, so is always encrypted */
    return 1;
}


static int
phypConnectIsSecure(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Phyp uses an SSH tunnel, so is always secure */
    return 1;
}


static int
phypConnectIsAlive(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;

    /* XXX we should be able to do something better but this is simple, safe,
     * and good enough for now. In worst case, the function will return true
     * even though the connection is not alive.
     */
    if (connection_data && connection_data->session)
        return 1;
    else
        return 0;
}


static int
phypDomainIsUpdated(virDomainPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

/* return the lpar_id given a name and a managed system name */
static int
phypGetLparID(LIBSSH2_SESSION * session, const char *managed_system,
              const char *name, virConnectPtr conn)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    int lpar_id = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lssyscfg -r lpar");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " --filter lpar_names=%s -F lpar_id", name);
    phypExecInt(session, &buf, conn, &lpar_id);
    return lpar_id;
}

/* return the lpar name given a lpar_id and a managed system name */
static char *
phypGetLparNAME(LIBSSH2_SESSION * session, const char *managed_system,
                unsigned int lpar_id, virConnectPtr conn)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    char *ret = NULL;
    int exit_status = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lssyscfg -r lpar");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " --filter lpar_ids=%d -F name", lpar_id);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0)
        VIR_FREE(ret);
    return ret;
}


/* Search into the uuid_table for a lpar_uuid given a lpar_id
 * and a managed system name
 *
 * return:  0 - record found
 *         -1 - not found
 * */
static int
phypGetLparUUID(unsigned char *uuid, int lpar_id, virConnectPtr conn)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    uuid_tablePtr uuid_table = phyp_driver->uuid_table;
    lparPtr *lpars = uuid_table->lpars;
    size_t i = 0;

    for (i = 0; i < uuid_table->nlpars; i++) {
        if (lpars[i]->id == lpar_id) {
            memcpy(uuid, lpars[i]->uuid, VIR_UUID_BUFLEN);
            return 0;
        }
    }

    return -1;
}

/*
 * type:
 * 0 - maxmem
 * 1 - memory
 * */
static unsigned long
phypGetLparMem(virConnectPtr conn, const char *managed_system, int lpar_id,
               int type)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    int memory = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (type != 1 && type != 0)
        return 0;

    virBufferAddLit(&buf, "lshwres");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -r mem --level lpar -F %s --filter lpar_ids=%d",
                      type ? "curr_mem" : "curr_max_mem", lpar_id);
    phypExecInt(session, &buf, conn, &memory);
    return memory;
}

static unsigned long
phypGetLparCPUGeneric(virConnectPtr conn, const char *managed_system,
                      int lpar_id, int type)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    int vcpus = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lshwres");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -r proc --level lpar -F %s --filter lpar_ids=%d",
                      type ? "curr_max_procs" : "curr_procs", lpar_id);
    phypExecInt(session, &buf, conn, &vcpus);
    return vcpus;
}

static unsigned long
phypGetLparCPU(virConnectPtr conn, const char *managed_system, int lpar_id)
{
    return phypGetLparCPUGeneric(conn, managed_system, lpar_id, 0);
}

static int
phypDomainGetVcpusFlags(virDomainPtr dom, unsigned int flags)
{
    phyp_driverPtr phyp_driver = dom->conn->privateData;
    char *managed_system = phyp_driver->managed_system;

    if (flags != (VIR_DOMAIN_VCPU_LIVE | VIR_DOMAIN_VCPU_MAXIMUM)) {
        virReportError(VIR_ERR_INVALID_ARG, _("unsupported flags: (0x%x)"), flags);
        return -1;
    }

    return phypGetLparCPUGeneric(dom->conn, managed_system, dom->id, 1);
}

static int
phypDomainGetMaxVcpus(virDomainPtr dom)
{
    return phypDomainGetVcpusFlags(dom, (VIR_DOMAIN_VCPU_LIVE |
                                         VIR_DOMAIN_VCPU_MAXIMUM));
}

static int
phypGetRemoteSlot(virConnectPtr conn, const char *managed_system,
                  const char *lpar_name)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    int remote_slot = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lshwres");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r virtualio --rsubtype scsi -F "
                      "remote_slot_num --filter lpar_names=%s", lpar_name);
    phypExecInt(session, &buf, conn, &remote_slot);
    return remote_slot;
}

/* XXX - is this needed? */
static char *phypGetBackingDevice(virConnectPtr, const char *, char *)
    ATTRIBUTE_UNUSED;
static char *
phypGetBackingDevice(virConnectPtr conn, const char *managed_system,
                     char *lpar_name)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    char *ret = NULL;
    int remote_slot = 0;
    int exit_status = 0;
    char *char_ptr;
    char *backing_device = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if ((remote_slot =
         phypGetRemoteSlot(conn, managed_system, lpar_name)) == -1)
        return NULL;

    virBufferAddLit(&buf, "lshwres");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r virtualio --rsubtype scsi -F "
                      "backing_devices --filter slots=%d", remote_slot);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    /* here is a little trick to deal returns of this kind:
     *
     * 0x8100000000000000//lv01
     *
     * the information we really need is only lv01, so we
     * need to skip a lot of things on the string.
     * */
    char_ptr = strchr(ret, '/');

    if (char_ptr) {
        char_ptr++;
        if (char_ptr[0] == '/')
            char_ptr++;
        else
            goto cleanup;

        if (VIR_STRDUP(backing_device, char_ptr) < 0)
            goto cleanup;
    } else {
        backing_device = ret;
        ret = NULL;
    }

    char_ptr = strchr(backing_device, '\n');

    if (char_ptr)
        *char_ptr = '\0';

cleanup:
    VIR_FREE(ret);

    return backing_device;
}

static char *
phypGetLparProfile(virConnectPtr conn, int lpar_id)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lssyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -r prof --filter lpar_ids=%d -F name|head -n 1",
                      lpar_id);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0)
        VIR_FREE(ret);
    return ret;
}

static int
phypGetVIOSNextSlotNumber(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    char *profile = NULL;
    int slot = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!(profile = phypGetLparProfile(conn, vios_id))) {
        VIR_ERROR(_("Unable to get VIOS profile name."));
        return -1;
    }

    virBufferAddLit(&buf, "lssyscfg");

    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);

    virBufferAsprintf(&buf, " -r prof --filter "
                      "profile_names=%s -F virtual_eth_adapters,"
                      "virtual_opti_pool_id,virtual_scsi_adapters,"
                      "virtual_serial_adapters|sed -e 's/\"//g' -e "
                      "'s/,/\\n/g'|sed -e 's/\\(^[0-9][0-9]\\*\\).*$/\\1/'"
                      "|sort|tail -n 1", profile);
    if (phypExecInt(session, &buf, conn, &slot) < 0)
        return -1;
    return slot + 1;
}

static int
phypCreateServerSCSIAdapter(virConnectPtr conn)
{
    int result = -1;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    char *profile = NULL;
    int slot = 0;
    char *vios_name = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!
        (vios_name =
         phypGetLparNAME(session, managed_system, vios_id, conn))) {
        VIR_ERROR(_("Unable to get VIOS name"));
        goto cleanup;
    }

    if (!(profile = phypGetLparProfile(conn, vios_id))) {
        VIR_ERROR(_("Unable to get VIOS profile name."));
        goto cleanup;
    }

    if ((slot = phypGetVIOSNextSlotNumber(conn)) == -1) {
        VIR_ERROR(_("Unable to get free slot number"));
        goto cleanup;
    }

    /* Listing all the virtual_scsi_adapter interfaces, the new adapter must
     * be appended to this list
     * */
    virBufferAddLit(&buf, "lssyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r prof --filter lpar_ids=%d,profile_names=%s"
                      " -F virtual_scsi_adapters|sed -e s/\\\"//g",
                      vios_id, profile);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    /* Here I change the VIOS configuration to append the new adapter
     * with the free slot I got with phypGetVIOSNextSlotNumber.
     * */
    virBufferAddLit(&buf, "chsyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r prof -i 'name=%s,lpar_id=%d,"
                      "\"virtual_scsi_adapters=%s,%d/server/any/any/1\"'",
                      vios_name, vios_id, ret, slot);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    /* Finally I add the new scsi adapter to VIOS using the same slot
     * I used in the VIOS configuration.
     * */
    virBufferAddLit(&buf, "chhwres -r virtualio --rsubtype scsi");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -p %s -o a -s %d -d 0 -a \"adapter_type=server\"",
                      vios_name, slot);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    result = 0;

cleanup:
    VIR_FREE(profile);
    VIR_FREE(vios_name);
    VIR_FREE(ret);

    return result;
}

static char *
phypGetVIOSFreeSCSIAdapter(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAddLit(&buf, "lsmap -all -field svsa backing -fmt , ");

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed '/,[^.*]/d; s/,//g; q'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0)
        VIR_FREE(ret);
    return ret;
}


static int
phypDomainAttachDevice(virDomainPtr domain, const char *xml)
{
    int result = -1;
    virConnectPtr conn = domain->conn;
    ConnectionData *connection_data = domain->conn->networkPrivateData;
    phyp_driverPtr phyp_driver = domain->conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    char *scsi_adapter = NULL;
    int slot = 0;
    char *vios_name = NULL;
    char *profile = NULL;
    virDomainDeviceDefPtr dev = NULL;
    virDomainDefPtr def = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *domain_name = NULL;

    if (VIR_ALLOC(def) < 0)
        goto cleanup;

    domain_name = escape_specialcharacters(domain->name);

    if (domain_name == NULL) {
        goto cleanup;
    }

    if (VIR_STRDUP(def->os.type, "aix") < 0)
        goto cleanup;

    dev = virDomainDeviceDefParse(xml, def, phyp_driver->caps, NULL,
                                  VIR_DOMAIN_XML_INACTIVE);
    if (!dev) {
        goto cleanup;
    }

    if (!
        (vios_name =
         phypGetLparNAME(session, managed_system, vios_id, conn))) {
        VIR_ERROR(_("Unable to get VIOS name"));
        goto cleanup;
    }

    /* First, let's look for a free SCSI Adapter
     * */
    if (!(scsi_adapter = phypGetVIOSFreeSCSIAdapter(conn))) {
        /* If not found, let's create one.
         * */
        if (phypCreateServerSCSIAdapter(conn) == -1) {
            VIR_ERROR(_("Unable to create new virtual adapter"));
            goto cleanup;
        } else {
            if (!(scsi_adapter = phypGetVIOSFreeSCSIAdapter(conn))) {
                VIR_ERROR(_("Unable to create new virtual adapter"));
                goto cleanup;
            }
        }
    }

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "mkvdev -vdev %s -vadapter %s",
                      dev->data.disk->src, scsi_adapter);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    if (!(profile = phypGetLparProfile(conn, domain->id))) {
        VIR_ERROR(_("Unable to get VIOS profile name."));
        goto cleanup;
    }

    /* Let's get the slot number for the adapter we just created
     * */
    virBufferAddLit(&buf, "lshwres -r virtualio --rsubtype scsi");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " slot_num,backing_device|grep %s|cut -d, -f1",
                      dev->data.disk->src);
    if (phypExecInt(session, &buf, conn, &slot) < 0)
        goto cleanup;

    /* Listing all the virtual_scsi_adapter interfaces, the new adapter must
     * be appended to this list
     * */
    virBufferAddLit(&buf, "lssyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -r prof --filter lpar_ids=%d,profile_names=%s"
                      " -F virtual_scsi_adapters|sed -e 's/\"//g'",
                      vios_id, profile);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    /* Here I change the LPAR configuration to append the new adapter
     * with the new slot we just created
     * */
    virBufferAddLit(&buf, "chsyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -r prof -i 'name=%s,lpar_id=%d,"
                      "\"virtual_scsi_adapters=%s,%d/client/%d/%s/0\"'",
                      domain_name, domain->id, ret, slot,
                      vios_id, vios_name);
    if (phypExecInt(session, &buf, conn, &slot) < 0)
        goto cleanup;

    /* Finally I add the new scsi adapter to VIOS using the same slot
     * I used in the VIOS configuration.
     * */
    virBufferAddLit(&buf, "chhwres -r virtualio --rsubtype scsi");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -p %s -o a -s %d -d 0 -a \"adapter_type=server\"",
                      domain_name, slot);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL) {
        VIR_ERROR(_
                   ("Possibly you don't have IBM Tools installed in your LPAR."
                    "Contact your support to enable this feature."));
        goto cleanup;
    }

    result = 0;

cleanup:
    VIR_FREE(ret);
    virDomainDeviceDefFree(dev);
    virDomainDefFree(def);
    VIR_FREE(vios_name);
    VIR_FREE(scsi_adapter);
    VIR_FREE(profile);
    VIR_FREE(domain_name);

    return result;
}

static char *
phypStorageVolGetKey(virConnectPtr conn, const char *name)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lslv %s -field lvid", name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed -e 's/^LV IDENTIFIER://' -e 's/ //g'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0)
        VIR_FREE(ret);
    return ret;
}

static char *
phypGetStoragePoolDevice(virConnectPtr conn, char *name)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lssp -detail -sp %s -field name", name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed '1d; s/ //g'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0)
        VIR_FREE(ret);
    return ret;
}

static unsigned long int
phypGetStoragePoolSize(virConnectPtr conn, char *name)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int sp_size = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lssp -detail -sp %s -field size", name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed '1d; s/ //g'");
    phypExecInt(session, &buf, conn, &sp_size);
    return sp_size;
}

static char *
phypBuildVolume(virConnectPtr conn, const char *lvname, const char *spname,
                unsigned int capacity)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int vios_id = phyp_driver->vios_id;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    char *ret = NULL;
    int exit_status = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *key = NULL;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "mklv -lv %s %s %d", lvname, spname, capacity);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0) {
        VIR_ERROR(_("Unable to create Volume: %s"), NULLSTR(ret));
        goto cleanup;
    }

    key = phypStorageVolGetKey(conn, lvname);

cleanup:
    VIR_FREE(ret);

    return key;
}

static virStorageVolPtr
phypStorageVolLookupByName(virStoragePoolPtr pool, const char *volname)
{
    char *key;
    virStorageVolPtr vol;

    key = phypStorageVolGetKey(pool->conn, volname);

    if (key == NULL)
        return NULL;

    vol = virGetStorageVol(pool->conn, pool->name, volname, key, NULL, NULL);

    VIR_FREE(key);

    return vol;
}

static virStorageVolPtr
phypStorageVolCreateXML(virStoragePoolPtr pool,
                        const char *xml, unsigned int flags)
{
    virCheckFlags(0, NULL);

    virStorageVolDefPtr voldef = NULL;
    virStoragePoolDefPtr spdef = NULL;
    virStorageVolPtr vol = NULL;
    virStorageVolPtr dup_vol = NULL;
    char *key = NULL;

    if (VIR_ALLOC(spdef) < 0)
        return NULL;

    /* Filling spdef manually
     * */
    if (pool->name != NULL) {
        spdef->name = pool->name;
    } else {
        VIR_ERROR(_("Unable to determine storage pool's name."));
        goto err;
    }

    if (memcpy(spdef->uuid, pool->uuid, VIR_UUID_BUFLEN) == NULL) {
        VIR_ERROR(_("Unable to determine storage pool's uuid."));
        goto err;
    }

    if ((spdef->capacity =
         phypGetStoragePoolSize(pool->conn, pool->name)) == -1) {
        VIR_ERROR(_("Unable to determine storage pools's size."));
        goto err;
    }

    /* Information not available */
    spdef->allocation = 0;
    spdef->available = 0;

    spdef->source.ndevice = 1;

    /*XXX source adapter not working properly, should show hdiskX */
    if ((spdef->source.adapter.data.name =
         phypGetStoragePoolDevice(pool->conn, pool->name)) == NULL) {
        VIR_ERROR(_("Unable to determine storage pools's source adapter."));
        goto err;
    }

    if ((voldef = virStorageVolDefParseString(spdef, xml)) == NULL) {
        VIR_ERROR(_("Error parsing volume XML."));
        goto err;
    }

    /* checking if this name already exists on this system */
    if ((dup_vol = phypStorageVolLookupByName(pool, voldef->name)) != NULL) {
        VIR_ERROR(_("StoragePool name already exists."));
        virObjectUnref(dup_vol);
        goto err;
    }

    /* The key must be NULL, the Power Hypervisor creates a key
     * in the moment you create the volume.
     * */
    if (voldef->key) {
        VIR_ERROR(_("Key must be empty, Power Hypervisor will create one for you."));
        goto err;
    }

    if (voldef->capacity) {
        VIR_ERROR(_("Capacity cannot be empty."));
        goto err;
    }

    key = phypBuildVolume(pool->conn, voldef->name, spdef->name,
                          voldef->capacity);

    if (key == NULL)
        goto err;

    if ((vol =
         virGetStorageVol(pool->conn, pool->name, voldef->name,
                          key, NULL, NULL)) == NULL)
        goto err;

    VIR_FREE(key);

    return vol;

err:
    VIR_FREE(key);
    virStorageVolDefFree(voldef);
    virStoragePoolDefFree(spdef);
    virObjectUnref(vol);
    return NULL;
}

static char *
phypStorageVolGetPhysicalVolumeByStoragePool(virStorageVolPtr vol, char *sp)
{
    virConnectPtr conn = vol->conn;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lssp -detail -sp %s -field pvname", sp);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed 1d");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0)
        VIR_FREE(ret);
    return ret;
}

static virStorageVolPtr
phypStorageVolLookupByPath(virConnectPtr conn, const char *volname)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    char *key = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virStorageVolPtr vol = NULL;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lslv %s -field vgname", volname);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed -e 's/^VOLUME GROUP://g' -e 's/ //g'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    key = phypStorageVolGetKey(conn, volname);

    if (key == NULL)
        goto cleanup;

    vol = virGetStorageVol(conn, ret, volname, key, NULL, NULL);

cleanup:
    VIR_FREE(ret);
    VIR_FREE(key);

    return vol;
}

static int
phypGetStoragePoolUUID(virConnectPtr conn, unsigned char *uuid,
                       const char *name)
{
    int result = -1;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lsdev -dev %s -attr vgserial_id", name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed '1,2d'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    if (memcpy(uuid, ret, VIR_UUID_BUFLEN) == NULL)
        goto cleanup;

    result = 0;

cleanup:
    VIR_FREE(ret);

    return result;
}

static virStoragePoolPtr
phypStoragePoolLookupByName(virConnectPtr conn, const char *name)
{
    unsigned char uuid[VIR_UUID_BUFLEN];

    if (phypGetStoragePoolUUID(conn, uuid, name) == -1)
        return NULL;

    return virGetStoragePool(conn, name, uuid, NULL, NULL);
}

static char *
phypStorageVolGetXMLDesc(virStorageVolPtr vol, unsigned int flags)
{
    virStorageVolDef voldef;
    virStoragePoolDef pool;
    virStoragePoolPtr sp;
    char *xml = NULL;

    virCheckFlags(0, NULL);

    memset(&voldef, 0, sizeof(virStorageVolDef));
    memset(&pool, 0, sizeof(virStoragePoolDef));

    sp = phypStoragePoolLookupByName(vol->conn, vol->pool);

    if (!sp)
        goto cleanup;

    if (sp->name != NULL) {
        pool.name = sp->name;
    } else {
        VIR_ERROR(_("Unable to determine storage sp's name."));
        goto cleanup;
    }

    if (memcpy(pool.uuid, sp->uuid, VIR_UUID_BUFLEN) == NULL) {
        VIR_ERROR(_("Unable to determine storage sp's uuid."));
        goto cleanup;
    }

    if ((pool.capacity = phypGetStoragePoolSize(sp->conn, sp->name)) == -1) {
        VIR_ERROR(_("Unable to determine storage sps's size."));
        goto cleanup;
    }

    /* Information not available */
    pool.allocation = 0;
    pool.available = 0;

    pool.source.ndevice = 1;

    if ((pool.source.adapter.data.name =
         phypGetStoragePoolDevice(sp->conn, sp->name)) == NULL) {
        VIR_ERROR(_("Unable to determine storage sps's source adapter."));
        goto cleanup;
    }

    if (vol->name != NULL)
        voldef.name = vol->name;
    else {
        VIR_ERROR(_("Unable to determine storage pool's name."));
        goto cleanup;
    }

    if (VIR_STRDUP(voldef.key, vol->key) < 0)
        goto cleanup;

    voldef.type = VIR_STORAGE_POOL_LOGICAL;

    xml = virStorageVolDefFormat(&pool, &voldef);

    VIR_FREE(voldef.key);

cleanup:
    virObjectUnref(sp);
    return xml;
}

/* The Volume Group path here will be treated as suggested in the
 * email on the libvirt mailling list. As soon as I can't get the
 * path for every volume, the path will be a representation in
 * the form:
 *
 * /physical_volume/storage_pool/logical_volume
 *
 * */
static char *
phypStorageVolGetPath(virStorageVolPtr vol)
{
    virConnectPtr conn = vol->conn;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    char *ret = NULL;
    char *path = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *pv;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lslv %s -field vgname", vol->name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAsprintf(&buf,
                      "|sed -e 's/^VOLUME GROUP://g' -e 's/ //g'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    pv = phypStorageVolGetPhysicalVolumeByStoragePool(vol, ret);

    if (!pv)
        goto cleanup;

    if (virAsprintf(&path, "/%s/%s/%s", pv, ret, vol->name) < 0)
        goto cleanup;

cleanup:
    VIR_FREE(ret);
    VIR_FREE(path);

    return path;
}

static int
phypStoragePoolListVolumes(virStoragePoolPtr pool, char **const volumes,
                           int nvolumes)
{
    bool success = false;
    virConnectPtr conn = pool->conn;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    int got = 0;
    size_t i;
    char *ret = NULL;
    char *volumes_list = NULL;
    char *char_ptr = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "lsvg -lv %s -field lvname", pool->name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|sed '1,2d'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    /* I need to parse the textual return in order to get the volumes */
    if (exit_status < 0 || ret == NULL)
        goto cleanup;
    else {
        volumes_list = ret;

        while (got < nvolumes) {
            char_ptr = strchr(volumes_list, '\n');

            if (char_ptr) {
                *char_ptr = '\0';
                if (VIR_STRDUP(volumes[got++], volumes_list) < 0)
                    goto cleanup;
                char_ptr++;
                volumes_list = char_ptr;
            } else
                break;
        }
    }

    success = true;

cleanup:
    if (!success) {
        for (i = 0; i < got; i++)
            VIR_FREE(volumes[i]);

        got = -1;
    }
    VIR_FREE(ret);
    return got;
}

static int
phypStoragePoolNumOfVolumes(virStoragePoolPtr pool)
{
    virConnectPtr conn = pool->conn;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    int nvolumes = -1;
    char *managed_system = phyp_driver->managed_system;
    int vios_id = phyp_driver->vios_id;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);
    virBufferAsprintf(&buf, "lsvg -lv %s -field lvname", pool->name);
    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');
    virBufferAddLit(&buf, "|grep -c '^.*$'");
    if (phypExecInt(session, &buf, conn, &nvolumes) < 0)
        return -1;

    /* We need to remove 2 line from the header text output */
    return nvolumes - 2;
}

static int
phypStoragePoolDestroy(virStoragePoolPtr pool)
{
    int result = -1;
    virConnectPtr conn = pool->conn;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int vios_id = phyp_driver->vios_id;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    char *ret = NULL;
    int exit_status = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "rmsp %s", pool->name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0) {
        VIR_ERROR(_("Unable to destroy Storage Pool: %s"), NULLSTR(ret));
        goto cleanup;
    }

    result = 0;

cleanup:
    VIR_FREE(ret);

    return result;
}

static int
phypBuildStoragePool(virConnectPtr conn, virStoragePoolDefPtr def)
{
    int result = -1;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virStoragePoolSource source = def->source;
    int vios_id = phyp_driver->vios_id;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    char *ret = NULL;
    int exit_status = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (source.adapter.type !=
        VIR_STORAGE_POOL_SOURCE_ADAPTER_TYPE_SCSI_HOST) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Only 'scsi_host' adapter is supported"));
        goto cleanup;
    }

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAsprintf(&buf, "mksp -f %schild %s", def->name,
                      source.adapter.data.name);

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0) {
        VIR_ERROR(_("Unable to create Storage Pool: %s"), NULLSTR(ret));
        goto cleanup;
    }

    result = 0;

cleanup:
    VIR_FREE(ret);

    return result;

}

static int
phypConnectNumOfStoragePools(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    int nsp = -1;
    char *managed_system = phyp_driver->managed_system;
    int vios_id = phyp_driver->vios_id;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAddLit(&buf, "lsvg");

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');

    virBufferAddLit(&buf, "|grep -c '^.*$'");
    phypExecInt(session, &buf, conn, &nsp);
    return nsp;
}

static int
phypConnectListStoragePools(virConnectPtr conn, char **const pools, int npools)
{
    bool success = false;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    int got = 0;
    size_t i;
    char *ret = NULL;
    char *storage_pools = NULL;
    char *char_ptr = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (system_type == HMC)
        virBufferAsprintf(&buf, "viosvrcmd -m %s --id %d -c '",
                          managed_system, vios_id);

    virBufferAddLit(&buf, "lsvg");

    if (system_type == HMC)
        virBufferAddChar(&buf, '\'');
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    /* I need to parse the textual return in order to get the storage pools */
    if (exit_status < 0 || ret == NULL)
        goto cleanup;
    else {
        storage_pools = ret;

        while (got < npools) {
            char_ptr = strchr(storage_pools, '\n');

            if (char_ptr) {
                *char_ptr = '\0';
                if (VIR_STRDUP(pools[got++], storage_pools) < 0)
                    goto cleanup;
                char_ptr++;
                storage_pools = char_ptr;
            } else
                break;
        }
    }

    success = true;

cleanup:
    if (!success) {
        for (i = 0; i < got; i++)
            VIR_FREE(pools[i]);

        got = -1;
    }
    VIR_FREE(ret);
    return got;
}

static virStoragePoolPtr
phypStoragePoolLookupByUUID(virConnectPtr conn,
                            const unsigned char *uuid)
{
    virStoragePoolPtr sp = NULL;
    int npools = 0;
    int gotpools = 0;
    char **pools = NULL;
    size_t i = 0;
    unsigned char *local_uuid = NULL;

    if (VIR_ALLOC_N(local_uuid, VIR_UUID_BUFLEN) < 0)
        goto err;

    if ((npools = phypConnectNumOfStoragePools(conn)) == -1)
        goto err;

    if (VIR_ALLOC_N(pools, npools) < 0)
        goto err;

    if ((gotpools = phypConnectListStoragePools(conn, pools, npools)) == -1)
        goto err;

    if (gotpools != npools) {
        virReportOOMError();
        goto err;
    }

    for (i = 0; i < gotpools; i++) {
        if (phypGetStoragePoolUUID(conn, local_uuid, pools[i]) == -1)
            continue;

        if (!memcmp(local_uuid, uuid, VIR_UUID_BUFLEN)) {
            sp = virGetStoragePool(conn, pools[i], uuid, NULL, NULL);
            VIR_FREE(local_uuid);
            VIR_FREE(pools);

            if (sp)
                return sp;
            else
                goto err;
        }
    }

err:
    VIR_FREE(local_uuid);
    VIR_FREE(pools);
    return NULL;
}

static virStoragePoolPtr
phypStoragePoolCreateXML(virConnectPtr conn,
                         const char *xml, unsigned int flags)
{
    virCheckFlags(0, NULL);

    virStoragePoolDefPtr def = NULL;
    virStoragePoolPtr dup_sp = NULL;
    virStoragePoolPtr sp = NULL;

    if (!(def = virStoragePoolDefParseString(xml)))
        goto err;

    /* checking if this name already exists on this system */
    if ((dup_sp = phypStoragePoolLookupByName(conn, def->name)) != NULL) {
        VIR_WARN("StoragePool name already exists.");
        virObjectUnref(dup_sp);
        goto err;
    }

    /* checking if ID or UUID already exists on this system */
    if ((dup_sp = phypStoragePoolLookupByUUID(conn, def->uuid)) != NULL) {
        VIR_WARN("StoragePool uuid already exists.");
        virObjectUnref(dup_sp);
        goto err;
    }

    if ((sp = virGetStoragePool(conn, def->name, def->uuid, NULL, NULL)) == NULL)
        goto err;

    if (phypBuildStoragePool(conn, def) == -1)
        goto err;

    return sp;

err:
    virStoragePoolDefFree(def);
    virObjectUnref(sp);
    return NULL;
}

static char *
phypStoragePoolGetXMLDesc(virStoragePoolPtr pool, unsigned int flags)
{
    virCheckFlags(0, NULL);

    virStoragePoolDef def;
    memset(&def, 0, sizeof(virStoragePoolDef));

    if (pool->name != NULL)
        def.name = pool->name;
    else {
        VIR_ERROR(_("Unable to determine storage pool's name."));
        goto err;
    }

    if (memcpy(def.uuid, pool->uuid, VIR_UUID_BUFLEN) == NULL) {
        VIR_ERROR(_("Unable to determine storage pool's uuid."));
        goto err;
    }

    if ((def.capacity =
         phypGetStoragePoolSize(pool->conn, pool->name)) == -1) {
        VIR_ERROR(_("Unable to determine storage pools's size."));
        goto err;
    }

    /* Information not available */
    def.allocation = 0;
    def.available = 0;

    def.source.ndevice = 1;

    /*XXX source adapter not working properly, should show hdiskX */
    if ((def.source.adapter.data.name =
         phypGetStoragePoolDevice(pool->conn, pool->name)) == NULL) {
        VIR_ERROR(_("Unable to determine storage pools's source adapter."));
        goto err;
    }

    return virStoragePoolDefFormat(&def);

err:
    return NULL;
}

static int
phypInterfaceDestroy(virInterfacePtr iface,
                     unsigned int flags)
{
    virCheckFlags(0, -1);

    ConnectionData *connection_data = iface->conn->networkPrivateData;
    phyp_driverPtr phyp_driver = iface->conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int exit_status = 0;
    int slot_num = 0;
    int lpar_id = 0;
    char *ret = NULL;
    int rv = -1;

    /* Getting the remote slot number */

    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype eth --level lpar "
                      " -F mac_addr,slot_num|"
                      " sed -n '/%s/ s/^.*,//p'", iface->mac);
    if (phypExecInt(session, &buf, iface->conn, &slot_num) < 0)
        goto cleanup;

    /* Getting the remote slot number */
    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype eth --level lpar "
                      " -F mac_addr,lpar_id|"
                      " sed -n '/%s/ s/^.*,//p'", iface->mac);
    if (phypExecInt(session, &buf, iface->conn, &lpar_id) < 0)
        goto cleanup;

    /* excluding interface */
    virBufferAddLit(&buf, "chhwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype eth"
                      " --id %d -o r -s %d", lpar_id, slot_num);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, iface->conn, false);

    if (exit_status < 0 || ret != NULL)
        goto cleanup;

    rv = 0;

cleanup:
    VIR_FREE(ret);
    return rv;
}

static virInterfacePtr
phypInterfaceDefineXML(virConnectPtr conn, const char *xml,
                       unsigned int flags)
{
    virCheckFlags(0, NULL);

    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int exit_status = 0;
    int slot = 0;
    char *ret = NULL;
    char name[PHYP_IFACENAME_SIZE];
    char mac[PHYP_MAC_SIZE];
    virInterfaceDefPtr def;
    virInterfacePtr result = NULL;

    if (!(def = virInterfaceDefParseString(xml)))
        goto cleanup;

    /* Now need to get the next free slot number */
    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype slot --level slot"
                      " -Fslot_num --filter lpar_names=%s"
                      " |sort|tail -n 1", def->name);
    if (phypExecInt(session, &buf, conn, &slot) < 0)
        goto cleanup;

    /* The next free slot itself: */
    slot++;

    /* Now adding the new network interface */
    virBufferAddLit(&buf, "chhwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype eth"
                      " -p %s -o a -s %d -a port_vlan_id=1,"
                      "ieee_virtual_eth=0", def->name, slot);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret != NULL)
        goto cleanup;

    /* Need to sleep a little while to wait for the HMC to
     * complete the execution of the command.
     * */
    sleep(1);

    /* Getting the new interface name */
    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype slot --level slot"
                      " |sed '/lpar_name=%s/!d; /slot_num=%d/!d; "
                      "s/^.*drc_name=//'", def->name, slot);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL) {
        /* roll back and excluding interface if error*/
        virBufferAddLit(&buf, "chhwres ");
        if (system_type == HMC)
            virBufferAsprintf(&buf, "-m %s ", managed_system);

        virBufferAsprintf(&buf,
                " -r virtualio --rsubtype eth"
                " -p %s -o r -s %d", def->name, slot);
        VIR_FREE(ret);
        ret = phypExecBuffer(session, &buf, &exit_status, conn, false);
        goto cleanup;
    }

    memcpy(name, ret, PHYP_IFACENAME_SIZE-1);

    /* Getting the new interface mac addr */
    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      "-r virtualio --rsubtype eth --level lpar "
                      " |sed '/lpar_name=%s/!d; /slot_num=%d/!d; "
                      "s/^.*mac_addr=//'", def->name, slot);
    VIR_FREE(ret);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    memcpy(mac, ret, PHYP_MAC_SIZE-1);

    result = virGetInterface(conn, name, mac);

cleanup:
    VIR_FREE(ret);
    virInterfaceDefFree(def);
    return result;
}

static virInterfacePtr
phypInterfaceLookupByName(virConnectPtr conn, const char *name)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int exit_status = 0;
    char *ret = NULL;
    int slot = 0;
    int lpar_id = 0;
    char mac[PHYP_MAC_SIZE];
    virInterfacePtr result = NULL;

    /*Getting the slot number for the interface */
    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype slot --level slot "
                      " -F drc_name,slot_num |"
                      " sed -n '/%s/ s/^.*,//p'", name);
    if (phypExecInt(session, &buf, conn, &slot) < 0)
        goto cleanup;

    /*Getting the lpar_id for the interface */
    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype slot --level slot "
                      " -F drc_name,lpar_id |"
                      " sed -n '/%s/ s/^.*,//p'", name);
    if (phypExecInt(session, &buf, conn, &lpar_id) < 0)
        goto cleanup;

    /*Getting the interface mac */
    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype eth --level lpar "
                      " -F lpar_id,slot_num,mac_addr|"
                      " sed -n '/%d,%d/ s/^.*,//p'", lpar_id, slot);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    memcpy(mac, ret, PHYP_MAC_SIZE-1);

    result = virGetInterface(conn, name, ret);

cleanup:
    VIR_FREE(ret);
    return result;
}

static int
phypInterfaceIsActive(virInterfacePtr iface)
{
    ConnectionData *connection_data = iface->conn->networkPrivateData;
    phyp_driverPtr phyp_driver = iface->conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int state = -1;

    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      " -r virtualio --rsubtype eth --level lpar "
                      " -F mac_addr,state |"
                      " sed -n '/%s/ s/^.*,//p'", iface->mac);
    phypExecInt(session, &buf, iface->conn, &state);
    return state;
}

static int
phypConnectListInterfaces(virConnectPtr conn, char **const names, int nnames)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int vios_id = phyp_driver->vios_id;
    int exit_status = 0;
    int got = 0;
    size_t i;
    char *ret = NULL;
    char *networks = NULL;
    char *char_ptr = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    bool success = false;

    virBufferAddLit(&buf, "lshwres");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r virtualio --rsubtype slot  --level slot|"
                      " sed '/eth/!d; /lpar_id=%d/d; s/^.*drc_name=//g'",
                      vios_id);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    /* I need to parse the textual return in order to get the network
     * interfaces */
    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    networks = ret;

    while (got < nnames) {
        char_ptr = strchr(networks, '\n');

        if (char_ptr) {
            *char_ptr = '\0';
            if (VIR_STRDUP(names[got++], networks) < 0)
                goto cleanup;
            char_ptr++;
            networks = char_ptr;
        } else {
            break;
        }
    }

cleanup:
    if (!success) {
        for (i = 0; i < got; i++)
            VIR_FREE(names[i]);
    }
    VIR_FREE(ret);
    return got;
}

static int
phypConnectNumOfInterfaces(virConnectPtr conn)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    char *managed_system = phyp_driver->managed_system;
    int system_type = phyp_driver->system_type;
    int vios_id = phyp_driver->vios_id;
    int nnets = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lshwres ");
    if (system_type == HMC)
        virBufferAsprintf(&buf, "-m %s ", managed_system);

    virBufferAsprintf(&buf,
                      "-r virtualio --rsubtype eth --level lpar|"
                      "grep -v lpar_id=%d|grep -c lpar_name", vios_id);
    phypExecInt(session, &buf, conn, &nnets);
    return nnets;
}

static int
phypGetLparState(virConnectPtr conn, unsigned int lpar_id)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *ret = NULL;
    int exit_status = 0;
    char *managed_system = phyp_driver->managed_system;
    int state = VIR_DOMAIN_NOSTATE;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lssyscfg -r lpar");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -F state --filter lpar_ids=%d", lpar_id);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    if (STREQ(ret, "Running"))
        state = VIR_DOMAIN_RUNNING;
    else if (STREQ(ret, "Not Activated"))
        state = VIR_DOMAIN_SHUTOFF;
    else if (STREQ(ret, "Shutting Down"))
        state = VIR_DOMAIN_SHUTDOWN;

cleanup:
    VIR_FREE(ret);
    return state;
}

/* XXX - is this needed? */
static int phypDiskType(virConnectPtr, char *) ATTRIBUTE_UNUSED;
static int
phypDiskType(virConnectPtr conn, char *backing_device)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *ret = NULL;
    int exit_status = 0;
    char *managed_system = phyp_driver->managed_system;
    int vios_id = phyp_driver->vios_id;
    int disk_type = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "viosvrcmd");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -p %d -c \"lssp -field name type "
                      "-fmt , -all|sed -n '/%s/ {\n s/^.*,//\n p\n}'\"",
                      vios_id, backing_device);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, true);

    if (exit_status < 0 || ret == NULL)
        goto cleanup;

    if (STREQ(ret, "LVPOOL"))
        disk_type = VIR_DOMAIN_DISK_TYPE_BLOCK;
    else if (STREQ(ret, "FBPOOL"))
        disk_type = VIR_DOMAIN_DISK_TYPE_FILE;

cleanup:
    VIR_FREE(ret);
    return disk_type;
}

static int
phypConnectNumOfDefinedDomains(virConnectPtr conn)
{
    return phypConnectNumOfDomainsGeneric(conn, 1);
}

static int
phypConnectNumOfDomains(virConnectPtr conn)
{
    return phypConnectNumOfDomainsGeneric(conn, 0);
}

static int
phypConnectListDomains(virConnectPtr conn, int *ids, int nids)
{
    return phypConnectListDomainsGeneric(conn, ids, nids, 0);
}

static int
phypConnectListDefinedDomains(virConnectPtr conn, char **const names, int nnames)
{
    bool success = false;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int exit_status = 0;
    int got = 0;
    size_t i;
    char *ret = NULL;
    char *domains = NULL;
    char *char_ptr = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "lssyscfg -r lpar");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAddLit(&buf, " -F name,state"
                      "|sed -n '/Not Activated/ {\n s/,.*$//\n p\n}'");
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    /* I need to parse the textual return in order to get the domains */
    if (exit_status < 0 || ret == NULL)
        goto cleanup;
    else {
        domains = ret;

        while (got < nnames) {
            char_ptr = strchr(domains, '\n');

            if (char_ptr) {
                *char_ptr = '\0';
                if (VIR_STRDUP(names[got++], domains) < 0)
                    goto cleanup;
                char_ptr++;
                domains = char_ptr;
            } else
                break;
        }
    }

    success = true;

cleanup:
    if (!success) {
        for (i = 0; i < got; i++)
            VIR_FREE(names[i]);

        got = -1;
    }
    VIR_FREE(ret);
    return got;
}

static virDomainPtr
phypDomainLookupByName(virConnectPtr conn, const char *lpar_name)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virDomainPtr dom = NULL;
    int lpar_id = 0;
    char *managed_system = phyp_driver->managed_system;
    unsigned char lpar_uuid[VIR_UUID_BUFLEN];

    lpar_id = phypGetLparID(session, managed_system, lpar_name, conn);
    if (lpar_id == -1)
        return NULL;

    if (phypGetLparUUID(lpar_uuid, lpar_id, conn) == -1)
        return NULL;

    dom = virGetDomain(conn, lpar_name, lpar_uuid);

    if (dom)
        dom->id = lpar_id;

    return dom;
}

static virDomainPtr
phypDomainLookupByID(virConnectPtr conn, int lpar_id)
{
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virDomainPtr dom = NULL;
    char *managed_system = phyp_driver->managed_system;
    unsigned char lpar_uuid[VIR_UUID_BUFLEN];

    char *lpar_name = phypGetLparNAME(session, managed_system, lpar_id,
                                      conn);

    if (phypGetLparUUID(lpar_uuid, lpar_id, conn) == -1)
        goto cleanup;

    dom = virGetDomain(conn, lpar_name, lpar_uuid);

    if (dom)
        dom->id = lpar_id;

cleanup:
    VIR_FREE(lpar_name);

    return dom;
}

static char *
phypDomainGetXMLDesc(virDomainPtr dom, unsigned int flags)
{
    ConnectionData *connection_data = dom->conn->networkPrivateData;
    phyp_driverPtr phyp_driver = dom->conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virDomainDef def;
    char *managed_system = phyp_driver->managed_system;

    /* Flags checked by virDomainDefFormat */

    memset(&def, 0, sizeof(virDomainDef));

    def.virtType = VIR_DOMAIN_VIRT_PHYP;
    def.id = dom->id;

    char *lpar_name = phypGetLparNAME(session, managed_system, def.id,
                                      dom->conn);

    if (lpar_name == NULL) {
        VIR_ERROR(_("Unable to determine domain's name."));
        goto err;
    }

    if (phypGetLparUUID(def.uuid, dom->id, dom->conn) == -1) {
        VIR_ERROR(_("Unable to generate random uuid."));
        goto err;
    }

    if ((def.mem.max_balloon =
         phypGetLparMem(dom->conn, managed_system, dom->id, 0)) == 0) {
        VIR_ERROR(_("Unable to determine domain's max memory."));
        goto err;
    }

    if ((def.mem.cur_balloon =
         phypGetLparMem(dom->conn, managed_system, dom->id, 1)) == 0) {
        VIR_ERROR(_("Unable to determine domain's memory."));
        goto err;
    }

    if ((def.maxvcpus = def.vcpus =
         phypGetLparCPU(dom->conn, managed_system, dom->id)) == 0) {
        VIR_ERROR(_("Unable to determine domain's CPU."));
        goto err;
    }

    return virDomainDefFormat(&def, flags);

err:
    return NULL;
}

static int
phypDomainResume(virDomainPtr dom)
{
    int result = -1;
    ConnectionData *connection_data = dom->conn->networkPrivateData;
    phyp_driverPtr phyp_driver = dom->conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "chsysstate");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r lpar -o on --id %d -f %s",
                      dom->id, dom->name);
    ret = phypExecBuffer(session, &buf, &exit_status, dom->conn, false);

    if (exit_status < 0)
        goto cleanup;

    result = 0;

cleanup:
    VIR_FREE(ret);

    return result;
}

static int
phypDomainReboot(virDomainPtr dom, unsigned int flags)
{
    int result = -1;
    ConnectionData *connection_data = dom->conn->networkPrivateData;
    virConnectPtr conn = dom->conn;
    LIBSSH2_SESSION *session = connection_data->session;
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags(0, -1);

    virBufferAddLit(&buf, "chsysstate");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf,
                      " -r lpar -o shutdown --id %d --immed --restart",
                      dom->id);
    ret = phypExecBuffer(session, &buf, &exit_status, dom->conn, false);

    if (exit_status < 0)
        goto cleanup;

    result = 0;

  cleanup:
    VIR_FREE(ret);

    return result;
}

static int
phypDomainShutdown(virDomainPtr dom)
{
    int result = -1;
    ConnectionData *connection_data = dom->conn->networkPrivateData;
    virConnectPtr conn = dom->conn;
    LIBSSH2_SESSION *session = connection_data->session;
    phyp_driverPtr phyp_driver = conn->privateData;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "chsysstate");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r lpar -o shutdown --id %d", dom->id);
    ret = phypExecBuffer(session, &buf, &exit_status, dom->conn, false);

    if (exit_status < 0)
        goto cleanup;

    result = 0;

cleanup:
    VIR_FREE(ret);

    return result;
}

static int
phypDomainGetInfo(virDomainPtr dom, virDomainInfoPtr info)
{
    phyp_driverPtr phyp_driver = dom->conn->privateData;
    char *managed_system = phyp_driver->managed_system;

    info->state = phypGetLparState(dom->conn, dom->id);

    if ((info->maxMem =
         phypGetLparMem(dom->conn, managed_system, dom->id, 0)) == 0)
        VIR_WARN("Unable to determine domain's max memory.");

    if ((info->memory =
         phypGetLparMem(dom->conn, managed_system, dom->id, 1)) == 0)
        VIR_WARN("Unable to determine domain's memory.");

    if ((info->nrVirtCpu =
         phypGetLparCPU(dom->conn, managed_system, dom->id)) == 0)
        VIR_WARN("Unable to determine domain's CPU.");

    return 0;
}

static int
phypDomainGetState(virDomainPtr dom,
                   int *state,
                   int *reason,
                   unsigned int flags)
{
    virCheckFlags(0, -1);

    *state = phypGetLparState(dom->conn, dom->id);
    if (reason)
        *reason = 0;

    return 0;
}

static int
phypDomainDestroyFlags(virDomainPtr dom,
                       unsigned int flags)
{
    int result = -1;
    ConnectionData *connection_data = dom->conn->networkPrivateData;
    phyp_driverPtr phyp_driver = dom->conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int exit_status = 0;
    char *ret = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags(0, -1);

    virBufferAddLit(&buf, "rmsyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r lpar --id %d", dom->id);
    ret = phypExecBuffer(session, &buf, &exit_status, dom->conn, false);

    if (exit_status < 0)
        goto cleanup;

    if (phypUUIDTable_RemLpar(dom->conn, dom->id) == -1)
        goto cleanup;

    dom->id = -1;
    result = 0;

cleanup:
    VIR_FREE(ret);

    return result;
}

static int
phypDomainDestroy(virDomainPtr dom)
{
    return phypDomainDestroyFlags(dom, 0);
}

static int
phypBuildLpar(virConnectPtr conn, virDomainDefPtr def)
{
    int result = -1;
    ConnectionData *connection_data = conn->networkPrivateData;
    phyp_driverPtr phyp_driver = conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    char *ret = NULL;
    int exit_status = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (!def->mem.cur_balloon) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Field <memory> on the domain XML file is missing or has "
                         "invalid value."));
        goto cleanup;
    }

    if (!def->mem.max_balloon) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Field <currentMemory> on the domain XML file is missing or "
                         "has invalid value."));
        goto cleanup;
    }

    if (def->ndisks < 1) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Domain XML must contain at least one <disk> element."));
        goto cleanup;
    }

    if (!def->disks[0]->src) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("Field <src> under <disk> on the domain XML file is "
                         "missing."));
        goto cleanup;
    }

    virBufferAddLit(&buf, "mksyscfg");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " -r lpar -p %s -i min_mem=%lld,desired_mem=%lld,"
                      "max_mem=%lld,desired_procs=%d,virtual_scsi_adapters=%s",
                      def->name, def->mem.cur_balloon,
                      def->mem.cur_balloon, def->mem.max_balloon,
                      (int) def->vcpus, def->disks[0]->src);
    ret = phypExecBuffer(session, &buf, &exit_status, conn, false);

    if (exit_status < 0) {
        VIR_ERROR(_("Unable to create LPAR. Reason: '%s'"), NULLSTR(ret));
        goto cleanup;
    }

    if (phypUUIDTable_AddLpar(conn, def->uuid, def->id) == -1) {
        VIR_ERROR(_("Unable to add LPAR to the table"));
        goto cleanup;
    }

    result = 0;

cleanup:
    VIR_FREE(ret);

    return result;
}

static virDomainPtr
phypDomainCreateXML(virConnectPtr conn,
                    const char *xml, unsigned int flags)
{
    virCheckFlags(0, NULL);

    ConnectionData *connection_data = conn->networkPrivateData;
    LIBSSH2_SESSION *session = connection_data->session;
    virDomainDefPtr def = NULL;
    virDomainPtr dom = NULL;
    phyp_driverPtr phyp_driver = conn->privateData;
    uuid_tablePtr uuid_table = phyp_driver->uuid_table;
    lparPtr *lpars = uuid_table->lpars;
    size_t i = 0;
    char *managed_system = phyp_driver->managed_system;

    virCheckFlags(0, NULL);

    if (!(def = virDomainDefParseString(xml, phyp_driver->caps,
                                        phyp_driver->xmlopt,
                                        1 << VIR_DOMAIN_VIRT_PHYP,
                                        VIR_DOMAIN_XML_SECURE)))
        goto err;

    /* checking if this name already exists on this system */
    if (phypGetLparID(session, managed_system, def->name, conn) != -1) {
        VIR_WARN("LPAR name already exists.");
        goto err;
    }

    /* checking if ID or UUID already exists on this system */
    for (i = 0; i < uuid_table->nlpars; i++) {
        if (lpars[i]->id == def->id || lpars[i]->uuid == def->uuid) {
            VIR_WARN("LPAR ID or UUID already exists.");
            goto err;
        }
    }

    if ((dom = virGetDomain(conn, def->name, def->uuid)) == NULL)
        goto err;

    if (phypBuildLpar(conn, def) == -1)
        goto err;

    if (phypDomainResume(dom) == -1)
        goto err;

    return dom;

err:
    virDomainDefFree(def);
    virObjectUnref(dom);
    return NULL;
}

static char *
phypConnectGetCapabilities(virConnectPtr conn)
{
    phyp_driverPtr phyp_driver = conn->privateData;
    char *xml;

    if ((xml = virCapabilitiesFormatXML(phyp_driver->caps)) == NULL)
        virReportOOMError();

    return xml;
}

static int
phypDomainSetVcpusFlags(virDomainPtr dom, unsigned int nvcpus,
                        unsigned int flags)
{
    ConnectionData *connection_data = dom->conn->networkPrivateData;
    phyp_driverPtr phyp_driver = dom->conn->privateData;
    LIBSSH2_SESSION *session = connection_data->session;
    int system_type = phyp_driver->system_type;
    char *managed_system = phyp_driver->managed_system;
    int exit_status = 0;
    char *ret = NULL;
    char operation;
    unsigned long ncpus = 0;
    unsigned int amount = 0;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (flags != VIR_DOMAIN_VCPU_LIVE) {
        virReportError(VIR_ERR_INVALID_ARG, _("unsupported flags: (0x%x)"), flags);
        return -1;
    }

    if ((ncpus = phypGetLparCPU(dom->conn, managed_system, dom->id)) == 0)
        return 0;

    if (nvcpus > phypDomainGetMaxVcpus(dom)) {
        VIR_ERROR(_("You are trying to set a number of CPUs bigger than "
                     "the max possible."));
        return 0;
    }

    if (ncpus > nvcpus) {
        operation = 'r';
        amount = nvcpus - ncpus;
    } else if (ncpus < nvcpus) {
        operation = 'a';
        amount = nvcpus - ncpus;
    } else
        return 0;

    virBufferAddLit(&buf, "chhwres -r proc");
    if (system_type == HMC)
        virBufferAsprintf(&buf, " -m %s", managed_system);
    virBufferAsprintf(&buf, " --id %d -o %c --procunits %d 2>&1 |sed "
                      "-e 's/^.*\\([0-9][0-9]*.[0-9][0-9]*\\).*$/\\1/'",
                      dom->id, operation, amount);
    ret = phypExecBuffer(session, &buf, &exit_status, dom->conn, false);

    if (exit_status < 0) {
        VIR_ERROR(_
                   ("Possibly you don't have IBM Tools installed in your LPAR."
                    " Contact your support to enable this feature."));
    }

    VIR_FREE(ret);
    return 0;

}

static int
phypDomainSetVcpus(virDomainPtr dom, unsigned int nvcpus)
{
    return phypDomainSetVcpusFlags(dom, nvcpus, VIR_DOMAIN_VCPU_LIVE);
}

static virDrvOpenStatus
phypStorageOpen(virConnectPtr conn,
                virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (conn->driver->no != VIR_DRV_PHYP)
        return VIR_DRV_OPEN_DECLINED;

    return VIR_DRV_OPEN_SUCCESS;
}

static int
phypStorageClose(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

static virDrvOpenStatus
phypInterfaceOpen(virConnectPtr conn,
                virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (conn->driver->no != VIR_DRV_PHYP)
        return VIR_DRV_OPEN_DECLINED;

    return VIR_DRV_OPEN_SUCCESS;
}

static int
phypInterfaceClose(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

static virDriver phypDriver = {
    .no = VIR_DRV_PHYP,
    .name = "PHYP",
    .connectOpen = phypConnectOpen, /* 0.7.0 */
    .connectClose = phypConnectClose, /* 0.7.0 */
    .connectGetCapabilities = phypConnectGetCapabilities, /* 0.7.3 */
    .connectListDomains = phypConnectListDomains, /* 0.7.0 */
    .connectNumOfDomains = phypConnectNumOfDomains, /* 0.7.0 */
    .domainCreateXML = phypDomainCreateXML, /* 0.7.3 */
    .domainLookupByID = phypDomainLookupByID, /* 0.7.0 */
    .domainLookupByName = phypDomainLookupByName, /* 0.7.0 */
    .domainResume = phypDomainResume, /* 0.7.0 */
    .domainShutdown = phypDomainShutdown, /* 0.7.0 */
    .domainReboot = phypDomainReboot, /* 0.9.1 */
    .domainDestroy = phypDomainDestroy, /* 0.7.3 */
    .domainDestroyFlags = phypDomainDestroyFlags, /* 0.9.4 */
    .domainGetInfo = phypDomainGetInfo, /* 0.7.0 */
    .domainGetState = phypDomainGetState, /* 0.9.2 */
    .domainSetVcpus = phypDomainSetVcpus, /* 0.7.3 */
    .domainSetVcpusFlags = phypDomainSetVcpusFlags, /* 0.8.5 */
    .domainGetVcpusFlags = phypDomainGetVcpusFlags, /* 0.8.5 */
    .domainGetMaxVcpus = phypDomainGetMaxVcpus, /* 0.7.3 */
    .domainGetXMLDesc = phypDomainGetXMLDesc, /* 0.7.0 */
    .connectListDefinedDomains = phypConnectListDefinedDomains, /* 0.7.0 */
    .connectNumOfDefinedDomains = phypConnectNumOfDefinedDomains, /* 0.7.0 */
    .domainAttachDevice = phypDomainAttachDevice, /* 0.8.2 */
    .connectIsEncrypted = phypConnectIsEncrypted, /* 0.7.3 */
    .connectIsSecure = phypConnectIsSecure, /* 0.7.3 */
    .domainIsUpdated = phypDomainIsUpdated, /* 0.8.6 */
    .connectIsAlive = phypConnectIsAlive, /* 0.9.8 */
};

static virStorageDriver phypStorageDriver = {
    .name = "PHYP",
    .storageOpen = phypStorageOpen, /* 0.8.2 */
    .storageClose = phypStorageClose, /* 0.8.2 */

    .connectNumOfStoragePools = phypConnectNumOfStoragePools, /* 0.8.2 */
    .connectListStoragePools = phypConnectListStoragePools, /* 0.8.2 */
    .storagePoolLookupByName = phypStoragePoolLookupByName, /* 0.8.2 */
    .storagePoolLookupByUUID = phypStoragePoolLookupByUUID, /* 0.8.2 */
    .storagePoolCreateXML = phypStoragePoolCreateXML, /* 0.8.2 */
    .storagePoolDestroy = phypStoragePoolDestroy, /* 0.8.2 */
    .storagePoolGetXMLDesc = phypStoragePoolGetXMLDesc, /* 0.8.2 */
    .storagePoolNumOfVolumes = phypStoragePoolNumOfVolumes, /* 0.8.2 */
    .storagePoolListVolumes = phypStoragePoolListVolumes, /* 0.8.2 */

    .storageVolLookupByName = phypStorageVolLookupByName, /* 0.8.2 */
    .storageVolLookupByPath = phypStorageVolLookupByPath, /* 0.8.2 */
    .storageVolCreateXML = phypStorageVolCreateXML, /* 0.8.2 */
    .storageVolGetXMLDesc = phypStorageVolGetXMLDesc, /* 0.8.2 */
    .storageVolGetPath = phypStorageVolGetPath, /* 0.8.2 */
};

static virInterfaceDriver phypInterfaceDriver = {
    .name = "PHYP",
    .interfaceOpen = phypInterfaceOpen, /* 0.9.1 */
    .interfaceClose = phypInterfaceClose, /* 0.9.1 */
    .connectNumOfInterfaces = phypConnectNumOfInterfaces, /* 0.9.1 */
    .connectListInterfaces = phypConnectListInterfaces, /* 0.9.1 */
    .interfaceLookupByName = phypInterfaceLookupByName, /* 0.9.1 */
    .interfaceDefineXML = phypInterfaceDefineXML, /* 0.9.1 */
    .interfaceDestroy = phypInterfaceDestroy, /* 0.9.1 */
    .interfaceIsActive = phypInterfaceIsActive /* 0.9.1 */
};

int
phypRegister(void)
{
    if (virRegisterDriver(&phypDriver) < 0)
        return -1;
    if (virRegisterStorageDriver(&phypStorageDriver) < 0)
        return -1;
    if (virRegisterInterfaceDriver(&phypInterfaceDriver) < 0)
        return -1;

    return 0;
}
