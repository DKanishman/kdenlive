#include "jobs/abstracttask.h"
#include "jobs/taskmanager.h"
#include <QMutexLocker>
namespace {
struct McpCancellationState
{
    QAtomicInt started{0};
    QAtomicInt observed{0};
    QAtomicInt finished{0};
};
class McpCancellationTask : public AbstractTask
{
public:
    explicit McpCancellationTask(const ObjectId &owner, std::shared_ptr<McpCancellationState> state)
        : AbstractTask(owner, AbstractTask::FILTERCLIPJOB, nullptr)
        , state(std::move(state))
    {
    }

protected:
    void run() override
    {
        AbstractTaskDone completion(m_owner.itemId, this);
        QMutexLocker lock(&m_runMutex);
        m_running = true;
        state->started.storeRelease(1);
        QElapsedTimer timer;
        timer.start();
        while (!m_isCanceled.loadAcquire() && timer.elapsed() < 3000)
            QThread::msleep(1);
        if (m_isCanceled.loadAcquire()) state->observed.storeRelease(1);
        // Simulate a worker that takes time to finish cleaning up after cancellation.
        QThread::msleep(250);
        m_running = false;
        m_progress = 100;
        state->finished.storeRelease(1);
    }

private:
    std::shared_ptr<McpCancellationState> state;
};
} // namespace
TEST_CASE("MCP background cancellation requests do not wait on worker cleanup", "[MCP][Cancellation]")
{
    McpProject project;
    project.open();
    const ObjectId owner(KdenliveObjectType::BinClip, 999999, QUuid());
    auto state = std::make_shared<McpCancellationState>();
    auto *task = new McpCancellationTask(owner, state);
    pCore->taskManager.startTask(owner.itemId, task);
    QElapsedTimer deadline;
    deadline.start();
    while (!state->started.loadAcquire() && deadline.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    REQUIRE(state->started.loadAcquire());
    QElapsedTimer request;
    request.start();
    CHECK(pCore->taskManager.requestCancellation(owner, AbstractTask::FILTERCLIPJOB));
    CHECK(request.elapsed() < 200);
    CHECK_FALSE(state->finished.loadAcquire());
    CHECK_FALSE(pCore->taskManager.requestCancellation(ObjectId(KdenliveObjectType::TimelineClip, owner.itemId, QUuid()), AbstractTask::FILTERCLIPJOB));
    deadline.restart();
    while ((pCore->taskManager.jobStatus(owner) != TaskManagerStatus::NoJob || !state->finished.loadAcquire()) && deadline.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    CHECK(state->observed.loadAcquire());
    CHECK(state->finished.loadAcquire());
    CHECK(pCore->taskManager.jobStatus(owner) == TaskManagerStatus::NoJob);
}
