/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Id.h"

using NodeLib::Id;

/*************************************************************
 * Compare (<) two ID's
 *************************************************************/
bool Id::operator<(const Id& other) const
{
    if (this->node != other.node)
    {
        return (this->node < other.node);
    }
    if (this->channel != other.channel)
    {
        return (this->channel < other.channel);
    }
    if (this->operation != other.operation)
    {
        return (this->operation < other.operation);
    }
    return false;
}

/*************************************************************
 * Comparison operator is equal
 *************************************************************/
bool Id::operator==(const Id& other) const
{
    if (this->node != other.node)
    {
        return false;
    }
    if (this->channel != other.channel)
    {
        return false;
    }
    if (this->operation != other.operation)
    {
        return false;
    }
    return true;
}
