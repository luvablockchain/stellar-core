// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/PublishStateMachine.h"
#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "history/FileTransferInfo.h"
#include "main/Application.h"
#include "main/Config.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/XDRStream.h"

#include "medida/metrics_registry.h"
#include "medida/counter.h"

#include <soci.h>

namespace stellar
{

const size_t ArchivePublisher::kRetryLimit = 16;

typedef FileTransferInfo<FilePublishState> FilePublishInfo;

struct StateSnapshot
{
    Application& mApp;
    HistoryArchiveState mLocalState;
    std::vector<std::shared_ptr<Bucket>> mLocalBuckets;
    TmpDir mSnapDir;
    std::shared_ptr<FilePublishInfo> mLedgerSnapFile;
    std::shared_ptr<FilePublishInfo> mTransactionSnapFile;
    std::shared_ptr<FilePublishInfo> mTransactionResultSnapFile;

    StateSnapshot(Application& app);
    bool writeHistoryBlocks() const;
};

ArchivePublisher::ArchivePublisher(
    Application& app, std::function<void(asio::error_code const&)> handler,
    std::shared_ptr<HistoryArchive> archive,
    std::shared_ptr<StateSnapshot> snap)
    : mApp(app)
    , mEndHandler(handler)
    , mState(PUBLISH_RETRYING)
    , mRetryCount(0)
    , mRetryTimer(app)
    , mArchive(archive)
    , mSnap(snap)
{
    enterBeginState();
}

void
ArchivePublisher::enterRetryingState()
{
    assert(mState == PUBLISH_BEGIN || mState == PUBLISH_SENDING ||
           mState == PUBLISH_COMMITTING);
    mState = PUBLISH_RETRYING;
    mRetryTimer.expires_from_now(std::chrono::seconds(2));
    mRetryTimer.async_wait(
        [this](asio::error_code const& ec)
        {
            if (this->mRetryCount++ > kRetryLimit)
            {
                CLOG(WARNING, "History")
                    << "Retry count " << kRetryLimit
                    << " exceeded, abandonning  publish-attempt on archive '"
                    << this->mArchive->getName() << "'";
                this->enterEndState();
            }
            else
            {
                CLOG(INFO, "History") << "Retrying publish to archive '"
                                      << this->mArchive->getName() << "'";
                this->enterBeginState();
            }
        });
}

void
ArchivePublisher::enterBeginState()
{
    assert(mState == PUBLISH_RETRYING);
    mState = PUBLISH_BEGIN;
    mArchive->getMostRecentState(
        mApp, [this](asio::error_code const& ec, HistoryArchiveState const& has)
        {
            if (ec)
            {
                CLOG(WARNING, "History") << "Publisher failed to retrieve "
                                            "state from history archive '"
                                         << this->mArchive->getName()
                                         << "', restarting publish";
                this->enterRetryingState();
            }
            else
            {
                this->enterObservedState(has);
            }
        });
}

void
ArchivePublisher::enterObservedState(HistoryArchiveState const& has)
{
    assert(mState == PUBLISH_BEGIN);
    mState = PUBLISH_OBSERVED;

    mArchiveState = has;
    std::vector<std::string> bucketsToSend =
        mSnap->mLocalState.differingBuckets(mArchiveState);
    std::map<std::string, std::shared_ptr<Bucket>> bucketsByHash;
    for (auto b : mSnap->mLocalBuckets)
    {
        bucketsByHash[binToHex(b->getHash())] = b;
    }

    std::vector<std::shared_ptr<FilePublishInfo>> filePublishInfos = {
        mSnap->mLedgerSnapFile, mSnap->mTransactionSnapFile,
        mSnap->mTransactionResultSnapFile};

    for (auto const& hash : bucketsToSend)
    {
        auto b = bucketsByHash[hash];
        assert(b);
        filePublishInfos.push_back(
            std::make_shared<FilePublishInfo>(FILE_PUBLISH_NEEDED, *b));
    }

    for (auto pi : filePublishInfos)
    {
        auto name = pi->baseName_nogz();
        auto i = mFileInfos.find(name);
        if (i == mFileInfos.end() ||
            i->second->getState() == FILE_PUBLISH_FAILED)
        {
            CLOG(DEBUG, "History") << "Queueing file " << name
                                   << " to send to archive '"
                                   << mArchive->getName() << "'";
            mFileInfos[name] = pi;
        }
        else
        {
            CLOG(DEBUG, "History")
                << "Not queueing file " << name << " to send to archive '"
                << mArchive->getName() << "'; file already queued";
        }
    }
    enterSendingState();
}

/**
 * If `ec` is an error, set the state for `name` to FILE_FAILED, otherwise
 * set it to `newGoodState`. In either case, re-enter FETCHING state.
 */
void
ArchivePublisher::fileStateChange(asio::error_code const& ec,
                                  std::string const& name,
                                  FilePublishState newGoodState)
{
    FilePublishState newState = newGoodState;
    if (ec)
    {
        CLOG(WARNING, "History") << "Publish action failed on " << name;
        newState = FILE_PUBLISH_FAILED;
        mError = ec;
    }
    mFileInfos[name]->setState(newState);
    if (mState != PUBLISH_RETRYING)
    {
        enterSendingState();
    }
}

void
ArchivePublisher::enterSendingState()
{
    assert(mState == PUBLISH_OBSERVED || mState == PUBLISH_SENDING);
    mState = PUBLISH_SENDING;

    FilePublishState minimumState = FILE_PUBLISH_UPLOADED;
    auto& hm = mApp.getHistoryManager();
    for (auto& pair : mFileInfos)
    {
        auto fi = pair.second;
        std::string name = fi->baseName_nogz();

        switch (fi->getState())
        {
        case FILE_PUBLISH_FAILED:
            break;

        case FILE_PUBLISH_NEEDED:
            fi->setState(FILE_PUBLISH_COMPRESSING);
            CLOG(DEBUG, "History") << "Compressing " << name;
            hm.compress(fi->localPath_nogz(),
                        [this, name](asio::error_code const& ec)
                        {
                            this->fileStateChange(ec, name,
                                                  FILE_PUBLISH_COMPRESSED);
                        },
                        true);
            break;

        case FILE_PUBLISH_COMPRESSING:
            break;

        case FILE_PUBLISH_COMPRESSED:
            fi->setState(FILE_PUBLISH_MAKING_DIR);
            CLOG(DEBUG, "History") << "Making remote directory "
                                   << fi->remoteDir();
            hm.mkdir(mArchive, fi->remoteDir(),
                     [this, name](asio::error_code const& ec)
                     {
                         this->fileStateChange(ec, name, FILE_PUBLISH_MADE_DIR);
                     });
            break;

        case FILE_PUBLISH_MAKING_DIR:
            break;

        case FILE_PUBLISH_MADE_DIR:
            fi->setState(FILE_PUBLISH_UPLOADING);
            CLOG(INFO, "History") << "Publishing " << name;
            hm.putFile(mArchive, fi->localPath_gz(), fi->remoteName(),
                       [this, name](asio::error_code const& ec)
                       {
                           this->fileStateChange(ec, name,
                                                 FILE_PUBLISH_UPLOADED);
                       });
            break;

        case FILE_PUBLISH_UPLOADING:
            break;

        case FILE_PUBLISH_UPLOADED:
            std::remove(fi->localPath_gz().c_str());
            break;
        }

        minimumState = std::min(fi->getState(), minimumState);
    }

    if (minimumState == FILE_PUBLISH_FAILED)
    {
        CLOG(WARNING, "History") << "Some file-puts failed, retrying";
        enterRetryingState();
    }
    else if (minimumState == FILE_PUBLISH_UPLOADED)
    {
        CLOG(INFO, "History") << "All file-puts succeeded";
        enterCommittingState();
    }
    else
    {
        CLOG(DEBUG, "History") << "Some file-puts still in progress";
        // Do nothing here; in-progress states have callbacks set up already
        // which will fire when they complete.
    }
}

void
ArchivePublisher::enterCommittingState()
{
    assert(mState == PUBLISH_SENDING);
    mState = PUBLISH_COMMITTING;
    mArchive->putState(
        mApp, mSnap->mLocalState, [this](asio::error_code const& ec)
        {
            if (ec)
            {
                CLOG(WARNING, "History")
                    << "Publisher failed to update state in history archive '"
                    << this->mArchive->getName() << "', restarting publish";
                this->enterRetryingState();
            }
            else
            {
                this->enterEndState();
            }
        });
}

void
ArchivePublisher::enterEndState()
{
    CLOG(DEBUG, "History") << "Finished publishing to archive '"
                           << this->mArchive->getName() << "'";
    mState = PUBLISH_END;
    mEndHandler(mError);
}

bool
ArchivePublisher::isDone() const
{
    return mState == PUBLISH_END;
}

PublishStateMachine::PublishStateMachine(Application& app)
    : mApp(app)
    , mPublishersSize(app.getMetrics().NewCounter(
                          {"history", "memory", "publishers"}))
    , mPendingSnapsSize(app.getMetrics().NewCounter(
                            {"history", "memory", "pending-snaps"}))
    , mRecheckRunningMergeTimer(app)
{
}

bool
PublishStateMachine::queueSnapshot(SnapshotPtr snap, PublishCallback handler)
{
    bool delayed = !mPendingSnaps.empty();
    mPendingSnaps.push_back(std::make_pair(snap, handler));
    mPendingSnapsSize.set_count(mPendingSnaps.size());
    if (delayed)
    {
        CLOG(WARNING, "History") << "Snapshot queued while already publishing";
    }
    else
    {
        writeNextSnapshot();
    }
    return delayed;
}

StateSnapshot::StateSnapshot(Application& app)
    : mApp(app)
    , mLocalState(app.getHistoryManager().getLastClosedHistoryArchiveState())
    , mSnapDir(app.getTmpDirManager().tmpDir("snapshot"))
    , mLedgerSnapFile(std::make_shared<FilePublishInfo>(
          FILE_PUBLISH_NEEDED, mSnapDir, HISTORY_FILE_TYPE_LEDGER,
          app.getHistoryManager().prevCheckpointLedger(
              mLocalState.currentLedger)))

    , mTransactionSnapFile(std::make_shared<FilePublishInfo>(
          FILE_PUBLISH_NEEDED, mSnapDir, HISTORY_FILE_TYPE_TRANSACTIONS,
          app.getHistoryManager().prevCheckpointLedger(
              mLocalState.currentLedger)))

    , mTransactionResultSnapFile(std::make_shared<FilePublishInfo>(
          FILE_PUBLISH_NEEDED, mSnapDir, HISTORY_FILE_TYPE_RESULTS,
          app.getHistoryManager().prevCheckpointLedger(
              mLocalState.currentLedger)))
{
    BucketList& buckets = app.getBucketManager().getBucketList();
    for (size_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto& level = buckets.getLevel(i);
        if (level.getNext().isLive())
        {
            mLocalBuckets.push_back(level.getNext().resolve());
        }
        mLocalBuckets.push_back(level.getCurr());
        mLocalBuckets.push_back(level.getSnap());
    }
}

bool
StateSnapshot::writeHistoryBlocks() const
{

    std::unique_ptr<soci::session> snapSess(
        mApp.getDatabase().canUsePool()
            ? make_unique<soci::session>(mApp.getDatabase().getPool())
            : nullptr);
    soci::session& sess(snapSess ? *snapSess : mApp.getDatabase().getSession());
    soci::transaction tx(sess);

    // The current "history block" is stored in _three_ files, one just ledger
    // headers, one TransactionHistoryEntry (which contain txSets) and
    // one TransactionHistoryResultEntry containing transaction set results.
    // All files are streamed out of the database, entry-by-entry.
    XDROutputFileStream ledgerOut, txOut, txResultOut;
    ledgerOut.open(mLedgerSnapFile->localPath_nogz());
    txOut.open(mTransactionSnapFile->localPath_nogz());
    txResultOut.open(mTransactionResultSnapFile->localPath_nogz());

    uint32_t count = mApp.getHistoryManager().getCheckpointFrequency();

    // 'mLocalState' describes the LCL, so its currentLedger will be 63, 127,
    // 191, etc. We want to start our snapshot at 64-before the _next_ ledger:
    // 0, 64, 128, etc.
    assert(mLocalState.currentLedger + 1 >= count);
    uint32_t begin = mLocalState.currentLedger + 1 - count;

    CLOG(DEBUG, "History") << "Streaming " << count
                           << " ledgers worth of history, from " << begin;

    size_t nHeaders = LedgerHeaderFrame::copyLedgerHeadersToStream(
        mApp.getDatabase(), sess, begin, count, ledgerOut);
    size_t nTxs = TransactionFrame::copyTransactionsToStream(
        mApp.getDatabase(), sess, begin, count, txOut, txResultOut);
    CLOG(DEBUG, "History") << "Wrote " << nHeaders << " ledger headers to "
                           << mLedgerSnapFile->localPath_nogz();
    CLOG(DEBUG, "History") << "Wrote " << nTxs << " transactions to "
                           << mTransactionSnapFile->localPath_nogz() << " and "
                           << mTransactionResultSnapFile->localPath_nogz();

    return true;
}

std::shared_ptr<StateSnapshot>
PublishStateMachine::takeSnapshot(Application& app)
{
    // Capture local state and _all_ the local buckets at this instant; these
    // may be expired from the bucketlist while the subsequent put-callbacks are
    // running but the buckets are immutable and we hold shared_ptrs to them
    // here, include those in the callbacks themselves.
    return std::make_shared<StateSnapshot>(app);
}

void
PublishStateMachine::writeNextSnapshot()
{
    // Once we've taken a (synchronous) snapshot of the buckets and db, we then
    // run writeHistoryBlocks() to get the tx and ledger history files written
    // out from the db. This may run synchronously (if we're not using a
    // thread-pool-friendly db backend) or asynchronously on the worker pool if
    // we're on, say, postgres). In either case, when complete it will call back
    // to snapshotWritten(), at which point we can begin the actual publishing
    // work.

    if (mPendingSnaps.empty())
        return;

    auto snap = mPendingSnaps.front().first;

    snap->mLocalState.resolveAnyReadyFutures();
    if (!snap->mLocalState.futuresAllResolved())
    {
        CLOG(WARNING, "History")
            << "Queued snapshot still awaiting running merges";
        mRecheckRunningMergeTimer.expires_from_now(std::chrono::seconds(2));
        mRecheckRunningMergeTimer.async_wait(
            [this](asio::error_code const& ec)
            {
                if (!ec)
                {
                    this->writeNextSnapshot();
                }
            });
        return;
    }

    if (mApp.getDatabase().canUsePool())
    {
        mApp.getWorkerIOService().post(
            [snap]()
            {
                asio::error_code ec;
                if (!snap->writeHistoryBlocks())
                {
                    ec = std::make_error_code(std::errc::io_error);
                }
                snap->mApp.getClock().getIOService().post(
                    [snap, ec]()
                    {
                        snap->mApp.getHistoryManager().snapshotWritten(ec);
                    });
            });
    }
    else
    {
        asio::error_code ec;
        if (!snap->writeHistoryBlocks())
        {
            ec = std::make_error_code(std::errc::io_error);
        }
        mApp.getHistoryManager().snapshotWritten(ec);
    }
}

void
PublishStateMachine::snapshotWritten(asio::error_code const& ec)
{
    auto snap = mPendingSnaps.front().first;

    if (ec)
    {
        CLOG(WARNING, "History")
            << "Failed to snapshot state, abandoning publication";
        finishOne(ec);
        return;
    }
    CLOG(DEBUG, "History") << "Publishing snapshot of ledger "
                           << snap->mLocalState.currentLedger;

    // Iterate over writable archives instantiating an ArchivePublisher for them
    // with a callback that returns to the PublishStateMachine and possibly
    // fires its own callback when they are all done.
    auto const& hist = mApp.getConfig().HISTORY;
    std::vector<std::shared_ptr<HistoryArchive>> writableArchives;
    for (auto const& pair : hist)
    {
        if (pair.second->hasGetCmd() && pair.second->hasPutCmd())
        {
            auto p = std::make_shared<ArchivePublisher>(
                mApp,
                [this](asio::error_code const& ec)
                {
                    this->snapshotPublished(ec);
                },
                pair.second, snap);
            mPublishers.push_back(p);
            mPublishersSize.set_count(mPublishers.size());
        }
    }
}

void
PublishStateMachine::snapshotPublished(asio::error_code const& ec)
{
    asio::error_code ecSaved(ec); // make a copy of ec as it could be deleted by following statement
    mPublishers.erase(std::remove_if(mPublishers.begin(), mPublishers.end(),
                                     [](std::shared_ptr<ArchivePublisher> p)
                                     {
        return p->isDone();
    }));
    mPublishersSize.set_count(mPublishers.size());
    CLOG(DEBUG, "History") << "Completed publish to archive, "
                           << mPublishers.size() << " remain";
    if (mPublishers.empty())
    {
        finishOne(ecSaved);
    }
}

void
PublishStateMachine::finishOne(asio::error_code const& ec)
{
    assert(!mPendingSnaps.empty());
    mPendingSnaps.front().second(ec);
    mPendingSnaps.pop_front();
    mPendingSnapsSize.set_count(mPendingSnaps.size());
    writeNextSnapshot();
}
}
