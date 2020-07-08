/*
 * virdomainjob.c: helper functions for domain jobs
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

#include "qemu_domain.h"
#include "qemu_migration.h"
#include "qemu_domainjob.h"
#include "viralloc.h"
#include "virlog.h"
#include "virerror.h"
#include "virtime.h"
#include "virthreadjob.h"

#define VIR_FROM_THIS VIR_FROM_DOMAIN

VIR_LOG_INIT("util.domainjob");


VIR_ENUM_IMPL(virDomainJob,
              VIR_JOB_LAST,
              "none",
              "query",
              "destroy",
              "suspend",
              "modify",
              "abort",
              "migration operation",
              "none",   /* async job is never stored in job.active */
              "async nested",
);

VIR_ENUM_IMPL(virDomainAgentJob,
              VIR_AGENT_JOB_LAST,
              "none",
              "query",
              "modify",
);

VIR_ENUM_IMPL(virDomainAsyncJob,
              VIR_ASYNC_JOB_LAST,
              "none",
              "migration out",
              "migration in",
              "save",
              "dump",
              "snapshot",
              "start",
              "backup",
);

const char *
virDomainAsyncJobPhaseToString(virDomainAsyncJob job,
                               int phase G_GNUC_UNUSED)
{
    switch (job) {
    case VIR_ASYNC_JOB_MIGRATION_OUT:
    case VIR_ASYNC_JOB_MIGRATION_IN:
        return qemuMigrationJobPhaseTypeToString(phase);

    case VIR_ASYNC_JOB_SAVE:
    case VIR_ASYNC_JOB_DUMP:
    case VIR_ASYNC_JOB_SNAPSHOT:
    case VIR_ASYNC_JOB_START:
    case VIR_ASYNC_JOB_NONE:
    case VIR_ASYNC_JOB_BACKUP:
        G_GNUC_FALLTHROUGH;
    case VIR_ASYNC_JOB_LAST:
        break;
    }

    return "none";
}

int
virDomainAsyncJobPhaseFromString(virDomainAsyncJob job,
                                  const char *phase)
{
    if (!phase)
        return 0;

    switch (job) {
    case VIR_ASYNC_JOB_MIGRATION_OUT:
    case VIR_ASYNC_JOB_MIGRATION_IN:
        return qemuMigrationJobPhaseTypeFromString(phase);

    case VIR_ASYNC_JOB_SAVE:
    case VIR_ASYNC_JOB_DUMP:
    case VIR_ASYNC_JOB_SNAPSHOT:
    case VIR_ASYNC_JOB_START:
    case VIR_ASYNC_JOB_NONE:
    case VIR_ASYNC_JOB_BACKUP:
        G_GNUC_FALLTHROUGH;
    case VIR_ASYNC_JOB_LAST:
        break;
    }

    if (STREQ(phase, "none"))
        return 0;
    else
        return -1;
}

void
virDomainJobInfoFree(virDomainJobInfoPtr info)
{
    info->cb.freeJobInfoPrivate(info);
    g_free(info->errmsg);
    g_free(info);
}


static void
virDomainCurrentJobInfoInit(virDomainJobObjPtr job)
{
    job->current = virDomainJobInfoAlloc();
    job->current->status = VIR_DOMAIN_JOB_STATUS_ACTIVE;
}


virDomainJobInfoPtr
virDomainJobInfoCopy(virDomainJobInfoPtr info)
{
    virDomainJobInfoPtr ret = virDomainJobInfoAlloc();

    memcpy(ret, info, sizeof(*info));
    ret->cb.copyJobInfoPrivate(info, ret);
    ret->errmsg = g_strdup(info->errmsg);

    return ret;
}

static void
virDomainObjResetJob(virDomainJobObjPtr job)
{
    job->active = VIR_JOB_NONE;
    job->owner = 0;
    job->ownerAPI = NULL;
    job->started = 0;
}


static void
virDomainObjResetAgentJob(virDomainJobObjPtr job)
{
    job->agentActive = VIR_AGENT_JOB_NONE;
    job->agentOwner = 0;
    job->agentOwnerAPI = NULL;
    job->agentStarted = 0;
}


static void
virDomainObjResetAsyncJob(virDomainJobObjPtr job)
{
    job->asyncJob = VIR_ASYNC_JOB_NONE;
    job->asyncOwner = 0;
    job->asyncOwnerAPI = NULL;
    job->asyncStarted = 0;
    job->phase = 0;
    job->mask = VIR_JOB_DEFAULT_MASK;
    job->abortJob = false;
    VIR_FREE(job->error);
    g_clear_pointer(&job->current, virDomainJobInfoFree);
    job->cb.freeJobPrivate(job);
    job->apiFlags = 0;
}

void
virDomainObjRestoreJob(virDomainObjPtr obj,
                        virDomainJobObjPtr job)
{
    virDomainObjPrivatePtr priv = obj->privateData;

    memset(job, 0, sizeof(*job));
    job->active = priv->job.active;
    job->owner = priv->job.owner;
    job->asyncJob = priv->job.asyncJob;
    job->asyncOwner = priv->job.asyncOwner;
    job->phase = priv->job.phase;
    job->privateData = g_steal_pointer(&priv->job.privateData);
    job->apiFlags = priv->job.apiFlags;

    virDomainObjResetJob(&priv->job);
    virDomainObjResetAsyncJob(&priv->job);
}

void
virDomainObjFreeJob(virDomainJobObjPtr job)
{
    virDomainObjResetJob(job);
    virDomainObjResetAsyncJob(job);
    g_clear_pointer(&job->current, virDomainJobInfoFree);
    g_clear_pointer(&job->completed, virDomainJobInfoFree);
    virCondDestroy(&job->cond);
    virCondDestroy(&job->asyncCond);
}

bool
virDomainTrackJob(virDomainJob job)
{
    return (VIR_DOMAIN_TRACK_JOBS & JOB_MASK(job)) != 0;
}


int
virDomainJobInfoUpdateTime(virDomainJobInfoPtr jobInfo)
{
    unsigned long long now;

    if (!jobInfo->started)
        return 0;

    if (virTimeMillisNow(&now) < 0)
        return -1;

    if (now < jobInfo->started) {
        VIR_WARN("Async job starts in the future");
        jobInfo->started = 0;
        return 0;
    }

    jobInfo->timeElapsed = now - jobInfo->started;
    return 0;
}

void
virDomainEventEmitJobCompleted(virObjectEventStatePtr domainEventstate,
                               virDomainObjPtr vm,
                               virDomainJobObjPtr job)
{
    virObjectEventPtr event;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int type;

    if (!job->completed)
        return;

    if (qemuDomainJobInfoToParams(job->completed, &type,
                                  &params, &nparams) < 0) {
        VIR_WARN("Could not get stats for completed job; domain %s",
                 vm->def->name);
    }

    event = virDomainEventJobCompletedNewFromObj(vm, params, nparams);
    virObjectEventStateQueue(domainEventState, event);
}

static virDomainJobType
virDomainJobStatusToType(virDomainJobStatus status)
{
    switch (status) {
    case VIR_DOMAIN_JOB_STATUS_NONE:
        break;

    case VIR_DOMAIN_JOB_STATUS_ACTIVE:
    case VIR_DOMAIN_JOB_STATUS_MIGRATING:
    case VIR_DOMAIN_JOB_STATUS_QEMU_COMPLETED:
    case VIR_DOMAIN_JOB_STATUS_POSTCOPY:
    case VIR_DOMAIN_JOB_STATUS_PAUSED:
        return VIR_DOMAIN_JOB_UNBOUNDED;

    case VIR_DOMAIN_JOB_STATUS_COMPLETED:
        return VIR_DOMAIN_JOB_COMPLETED;

    case VIR_DOMAIN_JOB_STATUS_FAILED:
        return VIR_DOMAIN_JOB_FAILED;

    case VIR_DOMAIN_JOB_STATUS_CANCELED:
        return VIR_DOMAIN_JOB_CANCELLED;
    }

    return VIR_DOMAIN_JOB_NONE;
}


void
virDomainObjSetAsyncJobMask(virDomainJobObjPtr job,
                             unsigned long long allowedJobs)
{
    if (!job->asyncJob)
        return;

    job->mask = allowedJobs | JOB_MASK(VIR_JOB_DESTROY);
}

void
virDomainObjReleaseAsyncJob(virDomainJobObjPtr job)
{
    VIR_DEBUG("Releasing ownership of '%s' async job",
              virDomainAsyncJobTypeToString(job->asyncJob));

    if (job->asyncOwner != virThreadSelfID()) {
        VIR_WARN("'%s' async job is owned by thread %llu",
                 virDomainAsyncJobTypeToString(job->asyncJob),
                 job->asyncOwner);
    }
    job->asyncOwner = 0;
}

static bool
virDomainNestedJobAllowed(virDomainJobObjPtr jobs, virDomainJob newJob)
{
    return !jobs->asyncJob ||
           newJob == QEMU_JOB_NONE ||
           (jobs->mask & JOB_MASK(newJob)) != 0;
}

bool
virDomainJobAllowed(virDomainJobObjPtr jobs, virDomainJobObj newJob)
{
    return !jobs->active && virDomainNestedJobAllowed(jobs, newJob);
}

static bool
virDomainObjCanSetJob(virDomainJobObjPtr job,
                       virDomainJob newJob,
                       virDomainAgentJob newAgentJob)
{
    return ((newJob == VIR_JOB_NONE ||
             job->active == VIR_JOB_NONE) &&
            (newAgentJob == VIR_AGENT_JOB_NONE ||
             job->agentActive == VIR_AGENT_JOB_NONE));
}
