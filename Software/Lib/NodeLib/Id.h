/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "EChannelId.h"
#include "EOperation.h"

namespace NodeLib
{
    // MAX_DATA=32, see RS485-Node-Protocol-Spec-STM32G030.md §9 (open item, defaulted)
    static const uint8_t MAX_DATA = 32;

    // Hard cap on the slave-node count -- sizes NodeMaster's fixed activeNodes[]
    // array and bounds a provisioned NodeId (ConfigStore::Valid()). Node::maxNodes
    // aliases this so there is one definition.
    static const uint8_t MAX_NODES = 25;

    struct __attribute__((packed)) Id
    {
        uint8_t   node;
        ChannelId channel;
        Operation operation;

        Id() :
            node(0),
            channel(ChannelId::INTERNAL_MSG),
            operation(Operation::GET)
        {
        }

        Id(const uint8_t node, const ChannelId channel) :
            node(node),
            channel(channel),
            operation(Operation::GET)
        {
        }

        Id(const uint8_t node, const ChannelId channel, const Operation operation) :
            node(node),
            channel(channel),
            operation(operation)
        {
        }

        bool operator<(const Id& other) const;
        bool operator==(const Id& other) const;

        inline bool operator!=(const Id& other) const
        {
            return !(*this == other);
        }
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const NodeLib::Id id)
    {
        oStrStream << " Node: " << id.node << " Channel: " << id.channel << " Operation: " << id.operation;

        return oStrStream;
    }

    // Variable-length payload replacing v1's single uint8_t Value, per
    // RS485-Node-Protocol-Spec-STM32G030.md §3/§8. Fixed-size slot (not a pointer)
    // so message queues stay heap-free, per spec §7.
    struct Message
    {
        Id      id;
        uint8_t data[MAX_DATA];
        uint8_t len;

        Message() :
            id(0, ChannelId::INTERNAL_MSG, Operation::GET),
            data{},
            len(0)
        {
        }

        Message(const uint8_t node, const Operation op) :
            id(node, ChannelId::INTERNAL_MSG, op),
            data{},
            len(0)
        {
        }

        Message(const Id& id) :
            id(id),
            data{},
            len(0)
        {
        }

        // Convenience for the common single-byte-value case (matches v1's Value
        // semantics -- most channels still only ever send one byte).
        Message(const uint8_t node, const ChannelId channel, const Operation operation, const uint8_t value) :
            id(node, channel, operation),
            data{value},
            len(1)
        {
        }

        Message(const Id& id, const uint8_t value) :
            id(id),
            data{value},
            len(1)
        {
        }
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const NodeLib::Message& m)
    {
        oStrStream << m.id << " Len: " << m.len;

        return oStrStream;
    }
} // namespace NodeLib
