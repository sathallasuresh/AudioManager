/**
 *  SPDX license identifier: MPL-2.0
 *
 * Copyright (C) 2012, BMW AG
 *
 * This file is part of GENIVI Project AudioManager.
 *
 * Contributions are licensed to the GENIVI Alliance under one or more
 * Contribution License Agreements.
 *
 * \copyright
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License, v. 2.0. If a  copy of the MPL was not distributed with
 * this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *
 * \author Christian Linke, christian.linke@bmw.de BMW 2011,2012
 * \author Aleksandar Donchev, aleksander.donchev@partner.bmw.de BMW 2017
 *
 * \file CAmSocketHandler.cpp
 * For further information see http://www.genivi.org/.
 *
 */

#include <cassert>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/poll.h>
#include <time.h>
#include <algorithm>
#include <features.h>
#include <csignal>
#include <unistd.h>
#include <string.h>

#include "CAmDltWrapper.h"
#include "CAmSocketHandler.h"

#ifdef WITH_TIMERFD
#include <sys/timerfd.h>
#endif

namespace am
{

#define CHECK_CALLER_THREAD_ID()\
    if(std::this_thread::get_id() != mThreadID)\
    {\
        logError("Sockethandler: Call from another thread detected!");\
        assert(false);\
    }



    
CAmSocketHandler::CAmSocketHandler() :
        mPipe(), //
        mDispatchDone(true), //
        mSetPollKeys(MAX_POLLHANDLE), //
        mListPoll(), //
        mSetTimerKeys(MAX_TIMERHANDLE),
        mListTimer(), //
        #ifndef WITH_TIMERFD  
        mListActiveTimer(), //
        #else
        mListRemovedTimers(),
        #endif
        mSetSignalhandlerKeys(MAX_POLLHANDLE), //
        mSignalHandlers(), //
        mRecreatePollfds(true),
        mInternalCodes(internal_codes_e::NO_ERROR),
        mSignalFdHandle(0),
        mListActivePolls(),
        mThreadID(std::this_thread::get_id())
        #ifndef WITH_TIMERFD
        ,mStartTime() //
        #endif
{
    if (pipe(mPipe) == -1)
    {
        mInternalCodes = internal_codes_e::PIPE_ERROR;
        logError("Sockethandler could not create pipe!");
    }

    //add the pipe to the poll - nothing needs to be processed here we just need the pipe to trigger the ppoll
    short event = 0;
    sh_pollHandle_t handle;
    event |= POLLIN;
    if (addFDPoll(mPipe[0], event, NULL, [](const pollfd pollfd, const sh_pollHandle_t, void*)
    {}, [](const sh_pollHandle_t, void*)
    {   return (false);}, NULL, NULL, handle) != E_OK)
        mInternalCodes |= internal_codes_e::FD_ERROR;
}

CAmSocketHandler::~CAmSocketHandler()
{
#ifdef WITH_TIMERFD
    closeRemovedTimers();
#endif    
    for (auto it : mListPoll)
    {
        close(it.pollfdValue.fd);
    }
    close(mPipe[0]);
    close(mPipe[1]);
}

//todo: maybe have some: give me more time returned?
/**
  * start the block listening for filedescriptors. This is the mainloop.
  */
void CAmSocketHandler::start_listenting()
{
    mDispatchDone = false;
    int16_t pollStatus;

    CHECK_CALLER_THREAD_ID()
    
#ifndef WITH_TIMERFD 
    clock_gettime(CLOCK_MONOTONIC, &mStartTime);
#endif    
    timespec buffertime;
    
    std::list<sh_poll_s*> listPoll;
    VectorListPoll_t::iterator listmPollIt;
    VectorListPollfd_t::iterator itMfdPollingArray;
    VectorListPollfd_t fdPollingArray; //!<the polling array for ppoll
    
    auto preparePollfd = [&](const sh_poll_s& row)
    {
        CAmSocketHandler::prepare((sh_poll_s&)row);
        pollfd temp = row.pollfdValue;
        temp.revents = 0;
        fdPollingArray.push_back(temp);
    };

    while (!mDispatchDone)
    {
        if (mRecreatePollfds)
        {
            #ifdef WITH_TIMERFD
            closeRemovedTimers();
            #endif
            fdPollingArray.clear();
            //freeze mListPoll by copying it - otherwise we get problems when we want to manipulate it during the next lines
            mListActivePolls = mListPoll;
            //there was a change in the setup, so we need to recreate the fdarray from the list
            std::for_each(mListActivePolls.begin(), mListActivePolls.end(), preparePollfd);
            mRecreatePollfds = false;
        }
        else
        {
            //first we go through the registered filedescriptors and check if someone needs preparation:
            std::for_each(mListActivePolls.begin(), mListActivePolls.end(), CAmSocketHandler::prepare);
        }

#ifndef WITH_TIMERFD
        timerCorrection();
#endif
        //block until something is on a filedescriptor

        if ((pollStatus = ppoll(&fdPollingArray[0], fdPollingArray.size(), insertTime(buffertime), NULL)) < 0)
        {
            if (errno == EINTR)
            {
                //a signal was received, that means it's time to go...
                pollStatus = 0;
            }
            else
            {
                logError("SocketHandler::start_listenting ppoll returned with error", errno);
                throw std::runtime_error(std::string("SocketHandler::start_listenting ppoll returned with error."));
            }
        }

        if (pollStatus != 0) //only check filedescriptors if there was a change
        {
            //todo: here could be a timer that makes sure naughty plugins return!
            listPoll.clear();
            //stage 0+1, call firedCB
            for (itMfdPollingArray = fdPollingArray.begin(); itMfdPollingArray != fdPollingArray.end(); itMfdPollingArray++)
            {
                itMfdPollingArray->revents &= itMfdPollingArray->events | POLLERR | POLLHUP;
                if ( itMfdPollingArray->revents!=0 )
                {                        
                    listmPollIt = mListActivePolls.begin();
                    std::advance(listmPollIt, std::distance(fdPollingArray.begin(), itMfdPollingArray));
                    
                    sh_poll_s & pollObj = *listmPollIt;
                    
                    listPoll.push_back(&pollObj);
                    CAmSocketHandler::fire(&pollObj);
                    itMfdPollingArray->revents = 0;
                }
            }
 
            //stage 2, lets ask around if some dispatching is necessary, the ones who need stay on the list
            listPoll.remove_if(CAmSocketHandler::noDispatching);

            //stage 3, the ones left need to dispatch, we do this as long as there is something to dispatch..
            do
            {
                listPoll.remove_if(CAmSocketHandler::dispatchingFinished);
            } while (!listPoll.empty());

        }
#ifndef WITH_TIMERFD
        else //Timerevent
        {
            //this was a timer event, we need to take care about the timers
            //find out the timedifference to starttime
            timerUp();
        }
#endif
    }
}

/**
  * exits the loop
  */
void CAmSocketHandler::stop_listening()
{
    mDispatchDone = true;
#ifndef WITH_TIMERFD
    //this is for all running timers only - we need to handle the additional offset here
    if (!mListActiveTimer.empty())
    {
        timespec currentTime, correctionTime;
        clock_gettime(CLOCK_MONOTONIC, &currentTime);
        correctionTime = timespecSub(currentTime, mStartTime);
        std::for_each(mListActiveTimer.begin(), mListActiveTimer.end(), [&correctionTime](sh_timer_s& t)
                {   t.countdown = timespecSub(t.countdown, correctionTime);});
    }
#endif
}

void CAmSocketHandler::exit_mainloop()
{
    //end the while loop
    stop_listening();

    //fire the ending filedescriptor
    int p(1);
    ssize_t result = write(mPipe[1], &p, sizeof(p));
}

bool CAmSocketHandler::fatalErrorOccurred() 
{ 
    return ((mInternalCodes&internal_codes_e::PIPE_ERROR)>0)||((mInternalCodes&internal_codes_e::FD_ERROR)>0);
}

am_Error_e CAmSocketHandler::getFDPollData(const sh_pollHandle_t handle, sh_poll_s & outPollData)
{
    VectorListPoll_t::iterator iterator = mListPoll.begin();
    for (; iterator != mListPoll.end(); ++iterator)
    {
        if (iterator->handle == handle)
        {
            outPollData = *iterator;
            return (E_OK);
        }
    }
    return (E_UNKNOWN);
}

/**
  * Adds a signal handler filedescriptor to the polling loop
  *
  */
am_Error_e CAmSocketHandler::listenToSignals(const std::vector<uint8_t> & listSignals)
{
    CHECK_CALLER_THREAD_ID()
    
    int fdErr;
    uint8_t addedSignals = 0;
    sigset_t sigset;
    
    if(0==listSignals.size())
    {
        logWarning("Empty signal list!");
        return (E_NOT_POSSIBLE);
    }
    
    /* Create a sigset of all the signals that we're interested in */
    fdErr = sigemptyset(&sigset);
    if (fdErr != 0)
    {
        logError("Could not create sigset!");
        return (E_NOT_POSSIBLE);
    }
    
    for(uint8_t itSignal : listSignals)
    {
        fdErr = sigaddset(&sigset, itSignal);
        if (fdErr != 0)
            logWarning("Could not add", itSignal);
        else
          addedSignals++;
    }
    
    if(0==addedSignals)
    {
        logWarning("None of the signals were added!");
        return (E_NOT_POSSIBLE);
    }

    /* We must block the signals in order for signalfd to receive them */
    fdErr = sigprocmask(SIG_BLOCK, &sigset, NULL);
    if (fdErr != 0)
    {
        logError("Could not block signals! They must be blocked in order to receive them!");
        return (E_NOT_POSSIBLE);
    }

    sh_poll_s sgPollData;
    if(mSignalFdHandle)
    {
      if(E_OK!=getFDPollData(mSignalFdHandle, sgPollData))
      {
          mSignalFdHandle = 0;
      }
    }
    
    if(0==mSignalFdHandle)
    {
      /* Create the signalfd */
      int signalHandlerFd = signalfd(-1, &sigset, SFD_NONBLOCK);
      if (signalHandlerFd == -1)
      {
          logError("Could not open signal fd!");
          return (E_NOT_POSSIBLE);
      }

      auto actionPoll = [this](const pollfd pollfd, const sh_pollHandle_t, void*)
      {
          const VectorSignalHandlers_t & signalHandlers = mSignalHandlers;
          /* We have a valid signal, read the info from the fd */
          struct signalfd_siginfo info;
          ssize_t bytes = read(pollfd.fd, &info, sizeof(info));
          if(bytes != sizeof(info))
          {
            //error received...
            logError("Failed to read from signal fd");
            throw std::runtime_error(std::string("Failed to read from signal fd."));
          }
          
          /* Notify all listeners */
          for(auto it: signalHandlers)
          it.callback(it.handle, info, it.userData);
      };
      /* We're going to add the signal fd through addFDPoll. At this point we don't have any signal listeners. */
      return addFDPoll(signalHandlerFd, POLLIN | POLLERR | POLLHUP, NULL, actionPoll, [](const sh_pollHandle_t, void*)
                                        {   return (false);}, NULL, NULL, mSignalFdHandle);
    }    
    else 
    {
        int signalHandlerFd = signalfd(sgPollData.pollfdValue.fd, &sigset, 0);
        if (signalHandlerFd == -1)
        {
            logError("Could not update signal fd!", strerror(errno));
            return (E_NOT_POSSIBLE);
        }
        return E_OK;
    }
}

/**
  * Adds a filedescriptor to the polling loop
  * @param fd the filedescriptor
  * @param event the event flags
  * @param prepare a std::function that is called before the loop is entered
  * @param fired a std::function that is called when the filedescriptor needs to be read
  * @param check a std::function that is called to check if further actions are neccessary
  * @param dispatch a std::function that is called to dispatch the received data
  * @param userData a pointer to userdata that is always passed around
  * @param handle the handle of this poll
  * @return E_OK if the descriptor was added, E_NON_EXISTENT if the fd is not valid
  */

am_Error_e CAmSocketHandler::addFDPoll(const int fd, 
                                       const short event, 
                                       std::function<void(const sh_pollHandle_t handle, void* userData)> prepare,
                                       std::function<void(const pollfd pollfd, const sh_pollHandle_t handle, void* userData)> fired, 
                                       std::function<bool(const sh_pollHandle_t handle, void* userData)> check,
                                       std::function<bool(const sh_pollHandle_t handle, void* userData)> dispatch, 
                                       void* userData, 
                                       sh_pollHandle_t& handle)
{
    CHECK_CALLER_THREAD_ID()
    
    if (!fdIsValid(fd))
        return (E_NON_EXISTENT);

    //create a new handle for the poll
    if (!nextHandle(mSetPollKeys))
    {
        logError("Could not create new polls, too many open!");
        return (E_NOT_POSSIBLE);
    }

    sh_poll_s pollData;
    pollData.pollfdValue.fd = fd;
    pollData.handle = mSetPollKeys.lastUsedID;
    pollData.pollfdValue.events = event;
    pollData.pollfdValue.revents = 0;
    pollData.prepareCB = prepare;
    pollData.firedCB = fired;
    pollData.checkCB = check;
    pollData.dispatchCB = dispatch;
    pollData.userData = userData;
    //add new data to the list
    mListPoll.push_back(pollData);

    mRecreatePollfds = true;

    handle = pollData.handle;
    return (E_OK);

}

/**
  * Adds a filedescriptor to the polling loop
  * @param fd the filedescriptor
  * @param event the event flags
  * @param prepare a callback that is called before the loop is entered
  * @param fired a callback that is called when the filedescriptor needs to be read
  * @param check a callback that is called to check if further actions are neccessary
  * @param dispatch a callback that is called to dispatch the received data
  * @param userData a pointer to userdata that is always passed around
  * @param handle the handle of this poll
  * @return E_OK if the descriptor was added, E_NON_EXISTENT if the fd is not valid
  */
am::am_Error_e CAmSocketHandler::addFDPoll(const int fd, const short event, IAmShPollPrepare *prepare, IAmShPollFired *fired, IAmShPollCheck *check, IAmShPollDispatch *dispatch, void *userData, sh_pollHandle_t & handle)
{

    std::function<void(const sh_pollHandle_t handle, void* userData)> prepareCB; //preperation callback
    std::function<void(const pollfd pollfd, const sh_pollHandle_t handle, void* userData)> firedCB; //fired callback
    std::function<bool(const sh_pollHandle_t handle, void* userData)> checkCB; //check callback
    std::function<bool(const sh_pollHandle_t handle, void* userData)> dispatchCB; //check callback

    if (prepare)
        prepareCB = std::bind(&IAmShPollPrepare::Call, prepare, std::placeholders::_1, std::placeholders::_2);
    if (fired)
        firedCB = std::bind(&IAmShPollFired::Call, fired, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    if (check)
        checkCB = std::bind(&IAmShPollCheck::Call, check, std::placeholders::_1, std::placeholders::_2);
    if (dispatch)
        dispatchCB = std::bind(&IAmShPollDispatch::Call, dispatch, std::placeholders::_1, std::placeholders::_2);

    return addFDPoll(fd, event, prepareCB, firedCB, checkCB, dispatchCB, userData, handle);
}

/**
  * removes a filedescriptor from the poll loop
  * @param handle
  * @return
  */
am_Error_e CAmSocketHandler::removeFDPoll(const sh_pollHandle_t handle)
{
    CHECK_CALLER_THREAD_ID()
    
    VectorListPoll_t::iterator iterator = mListPoll.begin();
    
    for (; iterator != mListPoll.end(); ++iterator)
    {
        if (iterator->handle == handle)
        {
            iterator = mListPoll.erase(iterator);
            mSetPollKeys.pollHandles.erase(handle);
            mRecreatePollfds = true;
            break;
        }
    }
    
    if (iterator == mListPoll.end())
        return (E_UNKNOWN);
    
    VectorListPoll_t::iterator iteratorActivePolls = mListActivePolls.begin();
    for (; iteratorActivePolls != mListActivePolls.end(); ++iteratorActivePolls)
    {
        if (iteratorActivePolls->handle == handle)
        {
            iteratorActivePolls->isValid = false;
            break;
        }
    }
    
    return (E_OK);
}

/**
  * Adds a callback for any signals
  * @param callback
  * @param handle the handle of this poll
  * @param userData a pointer to userdata that is always passed around
  * @return E_OK if the descriptor was added, E_NON_EXISTENT if the fd is not valid
  */
am_Error_e CAmSocketHandler::addSignalHandler(std::function<void(const sh_pollHandle_t handle, const signalfd_siginfo & info, void* userData)> callback, sh_pollHandle_t& handle, void * userData)
{
    CHECK_CALLER_THREAD_ID()
    
    if (!nextHandle(mSetSignalhandlerKeys))
    {
        logError("Could not create new polls, too many open!");
        return (E_NOT_POSSIBLE);
    }

    mSignalHandlers.emplace_back();
    mSignalHandlers.back().callback = callback;
    mSignalHandlers.back().handle = mSetSignalhandlerKeys.lastUsedID;
    mSignalHandlers.back().userData = userData;
    handle = mSetSignalhandlerKeys.lastUsedID;

    return E_OK;
}

/**
  * removes a signal handler from the list
  * @param handle is signal handler id
  * @return E_OK in case of success, E_UNKNOWN if the handler was not found.
  */
am_Error_e CAmSocketHandler::removeSignalHandler(const sh_pollHandle_t handle)
{
    CHECK_CALLER_THREAD_ID()
    
    VectorSignalHandlers_t::iterator it(mSignalHandlers.begin());
    for (; it != mSignalHandlers.end(); ++it)
    {
        if (it->handle == handle)
        {
            it = mSignalHandlers.erase(it);
            mSetSignalhandlerKeys.pollHandles.erase(handle);
            return (E_OK);
        }
    }
    return (E_UNKNOWN);
}

/**
  * adds a timer to the list of timers. The callback will be fired when the timer is up.
  * This is not a high precise timer, it is very coarse. It is meant to be used for timeouts when waiting
  * for an answer via a filedescriptor.
  * One time timer. If you need again a timer, you need to add a new timer in the callback of the old one.
  * @param timeouts timeouts time until the callback is fired
  * @param callback callback the callback
  * @param handle handle the handle that is created for the timer is returned. Can be used to remove the timer
  * @param userData pointer always passed with the call
  * @return E_OK in case of success
  */

am_Error_e CAmSocketHandler::addTimer(const timespec & timeouts, IAmShTimerCallBack* callback, sh_timerHandle_t& handle, void * userData, const bool repeats)
{
    assert(callback!=NULL);

    std::function<void(const sh_timerHandle_t handle, void* userData)> callbackFunc;
    callbackFunc = std::bind(&IAmShTimerCallBack::Call, callback, std::placeholders::_1, std::placeholders::_2);

    return addTimer(timeouts, callbackFunc, handle, userData, repeats);
}

am_Error_e CAmSocketHandler::addTimer(const timespec & timeouts, std::function<void(const sh_timerHandle_t handle, void* userData)> callback, sh_timerHandle_t& handle, void * userData, const bool repeats)
{
    CHECK_CALLER_THREAD_ID()
    assert(!((timeouts.tv_sec == 0) && (timeouts.tv_nsec == 0)));

    mListTimer.emplace_back();
    sh_timer_s & timerItem = mListTimer.back();

#ifndef WITH_TIMERFD 
    //create a new handle for the timer
    if (!nextHandle(mSetTimerKeys))
    {
        logError("Could not create new timers, too many open!");
        mListTimer.pop_back();
        return (E_NOT_POSSIBLE);
    }
    //create a new handle for the timer
    handle = mSetTimerKeys.lastUsedID;

    timerItem.countdown = timeouts;
    timerItem.callback = callback;
    timerItem.userData = userData;

    timerItem.handle = handle;

    //we add here the time difference between startTime and currenttime, because this time will be substracted later on in timecorrection
    timespec currentTime;
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    if (!mDispatchDone)//the mainloop is started
    timerItem.countdown = timespecAdd(timeouts, timespecSub(currentTime, mStartTime));
    mListActiveTimer.push_back(timerItem);
    mListActiveTimer.sort(compareCountdown);
    return (E_OK);

#else   
    timerItem.countdown.it_value = timeouts;
    if (repeats)
        timerItem.countdown.it_interval = timeouts;
    else
    {
        timespec zero;
        zero.tv_sec = 0;
        zero.tv_nsec = 0;
        timerItem.countdown.it_interval = zero;
    }

    timerItem.fd = -1;
    timerItem.userData = userData;
    am_Error_e err = createTimeFD(timerItem.countdown, timerItem.fd);
    if (err != E_OK)
    {
        mListTimer.pop_back();
        return err;
    }

    auto actionPoll = [this](const pollfd pollfd, const sh_pollHandle_t handle, void* userData)
    {
        uint64_t mExpirations;
        if(read(pollfd.fd, &mExpirations, sizeof(uint64_t))!=sizeof(uint64_t))
        {
            //error received...
            logError("Failed to read from timer fd");
            throw std::runtime_error(std::string("Failed to read from timer fd."));
        }
    };

    err = addFDPoll(timerItem.fd, 
                    POLLIN, 
                    NULL, 
                    actionPoll, 
                    [callback](const sh_pollHandle_t handle, void* userData)->bool{
                        callback(handle, userData);
                        return false;
                    },
                    NULL, 
                    userData, 
                    handle);
    if (E_OK == err)
    {
        timerItem.handle = handle;
    }
    else
    {
        mListTimer.pop_back();
    }
    return err;
#endif    

}

/**
  * removes a timer from the list of timers
  * @param handle the handle to the timer
  * @return E_OK in case of success, E_UNKNOWN if timer was not found.
  */
am_Error_e CAmSocketHandler::removeTimer(const sh_timerHandle_t handle)
{
    CHECK_CALLER_THREAD_ID()
    assert(handle != 0);

    //stop the current timer
#ifdef WITH_TIMERFD 
    std::list<sh_timer_s>::iterator it = mListTimer.begin();
    for (; it != mListTimer.end(); ++it)
    {
        if (it->handle == handle)
            break;
    }
    if (it == mListTimer.end())
        return (E_NON_EXISTENT);

    mListRemovedTimers.push_back(*it);
    mListTimer.erase(it);
    return removeFDPoll(handle);
#else
    stopTimer(handle);
    std::list<sh_timer_s>::iterator it(mListTimer.begin());
    while (it != mListTimer.end())
    {
        if (it->handle == handle)
        {
            it = mListTimer.erase(it);            
            mSetTimerKeys.pollHandles.erase(handle);
            return (E_OK);
        }
        else
            ++it;
    }
    return (E_UNKNOWN);
#endif
}

/**
  * restarts a timer and updates with a new interva
  * @param handle handle to the timer
  * @param timeouts new timout time
  * @return E_OK on success, E_NON_EXISTENT if the handle was not found
  */
am_Error_e CAmSocketHandler::updateTimer(const sh_timerHandle_t handle, const timespec & timeouts)
{
    CHECK_CALLER_THREAD_ID()
    
#ifdef WITH_TIMERFD
    std::list<sh_timer_s>::iterator it = mListTimer.begin();
    for (; it != mListTimer.end(); ++it)
    {
        if (it->handle == handle)
            break;
    }
    if (it == mListTimer.end())
        return (E_NON_EXISTENT);

    if (it->countdown.it_interval.tv_nsec != 0 || it->countdown.it_interval.tv_sec != 0)
        it->countdown.it_interval = timeouts;
    it->countdown.it_value = timeouts;

    if (!fdIsValid(it->fd))
    {
        am_Error_e err = createTimeFD(it->countdown, it->fd);
        if (err != E_OK)
            return err;
    }
    else
    {
        if (timerfd_settime(it->fd, 0, &it->countdown, NULL)<0)
        {
            logError("Failed to set timer duration");
            return E_NOT_POSSIBLE;
        }
    }
#else

    //update the mList ....
    sh_timer_s timerItem;
    std::list<sh_timer_s>::iterator it(mListTimer.begin()), activeIt(mListActiveTimer.begin());
    bool found(false);
    for (; it != mListTimer.end(); ++it)
    {
        if (it->handle == handle)
        {
            it->countdown = timeouts;
            timerItem = *it;
            found = true;
            break;
        }
    }
    if (!found)
    return (E_NON_EXISTENT);

    found = false;

    //we add here the time difference between startTime and currenttime, because this time will be substracted later on in timecorrection
    timespec currentTime, timeoutsCorrected;
    currentTime.tv_nsec=timeoutsCorrected.tv_nsec=0;
    currentTime.tv_sec=timeoutsCorrected.tv_sec=0;
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    if (!mDispatchDone)//the mainloop is started
    timeoutsCorrected = timespecAdd(timeouts, timespecSub(currentTime, mStartTime));

    for (; activeIt != mListActiveTimer.end(); ++activeIt)
    {
        if (activeIt->handle == handle)
        {
            activeIt->countdown = timeoutsCorrected;
            found = true;
            break;
        }
    }

    if (!found)
    timerItem.countdown = timeoutsCorrected;
    mListActiveTimer.push_back(timerItem);

    mListActiveTimer.sort(compareCountdown);

#endif
    return (E_OK);
}

/**
  * restarts a timer with the original value
  * @param handle
  * @return E_OK on success, E_NON_EXISTENT if the handle was not found
  */
am_Error_e CAmSocketHandler::restartTimer(const sh_timerHandle_t handle)
{
    CHECK_CALLER_THREAD_ID()
#ifdef WITH_TIMERFD
    std::list<sh_timer_s>::iterator it = mListTimer.begin();
    for (; it != mListTimer.end(); ++it)
    {
        if (it->handle == handle)
            break;
    }
    if (it == mListTimer.end())
        return (E_NON_EXISTENT);

    if (!fdIsValid(it->fd))
    {
        am_Error_e err = createTimeFD(it->countdown, it->fd);
        if (err != E_OK)
            return err;
    }
    else
    {
        if (timerfd_settime(it->fd, 0, &it->countdown, NULL)<0)
        {
            logError("Failed to set timer duration");
            return E_NOT_POSSIBLE;
        }
    }
#else

    sh_timer_s timerItem; //!<the original timer value
    //find the original value
    std::list<sh_timer_s>::iterator it(mListTimer.begin()), activeIt(mListActiveTimer.begin());
    bool found(false);
    for (; it != mListTimer.end(); ++it)
    {
        if (it->handle == handle)
        {
            timerItem = *it;
            found = true;
            break;
        }
    }
    if (!found)
    return (E_NON_EXISTENT);

    found = false;

    //we add here the time difference between startTime and currenttime, because this time will be substracted later on in timecorrection
    timespec currentTime, timeoutsCorrected;
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    if (!mDispatchDone)//the mainloop is started
    {
        timeoutsCorrected = timespecAdd(timerItem.countdown, timespecSub(currentTime, mStartTime));
        timerItem.countdown = timeoutsCorrected;
    }

    for (; activeIt != mListActiveTimer.end(); ++activeIt)
    {
        if (activeIt->handle == handle)
        {
            activeIt->countdown = timerItem.countdown;
            found = true;
            break;
        }
    }

    if (!found)
    mListActiveTimer.push_back(timerItem);

    mListActiveTimer.sort(compareCountdown);
#endif
    return (E_OK);
}

/**
  * stops a timer
  * @param handle
  * @return E_OK on success, E_NON_EXISTENT if the handle was not found
  */
am_Error_e CAmSocketHandler::stopTimer(const sh_timerHandle_t handle)
{
    CHECK_CALLER_THREAD_ID()
#ifdef WITH_TIMERFD
    std::list<sh_timer_s>::iterator it = mListTimer.begin();
    for (; it != mListTimer.end(); ++it)
    {
        if (it->handle == handle)
            break;
    }
    if (it == mListTimer.end())
        return (E_NON_EXISTENT);

    itimerspec countdown = it->countdown;
    countdown.it_value.tv_nsec = 0;
    countdown.it_value.tv_sec = 0;

    if (timerfd_settime(it->fd, 0, &countdown, NULL)<0)
    {
        logError("Failed to set timer duration");
        return E_NOT_POSSIBLE;
    }
    return (E_OK);
#else   
    //go through the list and remove the timer with the handle
    std::list<sh_timer_s>::iterator it(mListActiveTimer.begin());
    
    while (it != mListActiveTimer.end())
    {
        if (it->handle == handle)
        {
            it = mListActiveTimer.erase(it);
            return (E_OK);
        }
        else
            it++;
    }
    return (E_NON_EXISTENT);
#endif
}

/**
  * updates the eventFlags of a poll
  * @param handle
  * @param events
  * @return @return E_OK on succsess, E_NON_EXISTENT if fd was not found
  */
am_Error_e CAmSocketHandler::updateEventFlags(const sh_pollHandle_t handle, const short events)
{
    CHECK_CALLER_THREAD_ID()
    VectorListPoll_t::iterator iterator = mListPoll.begin();

    for (; iterator != mListPoll.end(); ++iterator)
    {
        if (iterator->handle == handle)
        {
            iterator->pollfdValue.events = events;
            mRecreatePollfds = true;
            return (E_OK);
        }
    }
    return (E_UNKNOWN);
}

/**
  * checks if a filedescriptor is validCAmShSubstractTime
  * @param fd the filedescriptor
  * @return true if the fd is valid
  */
bool CAmSocketHandler::fdIsValid(const int fd) const
{
    return (fcntl(fd, F_GETFL) != -1 || errno != EBADF);
}

#ifndef WITH_TIMERFD
/**
  * timer is up.
  */
void CAmSocketHandler::timerUp()
{
    //find out the timedifference to starttime
    static timespec currentTime, diffTime;
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    diffTime = timespecSub(currentTime, mStartTime);

    static auto countdownUp = [&](const sh_timer_s& row)->bool
    {
        timespec sub = timespecSub(row.countdown, diffTime);
        if (sub.tv_nsec == 0 && sub.tv_sec == 0)
        return (true);
        return (false);
    };

    //now we need to substract the diffTime from all timers and see if they are up
    std::list<sh_timer_s>::reverse_iterator overflowIter = std::find_if(mListActiveTimer.rbegin(), mListActiveTimer.rend(), countdownUp);

    //copy all fired timers into a list
    std::vector<sh_timer_s> tempList(overflowIter, mListActiveTimer.rend());

    //erase all fired timers
    std::list<sh_timer_s>::iterator it(overflowIter.base());
    mListActiveTimer.erase(mListActiveTimer.begin(), it);

    //call the callbacks for the timers
    std::for_each(tempList.begin(), tempList.end(), CAmSocketHandler::callTimer);
}

/**
  * correct timers and fire the ones who are up
  */
void CAmSocketHandler::timerCorrection()
{
    //get the current time and calculate the correction value
    static timespec currentTime, correctionTime;
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    correctionTime = timespecSub(currentTime, mStartTime);
    mStartTime = currentTime;

    static auto countdownZero = [](const sh_timer_s& row)->bool
    {
        if (row.countdown.tv_nsec == 0 && row.countdown.tv_sec == 0)
        return (true);
        return (false);
    };

    static auto substractTime = [&](sh_timer_s& t)
    {
        t.countdown = timespecSub(t.countdown, correctionTime);
    };

    if (!mListActiveTimer.empty())
    {

        //subtract the correction value from all items in the list
        std::for_each(mListActiveTimer.begin(), mListActiveTimer.end(), substractTime);

        //find the last occurrence of zero -> timer overflowed
        std::list<sh_timer_s>::reverse_iterator overflowIter = std::find_if(mListActiveTimer.rbegin(), mListActiveTimer.rend(), countdownZero);

        //only if a timer overflowed
        if (overflowIter != mListActiveTimer.rend())
        {
            //copy all timers that need to be called to a new list
            std::vector<sh_timer_s> tempList(overflowIter, mListActiveTimer.rend());

            //erase all fired timers
            std::list<sh_timer_s>::iterator it(overflowIter.base());
            mListActiveTimer.erase(mListActiveTimer.begin(), it);

            //call the callbacks for the timers
            std::for_each(tempList.begin(), tempList.end(), CAmSocketHandler::callTimer);
        }
    }
}
#endif

/**
  * prepare for poll
  */
void CAmSocketHandler::prepare(am::CAmSocketHandler::sh_poll_s& row)
{
    if (row.prepareCB)
    {
        try
        {
            row.prepareCB(row.handle, row.userData);
        } catch (std::exception& e)
        {
            logError("Sockethandler: Exception in Preparecallback,caught", e.what());
        }
    }
}

/**
  * fire callback
  */
void CAmSocketHandler::fire(const sh_poll_s* a)
{
    try
    {
        a->firedCB(a->pollfdValue, a->handle, a->userData);
    } catch (std::exception& e)
    {
        logError("Sockethandler: Exception in Preparecallback,caught", e.what());
    }
}

/**
  * should disptach
  */
bool CAmSocketHandler::noDispatching(const sh_poll_s* a)
{
    //remove from list of there is no checkCB
    if (nullptr == a->checkCB || false==a->isValid )
        return (true);
    return (!a->checkCB(a->handle, a->userData));
}

/**
  * disptach
  */
bool CAmSocketHandler::dispatchingFinished(const sh_poll_s* a)
{
    //remove from list of there is no dispatchCB
    if (nullptr == a->dispatchCB || false==a->isValid )
        return (true);
    return (!a->dispatchCB(a->handle, a->userData));
}

/**
  * is used to set the pointer for the ppoll command
  * @param buffertime
  * @return
  */
inline timespec* CAmSocketHandler::insertTime(timespec& buffertime)
{
#ifndef WITH_TIMERFD
    if (!mListActiveTimer.empty())
    {
        buffertime = mListActiveTimer.front().countdown;
        return (&buffertime);
    }
    else   
#endif         
    {
        return (NULL);
    }
}

#ifdef WITH_TIMERFD   
am_Error_e CAmSocketHandler::createTimeFD(const itimerspec & timeouts, int & fd)
{    
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
    {
        logError("Failed to create timer");
        return E_NOT_POSSIBLE;
    }

    if (timerfd_settime(fd, 0, &timeouts, NULL) < 0)
    {
        logError("Failed to set timer duration");
        return E_NOT_POSSIBLE;
    }
    return E_OK;
}

void CAmSocketHandler::closeRemovedTimers()
{
    std::list<sh_timer_s>::iterator it(mListRemovedTimers.begin());
    while (it != mListRemovedTimers.end())
    {
        if( it->fd > -1 )
            close( it->fd );
        ++it;
    }
    mListRemovedTimers.clear();
}

#endif 

void CAmSocketHandler::callTimer(sh_timer_s& a)
{
    try
    {
        a.callback(a.handle, a.userData);
    } catch (std::exception& e)
    {
        logError("Sockethandler: Exception in Timercallback,caught", e.what());
    }
}

bool CAmSocketHandler::nextHandle(sh_identifier_s & handle)
{
    //create a new handle for the poll
    const sh_pollHandle_t lastHandle(handle.lastUsedID);
    do
    {
        ++handle.lastUsedID;
        if (handle.lastUsedID == handle.limit)
        {
            handle.lastUsedID = 1;
        }
        if (handle.lastUsedID == lastHandle)
        {
            logError("Could not create new polls, too many open!");
            return (false);
        }

    } while (handle.pollHandles.find(handle.lastUsedID) != handle.pollHandles.end());

    handle.pollHandles.insert(handle.lastUsedID);

    return (true);
}

}

