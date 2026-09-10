/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Id.h"

#include "Test.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

CC_TEST(Id, EqualityComparesAllThreeFields)
{
    const Id a(3, Endpoint::DamperTarget, Operation::Set);
    const Id b(3, Endpoint::DamperTarget, Operation::Set);
    const Id differsByNode(4, Endpoint::DamperTarget, Operation::Set);
    const Id differsByEndpoint(3, Endpoint::DamperActual, Operation::Set);
    const Id differsByOperation(3, Endpoint::DamperTarget, Operation::Get);

    CC_CHECK(a == b);
    CC_CHECK(!(a != b));
    CC_CHECK(a != differsByNode);
    CC_CHECK(a != differsByEndpoint);
    CC_CHECK(a != differsByOperation);
}

CC_TEST(Id, OrderingIsNodeThenEndpointThenOperation)
{
    const Id low(1, Endpoint::DamperTarget, Operation::Get);
    const Id highNode(2, Endpoint::DamperTarget, Operation::Get);
    const Id highEndpoint(1, Endpoint::RoomTemp, Operation::Get);
    const Id highOperation(1, Endpoint::DamperTarget, Operation::Set);

    CC_CHECK(low < highNode);
    CC_CHECK(low < highEndpoint);
    CC_CHECK(low < highOperation);
    CC_CHECK(!(low < low));
}

CC_TEST(Message, DefaultIsAZeroLengthTransportGet)
{
    const Message m;
    CC_CHECK_EQ(m.len, 0);
    CC_CHECK_EQ(m.id.node, 0);
    CC_CHECK(m.id.endpoint == Endpoint::Transport);
    CC_CHECK(m.id.operation == Operation::Get);
}

CC_TEST(Message, TransportConstructorSetsEndpointAndNoData)
{
    const Message m(5, Operation::Announce);
    CC_CHECK_EQ(m.id.node, 5);
    CC_CHECK(m.id.endpoint == Endpoint::Transport);
    CC_CHECK(m.id.operation == Operation::Announce);
    CC_CHECK_EQ(m.len, 0);
}

CC_TEST(Message, SingleValueConstructorCarriesOneByte)
{
    const Message m(7, Endpoint::DamperTarget, Operation::Set, 42);
    CC_CHECK_EQ(m.id.node, 7);
    CC_CHECK(m.id.endpoint == Endpoint::DamperTarget);
    CC_CHECK_EQ(m.len, 1);
    CC_CHECK_EQ(m.data[0], 42);
}
