#include "prism/runtime/owner_feedback_session.hpp"

#include <cassert>
#include <limits>
#include <stdexcept>

int main()
{
    prism::runtime::OwnerFeedbackSession session;
    assert(!session.Deadline() && !session.Expired(1000) && !session.Adopt(0, 0));

    session.Begin(1, 100);
    assert(!session.Deadline() && !session.Expired(1000));
    assert(!session.Adopt(2, 1000));
    assert(session.Adopt(1, 1000));
    assert(session.Deadline() == 1100);
    assert(!session.Adopt(1, 2000));
    assert(!session.Expired(1099) && session.Expired(1100));

    session.Pause(1040);
    session.Pause(1060);
    assert(!session.Deadline() && !session.Expired(10000));
    session.Resume(10000);
    session.Resume(10010);
    assert(session.Deadline() == 10060);
    session.Pause(10050);
    session.Resume(20000);
    assert(session.Deadline() == 20010);
    assert(!session.Expired(20009) && session.Expired(20010));

    session.Begin(2, 100);
    assert(!session.Adopted() && !session.Deadline());
    assert(!session.Adopt(1, 0));
    assert(session.Adopt(2, 100, true));
    assert(!session.Deadline() && !session.Expired(10000));
    session.Resume(10000);
    assert(session.Deadline() == 10100);

    session.Begin(3, 0);
    assert(session.Adopt(3, 0));
    session.Pause(10);
    session.Resume(20);
    assert(!session.Deadline() && !session.Expired(std::numeric_limits<std::uint64_t>::max()));

    session.Begin(4, 10);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    assert(session.Adopt(4, maximum - 5));
    assert(session.Deadline() == maximum);
    assert(!session.Expired(maximum - 1) && session.Expired(maximum));
    session.Pause(maximum);
    session.Resume(maximum);
    assert(session.Expired(maximum));

    session.Clear();
    session.Resume(10);
    session.Pause(20);
    assert(!session.Generation() && !session.Adopted() && !session.Deadline());

    bool rejected = false;
    try {
        session.Begin(0, 10);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected && !session.Generation());
}
