/*
 * virdomainjob.h: helper functions for domain jobs
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

#define JOB_MASK(job)                  (job == 0 ? 0 : 1 << (job - 1))
#define VIR_JOB_DEFAULT_MASK \
    (JOB_MASK(VIR_JOB_QUERY) | \
     JOB_MASK(VIR_JOB_DESTROY) | \
     JOB_MASK(VIR_JOB_ABORT))

/* Jobs which have to be tracked in domain state XML. */
#define VIR_DOMAIN_TRACK_JOBS \
    (JOB_MASK(VIR_JOB_DESTROY) | \
     JOB_MASK(VIR_JOB_ASYNC))

/* Only 1 job is allowed at any time
 * A job includes *all* monitor commands, even those just querying
 * information, not merely actions */
typedef enum {
    VIR_JOB_NONE = 0,  /* Always set to 0 for easy if (jobActive) conditions */
    VIR_JOB_QUERY,         /* Doesn't change any state */
    VIR_JOB_DESTROY,       /* Destroys the domain (cannot be masked out) */
    VIR_JOB_SUSPEND,       /* Suspends (stops vCPUs) the domain */
    VIR_JOB_MODIFY,        /* May change state */
    VIR_JOB_ABORT,         /* Abort current async job */
    VIR_JOB_MIGRATION_OP,  /* Operation influencing outgoing migration */

    /* The following two items must always be the last items before JOB_LAST */
    VIR_JOB_ASYNC,         /* Asynchronous job */
    VIR_JOB_ASYNC_NESTED,  /* Normal job within an async job */

    VIR_JOB_LAST
} virDomainJob;
VIR_ENUM_DECL(virDomainJob);

typedef enum {
    VIR_AGENT_JOB_NONE = 0,    /* No agent job. */
    VIR_AGENT_JOB_QUERY,       /* Does not change state of domain */
    VIR_AGENT_JOB_MODIFY,      /* May change state of domain */

    VIR_AGENT_JOB_LAST
} virDomainAgentJob;
VIR_ENUM_DECL(virDomainAgentJob);

/* Async job consists of a series of jobs that may change state. Independent
 * jobs that do not change state (and possibly others if explicitly allowed by
 * current async job) are allowed to be run even if async job is active.
 */
typedef enum {
    VIR_ASYNC_JOB_NONE = 0,
    VIR_ASYNC_JOB_MIGRATION_OUT,
    VIR_ASYNC_JOB_MIGRATION_IN,
    VIR_ASYNC_JOB_SAVE,
    VIR_ASYNC_JOB_DUMP,
    VIR_ASYNC_JOB_SNAPSHOT,
    VIR_ASYNC_JOB_START,
    VIR_ASYNC_JOB_BACKUP,

    VIR_ASYNC_JOB_LAST
} virDomainAsyncJob;
VIR_ENUM_DECL(virDomainAsyncJob);

typedef enum {
    VIR_DOMAIN_JOB_STATUS_NONE = 0,
    VIR_DOMAIN_JOB_STATUS_ACTIVE,
    VIR_DOMAIN_JOB_STATUS_MIGRATING,
    VIR_DOMAIN_JOB_STATUS_QEMU_COMPLETED,
    VIR_DOMAIN_JOB_STATUS_PAUSED,
    VIR_DOMAIN_JOB_STATUS_POSTCOPY,
    VIR_DOMAIN_JOB_STATUS_COMPLETED,
    VIR_DOMAIN_JOB_STATUS_FAILED,
    VIR_DOMAIN_JOB_STATUS_CANCELED,
} virDomainJobStatus;

typedef enum {
    VIR_DOMAIN_JOB_STATS_TYPE_NONE = 0,
    VIR_DOMAIN_JOB_STATS_TYPE_MIGRATION,
    VIR_DOMAIN_JOB_STATS_TYPE_SAVEDUMP,
    VIR_DOMAIN_JOB_STATS_TYPE_MEMDUMP,
    VIR_DOMAIN_JOB_STATS_TYPE_BACKUP,
} virDomainJobStatsType;

typedef struct _virDomainMirrorStats virDomainMirrorStats;
typedef virDomainMirrorStats *virDomainMirrorStatsPtr;
struct _virDomainMirrorStats {
    unsigned long long transferred;
    unsigned long long total;
};

typedef struct _virDomainJobInfoInternal virDomainJobInfoInternal;
typedef virDomainJobInfoInternal *virDomainJobInfoInternalPtr;
struct _virDomainJobInfoInternal {
    virDomainJobStatus status;
    virDomainJobOperation operation;
    unsigned long long started; /* When the async job started */
    unsigned long long stopped; /* When the domain's CPUs were stopped */
    unsigned long long sent; /* When the source sent status info to the
                                destination (only for migrations). */
    unsigned long long received; /* When the destination host received status
                                    info from the source (migrations only). */
    /* Computed values */
    unsigned long long timeElapsed;
    long long timeDelta; /* delta = received - sent, i.e., the difference
                            between the source and the destination time plus
                            the time between the end of Perform phase on the
                            source and the beginning of Finish phase on the
                            destination. */
    bool timeDeltaSet;
    /* Raw values from QEMU */
    virDomainJobStatsType statsType;
    virDomainMirrorStats mirrorStats;

    char *errmsg; /* optional error message for failed completed jobs */

    void *privateData;  /* job specific collection of info */
    virDomainObjPrivateJobInfoCallbacks cb;
};

typedef struct _virDomainJobObj virDomainJobObj;
typedef virDomainJobObj *virDomainJobObjPtr;
struct _virDomainJobObj {
    virCond cond;                       /* Use to coordinate jobs */

    /* The following members are for QEMU_JOB_* */
    virDomainJob active;               /* Currently running job */
    unsigned long long owner;           /* Thread id which set current job */
    const char *ownerAPI;               /* The API which owns the job */
    unsigned long long started;         /* When the current job started */

    /* The following members are for QEMU_AGENT_JOB_* */
    virDomainAgentJob agentActive;     /* Currently running agent job */
    unsigned long long agentOwner;      /* Thread id which set current agent job */
    const char *agentOwnerAPI;          /* The API which owns the agent job */
    unsigned long long agentStarted;    /* When the current agent job started */

    /* The following members are for QEMU_ASYNC_JOB_* */
    virCond asyncCond;                  /* Use to coordinate with async jobs */
    virDomainAsyncJob asyncJob;        /* Currently active async job */
    unsigned long long asyncOwner;      /* Thread which set current async job */
    const char *asyncOwnerAPI;          /* The API which owns the async job */
    unsigned long long asyncStarted;    /* When the current async job started */
    int phase;                          /* Job phase (mainly for migrations) */
    unsigned long long mask;            /* Jobs allowed during async job */
    virDomainJobInfoInternalPtr current;       /* async job progress data */
    virDomainJobInfoInternalPtr completed;     /* statistics data of a recently completed job */
    bool abortJob;                      /* abort of the job requested */
    char *error;                        /* job event completion error */
    unsigned long apiFlags; /* flags passed to the API which started the async job */
    void *privateData;                  /* job specific collection of data */
    struct _virDomainObjPrivateJobCallbacks cb;
};

void
virDomainJobInfoFree(virDomainJobInfoInternalPtr info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(virDomainJobInfoInternal, virDomainJobInfoInternalFree);

virDomainJobInfoInternalPtr
virDomainJobInfoInternalCopy(virDomainJobInfoInternalPtr info);

const char *virDomainAsyncJobPhaseToString(virDomainAsyncJob job,
                                           int phase);
int virDomainAsyncJobPhaseFromString(virDomainAsyncJob job,
                                     const char *phase);

void qemuDomainEventEmitJobCompleted(virQEMUDriverPtr driver,
                                     virDomainObjPtr vm);

bool qemuDomainJobAllowed(qemuDomainJobObjPtr jobs, qemuDomainJob newJob);

int qemuDomainJobInfoUpdateTime(qemuDomainJobInfoPtr jobInfo)
    ATTRIBUTE_NONNULL(1);

bool qemuDomainTrackJob(qemuDomainJob job);

void qemuDomainObjRestoreJob(virDomainObjPtr obj,
                             qemuDomainJobObjPtr job);
