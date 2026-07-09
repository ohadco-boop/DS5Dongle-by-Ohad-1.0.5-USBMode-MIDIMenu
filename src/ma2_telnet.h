#pragma once
#include <cstdint>
#include <stdbool.h>

void ma2_telnet_init();
void ma2_telnet_tick();
void ma2_telnet_reconfigure();
bool ma2_telnet_connected();
bool ma2_telnet_logged_in();
const char* ma2_telnet_status();
void ma2_telnet_send_command(const char* cmd);

void ma2_remote_process_report(const uint8_t report[63]);
void ma2_remote_tick();
