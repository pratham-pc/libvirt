/*
 * qemu_domainjob.h: helper functions for QEMU domain jobs
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

#pragma once

#include <glib-object.h>

typedef void *(*qemuDomainObjJobInfoPrivateAlloc)(void);
typedef void (*qemuDomainObjJobInfoPrivateFree)(void *);
typedef void (*qemuDomainObjJobInfoPrivateCopy)(qemuDomainJobInfoPtr,
                                                qemuDomainJobInfoPtr);

typedef struct _qemuDomainObjPrivateJobInfoCallbacks qemuDomainObjPrivateJobInfoCallbacks;
struct _qemuDomainObjPrivateJobInfoCallbacks {
   qemuDomainObjJobInfoPrivateAlloc allocJobInfoPrivate;
   qemuDomainObjJobInfoPrivateFree freeJobInfoPrivate;
   qemuDomainObjJobInfoPrivateCopy copyJobInfoPrivate;
};

typedef void *(*qemuDomainObjPrivateJobAlloc)(void);
typedef void (*qemuDomainObjPrivateJobFree)(void *);
typedef int (*qemuDomainObjPrivateJobFormat)(virBufferPtr,
                                             virDomainObjPtr,
                                             qemuDomainJobObjPtr);
typedef int (*qemuDomainObjPrivateJobParse)(virDomainObjPtr,
                                            qemuDomainJobObjPtr,
                                            xmlXPathContextPtr,
                                            virDomainXMLOptionPtr);

typedef struct _qemuDomainObjPrivateJobCallbacks qemuDomainObjPrivateJobCallbacks;
struct _qemuDomainObjPrivateJobCallbacks {
   qemuDomainObjPrivateJobAlloc allocJobPrivate;
   qemuDomainObjPrivateJobFree freeJobPrivate;
   qemuDomainObjPrivateJobFormat formatJob;
   qemuDomainObjPrivateJobParse parseJob;
};

int qemuDomainObjBeginJob(virQEMUDriverPtr driver,
                          virDomainObjPtr obj,
                          qemuDomainJob job)
    G_GNUC_WARN_UNUSED_RESULT;
int qemuDomainObjBeginAgentJob(virQEMUDriverPtr driver,
                               virDomainObjPtr obj,
                               qemuDomainAgentJob agentJob)
    G_GNUC_WARN_UNUSED_RESULT;
int qemuDomainObjBeginAsyncJob(virQEMUDriverPtr driver,
                               virDomainObjPtr obj,
                               qemuDomainAsyncJob asyncJob,
                               virDomainJobOperation operation,
                               unsigned long apiFlags)
    G_GNUC_WARN_UNUSED_RESULT;
int qemuDomainObjBeginNestedJob(virQEMUDriverPtr driver,
                                virDomainObjPtr obj,
                                qemuDomainAsyncJob asyncJob)
    G_GNUC_WARN_UNUSED_RESULT;
int qemuDomainObjBeginJobNowait(virQEMUDriverPtr driver,
                                virDomainObjPtr obj,
                                qemuDomainJob job)
    G_GNUC_WARN_UNUSED_RESULT;

void qemuDomainObjEndJob(virQEMUDriverPtr driver,
                         virDomainObjPtr obj);
void qemuDomainObjEndAgentJob(virDomainObjPtr obj);
void qemuDomainObjEndAsyncJob(virQEMUDriverPtr driver,
                              virDomainObjPtr obj);
void qemuDomainObjAbortAsyncJob(virDomainObjPtr obj);
void qemuDomainObjSetJobPhase(virQEMUDriverPtr driver,
                              virDomainObjPtr obj,
                              int phase);
void qemuDomainObjSetAsyncJobMask(virDomainObjPtr obj,
                                  unsigned long long allowedJobs);

void qemuDomainObjDiscardAsyncJob(virQEMUDriverPtr driver,
                                  virDomainObjPtr obj);
void qemuDomainObjReleaseAsyncJob(virDomainObjPtr obj);

void qemuDomainRemoveInactiveJob(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm);

void qemuDomainRemoveInactiveJobLocked(virQEMUDriverPtr driver,
                                       virDomainObjPtr vm);

int qemuDomainJobInfoUpdateDowntime(qemuDomainJobInfoPtr jobInfo)
    ATTRIBUTE_NONNULL(1);
int qemuDomainJobInfoToInfo(qemuDomainJobInfoPtr jobInfo,
                            virDomainJobInfoPtr info)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);
int qemuDomainJobInfoToParams(qemuDomainJobInfoPtr jobInfo,
                              int *type,
                              virTypedParameterPtr *params,
                              int *nparams)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2)
    ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4);

void qemuDomainObjFreeJob(qemuDomainJobObjPtr job);

int qemuDomainObjInitJob(qemuDomainJobObjPtr job);

int
qemuDomainObjPrivateXMLFormatJob(virBufferPtr buf,
                                 virDomainObjPtr vm,
                                 qemuDomainJobObjPtr jobObj);

int
qemuDomainObjPrivateXMLParseJob(virDomainObjPtr vm,
                                qemuDomainJobObjPtr job,
                                xmlXPathContextPtr ctxt,
                                virDomainXMLOptionPtr xmlopt);
