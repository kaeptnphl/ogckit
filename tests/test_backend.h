// Controllable backend for core tests. Reset only after shutdown; all controls are synchronized.
#pragma once

#include "../src/internal.h"

namespace ogc {
namespace test_backend {

void reset();
void set_init_result(int code);
int add_device(uint16_t key, uint16_t vid, uint16_t pid,
               const InterfaceInfo* interfaces, int numInterfaces);
void set_bluetooth(int index, bool bluetooth);
void set_open_fail(int index, bool fail);
// Positive reports are consumed once; zero simulates an idle timeout.
void set_read_result(int index, int result, const uint8_t* data = nullptr, int len = 0);
void remove_device(int index);
void reconnect_device(int index);
int scan_count();
bool wait_for_scan(int count);
int open_count(int index);
int read_count(int index);
int close_count(int index);
int read_timeout_ms();
int init_count();
int exit_count();
void advance_clock(uint64_t ms);

void block_open(int index);
bool wait_for_open_blocked();
void release_open();
void block_scan();
bool wait_for_scan_blocked();
void release_scan();

}  // namespace test_backend
}  // namespace ogc
