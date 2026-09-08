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
    if (this->endpoint != other.endpoint)
    {
        return (this->endpoint < other.endpoint);
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
    if (this->endpoint != other.endpoint)
    {
        return false;
    }
    if (this->operation != other.operation)
    {
        return false;
    }
    return true;
}
