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

#include "vmem.h"

#include <cassert>
#include <fmt/core.h>

#include "champsim.h"
#include "dram_controller.h"
#include "my_memory_controller.h"
#include "util/bits.h"

#include <iostream>
#include <unistd.h>

using namespace champsim::data::data_literals;

VirtualMemory::VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                             MY_MEMORY_CONTROLLER& dram_, std::optional<uint64_t> randomization_seed_)
    : randomization_seed(randomization_seed_), dram(dram_), minor_fault_penalty(minor_penalty), pt_levels(page_table_levels),
      pte_page_size(page_table_page_size),
      next_pte_page(
          champsim::dynamic_extent{champsim::data::bits{LOG2_PAGE_SIZE}, champsim::data::bits{champsim::lg2(champsim::data::bytes{pte_page_size}.count())}}, 0)
{

  sleep(1);
  std::cout<<"vmem constructor"<<std::endl;
  assert(pte_page_size > 1_kiB);
  assert(champsim::is_power_of_2(pte_page_size.count()));

  champsim::page_number last_vpage{
      champsim::lowest_address_for_size(champsim::data::bytes{PAGE_SIZE + champsim::ipow(pte_page_size.count(), static_cast<unsigned>(pt_levels))})};
  champsim::data::bits required_bits{LOG2_PAGE_SIZE + champsim::lg2(last_vpage.to<uint64_t>())};
  if (required_bits > champsim::address::bits) {
    // fmt::print("[VMEM] WARNING: virtual memory configuration would require {} bits of addressing.\n", required_bits); // LCOV_EXCL_LINE
    std::cout << "[VMEM] WARNING: virtual memory configuration would require !required_bits! of addressing." << std::endl; // LCOV_EXCL_LINE
  }
  if (required_bits > champsim::data::bits{champsim::lg2(dram.size().count())}) {
    // fmt::print("[VMEM] WARNING: physical memory size is smaller than virtual memory size.\n"); // LCOV_EXCL_LINE
    std::cout << "[VMEM] WARNING: physical memory size is smaller than virtual memory size." << std::endl; // LCOV_EXCL_LINE
  }
  populate_pages();
  shuffle_pages();
  std::cout<<"returning from vmem constructor"<<std::endl;
}

VirtualMemory::VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                             MY_MEMORY_CONTROLLER& dram_)
    : VirtualMemory(page_table_page_size, page_table_levels, minor_penalty, dram_, {})
{
}

void VirtualMemory::populate_pages()
{
  std::cout<<"call for dram.size()"<<std::endl;
  std::cout << "dram.size(): " << dram.size().count() << " bytes" << std::endl;
  std::cout << "ppage_free_list.resize(): "<<((dram.size() - 1_MiB) / PAGE_SIZE).count()<<std::endl;
  assert(dram.size() > 1_MiB);
  std::cout<<"returned dram.size()"<<std::endl; // original expected was 16777216 BYTES? small?
  //ppage_free_list.resize(((dram.size() - 1_MiB) / PAGE_SIZE).count());
  ppage_free_list.resize(32768); //orig was 3206
  //std::cout<<"returned page_free_list.resize.count"<<std::endl;
  assert(ppage_free_list.size() != 0);
  champsim::page_number base_address =
      champsim::page_number{champsim::lowest_address_for_size(std::max<champsim::data::mebibytes>(champsim::data::bytes{PAGE_SIZE}, 1_MiB))};
  for (auto it = ppage_free_list.begin(); it != ppage_free_list.end(); it++) {
    *it = base_address;
    base_address++;
  }
  std::cout<<"returning from populate_pages()"<<std::endl;
}

void VirtualMemory::shuffle_pages()
{
  if (randomization_seed.has_value())
    std::shuffle(ppage_free_list.begin(), ppage_free_list.end(), std::mt19937_64{randomization_seed.value()});
}

champsim::dynamic_extent VirtualMemory::extent(std::size_t level) const
{
  const champsim::data::bits lower{LOG2_PAGE_SIZE + champsim::lg2(pte_page_size.count()) * (level - 1)};
  const auto size = static_cast<std::size_t>(champsim::lg2(pte_page_size.count()));
  return champsim::dynamic_extent{lower, size};
}

champsim::data::bits VirtualMemory::shamt(std::size_t level) const { return extent(level).lower; }

uint64_t VirtualMemory::get_offset(champsim::address vaddr, std::size_t level) const { return champsim::address_slice{extent(level), vaddr}.to<uint64_t>(); }

uint64_t VirtualMemory::get_offset(champsim::page_number vaddr, std::size_t level) const { return get_offset(champsim::address{vaddr}, level); }

champsim::page_number VirtualMemory::ppage_front() const
{
  assert(available_ppages() > 0);
  return ppage_free_list.front();
}

void VirtualMemory::ppage_pop()
{
  ppage_free_list.pop_front();
  if (available_ppages() == 0) {
    // fmt::print("[VMEM] WARNING: Out of physical memory, freeing ppages\n");
    std::cout << "[VMEM] WARNING: Out of physical memory, freeing ppages" << std::endl;
    populate_pages();
    shuffle_pages();
  }
}

std::size_t VirtualMemory::available_ppages() const { return (ppage_free_list.size()); }

std::pair<champsim::page_number, champsim::chrono::clock::duration> VirtualMemory::va_to_pa(uint32_t cpu_num, champsim::page_number vaddr)
{
  if (address_map != nullptr) {
    const uint64_t vaddr_bytes = vaddr.to<uint64_t>() << LOG2_PAGE_SIZE;
    auto entry = address_map->lookup(node_id, vaddr_bytes);
    if (entry && entry->type == SST::csimCore::AddressType::Pool) {
      const uint64_t start_page = entry->start >> LOG2_PAGE_SIZE;
      const uint64_t offset_pages = vaddr.to<uint64_t>() - start_page;
      const uint64_t pool_base_page = pool_pa_base >> LOG2_PAGE_SIZE;
      const uint64_t pool_offset_pages = entry->pool_offset >> LOG2_PAGE_SIZE;
      const champsim::page_number pool_ppage{pool_base_page + pool_offset_pages + offset_pages};

      auto [ppage, inserted] = vpage_to_ppage_map.try_emplace({cpu_num, champsim::page_number{vaddr}}, pool_ppage);
      auto penalty = inserted ? minor_fault_penalty : champsim::chrono::clock::duration::zero();
      return std::pair{ppage->second, penalty};
    }
  }

  auto [ppage, fault] = vpage_to_ppage_map.try_emplace({cpu_num, champsim::page_number{vaddr}}, ppage_front());

  // this vpage doesn't yet have a ppage mapping
  if (fault) {
    ppage_pop();
  }

  auto penalty = fault ? minor_fault_penalty : champsim::chrono::clock::duration::zero();

  if constexpr (champsim::debug_print) {
    // fmt::print("[VMEM] {} paddr: {} vpage: {} fault: {}\n", __func__, ppage->second, champsim::page_number{vaddr}, fault);
    std::cout << "[VMEM] " << __func__ << " paddr: " << ppage->second << " vpage: " << champsim::page_number{vaddr} << " fault: " << fault << std::endl;
  }

  return std::pair{ppage->second, penalty};
}

void VirtualMemory::set_address_map(const SST::csimCore::AddressMap* map, uint32_t node_id_, uint64_t pool_pa_base_)
{
  address_map = map;
  node_id = node_id_;
  pool_pa_base = pool_pa_base_;
}

std::pair<champsim::address, champsim::chrono::clock::duration> VirtualMemory::get_pte_pa(uint32_t cpu_num, champsim::page_number vaddr, std::size_t level)
{
  if (champsim::page_offset{next_pte_page} == champsim::page_offset{0}) {
    active_pte_page = ppage_front();
    ppage_pop();
  }

  champsim::dynamic_extent pte_table_entry_extent{champsim::address::bits, shamt(level)};
  auto [ppage, fault] =
      page_table.try_emplace({cpu_num, level, champsim::address_slice{pte_table_entry_extent, vaddr}}, champsim::splice(active_pte_page, next_pte_page));

  // this PTE doesn't yet have a mapping
  if (fault) {
    next_pte_page++;
  }

  auto offset = get_offset(vaddr, level);
  champsim::address paddr{
      champsim::splice(ppage->second, champsim::address_slice{champsim::dynamic_extent{champsim::data::bits{champsim::lg2(pte_entry::byte_multiple)},
                                                                                       static_cast<std::size_t>(champsim::lg2(pte_page_size.count()))},
                                                              offset})};
  if constexpr (champsim::debug_print) {
    // fmt::print("[VMEM] {} paddr: {} vaddr: {} pt_page_offset: {} translation_level: {} fault: {}\n", __func__, paddr, vaddr, offset, level, fault);
    std::cout << "[VMEM] " << __func__ << " paddr: " << paddr << " vaddr: " << vaddr << " pt_page_offset: " << offset << " translation_level: " << level << " fault: " << fault << std::endl;
  }

  auto penalty = minor_fault_penalty;
  if (!fault) {
    penalty = champsim::chrono::clock::duration::zero();
  }

  return {paddr, penalty};
}
