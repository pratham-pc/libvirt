/*
 * qemu_domainjob.c: helper functions for QEMU domain jobs
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

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_domainjob");

VIR_ENUM_IMPL(qemuDomainJob,
              QEMU_JOB_LAST,
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

VIR_ENUM_IMPL(qemuDomainAgentJob,
              QEMU_AGENT_JOB_LAST,
              "none",
              "query",
              "modify",
);

VIR_ENUM_IMPL(qemuDomainAsyncJob,
              QEMU_ASYNC_JOB_LAST,
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
qemuDomainAsyncJobPhaseToString(qemuDomainAsyncJob job,
                                int phase G_GNUC_UNUSED)
{
    switch (job) {
    case QEMU_ASYNC_JOB_MIGRATION_OUT:
    case QEMU_ASYNC_JOB_MIGRATION_IN:
        return qemuMigrationJobPhaseTypeToString(phase);

    case QEMU_ASYNC_JOB_SAVE:
    case QEMU_ASYNC_JOB_DUMP:
    case QEMU_ASYNC_JOB_SNAPSHOT:
    case QEMU_ASYNC_JOB_START:
    case QEMU_ASYNC_JOB_NONE:
    case QEMU_ASYNC_JOB_BACKUP:
        G_GNUC_FALLTHROUGH;
    case QEMU_ASYNC_JOB_LAST:
        break;
    }

    return "none";
}

int
qemuDomainAsyncJobPhaseFromString(qemuDomainAsyncJob job,
                                  const char *phase)
{
    if (!phase)
        return 0;

    switch (job) {
    case QEMU_ASYNC_JOB_MIGRATION_OUT:
    case QEMU_ASYNC_JOB_MIGRATION_IN:
        return qemuMigrationJobPhaseTypeFromString(phase);

    case QEMU_ASYNC_JOB_SAVE:
    case QEMU_ASYNC_JOB_DUMP:
    case QEMU_ASYNC_JOB_SNAPSHOT:
    case QEMU_ASYNC_JOB_START:
    case QEMU_ASYNC_JOB_NONE:
    case QEMU_ASYNC_JOB_BACKUP:
        G_GNUC_FALLTHROUGH;
    case QEMU_ASYNC_JOB_LAST:
        break;
    }

    if (STREQ(phase, "none"))
        return 0;
    else
        return -1;
}

int
qemuDomainObjInitJob(qemuDomainJobObjPtr job,
                     qemuDomainObjPrivateJobCallbacksPtr cb,
                     unsigned int maxQueuedJobs)
{
    memset(job, 0, sizeof(*job));
    job->cb = cb;
    job->maxQueuedJobs = maxQueuedJobs;

    if (!(job->privateData = job->cb->allocJobPrivate()))
        return -1;

    if (virCondInit(&job->cond) < 0) {
        job->cb->freeJobPrivate(job->privateData);
        return -1;
    }

    if (virCondInit(&job->asyncCond) < 0) {
        job->cb->freeJobPrivate(job->privateData);
        virCondDestroy(&job->cond);
        return -1;
    }

    return 0;
}


static void
qemuDomainObjResetJob(qemuDomainJobObjPtr job)
{
    job->active = QEMU_JOB_NONE;
    job->owner = 0;
    job->ownerAPI = NULL;
    job->started = 0;
}


static void
qemuDomainObjResetAgentJob(qemuDomainJobObjPtr job)
{
    job->agentActive = QEMU_AGENT_JOB_NONE;
    job->agentOwner = 0;
    job->agentOwnerAPI = NULL;
    job->agentStarted = 0;
}


static void
qemuDomainObjResetAsyncJob(qemuDomainJobObjPtr job)
{
    job->asyncJob = QEMU_ASYNC_JOB_NONE;
    job->asyncOwner = 0;
    job->asyncOwnerAPI = NULL;
    job->asyncStarted = 0;
    job->phase = 0;
    job->mask = QEMU_JOB_DEFAULT_MASK;
    job->abortJob = false;
    VIR_FREE(job->error);
    job->cb->resetJobPrivate(job->privateData);
    job->apiFlags = 0;
}

int
qemuDomainObjRestoreJob(virDomainObjPtr obj,
                        qemuDomainJobObjPtr job)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    memset(job, 0, sizeof(*job));
    job->active = priv->job.active;
    job->owner = priv->job.owner;
    job->asyncJob = priv->job.asyncJob;
    job->asyncOwner = priv->job.asyncOwner;
    job->phase = priv->job.phase;
    job->privateData = g_steal_pointer(&priv->job.privateData);
    job->apiFlags = priv->job.apiFlags;

    if (!(priv->job.privateData = priv->job.cb->allocJobPrivate()))
        return -1;
    job->cb = priv->job.cb;

    qemuDomainObjResetJob(&priv->job);
    qemuDomainObjResetAsyncJob(&priv->job);
    return 0;
}

void
qemuDomainObjFreeJob(qemuDomainJobObjPtr job)
{
    qemuDomainObjResetJob(job);
    qemuDomainObjResetAsyncJob(job);
    job->cb->freeJobPrivate(job->privateData);
    virCondDestroy(&job->cond);
    virCondDestroy(&job->asyncCond);
}

bool
qemuDomainTrackJob(qemuDomainJob job)
{
    return (QEMU_DOMAIN_TRACK_JOBS & JOB_MASK(job)) != 0;
}


void
qemuDomainObjSetJobPhase(virQEMUDriverPtr driver,
                         virDomainObjPtr obj,
                         int phase)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    unsigned long long me = virThreadSelfID();

    if (!priv->job.asyncJob)
        return;

    VIR_DEBUG("Setting '%s' phase to '%s'",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              qemuDomainAsyncJobPhaseToString(priv->job.asyncJob, phase));

    if (priv->job.asyncOwner && me != priv->job.asyncOwner) {
        VIR_WARN("'%s' async job is owned by thread %llu",
                 qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
                 priv->job.asyncOwner);
    }

    priv->job.phase = phase;
    priv->job.asyncOwner = me;
    qemuDomainObjSaveStatus(driver, obj);
}

void
qemuDomainObjSetAsyncJobMask(virDomainObjPtr obj,
                             unsigned long long allowedJobs)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    if (!priv->job.asyncJob)
        return;

    priv->job.mask = allowedJobs | JOB_MASK(QEMU_JOB_DESTROY);
}

void
qemuDomainObjDiscardAsyncJob(virQEMUDriverPtr driver, virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    if (priv->job.active == QEMU_JOB_ASYNC_NESTED)
        qemuDomainObjResetJob(&priv->job);
    qemuDomainObjResetAsyncJob(&priv->job);
    qemuDomainObjSaveStatus(driver, obj);
}

void
qemuDomainObjReleaseAsyncJob(virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    VIR_DEBUG("Releasing ownership of '%s' async job",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob));

    if (priv->job.asyncOwner != virThreadSelfID()) {
        VIR_WARN("'%s' async job is owned by thread %llu",
                 qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
                 priv->job.asyncOwner);
    }
    priv->job.asyncOwner = 0;
}

static bool
qemuDomainNestedJobAllowed(qemuDomainJobObjPtr jobs, qemuDomainJob newJob)
{
    return !jobs->asyncJob ||
           newJob == QEMU_JOB_NONE ||
           (jobs->mask & JOB_MASK(newJob)) != 0;
}

bool
qemuDomainJobAllowed(qemuDomainJobObjPtr jobs, qemuDomainJob newJob)
{
    return !jobs->active && qemuDomainNestedJobAllowed(jobs, newJob);
}

static bool
qemuDomainObjCanSetJob(qemuDomainJobObjPtr job,
                       qemuDomainJob newJob,
                       qemuDomainAgentJob newAgentJob)
{
    return ((newJob == QEMU_JOB_NONE ||
             job->active == QEMU_JOB_NONE) &&
            (newAgentJob == QEMU_AGENT_JOB_NONE ||
             job->agentActive == QEMU_AGENT_JOB_NONE));
}

/* Give up waiting for mutex after 30 seconds */
#define QEMU_JOB_WAIT_TIME (1000ull * 30)

/**
 * qemuDomainObjBeginJobInternal:
 * @driver: qemu driver
 * @obj: domain object
 * @job: qemuDomainJob to start
 * @asyncJob: qemuDomainAsyncJob to start
 * @nowait: don't wait trying to acquire @job
 *
 * Acquires job for a domain object which must be locked before
 * calling. If there's already a job running waits up to
 * QEMU_JOB_WAIT_TIME after which the functions fails reporting
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
qemuDomainObjBeginJobInternal(virQEMUDriverPtr driver,
                              virDomainObjPtr obj,
                              qemuDomainJobObjPtr jobObj,
                              qemuDomainJob job,
                              qemuDomainAgentJob agentJob,
                              qemuDomainAsyncJob asyncJob,
                              bool nowait)
{
    unsigned long long now;
    unsigned long long then;
    bool nested = job == QEMU_JOB_ASYNC_NESTED;
    bool async = job == QEMU_JOB_ASYNC;
    const char *blocker = NULL;
    const char *agentBlocker = NULL;
    int ret = -1;
    unsigned long long duration = 0;
    unsigned long long agentDuration = 0;
    unsigned long long asyncDuration = 0;

    VIR_DEBUG("Starting job: job=%s agentJob=%s asyncJob=%s "
              "(vm=%p name=%s, current job=%s agentJob=%s async=%s)",
              qemuDomainJobTypeToString(job),
              qemuDomainAgentJobTypeToString(agentJob),
              qemuDomainAsyncJobTypeToString(asyncJob),
              obj, obj->def->name,
              qemuDomainJobTypeToString(jobObj->active),
              qemuDomainAgentJobTypeToString(jobObj->agentActive),
              qemuDomainAsyncJobTypeToString(jobObj->asyncJob));

    if (virTimeMillisNow(&now) < 0)
        return -1;

    jobObj->jobs_queued++;
    then = now + QEMU_JOB_WAIT_TIME;

 retry:
    if ((!async && job != QEMU_JOB_DESTROY) &&
        jobObj->maxQueuedJobs &&
        jobObj->jobs_queued > jobObj->maxQueuedJobs) {
        goto error;
    }

    while (!nested && !qemuDomainNestedJobAllowed(jobObj, job)) {
        if (nowait)
            goto cleanup;

        VIR_DEBUG("Waiting for async job (vm=%p name=%s)", obj, obj->def->name);
        if (virCondWaitUntil(&jobObj->asyncCond, &obj->parent.lock, then) < 0)
            goto error;
    }

    while (!qemuDomainObjCanSetJob(jobObj, job, agentJob)) {
        if (nowait)
            goto cleanup;

        VIR_DEBUG("Waiting for job (vm=%p name=%s)", obj, obj->def->name);
        if (virCondWaitUntil(&jobObj->cond, &obj->parent.lock, then) < 0)
            goto error;
    }

    /* No job is active but a new async job could have been started while obj
     * was unlocked, so we need to recheck it. */
    if (!nested && !qemuDomainNestedJobAllowed(jobObj, job))
        goto retry;

    ignore_value(virTimeMillisNow(&now));

    if (job) {
        qemuDomainObjResetJob(jobObj);

        if (job != QEMU_JOB_ASYNC) {
            VIR_DEBUG("Started job: %s (async=%s vm=%p name=%s)",
                      qemuDomainJobTypeToString(job),
                      qemuDomainAsyncJobTypeToString(jobObj->asyncJob),
                      obj, obj->def->name);
            jobObj->active = job;
            jobObj->owner = virThreadSelfID();
            jobObj->ownerAPI = virThreadJobGet();
            jobObj->started = now;
        } else {
            VIR_DEBUG("Started async job: %s (vm=%p name=%s)",
                      qemuDomainAsyncJobTypeToString(asyncJob),
                      obj, obj->def->name);
            qemuDomainObjResetAsyncJob(jobObj);
            jobObj->cb->currentJobInfoInit(jobObj, now);
            jobObj->asyncJob = asyncJob;
            jobObj->asyncOwner = virThreadSelfID();
            jobObj->asyncOwnerAPI = virThreadJobGet();
            jobObj->asyncStarted = now;
        }
    }

    if (agentJob) {
        qemuDomainObjResetAgentJob(jobObj);

        VIR_DEBUG("Started agent job: %s (vm=%p name=%s job=%s async=%s)",
                  qemuDomainAgentJobTypeToString(agentJob),
                  obj, obj->def->name,
                  qemuDomainJobTypeToString(jobObj->active),
                  qemuDomainAsyncJobTypeToString(jobObj->asyncJob));
        jobObj->agentActive = agentJob;
        jobObj->agentOwner = virThreadSelfID();
        jobObj->agentOwnerAPI = virThreadJobGet();
        jobObj->agentStarted = now;
    }

    if (qemuDomainTrackJob(job))
        qemuDomainObjSaveStatus(driver, obj);

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
             qemuDomainJobTypeToString(job),
             qemuDomainAgentJobTypeToString(agentJob),
             qemuDomainAsyncJobTypeToString(asyncJob),
             obj->def->name,
             qemuDomainJobTypeToString(jobObj->active),
             qemuDomainAgentJobTypeToString(jobObj->agentActive),
             qemuDomainAsyncJobTypeToString(jobObj->asyncJob),
             jobObj->owner, NULLSTR(jobObj->ownerAPI),
             jobObj->agentOwner, NULLSTR(jobObj->agentOwnerAPI),
             jobObj->asyncOwner, NULLSTR(jobObj->asyncOwnerAPI),
             jobObj->apiFlags,
             duration / 1000, agentDuration / 1000, asyncDuration / 1000);

    if (job) {
        if (nested || qemuDomainNestedJobAllowed(jobObj, job))
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
    } else if (jobObj->maxQueuedJobs &&
               jobObj->jobs_queued > jobObj->maxQueuedJobs) {
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
    jobObj->jobs_queued--;
    return ret;
}

/*
 * obj must be locked before calling
 *
 * This must be called by anything that will change the VM state
 * in any way, or anything that will use the QEMU monitor.
 *
 * Successful calls must be followed by EndJob eventually
 */
int qemuDomainObjBeginJob(virQEMUDriverPtr driver,
                          virDomainObjPtr obj,
                          qemuDomainJob job)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;

    if (qemuDomainObjBeginJobInternal(driver, obj, jobObj, job,
                                      QEMU_AGENT_JOB_NONE,
                                      QEMU_ASYNC_JOB_NONE, false) < 0)
        return -1;
    else
        return 0;
}

/**
 * qemuDomainObjBeginAgentJob:
 *
 * Grabs agent type of job. Use if caller talks to guest agent only.
 *
 * To end job call qemuDomainObjEndAgentJob.
 */
int
qemuDomainObjBeginAgentJob(virQEMUDriverPtr driver,
                           virDomainObjPtr obj,
                           qemuDomainAgentJob agentJob)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;

    return qemuDomainObjBeginJobInternal(driver, obj, jobObj, QEMU_JOB_NONE,
                                         agentJob,
                                         QEMU_ASYNC_JOB_NONE, false);
}

int qemuDomainObjBeginAsyncJob(virQEMUDriverPtr driver,
                               virDomainObjPtr obj,
                               qemuDomainAsyncJob asyncJob,
                               virDomainJobOperation operation,
                               unsigned long apiFlags)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;

    if (qemuDomainObjBeginJobInternal(driver, obj, jobObj, QEMU_JOB_ASYNC,
                                      QEMU_AGENT_JOB_NONE,
                                      asyncJob, false) < 0)
        return -1;

    priv->job.cb->setJobInfoOperation(jobObj, operation);
    priv->job.apiFlags = apiFlags;
    return 0;
}

int
qemuDomainObjBeginNestedJob(virQEMUDriverPtr driver,
                            virDomainObjPtr obj,
                            qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;

    if (asyncJob != priv->job.asyncJob) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected async job %d type expected %d"),
                       asyncJob, priv->job.asyncJob);
        return -1;
    }

    if (priv->job.asyncOwner != virThreadSelfID()) {
        VIR_WARN("This thread doesn't seem to be the async job owner: %llu",
                 priv->job.asyncOwner);
    }

    return qemuDomainObjBeginJobInternal(driver, obj, jobObj,
                                         QEMU_JOB_ASYNC_NESTED,
                                         QEMU_AGENT_JOB_NONE,
                                         QEMU_ASYNC_JOB_NONE,
                                         false);
}

/**
 * qemuDomainObjBeginJobNowait:
 *
 * @driver: qemu driver
 * @obj: domain object
 * @job: qemuDomainJob to start
 *
 * Acquires job for a domain object which must be locked before
 * calling. If there's already a job running it returns
 * immediately without any error reported.
 *
 * Returns: see qemuDomainObjBeginJobInternal
 */
int
qemuDomainObjBeginJobNowait(virQEMUDriverPtr driver,
                            virDomainObjPtr obj,
                            qemuDomainJob job)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;

    return qemuDomainObjBeginJobInternal(driver, obj, jobObj, job,
                                         QEMU_AGENT_JOB_NONE,
                                         QEMU_ASYNC_JOB_NONE, true);
}

/*
 * obj must be locked and have a reference before calling
 *
 * To be called after completing the work associated with the
 * earlier qemuDomainBeginJob() call
 */
void
qemuDomainObjEndJob(virQEMUDriverPtr driver, virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;
    qemuDomainJob job = priv->job.active;

    jobObj->jobs_queued--;

    VIR_DEBUG("Stopping job: %s (async=%s vm=%p name=%s)",
              qemuDomainJobTypeToString(job),
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              obj, obj->def->name);

    qemuDomainObjResetJob(&priv->job);
    if (qemuDomainTrackJob(job))
        qemuDomainObjSaveStatus(driver, obj);
    /* We indeed need to wake up ALL threads waiting because
     * grabbing a job requires checking more variables. */
    virCondBroadcast(&priv->job.cond);
}

void
qemuDomainObjEndAgentJob(virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;
    qemuDomainAgentJob agentJob = priv->job.agentActive;

    jobObj->jobs_queued--;

    VIR_DEBUG("Stopping agent job: %s (async=%s vm=%p name=%s)",
              qemuDomainAgentJobTypeToString(agentJob),
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              obj, obj->def->name);

    qemuDomainObjResetAgentJob(&priv->job);
    /* We indeed need to wake up ALL threads waiting because
     * grabbing a job requires checking more variables. */
    virCondBroadcast(&priv->job.cond);
}

void
qemuDomainObjEndAsyncJob(virQEMUDriverPtr driver, virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;

    jobObj->jobs_queued--;

    VIR_DEBUG("Stopping async job: %s (vm=%p name=%s)",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              obj, obj->def->name);

    qemuDomainObjResetAsyncJob(&priv->job);
    qemuDomainObjSaveStatus(driver, obj);
    virCondBroadcast(&priv->job.asyncCond);
}

void
qemuDomainObjAbortAsyncJob(virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    VIR_DEBUG("Requesting abort of async job: %s (vm=%p name=%s)",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              obj, obj->def->name);

    priv->job.abortJob = true;
    virDomainObjBroadcast(obj);
}


static int
qemuDomainObjPrivateXMLFormatNBDMigrationSource(virBufferPtr buf,
                                                virStorageSourcePtr src,
                                                virDomainXMLOptionPtr xmlopt)
{
    g_auto(virBuffer) attrBuf = VIR_BUFFER_INITIALIZER;
    g_auto(virBuffer) childBuf = VIR_BUFFER_INIT_CHILD(buf);

    virBufferAsprintf(&attrBuf, " type='%s' format='%s'",
                      virStorageTypeToString(src->type),
                      virStorageFileFormatTypeToString(src->format));

    if (virDomainDiskSourceFormat(&childBuf, src, "source", 0, false,
                                  VIR_DOMAIN_DEF_FORMAT_STATUS,
                                  false, false, xmlopt) < 0)
        return -1;

    virXMLFormatElement(buf, "migrationSource", &attrBuf, &childBuf);

    return 0;
}


static int
qemuDomainObjPrivateXMLFormatNBDMigration(virBufferPtr buf,
                                          virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    size_t i;
    virDomainDiskDefPtr disk;
    qemuDomainDiskPrivatePtr diskPriv;

    for (i = 0; i < vm->def->ndisks; i++) {
        g_auto(virBuffer) attrBuf = VIR_BUFFER_INITIALIZER;
        g_auto(virBuffer) childBuf = VIR_BUFFER_INIT_CHILD(buf);
        disk = vm->def->disks[i];
        diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);

        virBufferAsprintf(&attrBuf, " dev='%s' migrating='%s'",
                          disk->dst, diskPriv->migrating ? "yes" : "no");

        if (diskPriv->migrSource &&
            qemuDomainObjPrivateXMLFormatNBDMigrationSource(&childBuf,
                                                            diskPriv->migrSource,
                                                            priv->driver->xmlopt) < 0)
            return -1;

        virXMLFormatElement(buf, "disk", &attrBuf, &childBuf);
    }

    return 0;
}

int
qemuDomainObjPrivateXMLFormatJob(virBufferPtr buf,
                                 virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuDomainJobObjPtr jobObj = &priv->job;
    g_auto(virBuffer) attrBuf = VIR_BUFFER_INITIALIZER;
    g_auto(virBuffer) childBuf = VIR_BUFFER_INIT_CHILD(buf);
    qemuDomainJob job = priv->job.active;

    if (!qemuDomainTrackJob(job))
        job = QEMU_JOB_NONE;

    if (job == QEMU_JOB_NONE &&
        priv->job.asyncJob == QEMU_ASYNC_JOB_NONE)
        return 0;

    virBufferAsprintf(&attrBuf, " type='%s' async='%s'",
                      qemuDomainJobTypeToString(job),
                      qemuDomainAsyncJobTypeToString(priv->job.asyncJob));

    if (priv->job.phase) {
        virBufferAsprintf(&attrBuf, " phase='%s'",
                          qemuDomainAsyncJobPhaseToString(priv->job.asyncJob,
                                                          priv->job.phase));
    }

    if (priv->job.asyncJob != QEMU_ASYNC_JOB_NONE)
        virBufferAsprintf(&attrBuf, " flags='0x%lx'", priv->job.apiFlags);

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_OUT &&
        qemuDomainObjPrivateXMLFormatNBDMigration(&childBuf, vm) < 0)
        return -1;

    if (jobObj->cb->formatJob(&childBuf, jobObj) < 0)
        return -1;

    virXMLFormatElement(buf, "job", &attrBuf, &childBuf);

    return 0;
}


static int
qemuDomainObjPrivateXMLParseJobNBDSource(xmlNodePtr node,
                                         xmlXPathContextPtr ctxt,
                                         virDomainDiskDefPtr disk,
                                         virDomainXMLOptionPtr xmlopt)
{
    VIR_XPATH_NODE_AUTORESTORE(ctxt);
    qemuDomainDiskPrivatePtr diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
    g_autofree char *format = NULL;
    g_autofree char *type = NULL;
    g_autoptr(virStorageSource) migrSource = NULL;
    xmlNodePtr sourceNode;

    ctxt->node = node;

    if (!(ctxt->node = virXPathNode("./migrationSource", ctxt)))
        return 0;

    if (!(type = virXMLPropString(ctxt->node, "type"))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing storage source type"));
        return -1;
    }

    if (!(format = virXMLPropString(ctxt->node, "format"))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing storage source format"));
        return -1;
    }

    if (!(migrSource = virDomainStorageSourceParseBase(type, format, NULL)))
        return -1;

    /* newer libvirt uses the <source> subelement instead of formatting the
     * source directly into <migrationSource> */
    if ((sourceNode = virXPathNode("./source", ctxt)))
        ctxt->node = sourceNode;

    if (virDomainStorageSourceParse(ctxt->node, ctxt, migrSource,
                                    VIR_DOMAIN_DEF_PARSE_STATUS, xmlopt) < 0)
        return -1;

    diskPriv->migrSource = g_steal_pointer(&migrSource);
    return 0;
}


static int
qemuDomainObjPrivateXMLParseJobNBD(virDomainObjPtr vm,
                                   xmlXPathContextPtr ctxt)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autofree xmlNodePtr *nodes = NULL;
    size_t i;
    int n;

    if ((n = virXPathNodeSet("./disk[@migrating='yes']", ctxt, &nodes)) < 0)
        return -1;

    if (n > 0) {
        if (priv->job.asyncJob != QEMU_ASYNC_JOB_MIGRATION_OUT) {
            VIR_WARN("Found disks marked for migration but we were not "
                     "migrating");
            n = 0;
        }
        for (i = 0; i < n; i++) {
            virDomainDiskDefPtr disk;
            g_autofree char *dst = NULL;

            if ((dst = virXMLPropString(nodes[i], "dev")) &&
                (disk = virDomainDiskByTarget(vm->def, dst))) {
                QEMU_DOMAIN_DISK_PRIVATE(disk)->migrating = true;

                if (qemuDomainObjPrivateXMLParseJobNBDSource(nodes[i], ctxt,
                                                             disk,
                                                             priv->driver->xmlopt) < 0)
                    return -1;
            }
        }
    }

    return 0;
}

int
qemuDomainObjPrivateXMLParseJob(virDomainObjPtr vm,
                                xmlXPathContextPtr ctxt)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuDomainJobObjPtr job = &priv->job;
    VIR_XPATH_NODE_AUTORESTORE(ctxt);
    g_autofree char *tmp = NULL;

    if (!(ctxt->node = virXPathNode("./job[1]", ctxt)))
        return 0;

    if ((tmp = virXPathString("string(@type)", ctxt))) {
        int type;

        if ((type = qemuDomainJobTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown job type %s"), tmp);
            return -1;
        }
        VIR_FREE(tmp);
        priv->job.active = type;
    }

    if ((tmp = virXPathString("string(@async)", ctxt))) {
        int async;

        if ((async = qemuDomainAsyncJobTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown async job type %s"), tmp);
            return -1;
        }
        VIR_FREE(tmp);
        priv->job.asyncJob = async;

        if ((tmp = virXPathString("string(@phase)", ctxt))) {
            priv->job.phase = qemuDomainAsyncJobPhaseFromString(async, tmp);
            if (priv->job.phase < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown job phase %s"), tmp);
                return -1;
            }
            VIR_FREE(tmp);
        }
    }

    if (virXPathULongHex("string(@flags)", ctxt, &priv->job.apiFlags) == -2) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid job flags"));
        return -1;
    }

    if (qemuDomainObjPrivateXMLParseJobNBD(vm, ctxt) < 0)
        return -1;

    if (job->cb->parseJob(ctxt, job) < 0)
        return -1;

    return 0;
}
