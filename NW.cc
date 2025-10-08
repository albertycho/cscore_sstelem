#include "sst_config.h"
#include "NW.h"
//#include "csEvent.h"

namespace SST {
namespace csimCore {
NWsim::NWsim(SST::ComponentId_t id, SST::Params& params): 
Component(id) 
{
    std::string clock_frequency_str = params.find<std::string>("clock", "2GHz");
    auto outf_name = params.find<std::string>("output_file", "~/SST_TEST/sst-elements/src/sst/elements/csnode/tests/asdfasdf2.out");
    outf.open(outf_name);
    if(!outf.is_open()){
        std::cerr<<"failed to open output file "<<outf_name<<std::endl;
    }

    registerClock(clock_frequency_str, new Clock::Handler<NWsim>(this,
			      &NWsim::NW_tick));
    
    int tmp_num_links=NUM_NODES*CORES_PER_NODE;
    for(int i=0; i<tmp_num_links; i++){
        DBGP(std::cout<<"NWsim: port_handler"<<i<<" configured"<<std::endl;)
        std::string port_handler_name = "port_handler" + std::to_string(i);
        linkHandler[i] = configureLink(port_handler_name, new Event::Handler<NWsim>(this, &NWsim::handleNWEvent));
    }
    DBGP(std::cout<<"NWsim initialized"<<std::endl;)

}


bool NWsim::NW_tick(Cycle_t){ 
    // this is for link testing with multiple nodes
    cycle_count++;
    //TODO - cycle the buffer, and send ready msgs to dests
    allowed_msgs = NW_LIM_MSG_PER_CYCLE;
    //DBG
    if(!delayed_msgs.empty()){std::cout<<"MSG Q Occupancy: "<<delayed_msgs.size()<<std::endl;}
    /////
    //DBGP(std::cout<<"NW_tick"<<std::endl;)
    while(!delayed_msgs.empty()){
        if(allowed_msgs<1){
            break;
        }
        //pop delayed_msgs head and send it to dest
        csEvent *cevent = *(delayed_msgs.begin());
        delayed_msgs.erase(delayed_msgs.begin());
        uint64_t dest = cevent->payload[1];
        linkHandler[dest]->send(cevent);

        //DBGP
        outf<<"sent delayed msg"<<std::endl;

        allowed_msgs--;

    }
    bool retval = false;
	return retval;
}

void NWsim::handleNWEvent(SST::Event *ev)
{
    //return;
    DBGP(std::cout<<"NW handleNWEvent"<<std::endl;)
    csEvent *cevent = dynamic_cast<csEvent*>(ev);
    //DBG
    // if(cevent){
    //     for(int i=0; i<16;i++){
    //         std::cout<<"NW is sending to node"<<i<<std::endl;
    //         linkHandler[i]->send(cevent);
    //     }        
    //     return;
    // }
    ///DBGEND
    if(cevent){
        //TODO: in final form, in this segment we push msg to delayed/BW constarined buffer
        if(allowed_msgs>0){
            //std::cout<<"payload size: "<<cevent->payload.size()<<", src: "<< cevent->payload[0]<<", cycle: "<< cevent->payload[1]<<std::endl;
            uint64_t dest = cevent->payload[1];
            uint64_t src = cevent->payload[0];
            outf<<"sending packet from Node"<< src<<" to Node"<<dest<<std::endl;
            linkHandler[dest]->send(cevent);
            
            allowed_msgs--;
        }
        else{
            delayed_msgs.push_back(cevent);
        }
    }
    else{
        std::cout<<"cevent NULL"<<std::endl;
    }
    return;
}

}
}
