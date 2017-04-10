#include "envoy/event/file_event.h"

#include "common/event/dispatcher_impl.h"

#include "test/mocks/common.h"

namespace Event {

class FileEventImplTest : public testing::Test {
public:
  void SetUp() override {
    int rc = socketpair(AF_UNIX, SOCK_DGRAM, 0, fds_);
    ASSERT_EQ(0, rc);
    int data = 1;
    rc = write(fds_[1], &data, sizeof(data));
    ASSERT_EQ(sizeof(data), static_cast<size_t>(rc));
  }

  void TearDown() override {
    close(fds_[0]);
    close(fds_[1]);
  }

protected:
  int fds_[2];
};

TEST_F(FileEventImplTest, EdgeTrigger) {
  DispatcherImpl dispatcher;
  ReadyWatcher read_event;
  EXPECT_CALL(read_event, ready()).Times(1);
  ReadyWatcher write_event;
  EXPECT_CALL(write_event, ready()).Times(1);

  Event::FileEventPtr file_event =
      dispatcher.createFileEvent(fds_[0], [&](uint32_t events) -> void {
        if (events & FileReadyType::Read) {
          read_event.ready();
        }

        if (events & FileReadyType::Write) {
          write_event.ready();
        }
      }, FileTriggerType::Edge);

  dispatcher.run(Event::Dispatcher::RunType::NonBlock);
}

TEST_F(FileEventImplTest, LevelTrigger) {
  DispatcherImpl dispatcher;
  ReadyWatcher read_event;
  EXPECT_CALL(read_event, ready()).Times(2);
  ReadyWatcher write_event;
  EXPECT_CALL(write_event, ready()).Times(2);

  int count = 2;
  Event::FileEventPtr file_event =
      dispatcher.createFileEvent(fds_[0], [&](uint32_t events) -> void {
        if (count-- == 0) {
          dispatcher.exit();
          return;
        }
        if (events & FileReadyType::Read) {
          read_event.ready();
        }

        if (events & FileReadyType::Write) {
          write_event.ready();
        }
      }, FileTriggerType::Level);

  dispatcher.run(Event::Dispatcher::RunType::Block);
}

TEST_F(FileEventImplTest, SetEnabled) {
  DispatcherImpl dispatcher;
  ReadyWatcher read_event;
  EXPECT_CALL(read_event, ready()).Times(2);
  ReadyWatcher write_event;
  EXPECT_CALL(write_event, ready()).Times(2);

  Event::FileEventPtr file_event =
      dispatcher.createFileEvent(fds_[0], [&](uint32_t events) -> void {
        if (events & FileReadyType::Read) {
          read_event.ready();
        }

        if (events & FileReadyType::Write) {
          write_event.ready();
        }
      }, FileTriggerType::Edge);

  file_event->setEnabled(FileReadyType::Read);
  dispatcher.run(Event::Dispatcher::RunType::NonBlock);

  file_event->setEnabled(FileReadyType::Write);
  dispatcher.run(Event::Dispatcher::RunType::NonBlock);

  file_event->setEnabled(0);
  dispatcher.run(Event::Dispatcher::RunType::NonBlock);

  file_event->setEnabled(FileReadyType::Read | FileReadyType::Write);
  dispatcher.run(Event::Dispatcher::RunType::NonBlock);
}

} // Event
