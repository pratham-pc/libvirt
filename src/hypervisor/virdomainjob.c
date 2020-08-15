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

#include "domain_conf.h"
#include "virdomainjob.h"
#include "virmigration.h"
#include "viralloc.h"
#include "virlog.h"
#include "virerror.h"
#include "virtime.h"
#include "virthreadjob.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("util.virdomainjob");

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
        return virMigrationJobPhaseTypeToString(phase);

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
        return virMigrationJobPhaseTypeFromString(phase);

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

int
virDomainObjInitJob(virDomainJobObjPtr job,
                    virDomainJobPrivateCallbacksPtr cb)
{
    memset(job, 0, sizeof(*job));
    job->cb = cb;

    if (!(job->privateData = job->cb->jobcb->allocJobPrivate()))
        return -1;

    if (virCondInit(&job->cond) < 0) {
        job->cb->jobcb->freeJobPrivate(job->privateData);
        return -1;
    }

    if (virCondInit(&job->asyncCond) < 0) {
        job->cb->jobcb->freeJobPrivate(job->privateData);
        virCondDestroy(&job->cond);
        return -1;
    }

    return 0;
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
    job->cb->jobcb->resetJobPrivate(job->privateData);
    job->apiFlags = 0;
}

int
virDomainObjRestoreJob(virDomainJobObjPtr job,
                       virDomainJobObjPtr oldJob)
{
    memset(oldJob, 0, sizeof(*oldJob));
    oldJob->active = job->active;
    oldJob->owner = job->owner;
    oldJob->asyncJob = job->asyncJob;
    oldJob->asyncOwner = job->asyncOwner;
    oldJob->phase = job->phase;
    oldJob->privateData = g_steal_pointer(&job->privateData);
    oldJob->apiFlags = job->apiFlags;

    if (!(job->privateData = job->cb->jobcb->allocJobPrivate()))
        return -1;
    oldJob->cb = job->cb;

    virDomainObjResetJob(job);
    virDomainObjResetAsyncJob(job);
    return 0;
}

void
virDomainObjFreeJob(virDomainJobObjPtr job)
{
    virDomainObjResetJob(job);
    virDomainObjResetAsyncJob(job);
    job->cb->jobcb->freeJobPrivate(job->privateData);
    virCondDestroy(&job->cond);
    virCondDestroy(&job->asyncCond);
}

bool
virDomainTrackJob(virDomainJob job)
{
    return (VIR_DOMAIN_TRACK_JOBS & JOB_MASK(job)) != 0;
}


void
virDomainObjSetJobPhase(virDomainObjPtr obj,
                        virDomainJobObjPtr job,
                        int phase)
{
    unsigned long long me = virThreadSelfID();

    if (!job->asyncJob)
        return;

    VIR_DEBUG("Setting '%s' phase to '%s'",
              virDomainAsyncJobTypeToString(job->asyncJob),
              virDomainAsyncJobPhaseToString(job->asyncJob, phase));

    if (job->asyncOwner && me != job->asyncOwner) {
        VIR_WARN("'%s' async job is owned by thread %llu",
                 virDomainAsyncJobTypeToString(job->asyncJob),
                 job->asyncOwner);
    }

    job->phase = phase;
    job->asyncOwner = me;
    job->cb->saveStatus(obj);
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
virDomainObjDiscardAsyncJob(virDomainObjPtr obj,
                            virDomainJobObjPtr job)
{
    if (job->active == VIR_JOB_ASYNC_NESTED)
        virDomainObjResetJob(job);
    virDomainObjResetAsyncJob(job);
    job->cb->saveStatus(obj);
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
           newJob == VIR_JOB_NONE ||
           (jobs->mask & JOB_MASK(newJob)) != 0;
}

bool
virDomainJobAllowed(virDomainJobObjPtr jobs, virDomainJob newJob)
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

/* Give up waiting for mutex after 30 seconds */
#define VIR_JOB_WAIT_TIME (1000ull * 30)

/**
 * virDomainObjBeginJobInternal:
 * @obj: domain object
 * @job: virDomainJob to start
 * @asyncJob: virDomainAsyncJob to start
 * @nowait: don't wait trying to acquire @job
 *
 * Acquires job for a domain object which must be locked before
 * calling. If there's already a job running waits up to
 * VIR_JOB_WAIT_TIME after which the functions fails reporting
 * an error unless @nowait is set.
 *
 * If @nowait is true this function tries to acquire job and if
 * it fails, then it returns immediately without waiting. No
 * error is reported in this case.
 *
 * Returns: 0 on success,
 *         -2 if unable to start job because of timeout or
 *            maxQueuedJobs limit,
 *         -1 otherwise.
 */
static int ATTRIBUTE_NONNULL(1)
virDomainObjBeginJobInternal(virDomainObjPtr obj,
                             virDomainJobObjPtr jobObj,
                             virDomainJob job,
                             virDomainAgentJob agentJob,
                             virDomainAsyncJob asyncJob,
                             bool nowait)
{
    unsigned long long now;
    unsigned long long then;
    bool nested = job == VIR_JOB_ASYNC_NESTED;
    bool async = job == VIR_JOB_ASYNC;
    const char *blocker = NULL;
    const char *agentBlocker = NULL;
    int ret = -1;
    unsigned long long duration = 0;
    unsigned long long agentDuration = 0;
    unsigned long long asyncDuration = 0;

    VIR_DEBUG("Starting job: job=%s agentJob=%s asyncJob=%s "
              "(vm=%p name=%s, current job=%s agentJob=%s async=%s)",
              virDomainJobTypeToString(job),
              virDomainAgentJobTypeToString(agentJob),
              virDomainAsyncJobTypeToString(asyncJob),
              obj, obj->def->name,
              virDomainJobTypeToString(jobObj->active),
              virDomainAgentJobTypeToString(jobObj->agentActive),
              virDomainAsyncJobTypeToString(jobObj->asyncJob));

    if (virTimeMillisNow(&now) < 0)
        return -1;

    jobObj->cb->jobcb->increaseJobsQueued(obj);
    then = now + VIR_JOB_WAIT_TIME;

 retry:
    if ((!async && job != VIR_JOB_DESTROY) &&
        jobObj->cb->jobcb->getMaxQueuedJobs(obj) &&
        jobObj->cb->jobcb->getJobsQueued(obj) > jobObj->cb->jobcb->getMaxQueuedJobs(obj)) {
        goto error;
    }

    while (!nested && !virDomainNestedJobAllowed(jobObj, job)) {
        if (nowait)
            goto cleanup;

        VIR_DEBUG("Waiting for async job (vm=%p name=%s)", obj, obj->def->name);
        if (virCondWaitUntil(&jobObj->asyncCond, &obj->parent.lock, then) < 0)
            goto error;
    }

    while (!virDomainObjCanSetJob(jobObj, job, agentJob)) {
        if (nowait)
            goto cleanup;

        VIR_DEBUG("Waiting for job (vm=%p name=%s)", obj, obj->def->name);
        if (virCondWaitUntil(&jobObj->cond, &obj->parent.lock, then) < 0)
            goto error;
    }

    /* No job is active but a new async job could have been started while obj
     * was unlocked, so we need to recheck it. */
    if (!nested && !virDomainNestedJobAllowed(jobObj, job))
        goto retry;

    ignore_value(virTimeMillisNow(&now));

    if (job) {
        virDomainObjResetJob(jobObj);

        if (job != VIR_JOB_ASYNC) {
            VIR_DEBUG("Started job: %s (async=%s vm=%p name=%s)",
                      virDomainJobTypeToString(job),
                      virDomainAsyncJobTypeToString(jobObj->asyncJob),
                      obj, obj->def->name);
            jobObj->active = job;
            jobObj->owner = virThreadSelfID();
            jobObj->ownerAPI = virThreadJobGet();
            jobObj->started = now;
        } else {
            VIR_DEBUG("Started async job: %s (vm=%p name=%s)",
                      virDomainAsyncJobTypeToString(asyncJob),
                      obj, obj->def->name);
            virDomainObjResetAsyncJob(jobObj);
            jobObj->cb->jobcb->currentJobInfoInit(jobObj, now);
            jobObj->asyncJob = asyncJob;
            jobObj->asyncOwner = virThreadSelfID();
            jobObj->asyncOwnerAPI = virThreadJobGet();
            jobObj->asyncStarted = now;
        }
    }

    if (agentJob) {
        virDomainObjResetAgentJob(jobObj);

        VIR_DEBUG("Started agent job: %s (vm=%p name=%s job=%s async=%s)",
                  virDomainAgentJobTypeToString(agentJob),
                  obj, obj->def->name,
                  virDomainJobTypeToString(jobObj->active),
                  virDomainAsyncJobTypeToString(jobObj->asyncJob));
        jobObj->agentActive = agentJob;
        jobObj->agentOwner = virThreadSelfID();
        jobObj->agentOwnerAPI = virThreadJobGet();
        jobObj->agentStarted = now;
    }

    if (virDomainTrackJob(job))
        jobObj->cb->saveStatus(obj);

    return 0;

 error:
    ignore_value(virTimeMillisNow(&now));
    if (jobObj->active && jobObj->started)
        duration = now - jobObj->started;
    if (jobObj->agentActive && jobObj->agentStarted)
        agentDuration = now - jobObj->agentStarted;
    if (jobObj->asyncJob && jobObj->asyncStarted)
        asyncDuration = now - jobObj->asyncStarted;

    VIR_WARN("Cannot start job (%s, %s, %s) for domain %s; "
             "current job is (%s, %s, %s) "
             "owned by (%llu %s, %llu %s, %llu %s (flags=0x%lx)) "
             "for (%llus, %llus, %llus)",
             virDomainJobTypeToString(job),
             virDomainAgentJobTypeToString(agentJob),
             virDomainAsyncJobTypeToString(asyncJob),
             obj->def->name,
             virDomainJobTypeToString(jobObj->active),
             virDomainAgentJobTypeToString(jobObj->agentActive),
             virDomainAsyncJobTypeToString(jobObj->asyncJob),
             jobObj->owner, NULLSTR(jobObj->ownerAPI),
             jobObj->agentOwner, NULLSTR(jobObj->agentOwnerAPI),
             jobObj->asyncOwner, NULLSTR(jobObj->asyncOwnerAPI),
             jobObj->apiFlags,
             duration / 1000, agentDuration / 1000, asyncDuration / 1000);

    if (job) {
        if (nested || virDomainNestedJobAllowed(jobObj, job))
            blocker = jobObj->ownerAPI;
        else
            blocker = jobObj->asyncOwnerAPI;
    }

    if (agentJob)
        agentBlocker = jobObj->agentOwnerAPI;

    if (errno == ETIMEDOUT) {
        if (blocker && agentBlocker) {
            virReportError(VIR_ERR_OPERATION_TIMEOUT,
                           _("cannot acquire state change "
                             "lock (held by monitor=%s agent=%s)"),
                           blocker, agentBlocker);
        } else if (blocker) {
            virReportError(VIR_ERR_OPERATION_TIMEOUT,
                           _("cannot acquire state change "
                             "lock (held by monitor=%s)"),
                           blocker);
        } else if (agentBlocker) {
            virReportError(VIR_ERR_OPERATION_TIMEOUT,
                           _("cannot acquire state change "
                             "lock (held by agent=%s)"),
                           agentBlocker);
        } else {
            virReportError(VIR_ERR_OPERATION_TIMEOUT, "%s",
                           _("cannot acquire state change lock"));
        }
        ret = -2;
    } else if (jobObj->cb->jobcb->getMaxQueuedJobs(obj) &&
               jobObj->cb->jobcb->getJobsQueued(obj) > jobObj->cb->jobcb->getMaxQueuedJobs(obj)) {
        if (blocker && agentBlocker) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("cannot acquire state change "
                             "lock (held by monitor=%s agent=%s) "
                             "due to max_queued limit"),
                           blocker, agentBlocker);
        } else if (blocker) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("cannot acquire state change "
                             "lock (held by monitor=%s) "
                             "due to max_queued limit"),
                           blocker);
        } else if (agentBlocker) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("cannot acquire state change "
                             "lock (held by agent=%s) "
                             "due to max_queued limit"),
                           agentBlocker);
        } else {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("cannot acquire state change lock "
                             "due to max_queued limit"));
        }
        ret = -2;
    } else {
        virReportSystemError(errno, "%s", _("cannot acquire job mutex"));
    }

 cleanup:
    jobObj->cb->jobcb->decreaseJobsQueued(obj);
    return ret;
}

/*
 * obj must be locked before calling
 *
 * This must be called by anything that will change the VM state
 * in any way, or anything that will use the (QEMU) monitor.
 *
 * Successful calls must be followed by EndJob eventually
 */
int virDomainObjBeginJob(virDomainObjPtr obj,
                         virDomainJobObjPtr jobObj,
                         virDomainJob job)
{
    if (virDomainObjBeginJobInternal(obj, jobObj, job,
                                     VIR_AGENT_JOB_NONE,
                                     VIR_ASYNC_JOB_NONE, false) < 0)
        return -1;
    else
        return 0;
}

/**
 * virDomainObjBeginAgentJob:
 *
 * Grabs agent type of job. Use if caller talks to guest agent only.
 *
 * To end job call virDomainObjEndAgentJob.
 */
int
virDomainObjBeginAgentJob(virDomainObjPtr obj,
                          virDomainJobObjPtr jobObj,
                          virDomainAgentJob agentJob)
{
    return virDomainObjBeginJobInternal(obj, jobObj, VIR_JOB_NONE,
                                        agentJob,
                                        VIR_ASYNC_JOB_NONE, false);
}

int virDomainObjBeginAsyncJob(virDomainObjPtr obj,
                              virDomainJobObjPtr jobObj,
                              virDomainAsyncJob asyncJob,
                              virDomainJobOperation operation,
                              unsigned long apiFlags)
{
    if (virDomainObjBeginJobInternal(obj, jobObj, VIR_JOB_ASYNC,
                                     VIR_AGENT_JOB_NONE,
                                     asyncJob, false) < 0)
        return -1;

    jobObj->cb->jobcb->setJobInfoOperation(jobObj, operation);
    jobObj->apiFlags = apiFlags;
    return 0;
}

int
virDomainObjBeginNestedJob(virDomainObjPtr obj,
                           virDomainJobObjPtr jobObj,
                           virDomainAsyncJob asyncJob)
{
    if (asyncJob != jobObj->asyncJob) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected async job %d type expected %d"),
                       asyncJob, jobObj->asyncJob);
        return -1;
    }

    if (jobObj->asyncOwner != virThreadSelfID()) {
        VIR_WARN("This thread doesn't seem to be the async job owner: %llu",
                 jobObj->asyncOwner);
    }

    return virDomainObjBeginJobInternal(obj, jobObj,
                                         VIR_JOB_ASYNC_NESTED,
                                         VIR_AGENT_JOB_NONE,
                                         VIR_ASYNC_JOB_NONE,
                                         false);
}

/**
 * virDomainObjBeginJobNowait:
 *
 * @obj: domain object
 * @jobObj: virDomainJobObjPtr
 * @job: virDomainJob to start
 *
 * Acquires job for a domain object which must be locked before
 * calling. If there's already a job running it returns
 * immediately without any error reported.
 *
 * Returns: see virDomainObjBeginJobInternal
 */
int
virDomainObjBeginJobNowait(virDomainObjPtr obj,
                           virDomainJobObjPtr jobObj,
                           virDomainJob job)
{
    return virDomainObjBeginJobInternal(obj, jobObj, job,
                                        VIR_AGENT_JOB_NONE,
                                        VIR_ASYNC_JOB_NONE, true);
}

/*
 * obj must be locked and have a reference before calling
 *
 * To be called after completing the work associated with the
 * earlier virDomainBeginJob() call
 */
void
virDomainObjEndJob(virDomainObjPtr obj, virDomainJobObjPtr jobObj)
{
    virDomainJob job = jobObj->active;

    jobObj->cb->jobcb->decreaseJobsQueued(obj);

    VIR_DEBUG("Stopping job: %s (async=%s vm=%p name=%s)",
              virDomainJobTypeToString(job),
              virDomainAsyncJobTypeToString(jobObj->asyncJob),
              obj, obj->def->name);

    virDomainObjResetJob(jobObj);
    if (virDomainTrackJob(job))
        jobObj->cb->saveStatus(obj);
    /* We indeed need to wake up ALL threads waiting because
     * grabbing a job requires checking more variables. */
    virCondBroadcast(&jobObj->cond);
}

void
virDomainObjEndAgentJob(virDomainObjPtr obj,
                        virDomainJobObjPtr jobObj)
{
    virDomainAgentJob agentJob = jobObj->agentActive;

    jobObj->cb->jobcb->decreaseJobsQueued(obj);

    VIR_DEBUG("Stopping agent job: %s (async=%s vm=%p name=%s)",
              virDomainAgentJobTypeToString(agentJob),
              virDomainAsyncJobTypeToString(jobObj->asyncJob),
              obj, obj->def->name);

    virDomainObjResetAgentJob(jobObj);
    /* We indeed need to wake up ALL threads waiting because
     * grabbing a job requires checking more variables. */
    virCondBroadcast(&jobObj->cond);
}

void
virDomainObjEndAsyncJob(virDomainObjPtr obj,
                        virDomainJobObjPtr jobObj)
{
    jobObj->cb->jobcb->decreaseJobsQueued(obj);

    VIR_DEBUG("Stopping async job: %s (vm=%p name=%s)",
              virDomainAsyncJobTypeToString(jobObj->asyncJob),
              obj, obj->def->name);

    virDomainObjResetAsyncJob(jobObj);
    jobObj->cb->saveStatus(obj);
    virCondBroadcast(&jobObj->asyncCond);
}

void
virDomainObjAbortAsyncJob(virDomainObjPtr obj,
                          virDomainJobObjPtr job)
{
    VIR_DEBUG("Requesting abort of async job: %s (vm=%p name=%s)",
              virDomainAsyncJobTypeToString(job->asyncJob),
              obj, obj->def->name);

    job->abortJob = true;
    virDomainObjBroadcast(obj);
}

int
virDomainObjPrivateXMLFormatJob(virBufferPtr buf,
                                virDomainObjPtr vm,
                                virDomainJobObjPtr jobObj)
{
    g_auto(virBuffer) attrBuf = VIR_BUFFER_INITIALIZER;
    g_auto(virBuffer) childBuf = VIR_BUFFER_INIT_CHILD(buf);
    virDomainJob job = jobObj->active;

    if (!virDomainTrackJob(job))
        job = VIR_JOB_NONE;

    if (job == VIR_JOB_NONE &&
        jobObj->asyncJob == VIR_ASYNC_JOB_NONE)
        return 0;

    virBufferAsprintf(&attrBuf, " type='%s' async='%s'",
                      virDomainJobTypeToString(job),
                      virDomainAsyncJobTypeToString(jobObj->asyncJob));

    if (jobObj->phase) {
        virBufferAsprintf(&attrBuf, " phase='%s'",
                          virDomainAsyncJobPhaseToString(jobObj->asyncJob,
                                                          jobObj->phase));
    }

    if (jobObj->asyncJob != VIR_ASYNC_JOB_NONE)
        virBufferAsprintf(&attrBuf, " flags='0x%lx'", jobObj->apiFlags);

    if (jobObj->cb->jobcb->formatJob(&childBuf, jobObj, vm) < 0)
        return -1;

    virXMLFormatElement(buf, "job", &attrBuf, &childBuf);

    return 0;
}


int
virDomainObjPrivateXMLParseJob(virDomainObjPtr vm,
                               xmlXPathContextPtr ctxt,
                               virDomainJobObjPtr job)
{
    VIR_XPATH_NODE_AUTORESTORE(ctxt)
    g_autofree char *tmp = NULL;

    if (!(ctxt->node = virXPathNode("./job[1]", ctxt)))
        return 0;

    if ((tmp = virXPathString("string(@type)", ctxt))) {
        int type;

        if ((type = virDomainJobTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown job type %s"), tmp);
            return -1;
        }
        VIR_FREE(tmp);
        job->active = type;
    }

    if ((tmp = virXPathString("string(@async)", ctxt))) {
        int async;

        if ((async = virDomainAsyncJobTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown async job type %s"), tmp);
            return -1;
        }
        VIR_FREE(tmp);
        job->asyncJob = async;

        if ((tmp = virXPathString("string(@phase)", ctxt))) {
            job->phase = virDomainAsyncJobPhaseFromString(async, tmp);
            if (job->phase < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown job phase %s"), tmp);
                return -1;
            }
            VIR_FREE(tmp);
        }
    }

    if (virXPathULongHex("string(@flags)", ctxt, &job->apiFlags) == -2) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid job flags"));
        return -1;
    }

    if (job->cb->jobcb->parseJob(ctxt, job, vm) < 0)
        return -1;

    return 0;
}
