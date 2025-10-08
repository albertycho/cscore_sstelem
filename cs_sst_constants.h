/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CS_SST_CONSTANTS_H
#define CS_SST_CONSTANTS_H

//#define NUM_NODES 16
//#define CORES_PER_NODE 1

#define NUM_NODES 16
#define CORES_PER_NODE 1

/* BW rate limit. 
 *  for 1/28 scale
 *  Assuming 16B for rd request and 64B for response with data,
 *    CXL: 1.4GB/s --> 0.7B per cycle
 *         req : 16/0.7 = 23
 *         resp: 64/0.7 = 91
 *    NW (TODO - how big is a msg? Assuming 80B for now) Assume 100Gb/s (=12.5GB/s) NIC at full scale
 *      scale down BW is 0.45GB/s (let's say 0.5GB/s). 0.25B per cycle
 *         80B/0.5B = 160
 *         
 *        
 */
#define CYCLES_CXL_REQ 23
#define CYCLES_CXL_DATA 91
#define CYCLES_NW   160

/* NW_LIM_MSG_PER_CYCLE = (NWBW_in_GBps) / (msgsize * NW_frequency) 
 *                      = (12GBps) / (64B? * 2Billion?)
 * may need cycle countdown (1 msg per N cycles)
 */
#define NW_LIM_MSG_PER_CYCLE 100

#define HEARTBEAT_PERIOD 10000000

#define DBG_PRINT_ON 1
#define MINIMAL_P 0

#if DBG_PRINT_ON
//#define DBGP(x) x
#define DBGP(x) { x }
#else
#define DBGP(x)
#endif

#if MINIMAL_P
//#define DBGP(x) x
#define NORMAL_PRINT(x)
#else
#define NORMAL_PRINT(x) { x }
#endif


#endif
