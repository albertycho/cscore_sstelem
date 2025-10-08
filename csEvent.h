// Copyright 2009-2023 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2023, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef _CSEVENT_H
#define _CSEVENT_H

#include <sst/core/event.h>

/*
 * NW packet: 
 *      paylaod[0]=src
 *      paylaod[1]=dest
 *      paylaod[2]=incycle
 */

namespace SST {
namespace csimCore {

/*
 *  Example Event that gets passed on links 
 *  between components in this library
 *
 * All Events inherit from SST::Event
 * Events that can be sent on regular links need to be serializable:
 *  1. Implement the serialize_order function which
 *      a. Calls the base class serialize_order
 *      b. Serializes ALL fields in the event
 *  2. Declare themselves serializable with 'ImplementSerializable(FULLY::QUALIFIED::CLASS::NAME)'
 *
 */

class csEvent : public SST::Event
{
public:
    // Constructor
    csEvent() : SST::Event(), last(false) { }
    
    // Example data members
    //std::vector<char> payload;
    std::vector<uint64_t> payload;

    bool last;

    // Events must provide a serialization function that serializes
    // all data members of the event
    void serialize_order(SST::Core::Serialization::serializer &ser)  override {
        Event::serialize_order(ser);
        ser & payload;
        ser & last;
    }

    // Register this event as serializable
    ImplementSerializable(SST::csimCore::csEvent);
};

} // namespace simpleElementExample
} // namespace SST

#endif /* _CSEVENT_H */
