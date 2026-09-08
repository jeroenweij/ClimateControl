/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "EEndpoint.h"
#include "EOperation.h"

namespace NodeLib
{
    // MAX_DATA=32, see RS485-Node-Protocol-Spec-STM32G030.md §9 (open item, defaulted)
    static const uint8_t MAX_DATA = 32;

    // Hard cap on the slave-node count -- sizes NodeMaster's fixed activeNodes[]
    // array and bounds a provisioned NodeId (ConfigStore::Valid()). Node::maxNodes
    // aliases this so there is one definition.
    static const uint8_t MAX_NODES = 25;

    // Broadcast address -- valid only with Operation::Set (fire-and-forget, no
    // reply). See Spec/Node-Message-Model-Spec.md §2.
    static const uint8_t BROADCAST_NODE = 0xFF;

    struct __attribute__((packed)) Id
    {
        uint8_t   node;
        Endpoint  endpoint;
        Operation operation;

        Id() :
            node(0),
            endpoint(Endpoint::Transport),
            operation(Operation::Get)
        {
        }

        Id(const uint8_t node, const Endpoint endpoint) :
            node(node),
            endpoint(endpoint),
            operation(Operation::Get)
        {
        }

        Id(const uint8_t node, const Endpoint endpoint, const Operation operation) :
            node(node),
            endpoint(endpoint),
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
        oStrStream << " Node: " << id.node << " Endpoint: " << id.endpoint << " Operation: " << id.operation;

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
            id(0, Endpoint::Transport, Operation::Get),
            data{},
            len(0)
        {
        }

        // Transport-level messages (Discover / Announce / Poll / Done).
        Message(const uint8_t node, const Operation op) :
            id(node, Endpoint::Transport, op),
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

        // Convenience for the common single-byte-value case.
        Message(const uint8_t node, const Endpoint endpoint, const Operation operation, const uint8_t value) :
            id(node, endpoint, operation),
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
