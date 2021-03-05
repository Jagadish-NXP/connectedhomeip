/*
 *    Copyright (c) 2020-2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file implements the CHIP reliable message protocol.
 *
 */

#include <inttypes.h>

#include <messaging/ReliableMessageMgr.h>

#include <messaging/ErrorCategory.h>
#include <messaging/Flags.h>
#include <messaging/ReliableMessageContext.h>
#include <support/BitFlags.h>
#include <support/CHIPFaultInjection.h>
#include <support/CodeUtils.h>
#include <support/logging/CHIPLogging.h>

namespace chip {
namespace Messaging {

ReliableMessageMgr::RetransTableEntry::RetransTableEntry() : rc(nullptr), nextRetransTimeTick(0), sendCount(0) {}

ReliableMessageMgr::ReliableMessageMgr(std::array<ExchangeContext, CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS> & contextPool) :
    mContextPool(contextPool), mSystemLayer(nullptr), mSessionMgr(nullptr), mCurrentTimerExpiry(0),
    mTimerIntervalShift(CHIP_CONFIG_RMP_TIMER_DEFAULT_PERIOD_SHIFT)
{}

ReliableMessageMgr::~ReliableMessageMgr() {}

void ReliableMessageMgr::Init(chip::System::Layer * systemLayer, SecureSessionMgr * sessionMgr)
{
    mSystemLayer = systemLayer;
    mSessionMgr  = sessionMgr;

    mTimeStampBase      = System::Timer::GetCurrentEpoch();
    mCurrentTimerExpiry = 0;
}

void ReliableMessageMgr::Shutdown()
{
    StopTimer();

    mSystemLayer = nullptr;
    mSessionMgr  = nullptr;

    // Clear the retransmit table
    for (RetransTableEntry & rEntry : mRetransTable)
    {
        ClearRetransTable(rEntry);
    }
}

uint64_t ReliableMessageMgr::GetTickCounterFromTimePeriod(uint64_t period)
{
    return (period >> mTimerIntervalShift);
}

uint64_t ReliableMessageMgr::GetTickCounterFromTimeDelta(uint64_t newTime)
{
    return GetTickCounterFromTimePeriod(newTime - mTimeStampBase);
}

#if defined(RMP_TICKLESS_DEBUG)
void ReliableMessageMgr::TicklessDebugDumpRetransTable(const char * log)
{
    ChipLogProgress(ExchangeManager, log);

    for (RetransTableEntry & entry : mRetransTable)
    {
        if (entry.rc)
        {
            ChipLogProgress(ExchangeManager, "EC:%04" PRIX16 " MsgId:%08" PRIX32 " NextRetransTimeCtr:%04" PRIX16, entry.rc,
                            entry.msgId, entry.nextRetransTimeTick);
        }
    }
}
#else
void ReliableMessageMgr::TicklessDebugDumpRetransTable(const char * log)
{
    return;
}
#endif // RMP_TICKLESS_DEBUG

void ReliableMessageMgr::ExecuteActions()
{
#if defined(RMP_TICKLESS_DEBUG)
    ChipLogProgress(ExchangeManager, "ReliableMessageMgr::ExecuteActions");
#endif

    ExecuteForAllContext([](ReliableMessageContext * rc) {
        if (rc->IsAckPending())
        {
            if (0 == rc->mNextAckTimeTick)
            {
#if defined(RMP_TICKLESS_DEBUG)
                ChipLogProgress(ExchangeManager, "ReliableMessageMgr::ExecuteActions sending ACK");
#endif
                // Send the Ack in a SecureChannel::StandaloneAck message
                rc->SendStandaloneAckMessage();
                rc->SetAckPending(false);
            }
        }
    });

    TicklessDebugDumpRetransTable("ReliableMessageMgr::ExecuteActions Dumping mRetransTable entries before processing");

    // Retransmit / cancel anything in the retrans table whose retrans timeout
    // has expired
    for (RetransTableEntry & entry : mRetransTable)
    {
        ReliableMessageContext * rc = entry.rc;
        CHIP_ERROR err              = CHIP_NO_ERROR;

        if (!rc || entry.nextRetransTimeTick != 0)
            continue;

        uint8_t sendCount = entry.sendCount;

        if (sendCount == rc->mConfig.mMaxRetrans)
        {
            err = CHIP_ERROR_MESSAGE_NOT_ACKNOWLEDGED;

            ChipLogError(ExchangeManager, "Failed to Send CHIP MsgId:%08" PRIX32 " sendCount: %" PRIu8 " max retries: %" PRIu8,
                         entry.retainedBuf.GetMsgId(), sendCount, rc->mConfig.mMaxRetrans);

            // Remove from Table
            ClearRetransTable(entry);
        }

        // Resend from Table (if the operation fails, the entry is cleared)
        if (err == CHIP_NO_ERROR)
            err = SendFromRetransTable(&entry);

        if (err == CHIP_NO_ERROR)
        {
            // If the retransmission was successful, update the passive timer
            entry.nextRetransTimeTick = static_cast<uint16_t>(rc->GetCurrentRetransmitTimeoutTick());
#if !defined(NDEBUG)
            ChipLogProgress(ExchangeManager, "Retransmit MsgId:%08" PRIX32 " Send Cnt %d", entry.retainedBuf.GetMsgId(),
                            entry.sendCount);
#endif
        }

        if (err != CHIP_NO_ERROR && rc->mDelegate)
            rc->mDelegate->OnSendError(err);
    }

    TicklessDebugDumpRetransTable("ReliableMessageMgr::ExecuteActions Dumping mRetransTable entries after processing");
}

static void TickProceed(uint16_t & time, uint64_t ticks)
{
    if (time >= ticks)
    {
        time = static_cast<uint16_t>(time - ticks);
    }
    else
    {
        time = 0;
    }
}

void ReliableMessageMgr::ExpireTicks()
{
    uint64_t now = System::Timer::GetCurrentEpoch();

    // Number of full ticks elapsed since last timer processing.  We always round down
    // to the previous tick.  If we are between tick boundaries, the extra time since the
    // last virtual tick is not accounted for here (it will be accounted for when resetting
    // the ReliableMessageProtocol timer)
    uint64_t deltaTicks = GetTickCounterFromTimeDelta(now);

#if defined(RMP_TICKLESS_DEBUG)
    ChipLogProgress(ExchangeManager, "ReliableMessageMgr::ExpireTicks at %" PRIu64 ", %" PRIu64 ", %u", now, mTimeStampBase,
                    deltaTicks);
#endif

    ExecuteForAllContext([deltaTicks](ReliableMessageContext * rc) {
        if (rc->IsAckPending())
        {
            // Decrement counter of Ack timestamp by the elapsed timer ticks
            TickProceed(rc->mNextAckTimeTick, deltaTicks);
#if defined(RMP_TICKLESS_DEBUG)
            ChipLogProgress(ExchangeManager, "ReliableMessageMgr::ExpireTicks set mNextAckTimeTick to %u", rc->mNextAckTimeTick);
#endif
        }
    });

    for (RetransTableEntry & entry : mRetransTable)
    {
        ReliableMessageContext * rc = entry.rc;
        if (rc)
        {
            // Decrement Retransmit timeout by elapsed timeticks
            TickProceed(entry.nextRetransTimeTick, deltaTicks);
#if defined(RMP_TICKLESS_DEBUG)
            ChipLogProgress(ExchangeManager, "ReliableMessageMgr::ExpireTicks set nextRetransTimeTick to %u",
                            entry.nextRetransTimeTick);
#endif
        } // rc entry is allocated
    }

    // Re-Adjust the base time stamp to the most recent tick boundary
    mTimeStampBase += (deltaTicks << mTimerIntervalShift);

#if defined(RMP_TICKLESS_DEBUG)
    ChipLogProgress(ExchangeManager, "ReliableMessageMgr::ExpireTicks mTimeStampBase to %" PRIu64, mTimeStampBase);
#endif
}

void ReliableMessageMgr::Timeout(System::Layer * aSystemLayer, void * aAppState, System::Error aError)
{
    ReliableMessageMgr * manager = reinterpret_cast<ReliableMessageMgr *>(aAppState);

    VerifyOrDie((aSystemLayer != nullptr) && (manager != nullptr));

#if defined(RMP_TICKLESS_DEBUG)
    ChipLogProgress(ExchangeManager, "ReliableMessageMgr::Timeout\n");
#endif

    // Make sure all tick counts are sync'd to the current time
    manager->ExpireTicks();

    // Execute any actions that are due this tick
    manager->ExecuteActions();

    // Calculate next physical wakeup
    manager->StartTimer();
}

CHIP_ERROR ReliableMessageMgr::AddToRetransTable(ReliableMessageContext * rc, RetransTableEntry ** rEntry)
{
    bool added     = false;
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrDie(rc != nullptr && rc->mExchange != nullptr);

    for (RetransTableEntry & entry : mRetransTable)
    {
        // Check the exchContext pointer for finding an empty slot in Table
        if (!entry.rc)
        {
            // Expire any virtual ticks that have expired so all wakeup sources reflect the current time
            ExpireTicks();

            entry.rc          = rc;
            entry.sendCount   = 0;
            entry.retainedBuf = EncryptedPacketBufferHandle();

            *rEntry = &entry;

            // Increment the reference count
            rc->Retain();
            added = true;

            break;
        }
    }

    if (!added)
    {
        ChipLogError(ExchangeManager, "mRetransTable Already Full");
        err = CHIP_ERROR_RETRANS_TABLE_FULL;
    }

    return err;
}

void ReliableMessageMgr::StartRetransmision(RetransTableEntry * entry)
{
    VerifyOrDie(entry != nullptr && entry->rc != nullptr);

    entry->nextRetransTimeTick = static_cast<uint16_t>(entry->rc->GetCurrentRetransmitTimeoutTick() +
                                                       GetTickCounterFromTimeDelta(System::Timer::GetCurrentEpoch()));

    // Check if the timer needs to be started and start it.
    StartTimer();
}

void ReliableMessageMgr::PauseRetransmision(ReliableMessageContext * rc, uint32_t PauseTimeMillis)
{
    for (RetransTableEntry & entry : mRetransTable)
    {
        if (entry.rc == rc)
        {
            entry.nextRetransTimeTick = static_cast<uint16_t>(entry.nextRetransTimeTick + (PauseTimeMillis >> mTimerIntervalShift));
            break;
        }
    }
}

void ReliableMessageMgr::ResumeRetransmision(ReliableMessageContext * rc)
{
    for (RetransTableEntry & entry : mRetransTable)
    {
        if (entry.rc == rc)
        {
            entry.nextRetransTimeTick = 0;
            break;
        }
    }
}

bool ReliableMessageMgr::CheckAndRemRetransTable(ReliableMessageContext * rc, uint32_t ackMsgId)
{
    for (RetransTableEntry & entry : mRetransTable)
    {
        if ((entry.rc == rc) && entry.retainedBuf.GetMsgId() == ackMsgId)
        {
            // Clear the entry from the retransmision table.
            ClearRetransTable(entry);

#if !defined(NDEBUG)
            ChipLogProgress(ExchangeManager, "Rxd Ack; Removing MsgId:%08" PRIX32 " from Retrans Table", ackMsgId);
#endif
            return true;
        }
    }

    return false;
}

CHIP_ERROR ReliableMessageMgr::SendFromRetransTable(RetransTableEntry * entry)
{
    CHIP_ERROR err              = CHIP_NO_ERROR;
    ReliableMessageContext * rc = entry->rc;

    if (rc)
    {
        err = mSessionMgr->SendEncryptedMessage(entry->rc->mExchange->GetSecureSession(), std::move(entry->retainedBuf),
                                                &entry->retainedBuf);

        if (err == CHIP_NO_ERROR)
        {
            // Update the counters
            entry->sendCount++;
        }
        else
        {
            // Remove from table
            ChipLogError(ExchangeManager, "Crit-err %ld when sending CHIP MsgId:%08" PRIX32 ", send tries: %d", long(err),
                         entry->retainedBuf.GetMsgId(), entry->sendCount);

            ClearRetransTable(*entry);
        }
    }
    else
    {
        ChipLogError(ExchangeManager, "Table entry invalid");
    }

    return err;
}

void ReliableMessageMgr::ClearRetransTable(ReliableMessageContext * rc)
{
    for (RetransTableEntry & entry : mRetransTable)
    {
        if (entry.rc == rc)
        {
            // Clear the retransmit table entry.
            ClearRetransTable(entry);
        }
    }
}

void ReliableMessageMgr::ClearRetransTable(RetransTableEntry & rEntry)
{
    if (rEntry.rc)
    {
        VerifyOrDie(rEntry.rc->mExchange != nullptr);

        // Expire any virtual ticks that have expired so all wakeup sources reflect the current time
        ExpireTicks();

        rEntry.rc->Release();
        rEntry.rc = nullptr;

        // Clear all other fields
        rEntry = RetransTableEntry();

        // Schedule next physical wakeup, unless shutting down
        if (mSystemLayer)
            StartTimer();
    }
}

void ReliableMessageMgr::FailRetransTableEntries(ReliableMessageContext * rc, CHIP_ERROR err)
{
    for (RetransTableEntry & entry : mRetransTable)
    {
        if (entry.rc == rc)
        {
            // Remove the entry from the retransmission table.
            ClearRetransTable(entry);

            // Application callback OnSendError.
            rc->mDelegate->OnSendError(err);
        }
    }
}

void ReliableMessageMgr::StartTimer()
{
    CHIP_ERROR res            = CHIP_NO_ERROR;
    uint64_t nextWakeTimeTick = UINT64_MAX;
    bool foundWake            = false;

    // When do we need to next wake up to send an ACK?

    ExecuteForAllContext([&nextWakeTimeTick, &foundWake](ReliableMessageContext * rc) {
        if (rc->IsAckPending() && rc->mNextAckTimeTick < nextWakeTimeTick)
        {
            nextWakeTimeTick = rc->mNextAckTimeTick;
            foundWake        = true;
#if defined(RMP_TICKLESS_DEBUG)
            ChipLogProgress(ExchangeManager, "ReliableMessageMgr::StartTimer next ACK time %u", nextWakeTimeTick);
#endif
        }
    });

    for (RetransTableEntry & entry : mRetransTable)
    {
        ReliableMessageContext * rc = entry.rc;
        if (rc)
        {
            // When do we need to next wake up for ReliableMessageProtocol retransmit?
            if (entry.nextRetransTimeTick < nextWakeTimeTick)
            {
                nextWakeTimeTick = entry.nextRetransTimeTick;
                foundWake        = true;
#if defined(RMP_TICKLESS_DEBUG)
                ChipLogProgress(ExchangeManager, "ReliableMessageMgr::StartTimer RetransTime %u", nextWakeTimeTick);
#endif
            }
        }
    }

    if (foundWake)
    {
        // Set timer for next tick boundary - subtract the elapsed time from the current tick
        System::Timer::Epoch timerExpiryEpoch = (nextWakeTimeTick << mTimerIntervalShift) + mTimeStampBase;

#if defined(RMP_TICKLESS_DEBUG)
        ChipLogProgress(ExchangeManager, "ReliableMessageMgr::StartTimer wake at %" PRIu64 " ms (%" PRIu64 " %" PRIu64 ")",
                        timerExpiryEpoch, nextWakeTimeTick, mTimeStampBase);
#endif
        if (timerExpiryEpoch != mCurrentTimerExpiry)
        {
            // If the tick boundary has expired in the past (delayed processing of event due to other system activity),
            // expire the timer immediately
            uint64_t now           = System::Timer::GetCurrentEpoch();
            uint64_t timerArmValue = (timerExpiryEpoch > now) ? timerExpiryEpoch - now : 0;

#if defined(RMP_TICKLESS_DEBUG)
            ChipLogProgress(ExchangeManager, "ReliableMessageMgr::StartTimer set timer for %" PRIu64, timerArmValue);
#endif
            StopTimer();
            res = mSystemLayer->StartTimer((uint32_t) timerArmValue, Timeout, this);

            VerifyOrDieWithMsg(res == CHIP_NO_ERROR, ExchangeManager, "Cannot start ReliableMessageMgr::Timeout\n");
            mCurrentTimerExpiry = timerExpiryEpoch;
#if defined(RMP_TICKLESS_DEBUG)
        }
        else
        {
            ChipLogProgress(ExchangeManager, "ReliableMessageMgr::StartTimer timer already set for %" PRIu64, timerExpiryEpoch);
#endif
        }
    }
    else
    {
#if defined(RMP_TICKLESS_DEBUG)
        ChipLogProgress(ExchangeManager, "Not setting ReliableMessageProtocol timeout at %" PRIu64,
                        System::Timer::GetCurrentEpoch());
#endif
        StopTimer();
    }

    TicklessDebugDumpRetransTable("ReliableMessageMgr::StartTimer Dumping mRetransTable entries after setting wakeup times");
}

void ReliableMessageMgr::StopTimer()
{
    mSystemLayer->CancelTimer(Timeout, this);
}

int ReliableMessageMgr::TestGetCountRetransTable()
{
    int count = 0;

    for (RetransTableEntry & entry : mRetransTable)
    {
        ReliableMessageContext * rc = entry.rc;
        if (rc)
            count++;
    }

    return count;
}

} // namespace Messaging
} // namespace chip