// copyright 2013 Brandon

#include "../tools_stdafx.h"

#ifdef PTHREADS

#include <signal.h>
#include <string.h>
#include <sched.h>
#include <new>
#include <assert.h>

#if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
#   include<time.h>
#else
#   include<sys/time.h>
#endif
#include "AThread.h"
#include "ThreadHandle.h"
#include "../mutex/Mutex.h"

using tools::AThread;
using tools::ThreadHandle;

namespace {

struct ThreadRunnerArg {
    AThread *mObj;
    PosixCondVar *mCondVar;
};
}


AThread::AThread()
    : mIsStopRequested(false),
    mHandle(0),
    mAccessMutex(),
    mIsCreated(false),
    mDefaultPriority(-1)
{
    mCondVar = new PosixCondVar();

    int policy = 0;
    struct sched_param param = {0};
    if (pthread_getschedparam(pthread_self(), &policy, &param) == 0)
        mDefaultPriority = param.sched_priority;
}


AThread::~AThread()
{
    Mutex::Lock lock(mAccessMutex);

    if (mCondVar)
    {
        pthread_cond_broadcast(&mCondVar->getCondVar());
        mCondVar->release();
        mCondVar = 0;
    }

    if (mIsCreated)
    {
        #ifdef ANDROID
            pthread_kill(mHandle.getHandle(), SIGUSR1);
        #else
            pthread_cancel(mHandle.getHandle());
        #endif

        mHandle.setHandle(0);
        mIsCreated = false;
    }
}


ThreadHandle const *AThread::getHandle() const
{
    return &mHandle;
}

bool AThread::start()
{
    Mutex::Lock lock(mAccessMutex);

    if (isRunning())
        return true;

    mCondVar->mIsSignaled = false;

    // the created thread is Detached i.e. cannot be joined - we implement
    // join by ourselves
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    PosixCondVar *condVar = new PosixCondVar();
    ThreadRunnerArg arg = {this, condVar};

    pthread_t thread;
    int result = pthread_create(&thread, &attr, &threadRunner, (void*) &arg);

    if (result == 0)
    {
        pthread_mutex_lock(&condVar->getMutex());
        while (!condVar->mIsSignaled)
            pthread_cond_wait(&condVar->getCondVar(), &condVar->getMutex());
        pthread_mutex_unlock(&condVar->getMutex());
    }

    condVar->release();

    pthread_attr_destroy(&attr);
    if (result)
        return false;
    else
    {
        mHandle.setHandle(thread);
        mIsCreated = true;
    }

    return true;
}


bool AThread::join(unsigned long waitMilliseconds)
{
    PosixCondVar *localCondVar = 0;
    {
        Mutex::Lock lock(mAccessMutex);
        localCondVar = mCondVar;
        if (localCondVar)
            localCondVar->addRef();
        else
            return false;
    }

    pthread_mutex_lock(&localCondVar->getMutex());

    if (!isRunning())
    {
        pthread_mutex_unlock(&localCondVar->getMutex());
        localCondVar->release();
        return true;
    }

    int result = 0;
    if (waitMilliseconds == INFINITE)
    {
        while (!localCondVar->mIsSignaled)
            result = pthread_cond_wait(&localCondVar->getCondVar(), &localCondVar->getMutex());
    }
    else
    {
        struct timespec time = {0};
        #if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
            clock_gettime(CLOCK_REALTIME, &time);
        #else
            struct timeval tv;
            gettimeofday(&tv, NULL);
            time.tv_sec = tv.tv_sec;
            time.tv_nsec = tv.tv_usec * 1000;
        #endif

        unsigned long sec = time.tv_sec + waitMilliseconds / 1000;
        unsigned long nsec = time.tv_nsec + (waitMilliseconds % 1000) * 1000000;
        time.tv_sec = sec + nsec / 1000000000;
        time.tv_nsec = nsec % 1000000000;

        while (!localCondVar->mIsSignaled)
            if ((result = pthread_cond_timedwait(&localCondVar->getCondVar(), &localCondVar->getMutex(), &time)) == ETIMEDOUT)
                break;
    }

    pthread_mutex_unlock(&localCondVar->getMutex());
    localCondVar->release();

    return result == 0;
}

void AThread::requestStop()
{
    mIsStopRequested = true;
}

bool AThread::isStopRequested() const
{
    return mIsStopRequested;
}

bool AThread::isRunning() const
{
    Mutex::Lock lock(mAccessMutex);
    if (mIsCreated)
        return pthread_kill(mHandle.getHandle(), 0) == 0;
    return false;
}

void *AThread::threadRunner(void *param)
{
    if (!param)
        assert(0);
        //pthread_exit(0);

    ThreadRunnerArg *arg = static_cast<ThreadRunnerArg*>(param);
    AThread *obj = arg->mObj;

    PosixCondVar *localCondVar = obj->mCondVar;
    if (localCondVar)
        localCondVar->addRef();

    PosixCondVar *condVar = arg->mCondVar;
    pthread_mutex_lock(&condVar->getMutex());
    condVar->mIsSignaled = true;
    pthread_mutex_unlock(&condVar->getMutex());
    pthread_cond_signal(&condVar->getCondVar());

    if (obj->run() == THREAD_OBJECT_HAS_DELETED)
    {

        if (localCondVar)
        {
            pthread_mutex_lock(&localCondVar->getMutex());
            localCondVar->mIsSignaled = true;
            pthread_mutex_unlock(&localCondVar->getMutex());
            pthread_cond_broadcast(&localCondVar->getCondVar());
            localCondVar->release();
        }
        pthread_exit(0);
    }

    if (localCondVar)
        localCondVar->release();

    pthread_cleanup_push(threadCleanup, obj);
    pthread_exit(0);
    pthread_cleanup_pop(0);
}

void AThread::threadCleanup(void *param)
{
    if (!param)
        return;

    AThread *obj = static_cast<AThread*>(param);
    Mutex::Lock lock(obj->mAccessMutex);
    obj->mIsCreated = false;

    pthread_mutex_lock(&obj->mCondVar->getMutex());
    obj->mCondVar->mIsSignaled = true;
    pthread_mutex_unlock(&obj->mCondVar->getMutex());
    pthread_cond_broadcast(&obj->mCondVar->getCondVar());
}

/**
 Get thread priority.
*/
AThread::PriorityLevel AThread::getPriority() const
{
    int policy = 0;
    struct sched_param param = {0};

    int ret_val = pthread_getschedparam(mHandle.getHandle(), &policy, &param);
    if (ret_val == 0)
    {
        if (param.sched_priority == mDefaultPriority)
            return PriorityDefault;
        else if (param.sched_priority == sched_get_priority_min(policy))
            return PriorityLow;
        else if (param.sched_priority == sched_get_priority_max(policy))
            return PriorityHigh;
        else if (param.sched_priority == ((sched_get_priority_min(policy) + sched_get_priority_min(policy)) / 2))
            return PriorityNormal;
        else
            return PriorityUnknown;
    }

    return PriorityError;
}

/**
 Set thread priority.
*/
bool AThread::setPriority(PriorityLevel ePrior)
{
    int policy = 0;
    struct sched_param param = {0};
    int ret_val = 0;

    ret_val = pthread_getschedparam(mHandle.getHandle(), &policy, &param);
    if (ret_val != 0)
        return false;

    switch (ePrior)
    {
    case PriorityLow:
        param.sched_priority = sched_get_priority_min(policy);
        break;
    case PriorityNormal:
        param.sched_priority = (sched_get_priority_min(policy) + sched_get_priority_max(policy)) / 2;
        break;
    case PriorityHigh:
        param.sched_priority = sched_get_priority_max(policy);
        break;
    case PriorityDefault:
        param.sched_priority = mDefaultPriority;
        break;
    default:
        return false;
    }

    ret_val = pthread_setschedparam(mHandle.getHandle(), policy, &param);

    return ret_val == 0;
}

#endif // PTHREADS
